/*****************************************************************************
 * renderer.c
 *****************************************************************************
 * Copyright (C) 2004-2020 VLC authors and VideoLAN
 * Copyright (C) 2009, 2011 Laurent Aimar
 *
 * Authors: Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
 *          Ilkka Ollakka <ileoo@videolan.org>
 *          Rémi Denis-Courmont
 *          Adrien Maglo <magsoft at videolan dot org>
 *          Felix Paul Kühne <fkuehne at videolan dot org>
 *          Pierre d'Herbemont <pdherbemont at videolan dot org>
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
# include "config.h"
#endif

#include "renderer.h"

#include <assert.h>
#include <vlc_common.h>
#include <vlc_es.h>
#include <vlc_picture.h>

#include "gl_util.h"
#include "internal.h"
#include "interop.h"
#include "vout_helper.h"

#define SPHERE_RADIUS 1.f

static const GLfloat identity[] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
};

static void getZoomMatrix(float zoom, GLfloat matrix[static 16]) {

    const GLfloat m[] = {
        /* x   y     z     w */
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, zoom, 1.0f
    };

    memcpy(matrix, m, sizeof(m));
}

/* perspective matrix see https://www.opengl.org/sdk/docs/man2/xhtml/gluPerspective.xml */
static void getProjectionMatrix(float sar, float fovy, GLfloat matrix[static 16]) {

    float zFar  = 1000;
    float zNear = 0.01;

    float f = 1.f / tanf(fovy / 2.f);

    const GLfloat m[] = {
        f / sar, 0.f,                   0.f,                0.f,
        0.f,     f,                     0.f,                0.f,
        0.f,     0.f,     (zNear + zFar) / (zNear - zFar), -1.f,
        0.f,     0.f, (2 * zNear * zFar) / (zNear - zFar),  0.f};

     memcpy(matrix, m, sizeof(m));
}

static void getViewpointMatrixes(struct vlc_gl_renderer *renderer,
                                 video_projection_mode_t projection_mode)
{
    if (projection_mode == PROJECTION_MODE_EQUIRECTANGULAR
        || projection_mode == PROJECTION_MODE_CUBEMAP_LAYOUT_STANDARD)
    {
        getProjectionMatrix(renderer->f_sar, renderer->f_fovy,
                            renderer->var.ProjectionMatrix);
        getZoomMatrix(renderer->f_z, renderer->var.ZoomMatrix);

        /* renderer->vp has been reversed and is a world transform */
        vlc_viewpoint_to_4x4(&renderer->vp, renderer->var.ViewMatrix);
    }
    else
    {
        memcpy(renderer->var.ProjectionMatrix, identity, sizeof(identity));
        memcpy(renderer->var.ZoomMatrix, identity, sizeof(identity));
        memcpy(renderer->var.ViewMatrix, identity, sizeof(identity));
    }

}

static void getOrientationTransformMatrix(video_orientation_t orientation,
                                          GLfloat matrix[static 16])
{
    memcpy(matrix, identity, sizeof(identity));

    const int k_cos_pi = -1;
    const int k_cos_pi_2 = 0;
    const int k_cos_n_pi_2 = 0;

    const int k_sin_pi = 0;
    const int k_sin_pi_2 = 1;
    const int k_sin_n_pi_2 = -1;

    switch (orientation) {

        case ORIENT_ROTATED_90:
            matrix[0 * 4 + 0] = k_cos_pi_2;
            matrix[0 * 4 + 1] = -k_sin_pi_2;
            matrix[1 * 4 + 0] = k_sin_pi_2;
            matrix[1 * 4 + 1] = k_cos_pi_2;
            matrix[3 * 4 + 1] = 1;
            break;
        case ORIENT_ROTATED_180:
            matrix[0 * 4 + 0] = k_cos_pi;
            matrix[0 * 4 + 1] = -k_sin_pi;
            matrix[1 * 4 + 0] = k_sin_pi;
            matrix[1 * 4 + 1] = k_cos_pi;
            matrix[3 * 4 + 0] = 1;
            matrix[3 * 4 + 1] = 1;
            break;
        case ORIENT_ROTATED_270:
            matrix[0 * 4 + 0] = k_cos_n_pi_2;
            matrix[0 * 4 + 1] = -k_sin_n_pi_2;
            matrix[1 * 4 + 0] = k_sin_n_pi_2;
            matrix[1 * 4 + 1] = k_cos_n_pi_2;
            matrix[3 * 4 + 0] = 1;
            break;
        case ORIENT_HFLIPPED:
            matrix[0 * 4 + 0] = -1;
            matrix[3 * 4 + 0] = 1;
            break;
        case ORIENT_VFLIPPED:
            matrix[1 * 4 + 1] = -1;
            matrix[3 * 4 + 1] = 1;
            break;
        case ORIENT_TRANSPOSED:
            matrix[0 * 4 + 0] = 0;
            matrix[1 * 4 + 1] = 0;
            matrix[2 * 4 + 2] = -1;
            matrix[0 * 4 + 1] = 1;
            matrix[1 * 4 + 0] = 1;
            break;
        case ORIENT_ANTI_TRANSPOSED:
            matrix[0 * 4 + 0] = 0;
            matrix[1 * 4 + 1] = 0;
            matrix[2 * 4 + 2] = -1;
            matrix[0 * 4 + 1] = -1;
            matrix[1 * 4 + 0] = -1;
            matrix[3 * 4 + 0] = 1;
            matrix[3 * 4 + 1] = 1;
            break;
        default:
            break;
    }
}

