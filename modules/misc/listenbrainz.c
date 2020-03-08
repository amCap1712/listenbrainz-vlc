/*****************************************************************************
 * listenbrainz.c : ListenBrainz submission plugin
 * ListenBrainz Submit Listens API 1
 * https://api.listenbrainz.org/1/submit-listens
 *****************************************************************************
 * Author: Kartik Ohri <kartikohri13 at gmail dot com>
 *
 * Copyright (C) 2020 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <time.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_input_item.h>
#include <vlc_dialog.h>
#include <vlc_meta.h>
#include <vlc_memstream.h>
#include <vlc_stream.h>
#include <vlc_url.h>
#include <vlc_tls.h>
#include <vlc_player.h>
#include <vlc_playlist.h>
#include <vlc_vector.h>

typedef struct listen_t
{
    char *psz_artist;
    char *psz_title;
    char *psz_album;
    char *psz_track_number;
    int i_length;
    char *psz_musicbrainz_id;
    time_t date;
} listen_t;

typedef struct VLC_VECTOR (listen_t) vlc_vector_listen_t;

struct intf_sys_t
{
    vlc_vector_listen_t p_queue;

    vlc_playlist_t *playlist;
    struct vlc_playlist_listener_id *playlist_listener;
    struct vlc_player_listener_id *player_listener;
    struct vlc_player_timer_id *timer_listener;

    vlc_mutex_t lock;
    vlc_cond_t wait;                // song to submit event
    vlc_thread_t thread;            // thread to submit song

    struct vlc_memstream payload, request;

    vlc_url_t p_submit_url;         // where to submit data
    char *psz_user_token;           // authentication token

    listen_t p_current_song;
    bool b_meta_read;               // check if song metadata is already read

    int64_t i_played_time;
};

static int Open (vlc_object_t *);

static void Close (vlc_object_t *);

static void *Run (void *);

#define USER_TOKEN_TEXT      N_("User token")
#define USER_TOKEN_LONGTEXT  N_("The user token of your ListenBrainz account")
#define URL_TEXT             N_("Submission URL")
#define URL_LONGTEXT         N_("The URL set for an alternative ListenBrainz instance")

/****************************************************************************
 * Module descriptor
 ****************************************************************************/

vlc_module_begin ()
    set_category(CAT_INTERFACE)
    set_subcategory(SUBCAT_INTERFACE_CONTROL)
    set_shortname (N_ ("ListenBrainz"))
    set_description (N_ ("Submit listens to ListenBrainz"))
    add_string("listenbrainz_user_token", "", USER_TOKEN_TEXT, USER_TOKEN_LONGTEXT, false)
    add_string("listenbrainz_submission_url", "api.listenbrainz.org", URL_TEXT, URL_LONGTEXT, false)
    set_capability("interface", 0)
    set_callbacks(Open, Close)
vlc_module_end ()

static void DeleteSong (listen_t *p_song)
{
    FREENULL (p_song->psz_artist);
    FREENULL (p_song->psz_album);
    FREENULL (p_song->psz_title);
    FREENULL (p_song->psz_musicbrainz_id);
    FREENULL (p_song->psz_track_number);
    p_song->date = 0;
}

