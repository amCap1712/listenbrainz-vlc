#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_codec.h>
#include <rav1e/rav1e.h>

#define SOUT_CFG_PREFIX "sout-rav1e-"

static const int bitdepth_values_list[] = {8, 10};
static const char *bitdepth_values_name_list[] = {N_("8 bpp"), N_("10 bpp")};

static int OpenEncoder(vlc_object_t *);
static void CloseEncoder(vlc_object_t* p_this);
static block_t *Encode(encoder_t *p_enc, picture_t *p_pict);

vlc_module_begin()
    set_shortname("rav1e")
    set_description(N_("rav1e video encoder"))
    set_capability("encoder", 101)
    set_callbacks(OpenEncoder, CloseEncoder)
    add_integer( SOUT_CFG_PREFIX "profile", 0, "Profile", NULL, true)
        change_integer_range( 0, 3 )
    add_integer( SOUT_CFG_PREFIX "bitdepth", 8, "Bit Depth", NULL, true )
        change_integer_list( bitdepth_values_list, bitdepth_values_name_list )
    add_integer( SOUT_CFG_PREFIX "tile-rows", 0, "Tile Rows (in log2 units)", NULL, true )
        change_integer_range( 0, 6 )
    add_integer( SOUT_CFG_PREFIX "tile-columns", 0, "Tile Columns (in log2 units)", NULL, true )
        change_integer_range( 0, 6 ) 
vlc_module_end()

typedef struct {
    struct RaConfig *ra_config;
    struct RaContext *ra_context;
} encoder_sys_t;

static int OpenEncoder(vlc_object_t *p_this)
{
    encoder_t *p_enc = (encoder_t *) p_this;
    encoder_sys_t *p_sys;
    
    if(p_enc->fmt_out.i_codec != VLC_CODEC_AV1)
        return VLC_EGENERIC;
    
    p_sys = malloc(sizeof(*p_sys));
    if(p_sys == NULL)
        return VLC_ENOMEM;
    
    p_enc->p_sys = p_sys;
    
    p_sys->ra_config = rav1e_config_default();
    if(!p_sys->ra_config) {
        printf("Unable to initialize configuration\n");
        free(p_sys);
        return VLC_EGENERIC;
    }
    
    int ret;
    
    ret = rav1e_config_parse_int(p_sys->ra_config, "height", p_enc->fmt_in.video.i_visible_height);
    if(ret < 0) {
        printf("Unable to set height\n");
        goto clean;
    }
    
    ret = rav1e_config_parse_int(p_sys->ra_config, "width", p_enc->fmt_in.video.i_visible_width);
    if(ret < 0) {
        printf("Unable to set width\n");
        goto clean;
    }

    RaRational *timebase = malloc(sizeof(RaRational));
    if (timebase == NULL) {
        printf("%s", "Unable to set width\n");
        goto clean;
    }

    timebase->num = p_enc->fmt_in.video.i_frame_rate_base;
    timebase->den = p_enc->fmt_in.video.i_frame_rate;
    rav1e_config_set_time_base(p_sys->ra_config, *timebase);
    
    int i_tile_rows = var_InheritInteger( p_enc, SOUT_CFG_PREFIX "tile-rows" );
    int i_tile_columns = var_InheritInteger( p_enc, SOUT_CFG_PREFIX "tile-columns" );
    i_tile_rows = 2 << i_tile_rows;
    i_tile_columns = 2 << i_tile_columns;
    
    ret = rav1e_config_parse_int(p_sys->ra_config, "tile_rows", i_tile_rows);
    if(ret < 0) {
        printf("Unable to set tile rows\n");
        goto clean;
    }
    
    ret = rav1e_config_parse_int(p_sys->ra_config, "tile_cols", i_tile_columns);
    if(ret < 0) {
        printf("Unable to set tile columns\n");
        goto clean;
    }
    
    int i_bitdepth = var_InheritInteger(p_enc, SOUT_CFG_PREFIX "bitdepth");
    p_enc->fmt_in.i_codec = i_bitdepth == 8 ? VLC_CODEC_I420 : VLC_CODEC_I420_10L;
        
    p_sys->ra_context = rav1e_context_new(p_sys->ra_config);
    if(!p_sys->ra_context) {
        printf("Unable to allocate a new context\n");
        goto clean;
    }
    
    p_enc->pf_encode_video = Encode;
    return VLC_SUCCESS;
    
clean:
    rav1e_config_unref(p_sys->ra_config);
    free(p_sys);
    return VLC_EGENERIC;
}

static block_t *Encode(encoder_t *p_enc, picture_t *p_pict)
{
    encoder_sys_t *p_sys = p_enc->p_sys;
    RaContext *ctx = p_sys->ra_context;
    block_t *p_out = NULL;
    
    if (!p_pict) return NULL;
    
    printf("\n");
    RaFrame *frame = rav1e_frame_new(ctx);
    if(frame == NULL) {
        printf("Unable to create new frame\n");
        goto clean;
    }
        
    int idx, ret;
    for(idx = 0; idx < p_pict->i_planes; idx++)
        rav1e_frame_fill_plane(frame, idx, 
                               p_pict->p[idx].p_pixels, 
                               p_pict->p[idx].i_pitch * p_pict->p[idx].i_visible_lines,
                               p_pict->p[idx].i_pitch,
                               p_pict->p[idx].i_pixel_pitch);
    
    ret = rav1e_send_frame(ctx, frame);
    printf("Sending Frame: %s\n",rav1e_status_to_str(ret));
    if (ret != 0)
        goto clean;
    rav1e_frame_unref(frame);
    
    while(true) {
        RaPacket *pkt = NULL;
        ret = rav1e_receive_packet(ctx, &pkt);
        printf("Receiving packet: %s\n",rav1e_status_to_str(ret));
    
        if (ret == RA_ENCODER_STATUS_SUCCESS) {
            printf("Packet %"PRIu64"\n", pkt->input_frameno);
            
            block_t *p_block = block_Alloc(pkt->len);
            if(unlikely(p_block == NULL)) {
                block_ChainRelease(p_out);
                p_out = NULL;
                break;
            }
            
            memcpy(p_block->p_buffer, pkt->data, pkt->len);
            p_block->i_dts = p_block->i_pts = VLC_TICK_FROM_MS(pkt->input_frameno * 1000 / 25); // HACK
            printf("%ld\n", p_block->i_pts);
            if(pkt->frame_type == RA_FRAME_TYPE_KEY)
                p_block->i_flags |= BLOCK_FLAG_TYPE_I;
            block_ChainAppend(&p_out, p_block);
            rav1e_packet_unref(pkt);
        } else if(ret == RA_ENCODER_STATUS_LIMIT_REACHED || ret == RA_ENCODER_STATUS_ENCODED ||
            ret == RA_ENCODER_STATUS_NEED_MORE_DATA)
            break;
        else
            goto clean;
    }
     
    return p_out;
    
clean:
    free(p_out);
    return NULL;
}

static void CloseEncoder(vlc_object_t* p_this)
{
    encoder_t *p_enc = (encoder_t *) p_this;
    encoder_sys_t *p_sys = p_enc->p_sys;
    rav1e_context_unref(p_sys->ra_context);
    rav1e_config_unref(p_sys->ra_config);
    free(p_sys);
}