static void
InitStereoMatrix(GLfloat matrix_out[static 3*3],
                 video_multiview_mode_t multiview_mode)
{
    /*
     * The stereo matrix transforms 2D pictures coordinates to crop the
     * content, in order to view only one eye.
     *
     * This 2D transformation is affine, so the matrix is 3x3 and applies to 3D
     * vectors in the form (x, y, 1).
     *
     * Note that since for now, we always crop the left eye, in practice the
     * offset is always 0, so the transform is actually linear (a 2x2 matrix
     * would be sufficient).
     */

#define COL(x) (x*3)
#define ROW(x) (x)

    /* Initialize to identity 3x3 */
    memset(matrix_out, 0, 3 * 3 * sizeof(float));
    matrix_out[COL(0) + ROW(0)] = 1;
    matrix_out[COL(1) + ROW(1)] = 1;
    matrix_out[COL(2) + ROW(2)] = 1;

    switch (multiview_mode)
    {
        case MULTIVIEW_STEREO_SBS:
            /*
             * +----------+----------+
             * |          .          |
             * |  LEFT    .   RIGHT  |
             * |  EYE     .     EYE  |
             * |          .          |
             * +----------+----------+
             *
             * To crop the coordinates to the left eye, divide the x
             * coordinates by 2:
             *
             *            / 0.5  0    0 \
             *  matrix =  | 0    1    0 |
             *            \ 0    0    1 /
             */
            matrix_out[COL(0) + ROW(0)] = 0.5;
            break;
        case MULTIVIEW_STEREO_TB:
            /*
             * +----------+
             * |          |
             * |  LEFT    |
             * |  EYE     |
             * |          |
             * +..........+
             * |          |
             * |   RIGHT  |
             * |     EYE  |
             * |          |
             * +----------+
             *
             * To crop the coordinates to the left eye, divide the y
             * coordinates by 2:
             *
             *            / 1    0    0 \
             *  matrix =  | 0    0.5  0 |
             *            \ 0    0    1 /
             */
            matrix_out[COL(1) + ROW(1)] = 0.5;
            break;
        default:
            break;
    }
#undef COL
#undef ROW
}

static char *
BuildVertexShader(const struct vlc_gl_renderer *renderer)
{
    /* Basic vertex shader */
    static const char *template =
        "#version %u\n"
        "attribute vec2 PicCoordsIn;\n"
        "varying vec2 PicCoords;\n"
        "attribute vec3 VertexPosition;\n"
        "uniform mat3 StereoMatrix;\n"
        "uniform mat4 ProjectionMatrix;\n"
        "uniform mat4 ZoomMatrix;\n"
        "uniform mat4 ViewMatrix;\n"
        "void main() {\n"
        " PicCoords = (StereoMatrix * vec3(PicCoordsIn, 1.0)).st;\n"
        " gl_Position = ProjectionMatrix * ZoomMatrix * ViewMatrix\n"
        "               * vec4(VertexPosition, 1.0);\n"
        "}";

    char *code;
    if (asprintf(&code, template, renderer->glsl_version) < 0)
        return NULL;

    if (renderer->b_dump_shaders)
        msg_Dbg(renderer->gl, "\n=== Vertex shader for fourcc: %4.4s ===\n%s\n",
                (const char *) &renderer->interop->fmt.i_chroma, code);
    return code;
}

static char *
BuildFragmentShader(struct vlc_gl_renderer *renderer)
{
    struct vlc_gl_interop *interop = renderer->interop;
    char *vlc_texture =
        opengl_fragment_shader_init(renderer, interop->tex_target,
                                    interop->sw_fmt.i_chroma,
                                    interop->sw_fmt.space);
    if (!vlc_texture)
        return NULL;

    static const char *template =
        "#version %u\n"
        "%s" /* extensions */
        "%s" /* precision header */
        "%s" /* vlc_texture definition */
        "varying vec2 PicCoords;\n"
        "void main() {\n"
        " gl_FragColor = vlc_texture(PicCoords);\n"
        "}\n";

    /* TODO move extensions back to fragment_shaders.c */
    const char *extensions = interop->tex_target == GL_TEXTURE_EXTERNAL_OES
                           ? "#extension GL_OES_EGL_image_external : require\n"
                           : "";

    char *code;
    int ret = asprintf(&code, template, renderer->glsl_version, extensions,
                       renderer->glsl_precision_header, vlc_texture);
    free(vlc_texture);
    if (ret < 0)
        return NULL;

    if (renderer->b_dump_shaders)
        msg_Dbg(renderer->gl, "\n=== Fragment shader for fourcc: %4.4s, colorspace: %d ===\n%s\n",
                              (const char *) &interop->sw_fmt.i_chroma,
                              interop->sw_fmt.space, code);

    return code;
}