static void ReadMetaData (intf_thread_t *p_this)
{
    bool b_skip = 0;
    intf_sys_t *p_sys = p_this->p_sys;

    vlc_player_t *player = vlc_playlist_GetPlayer (p_sys->playlist);
    input_item_t *item = vlc_player_GetCurrentMedia (player);
    if ( item == NULL )
        return;

    vlc_mutex_lock (&p_sys->lock);

    p_sys->b_meta_read = true;
    time (&p_sys->p_current_song.date);

#define RETRIEVE_METADATA(a, b) do { \
        char *psz_data = input_item_Get##b(item); \
        if (psz_data && *psz_data) \
            a = vlc_uri_encode(psz_data); \
        free(psz_data); \
    } while (0)

    RETRIEVE_METADATA(p_sys->p_current_song.psz_artist, Artist);
    if ( !p_sys->p_current_song.psz_artist )
    {
        msg_Dbg (p_this, "Artist missing.");
        DeleteSong (&p_sys->p_current_song);
        b_skip = 1;
    }

    RETRIEVE_METADATA(p_sys->p_current_song.psz_title, Title);
    if ( b_skip || !p_sys->p_current_song.psz_title )
    {
        msg_Dbg (p_this, "Track name missing.");
        DeleteSong (&p_sys->p_current_song);
        b_skip = 1;
    }

    if ( !b_skip )
    {
        RETRIEVE_METADATA(p_sys->p_current_song.psz_album, Album);
        RETRIEVE_METADATA(p_sys->p_current_song.psz_musicbrainz_id, TrackID);
        RETRIEVE_METADATA(p_sys->p_current_song.psz_track_number, TrackNum);
        p_sys->p_current_song.i_length = SEC_FROM_VLC_TICK (input_item_GetDuration (item));
        msg_Dbg (p_this, "Meta data registered");
        vlc_cond_signal (&p_sys->wait);
    }
    vlc_mutex_unlock (&p_sys->lock);

#undef RETRIEVE_METADATA
}

static listen_t CopySong (listen_t song)
{
    listen_t *copy = (listen_t *) malloc (sizeof (*copy));
    memset (copy, 0, sizeof (*copy));
    if ( song.psz_title )
        copy->psz_title = strdup (song.psz_title);
    if ( song.psz_artist )
        copy->psz_artist = strdup (song.psz_artist);
    if ( song.psz_album )
        copy->psz_album = strdup (song.psz_album);
    if ( song.psz_musicbrainz_id )
        copy->psz_musicbrainz_id = strdup (song.psz_musicbrainz_id);
    if ( song.psz_track_number )
        copy->psz_track_number = strdup (song.psz_track_number);
    copy->date = song.date;
    return *copy;
}

static void Enqueue (intf_thread_t *p_this)
{
    bool b_skip = 0;
    intf_sys_t *p_sys = p_this->p_sys;

    vlc_mutex_lock (&p_sys->lock);

    if ( !p_sys->p_current_song.psz_artist || !*p_sys->p_current_song.psz_artist ||
         !p_sys->p_current_song.psz_title || !*p_sys->p_current_song.psz_title )
    {
        msg_Dbg (p_this, "Missing artist or title, not submitting");
        b_skip = 1;
    }

    if ( p_sys->p_current_song.i_length == 0 )
        p_sys->p_current_song.i_length = p_sys->i_played_time;

    if ( !b_skip && p_sys->i_played_time < 30 )
    {
        msg_Dbg (p_this, "Song not listened long enough, not submitting");
        b_skip = 1;
    }

    if ( !b_skip )
    {
        msg_Dbg (p_this, "Song will be submitted.");
        listen_t p_copy_to_queue = CopySong (p_sys->p_current_song);
        bool b_success = vlc_vector_push (&p_sys->p_queue, p_copy_to_queue);
        if ( !b_success )
            msg_Warn (p_this, "Error: Unable to enqueue song");
    }

    vlc_cond_signal (&p_sys->wait);
    DeleteSong (&p_sys->p_current_song);
    p_sys->b_meta_read = false;
    vlc_mutex_unlock (&p_sys->lock);
}

static void PlayerStateChanged (vlc_player_t *player, enum vlc_player_state state, void *data)
{
    intf_thread_t *intf = data;
    intf_sys_t *p_sys = intf->p_sys;

    if ( vlc_player_GetVideoTrackCount (player) )
    {
        msg_Dbg (intf, "Not an audio-only input, not submitting");
        return;
    }

    if ( !p_sys->b_meta_read && state >= VLC_PLAYER_STATE_PLAYING )
    {
        ReadMetaData (intf);
        return;
    }

    if ( state == VLC_PLAYER_STATE_STOPPED )
        Enqueue (intf);
}