static int
opengl_link_program(struct vlc_gl_renderer *renderer)
{
    struct vlc_gl_interop *interop = renderer->interop;
    const opengl_vtable_t *vt = renderer->vt;

    char *vertex_shader = BuildVertexShader(renderer);
    if (!vertex_shader)
        return VLC_EGENERIC;

    char *fragment_shader = BuildFragmentShader(renderer);
    if (!fragment_shader)
    {
        free(vertex_shader);
        return VLC_EGENERIC;
    }

    assert(interop->tex_target != 0 &&
           interop->tex_count > 0 &&
           interop->ops->update_textures != NULL &&
           renderer->pf_fetch_locations != NULL &&
           renderer->pf_prepare_shader != NULL);

    GLuint program_id =
        vlc_gl_BuildProgram(VLC_OBJECT(renderer->gl), vt,
                            1, (const char **) &vertex_shader,
                            1, (const char **) &fragment_shader);
    free(vertex_shader);
    free(fragment_shader);
    if (!program_id)
        return VLC_EGENERIC;

    /* Fetch UniformLocations and AttribLocations */
#define GET_LOC(type, x, str) do { \
    x = vt->Get##type##Location(program_id, str); \
    assert(x != -1); \
    if (x == -1) { \
        msg_Err(renderer->gl, "Unable to Get"#type"Location(%s)", str); \
        goto error; \
    } \
} while (0)
#define GET_ULOC(x, str) GET_LOC(Uniform, renderer->uloc.x, str)
#define GET_ALOC(x, str) GET_LOC(Attrib, renderer->aloc.x, str)
    GET_ULOC(TransformMatrix, "TransformMatrix");
    GET_ULOC(OrientationMatrix, "OrientationMatrix");
    GET_ULOC(StereoMatrix, "StereoMatrix");
    GET_ULOC(ProjectionMatrix, "ProjectionMatrix");
    GET_ULOC(ViewMatrix, "ViewMatrix");
    GET_ULOC(ZoomMatrix, "ZoomMatrix");

    GET_ALOC(PicCoordsIn, "PicCoordsIn");
    GET_ALOC(VertexPosition, "VertexPosition");

    GET_ULOC(TexCoordsMap[0], "TexCoordsMap0");
    /* MultiTexCoord 1 and 2 can be optimized out if not used */
    if (interop->tex_count > 1)
        GET_ULOC(TexCoordsMap[1], "TexCoordsMap1");
    else
        renderer->uloc.TexCoordsMap[1] = -1;
    if (interop->tex_count > 2)
        GET_ULOC(TexCoordsMap[2], "TexCoordsMap2");
    else
        renderer->uloc.TexCoordsMap[2] = -1;
#undef GET_LOC
#undef GET_ULOC
#undef GET_ALOC
    int ret = renderer->pf_fetch_locations(renderer, program_id);
    assert(ret == VLC_SUCCESS);
    if (ret != VLC_SUCCESS)
    {
        msg_Err(renderer->gl, "Unable to get locations from tex_conv");
        goto error;
    }

    renderer->program_id = program_id;

    return VLC_SUCCESS;

error:
    vt->DeleteProgram(program_id);
    renderer->program_id = 0;
    return VLC_EGENERIC;
}

void
vlc_gl_renderer_Delete(struct vlc_gl_renderer *renderer)
{
    struct vlc_gl_interop *interop = renderer->interop;
    const opengl_vtable_t *vt = renderer->vt;

    vt->DeleteBuffers(1, &renderer->vertex_buffer_object);
    vt->DeleteBuffers(1, &renderer->index_buffer_object);
    vt->DeleteBuffers(1, &renderer->texture_buffer_object);

    if (!interop->handle_texs_gen)
        vt->DeleteTextures(interop->tex_count, renderer->textures);

    vlc_gl_interop_Delete(interop);
    if (renderer->program_id != 0)
        vt->DeleteProgram(renderer->program_id);

#ifdef HAVE_LIBPLACEBO
    FREENULL(renderer->uloc.pl_vars);
    if (renderer->pl_ctx)
        pl_context_destroy(&renderer->pl_ctx);
#endif

    free(renderer);
}

static int SetupCoords(struct vlc_gl_renderer *renderer);

struct vlc_gl_renderer *
vlc_gl_renderer_New(vlc_gl_t *gl, const struct vlc_gl_api *api,
                    vlc_video_context *context, const video_format_t *fmt,
                    bool b_dump_shaders)
{
    const opengl_vtable_t *vt = &api->vt;

    struct vlc_gl_renderer *renderer = calloc(1, sizeof(*renderer));
    if (!renderer)
        return NULL;

    struct vlc_gl_interop *interop =
        vlc_gl_interop_New(gl, api, context, fmt, false);
    if (!interop)
    {
        free(renderer);
        return NULL;
    }

    renderer->interop = interop;

    renderer->gl = gl;
    renderer->api = api;
    renderer->vt = vt;
    renderer->b_dump_shaders = b_dump_shaders;
#if defined(USE_OPENGL_ES2)
    renderer->glsl_version = 100;
    renderer->glsl_precision_header = "precision highp float;\n";
#else
    renderer->glsl_version = 120;
    renderer->glsl_precision_header = "";
#endif

#ifdef HAVE_LIBPLACEBO
    // Create the main libplacebo context
    renderer->pl_ctx = vlc_placebo_Create(VLC_OBJECT(gl));
    if (renderer->pl_ctx) {
#   if PL_API_VER >= 20
        renderer->pl_sh = pl_shader_alloc(renderer->pl_ctx, NULL);
#   elif PL_API_VER >= 6
        renderer->pl_sh = pl_shader_alloc(renderer->pl_ctx, NULL, 0);
#   else
        renderer->pl_sh = pl_shader_alloc(renderer->pl_ctx, NULL, 0, 0);
#   endif
    }
#endif

    int ret = opengl_link_program(renderer);
    if (ret != VLC_SUCCESS)
    {
        vlc_gl_renderer_Delete(renderer);
        return NULL;
    }

    InitStereoMatrix(renderer->var.StereoMatrix, interop->fmt.multiview_mode);

    getOrientationTransformMatrix(interop->fmt.orientation,
                                  renderer->var.OrientationMatrix);
    getViewpointMatrixes(renderer, interop->fmt.projection_mode);

    /* Update the fmt to main program one */
    renderer->fmt = interop->fmt;
    /* The orientation is handled by the orientation matrix */
    renderer->fmt.orientation = fmt->orientation;

    /* Texture size */
    for (unsigned j = 0; j < interop->tex_count; j++) {
        const GLsizei w = renderer->fmt.i_visible_width  * interop->texs[j].w.num
                        / interop->texs[j].w.den;
        const GLsizei h = renderer->fmt.i_visible_height * interop->texs[j].h.num
                        / interop->texs[j].h.den;
        if (api->supports_npot) {
            renderer->tex_width[j]  = w;
            renderer->tex_height[j] = h;
        } else {
            renderer->tex_width[j]  = vlc_align_pot(w);
            renderer->tex_height[j] = vlc_align_pot(h);
        }
    }

    if (!interop->handle_texs_gen)
    {
        ret = vlc_gl_interop_GenerateTextures(interop, renderer->tex_width,
                                              renderer->tex_height,
                                              renderer->textures);
        if (ret != VLC_SUCCESS)
        {
            vlc_gl_renderer_Delete(renderer);
            return NULL;
        }
    }

    /* */
    vt->Disable(GL_BLEND);
    vt->Disable(GL_DEPTH_TEST);
    vt->DepthMask(GL_FALSE);
    vt->Enable(GL_CULL_FACE);
    vt->ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    vt->Clear(GL_COLOR_BUFFER_BIT);

    vt->GenBuffers(1, &renderer->vertex_buffer_object);
    vt->GenBuffers(1, &renderer->index_buffer_object);
    vt->GenBuffers(1, &renderer->texture_buffer_object);

    ret = SetupCoords(renderer);
    if (ret != VLC_SUCCESS)
    {
        vlc_gl_renderer_Delete(renderer);
        return NULL;
    }

    return renderer;
}

static void UpdateZ(struct vlc_gl_renderer *renderer)
{
    /* Do trigonometry to calculate the minimal z value
     * that will allow us to zoom out without seeing the outside of the
     * sphere (black borders). */
    float tan_fovx_2 = tanf(renderer->f_fovx / 2);
    float tan_fovy_2 = tanf(renderer->f_fovy / 2);
    float z_min = - SPHERE_RADIUS / sinf(atanf(sqrtf(
                    tan_fovx_2 * tan_fovx_2 + tan_fovy_2 * tan_fovy_2)));

    /* The FOV value above which z is dynamically calculated. */
    const float z_thresh = 90.f;

    if (renderer->f_fovx <= z_thresh * M_PI / 180)
        renderer->f_z = 0;
    else
    {
        float f = z_min / ((FIELD_OF_VIEW_DEGREES_MAX - z_thresh) * M_PI / 180);
        renderer->f_z = f * renderer->f_fovx - f * z_thresh * M_PI / 180;
        if (renderer->f_z < z_min)
            renderer->f_z = z_min;
    }
}