static void UpdateState (const struct vlc_player_timer_point *value, void *data)
{
    intf_thread_t *intf = data;
    intf_sys_t *p_sys = intf->p_sys;
    p_sys->i_played_time = SEC_FROM_VLC_TICK (value->ts - VLC_TICK_0);
}

static void PlayingStopped (vlc_tick_t system_date, void *data){}

static void PlaylistItemChanged (vlc_playlist_t *playlist, ssize_t index, void *data)
{
    VLC_UNUSED (index);

    intf_thread_t *intf = data;
    if ( index > 0 )
        Enqueue (intf);

    intf_sys_t *p_sys = intf->p_sys;
    p_sys->b_meta_read = false;

    vlc_player_t *player = vlc_playlist_GetPlayer (playlist);
    input_item_t *item = vlc_player_GetCurrentMedia (player);

    if ( !item || vlc_player_GetVideoTrackCount (player) )
    {
        msg_Dbg (intf, "Invalid item or not an audio-only input.");
        return;
    }

    p_sys->i_played_time = 0;

    if ( input_item_IsPreparsed (item) )
        ReadMetaData (intf);
}

static int PreparePayload (intf_thread_t *p_this)
{
    intf_sys_t *p_sys = p_this->p_sys;
    struct vlc_memstream payload;
    vlc_memstream_open (&payload);

    vlc_mutex_lock (&p_sys->lock);

    if ( p_sys->p_queue.size == 1 )
        vlc_memstream_printf (&payload, "{\"listen_type\":\"single\",\"payload\":[");
    else
        vlc_memstream_printf (&payload, "{\"listen_type\":\"import\",\"payload\":[");

    for ( int i_song = 0; i_song < (int) p_sys->p_queue.size; i_song++ )
    {
        listen_t *p_song = &p_sys->p_queue.data[i_song];

        vlc_memstream_printf (&payload, "{\"listened_at\": %"PRIu64, (uint64_t) p_song->date);
        vlc_memstream_printf (&payload, ", \"track_metadata\": {\"artist_name\": \"%s\", ",
                              vlc_uri_decode (p_song->psz_artist));
        vlc_memstream_printf (&payload, " \"track_name\": \"%s\", ", vlc_uri_decode (p_song->psz_title));
        if ( !EMPTY_STR (p_song->psz_album) )
            vlc_memstream_printf (&payload, " \"release_name\": \"%s\"", vlc_uri_decode (p_song->psz_album));
        if ( !EMPTY_STR (p_song->psz_musicbrainz_id) )
            vlc_memstream_printf (&payload, ", \"additional_info\": {\"recording_mbid\":\"%s\"} ",
                                  vlc_uri_decode (p_song->psz_musicbrainz_id));
        vlc_memstream_printf (&payload, "}}");
    }

    vlc_memstream_printf (&payload, "]}");
    vlc_mutex_unlock (&p_sys->lock);

    int i_status = vlc_memstream_close (&payload);
    if ( !i_status )
        p_sys->payload = payload;
    msg_Dbg (p_this, "Payload: %s", payload.ptr);
    return i_status;
}

static int PrepareRequest (intf_thread_t *p_this)
{
    intf_sys_t *p_sys = p_this->p_sys;
    struct vlc_memstream request;

    vlc_mutex_lock (&p_sys->lock);

    vlc_memstream_open (&request);
    vlc_memstream_printf (&request, "POST %s HTTP/1.1\r\n", p_sys->p_submit_url.psz_path);
    vlc_memstream_printf (&request, "Host: %s\r\n", p_sys->p_submit_url.psz_host);
    vlc_memstream_printf (&request, "Authorization: Token %s\r\n", p_sys->psz_user_token);
    vlc_memstream_puts (&request, "User-Agent: "PACKAGE"/"VERSION"\r\n");
    vlc_memstream_puts (&request, "Connection: close\r\n");
    vlc_memstream_puts (&request, "Accept-Encoding: identity\r\n");
    vlc_memstream_printf (&request, "Content-Length: %zu\r\n", p_sys->payload.length);
    vlc_memstream_puts (&request, "\r\n");
    vlc_memstream_write (&request, p_sys->payload.ptr, p_sys->payload.length);
    vlc_memstream_puts (&request, "\r\n\r\n");

    free (p_sys->payload.ptr);

    vlc_mutex_unlock (&p_sys->lock);

    int i_status = vlc_memstream_close (&request);
    if ( !i_status )
        p_sys->request = request;
    return i_status;
}