static void UpdateFOVy(struct vlc_gl_renderer *renderer)
{
    renderer->f_fovy = 2 * atanf(tanf(renderer->f_fovx / 2) / renderer->f_sar);
}

int
vlc_gl_renderer_SetViewpoint(struct vlc_gl_renderer *renderer,
                             const vlc_viewpoint_t *p_vp)
{
    if (p_vp->fov > FIELD_OF_VIEW_DEGREES_MAX
            || p_vp->fov < FIELD_OF_VIEW_DEGREES_MIN)
        return VLC_EBADVAR;

    // Convert degree into radian
    float f_fovx = p_vp->fov * (float)M_PI / 180.f;

    /* vgl->vp needs to be converted into world transform */
    vlc_viewpoint_reverse(&renderer->vp, p_vp);

    if (fabsf(f_fovx - renderer->f_fovx) >= 0.001f)
    {
        /* FOVx has changed. */
        renderer->f_fovx = f_fovx;
        UpdateFOVy(renderer);
        UpdateZ(renderer);
    }
    getViewpointMatrixes(renderer, renderer->fmt.projection_mode);

    return VLC_SUCCESS;
}

void
vlc_gl_renderer_SetWindowAspectRatio(struct vlc_gl_renderer *renderer,
                                     float f_sar)
{
    /* Each time the window size changes, we must recompute the minimum zoom
     * since the aspect ration changes.
     * We must also set the new current zoom value. */
    renderer->f_sar = f_sar;
    UpdateFOVy(renderer);
    UpdateZ(renderer);
    getViewpointMatrixes(renderer, renderer->fmt.projection_mode);
}

static int BuildSphere(GLfloat **vertexCoord, GLfloat **textureCoord, unsigned *nbVertices,
                       GLushort **indices, unsigned *nbIndices)
{
    unsigned nbLatBands = 128;
    unsigned nbLonBands = 128;

    *nbVertices = (nbLatBands + 1) * (nbLonBands + 1);
    *nbIndices = nbLatBands * nbLonBands * 3 * 2;

    *vertexCoord = vlc_alloc(*nbVertices * 3, sizeof(GLfloat));
    if (*vertexCoord == NULL)
        return VLC_ENOMEM;
    *textureCoord = vlc_alloc(*nbVertices * 2, sizeof(GLfloat));
    if (*textureCoord == NULL)
    {
        free(*vertexCoord);
        return VLC_ENOMEM;
    }
    *indices = vlc_alloc(*nbIndices, sizeof(GLushort));
    if (*indices == NULL)
    {
        free(*textureCoord);
        free(*vertexCoord);
        return VLC_ENOMEM;
    }

    for (unsigned lat = 0; lat <= nbLatBands; lat++) {
        float theta = lat * (float) M_PI / nbLatBands;
        float sinTheta, cosTheta;

        sincosf(theta, &sinTheta, &cosTheta);

        for (unsigned lon = 0; lon <= nbLonBands; lon++) {
            float phi = lon * 2 * (float) M_PI / nbLonBands;
            float sinPhi, cosPhi;

            sincosf(phi, &sinPhi, &cosPhi);

            float x = cosPhi * sinTheta;
            float y = cosTheta;
            float z = sinPhi * sinTheta;

            unsigned off1 = (lat * (nbLonBands + 1) + lon) * 3;
            (*vertexCoord)[off1] = SPHERE_RADIUS * x;
            (*vertexCoord)[off1 + 1] = SPHERE_RADIUS * y;
            (*vertexCoord)[off1 + 2] = SPHERE_RADIUS * z;

            unsigned off2 = (lat * (nbLonBands + 1) + lon) * 2;
            float u = (float)lon / nbLonBands;
            float v = (float)lat / nbLatBands;
            (*textureCoord)[off2] = u;
            (*textureCoord)[off2 + 1] = v;
        }
    }

    for (unsigned lat = 0; lat < nbLatBands; lat++) {
        for (unsigned lon = 0; lon < nbLonBands; lon++) {
            unsigned first = (lat * (nbLonBands + 1)) + lon;
            unsigned second = first + nbLonBands + 1;

            unsigned off = (lat * nbLatBands + lon) * 3 * 2;

            (*indices)[off] = first;
            (*indices)[off + 1] = second;
            (*indices)[off + 2] = first + 1;

            (*indices)[off + 3] = second;
            (*indices)[off + 4] = second + 1;
            (*indices)[off + 5] = first + 1;
        }
    }

    return VLC_SUCCESS;
}


static int BuildCube(float padW, float padH,
                     GLfloat **vertexCoord, GLfloat **textureCoord, unsigned *nbVertices,
                     GLushort **indices, unsigned *nbIndices)
{
    *nbVertices = 4 * 6;
    *nbIndices = 6 * 6;

    *vertexCoord = vlc_alloc(*nbVertices * 3, sizeof(GLfloat));
    if (*vertexCoord == NULL)
        return VLC_ENOMEM;
    *textureCoord = vlc_alloc(*nbVertices * 2, sizeof(GLfloat));
    if (*textureCoord == NULL)
    {
        free(*vertexCoord);
        return VLC_ENOMEM;
    }
    *indices = vlc_alloc(*nbIndices, sizeof(GLushort));
    if (*indices == NULL)
    {
        free(*textureCoord);
        free(*vertexCoord);
        return VLC_ENOMEM;
    }

    static const GLfloat coord[] = {
        -1.0,    1.0,    -1.0f, // front
        -1.0,    -1.0,   -1.0f,
        1.0,     1.0,    -1.0f,
        1.0,     -1.0,   -1.0f,

        -1.0,    1.0,    1.0f, // back
        -1.0,    -1.0,   1.0f,
        1.0,     1.0,    1.0f,
        1.0,     -1.0,   1.0f,

        -1.0,    1.0,    -1.0f, // left
        -1.0,    -1.0,   -1.0f,
        -1.0,     1.0,    1.0f,
        -1.0,     -1.0,   1.0f,

        1.0f,    1.0,    -1.0f, // right
        1.0f,   -1.0,    -1.0f,
        1.0f,   1.0,     1.0f,
        1.0f,   -1.0,    1.0f,

        -1.0,    -1.0,    1.0f, // bottom
        -1.0,    -1.0,   -1.0f,
        1.0,     -1.0,    1.0f,
        1.0,     -1.0,   -1.0f,

        -1.0,    1.0,    1.0f, // top
        -1.0,    1.0,   -1.0f,
        1.0,     1.0,    1.0f,
        1.0,     1.0,   -1.0f,
    };

    memcpy(*vertexCoord, coord, *nbVertices * 3 * sizeof(GLfloat));

    float col[] = {0.f, 1.f/3, 2.f/3, 1.f};
    float row[] = {0.f, 1.f/2, 1.0};

    const GLfloat tex[] = {
        col[1] + padW, row[1] + padH, // front
        col[1] + padW, row[2] - padH,
        col[2] - padW, row[1] + padH,
        col[2] - padW, row[2] - padH,

        col[3] - padW, row[1] + padH, // back
        col[3] - padW, row[2] - padH,
        col[2] + padW, row[1] + padH,
        col[2] + padW, row[2] - padH,

        col[2] - padW, row[0] + padH, // left
        col[2] - padW, row[1] - padH,
        col[1] + padW, row[0] + padH,
        col[1] + padW, row[1] - padH,

        col[0] + padW, row[0] + padH, // right
        col[0] + padW, row[1] - padH,
        col[1] - padW, row[0] + padH,
        col[1] - padW, row[1] - padH,

        col[0] + padW, row[2] - padH, // bottom
        col[0] + padW, row[1] + padH,
        col[1] - padW, row[2] - padH,
        col[1] - padW, row[1] + padH,

        col[2] + padW, row[0] + padH, // top
        col[2] + padW, row[1] - padH,
        col[3] - padW, row[0] + padH,
        col[3] - padW, row[1] - padH,
    };

    memcpy(*textureCoord, tex,
           *nbVertices * 2 * sizeof(GLfloat));

    const GLushort ind[] = {
        0, 1, 2,       2, 1, 3, // front
        6, 7, 4,       4, 7, 5, // back
        10, 11, 8,     8, 11, 9, // left
        12, 13, 14,    14, 13, 15, // right
        18, 19, 16,    16, 19, 17, // bottom
        20, 21, 22,    22, 21, 23, // top
    };

    memcpy(*indices, ind, *nbIndices * sizeof(GLushort));

    return VLC_SUCCESS;
}

static int BuildRectangle(GLfloat **vertexCoord, GLfloat **textureCoord, unsigned *nbVertices,
                          GLushort **indices, unsigned *nbIndices)
{
    *nbVertices = 4;
    *nbIndices = 6;

    *vertexCoord = vlc_alloc(*nbVertices * 3, sizeof(GLfloat));
    if (*vertexCoord == NULL)
        return VLC_ENOMEM;
    *textureCoord = vlc_alloc(*nbVertices * 2, sizeof(GLfloat));
    if (*textureCoord == NULL)
    {
        free(*vertexCoord);
        return VLC_ENOMEM;
    }
    *indices = vlc_alloc(*nbIndices, sizeof(GLushort));
    if (*indices == NULL)
    {
        free(*textureCoord);
        free(*vertexCoord);
        return VLC_ENOMEM;
    }

    static const GLfloat coord[] = {
       -1.0,    1.0,    -1.0f,
       -1.0,    -1.0,   -1.0f,
       1.0,     1.0,    -1.0f,
       1.0,     -1.0,   -1.0f
    };

    memcpy(*vertexCoord, coord, *nbVertices * 3 * sizeof(GLfloat));

    static const GLfloat tex[] = {
        0.0, 0.0,
        0.0, 1.0,
        1.0, 0.0,
        1.0, 1.0,
    };

    memcpy(*textureCoord, tex, *nbVertices * 2 * sizeof(GLfloat));

    const GLushort ind[] = {
        0, 1, 2,
        2, 1, 3
    };

    memcpy(*indices, ind, *nbIndices * sizeof(GLushort));

    return VLC_SUCCESS;
}