static int SendRequest (intf_thread_t *p_this)
{
    uint8_t p_buffer[1024];
    int i_ret;

    intf_sys_t *p_sys = p_this->p_sys;
    vlc_tls_client_t *creds = vlc_tls_ClientCreate (VLC_OBJECT (p_this));
    vlc_tls_t *sock = vlc_tls_SocketOpenTLS (creds, p_sys->p_submit_url.psz_host, 443, NULL, NULL, NULL);

    if ( sock == NULL )
    {
        free (p_sys->request.ptr);
        return 1;
    }

    i_ret = vlc_tls_Write (sock, p_sys->request.ptr, p_sys->request.length);
    free (p_sys->request.ptr);

    if ( i_ret == -1 )
    {
        vlc_tls_Close (sock);
        return 1;
    }

    i_ret = vlc_tls_Read (sock, p_buffer, sizeof (p_buffer) - 1, false);
    msg_Dbg (p_this, "Response: %s", (char *) p_buffer);
    vlc_tls_Close (sock);
    if ( i_ret <= 0 )
    {
        msg_Warn (p_this, "No response");
        return 1;
    }
    p_buffer[i_ret] = '\0';
    if ( strstr ((char *) p_buffer, "OK") )
    {
        vlc_vector_clear (&p_sys->p_queue);
        msg_Dbg (p_this, "Submission successful!");
    }
    else
    {
        msg_Warn (p_this, "Error: %s", (char *) p_buffer);
        return 1;
    }

    return 0;
}