static int SetupCoords(struct vlc_gl_renderer *renderer)
{
    const opengl_vtable_t *vt = renderer->vt;

    GLfloat *vertexCoord, *textureCoord;
    GLushort *indices;
    unsigned nbVertices, nbIndices;

    int i_ret;
    switch (renderer->fmt.projection_mode)
    {
    case PROJECTION_MODE_RECTANGULAR:
        i_ret = BuildRectangle(&vertexCoord, &textureCoord, &nbVertices,
                               &indices, &nbIndices);
        break;
    case PROJECTION_MODE_EQUIRECTANGULAR:
        i_ret = BuildSphere(&vertexCoord, &textureCoord, &nbVertices,
                            &indices, &nbIndices);
        break;
    case PROJECTION_MODE_CUBEMAP_LAYOUT_STANDARD:
        i_ret = BuildCube((float)renderer->fmt.i_cubemap_padding / renderer->fmt.i_width,
                          (float)renderer->fmt.i_cubemap_padding / renderer->fmt.i_height,
                          &vertexCoord, &textureCoord, &nbVertices,
                          &indices, &nbIndices);
        break;
    default:
        i_ret = VLC_EGENERIC;
        break;
    }

    if (i_ret != VLC_SUCCESS)
        return i_ret;

    vt->BindBuffer(GL_ARRAY_BUFFER, renderer->texture_buffer_object);
    vt->BufferData(GL_ARRAY_BUFFER, nbVertices * 2 * sizeof(GLfloat),
                   textureCoord, GL_STATIC_DRAW);

    vt->BindBuffer(GL_ARRAY_BUFFER, renderer->vertex_buffer_object);
    vt->BufferData(GL_ARRAY_BUFFER, nbVertices * 3 * sizeof(GLfloat),
                   vertexCoord, GL_STATIC_DRAW);

    vt->BindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer->index_buffer_object);
    vt->BufferData(GL_ELEMENT_ARRAY_BUFFER, nbIndices * sizeof(GLushort),
                   indices, GL_STATIC_DRAW);

    free(textureCoord);
    free(vertexCoord);
    free(indices);

    renderer->nb_indices = nbIndices;

    return VLC_SUCCESS;
}

static void DrawWithShaders(struct vlc_gl_renderer *renderer)
{
    const struct vlc_gl_interop *interop = renderer->interop;
    const opengl_vtable_t *vt = renderer->vt;
    renderer->pf_prepare_shader(renderer, renderer->tex_width,
                                renderer->tex_height, 1.0f);

    for (unsigned j = 0; j < interop->tex_count; j++) {
        assert(renderer->textures[j] != 0);
        vt->ActiveTexture(GL_TEXTURE0+j);
        vt->BindTexture(interop->tex_target, renderer->textures[j]);

        vt->UniformMatrix3fv(renderer->uloc.TexCoordsMap[j], 1, GL_FALSE,
                             renderer->var.TexCoordsMap[j]);
    }

    vt->BindBuffer(GL_ARRAY_BUFFER, renderer->texture_buffer_object);
    assert(renderer->aloc.PicCoordsIn != -1);
    vt->EnableVertexAttribArray(renderer->aloc.PicCoordsIn);
    vt->VertexAttribPointer(renderer->aloc.PicCoordsIn, 2, GL_FLOAT, 0, 0, 0);

    vt->BindBuffer(GL_ARRAY_BUFFER, renderer->vertex_buffer_object);
    vt->BindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer->index_buffer_object);
    vt->EnableVertexAttribArray(renderer->aloc.VertexPosition);
    vt->VertexAttribPointer(renderer->aloc.VertexPosition, 3, GL_FLOAT, 0, 0, 0);

    const GLfloat *tm = NULL;
    if (interop->ops && interop->ops->get_transform_matrix)
        tm = interop->ops->get_transform_matrix(interop);
    if (!tm)
        tm = identity;

    vt->UniformMatrix4fv(renderer->uloc.TransformMatrix, 1, GL_FALSE, tm);

    vt->UniformMatrix4fv(renderer->uloc.OrientationMatrix, 1, GL_FALSE,
                         renderer->var.OrientationMatrix);
    vt->UniformMatrix3fv(renderer->uloc.StereoMatrix, 1, GL_FALSE,
                         renderer->var.StereoMatrix);
    vt->UniformMatrix4fv(renderer->uloc.ProjectionMatrix, 1, GL_FALSE,
                         renderer->var.ProjectionMatrix);
    vt->UniformMatrix4fv(renderer->uloc.ViewMatrix, 1, GL_FALSE,
                         renderer->var.ViewMatrix);
    vt->UniformMatrix4fv(renderer->uloc.ZoomMatrix, 1, GL_FALSE,
                         renderer->var.ZoomMatrix);

    vt->DrawElements(GL_TRIANGLES, renderer->nb_indices, GL_UNSIGNED_SHORT, 0);
}

int
vlc_gl_renderer_Prepare(struct vlc_gl_renderer *renderer, picture_t *picture)
{
    const struct vlc_gl_interop *interop = renderer->interop;
    const video_format_t *source = &picture->format;

    if (source->i_x_offset != renderer->last_source.i_x_offset
     || source->i_y_offset != renderer->last_source.i_y_offset
     || source->i_visible_width != renderer->last_source.i_visible_width
     || source->i_visible_height != renderer->last_source.i_visible_height)
    {
        memset(renderer->var.TexCoordsMap, 0,
               sizeof(renderer->var.TexCoordsMap));
        for (unsigned j = 0; j < interop->tex_count; j++)
        {
            float scale_w = (float)interop->texs[j].w.num / interop->texs[j].w.den
                          / renderer->tex_width[j];
            float scale_h = (float)interop->texs[j].h.num / interop->texs[j].h.den
                          / renderer->tex_height[j];

            /* Warning: if NPOT is not supported a larger texture is
               allocated. This will cause right and bottom coordinates to
               land on the edge of two texels with the texels to the
               right/bottom uninitialized by the call to
               glTexSubImage2D. This might cause a green line to appear on
               the right/bottom of the display.
               There are two possible solutions:
               - Manually mirror the edges of the texture.
               - Add a "-1" when computing right and bottom, however the
               last row/column might not be displayed at all.
            */
            float left   = (source->i_x_offset +                       0 ) * scale_w;
            float top    = (source->i_y_offset +                       0 ) * scale_h;
            float right  = (source->i_x_offset + source->i_visible_width ) * scale_w;
            float bottom = (source->i_y_offset + source->i_visible_height) * scale_h;

            /**
             * This matrix converts from picture coordinates (in range [0; 1])
             * to textures coordinates where the picture is actually stored
             * (removing paddings).
             *
             *        texture           (in texture coordinates)
             *       +----------------+--- 0.0
             *       |                |
             *       |  +---------+---|--- top
             *       |  | picture |   |
             *       |  +---------+---|--- bottom
             *       |  .         .   |
             *       |  .         .   |
             *       +----------------+--- 1.0
             *       |  .         .   |
             *      0.0 left  right  1.0  (in texture coordinates)
             *
             * In particular:
             *  - (0.0, 0.0) is mapped to (left, top)
             *  - (1.0, 1.0) is mapped to (right, bottom)
             *
             * This is an affine 2D transformation, so the input coordinates
             * are given as a 3D vector in the form (x, y, 1), and the output
             * is (x', y', 1).
             *
             * The paddings are l (left), r (right), t (top) and b (bottom).
             *
             *               / (r-l)   0     l \
             *      matrix = |   0   (b-t)   t |
             *               \   0     0     1 /
             *
             * It is stored in column-major order.
             */
            GLfloat *matrix = renderer->var.TexCoordsMap[j];
#define COL(x) (x*3)
#define ROW(x) (x)
            matrix[COL(0) + ROW(0)] = right - left;
            matrix[COL(1) + ROW(1)] = bottom - top;
            matrix[COL(2) + ROW(0)] = left;
            matrix[COL(2) + ROW(1)] = top;
#undef COL
#undef ROW
        }

        renderer->last_source.i_x_offset = source->i_x_offset;
        renderer->last_source.i_y_offset = source->i_y_offset;
        renderer->last_source.i_visible_width = source->i_visible_width;
        renderer->last_source.i_visible_height = source->i_visible_height;
    }

    /* Update the texture */
    return interop->ops->update_textures(interop, renderer->textures,
                                         renderer->tex_width,
                                         renderer->tex_height, picture,
                                         NULL);
}

int
vlc_gl_renderer_Draw(struct vlc_gl_renderer *renderer)
{
    const opengl_vtable_t *vt = renderer->vt;

    vt->Clear(GL_COLOR_BUFFER_BIT);

    vt->UseProgram(renderer->program_id);

    DrawWithShaders(renderer);

    return VLC_SUCCESS;
}