static int Open (vlc_object_t *p_this)
{
    intf_thread_t *p_intf = (intf_thread_t *) p_this;
    intf_sys_t *p_sys = calloc (1, sizeof (intf_sys_t));
    bool b_fail = 0;
    int i_ret;
    char *psz_submission_url, *psz_url;

    if ( !p_sys )
        return VLC_ENOMEM;

    p_intf->p_sys = p_sys;

    p_sys->psz_user_token = var_InheritString (p_intf, "listenbrainz_user_token");
    if ( EMPTY_STR (p_sys->psz_user_token) )
    {
        free (p_sys->psz_user_token);
        vlc_dialog_display_error (p_intf,
                                  _ ("ListenBrainz User Token not set"), "%s",
                                  _ ("Please set a user token or disable the ListenBrainz plugin, and restart VLC.\n"
                                     " Visit https://listenbrainz.org/profile/ to get a user token."));
        free (p_sys);
        return VLC_EGENERIC;
    }

    psz_submission_url = var_InheritString (p_intf, "listenbrainz_submission_url");
    if ( psz_submission_url )
    {
        i_ret = asprintf (&psz_url, "https://%s/1/submit-listens", psz_submission_url);
        free (psz_submission_url);
        if ( i_ret == -1 )
            b_fail = 1;
        vlc_UrlParse (&p_sys->p_submit_url, psz_url);
        free (psz_url);
    }
    else
        b_fail = 1;

    if ( b_fail )
    {
        vlc_dialog_display_error (p_intf,
                                  _ ("ListenBrainz API URL Invalid"), "%s",
                                  _ ("Please set a valid endpoint URL. The default value is api.listenbrainz.org ."));
        free (p_sys);
        return VLC_EGENERIC;
    }

    static struct vlc_playlist_callbacks const playlist_cbs =
            {
                    .on_current_index_changed = PlaylistItemChanged,
            };
    static struct vlc_player_cbs const player_cbs =
            {
                    .on_state_changed = PlayerStateChanged,
            };
    static struct vlc_player_timer_cbs const timer_cbs =
            {
                    .on_update = UpdateState,
                    .on_discontinuity = PlayingStopped,
            };

    vlc_playlist_t *playlist = p_sys->playlist = vlc_intf_GetMainPlaylist (p_intf);
    vlc_player_t *player = vlc_playlist_GetPlayer (playlist);

    vlc_playlist_Lock (playlist);
    p_sys->playlist_listener = vlc_playlist_AddListener (playlist, &playlist_cbs, p_intf, false);
    if ( !p_sys->playlist_listener )
    {
        vlc_playlist_Unlock (playlist);
        b_fail = 1;
    }
    else
    {
        p_sys->player_listener = vlc_player_AddListener (player, &player_cbs, p_intf);
        vlc_playlist_Unlock (playlist);
        if ( !p_sys->player_listener )
            b_fail = 1;
        else
        {
            p_sys->timer_listener = vlc_player_AddTimer (player, VLC_TICK_FROM_SEC (1), &timer_cbs, p_intf);
            if ( !p_sys->timer_listener )
                b_fail = 1;
        }
    }
    if ( !b_fail )
    {
        vlc_mutex_init (&p_sys->lock);
        vlc_cond_init (&p_sys->wait);

        if ( vlc_clone (&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW) )
            b_fail = 1;
    }
    if ( b_fail )
    {
        if ( p_sys->playlist_listener )
        {
            vlc_playlist_Lock (playlist);
            if ( p_sys->player_listener )
                vlc_player_RemoveListener (player, p_sys->player_listener);
            if ( p_sys->timer_listener )
                vlc_player_RemoveTimer (player, p_sys->timer_listener);
            vlc_playlist_RemoveListener (playlist, p_sys->playlist_listener);
            vlc_playlist_Unlock (playlist);
        }
        free (p_sys);
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static void Close (vlc_object_t *p_this)
{
    intf_thread_t *p_intf = (intf_thread_t *) p_this;
    intf_sys_t *p_sys = p_intf->p_sys;
    vlc_playlist_t *playlist = p_sys->playlist;

    vlc_cancel (p_sys->thread);
    vlc_join (p_sys->thread, NULL);

    vlc_vector_clear (&p_sys->p_queue);
    vlc_UrlClean (&p_sys->p_submit_url);

    vlc_playlist_Lock (playlist);
    vlc_player_RemoveListener (vlc_playlist_GetPlayer (playlist), p_sys->player_listener);
    vlc_playlist_RemoveListener (playlist, p_sys->playlist_listener);
    vlc_playlist_Unlock (playlist);

    free (p_sys);
}

static void *Run (void *data)
{
    intf_thread_t *p_intf = data;
    int canc = vlc_savecancel ();
    int i_status;
    bool b_wait = 0;

    intf_sys_t *p_sys = p_intf->p_sys;

    while ( 1 )
    {
        vlc_restorecancel (canc);

        if ( b_wait )
            vlc_tick_wait (vlc_tick_now () + VLC_TICK_FROM_SEC (60)); // wait for 1 min

        vlc_mutex_lock (&p_sys->lock);
        mutex_cleanup_push (&p_sys->lock) ;

                while ( p_sys->p_queue.size == 0 )
                    vlc_cond_wait (&p_sys->wait, &p_sys->lock);

        vlc_cleanup_pop ();
        vlc_mutex_unlock (&p_sys->lock);
        canc = vlc_savecancel ();

        i_status = PreparePayload (p_intf);
        if ( i_status )
        {
            msg_Warn (p_intf, "Error: Unable to generate payload");
            break;
        }

        i_status = PrepareRequest (p_intf);
        if ( i_status )
        {
            msg_Warn (p_intf, "Error: Unable to generate request body");
            break;
        }

        i_status = SendRequest (p_intf);
        if ( i_status )
        {
            msg_Warn (p_intf, "Error: Could not transmit request");
            b_wait = 1;
            continue;
        }

        b_wait = 0;
    }

    vlc_restorecancel (canc);
    return NULL;
}

