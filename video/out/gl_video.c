/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 *
 * You can alternatively redistribute this file and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include <libavutil/common.h>

#include "gl_video.h"

#include "misc/bstr.h"
#include "gl_common.h"
#include "gl_utils.h"
#include "gl_hwdec.h"
#include "gl_osd.h"
#include "filter_kernels.h"
#include "aspect.h"
#include "video/memcpy_pic.h"
#include "bitmap_packer.h"
#include "dither.h"

static const char vo_opengl_shaders[] =
// Generated from gl_video_shaders.glsl
#include "video/out/gl_video_shaders.h"
;

// Pixel width of 1D lookup textures.
#define LOOKUP_TEXTURE_SIZE 256

// Texture units 0-3 are used by the video, with unit 0 for free use.
// Units 4-5 are used for scaler LUTs.
#define TEXUNIT_SCALERS 4
#define TEXUNIT_3DLUT 6
#define TEXUNIT_DITHER 7

// scale/cscale arguments that map directly to shader filter routines.
// Note that the convolution filters are not included in this list.
static const char *const fixed_scale_filters[] = {
    "bilinear",
    "bicubic_fast",
    "sharpen3",
    "sharpen5",
    NULL
};

// must be sorted, and terminated with 0
// 2 & 6 are special-cased, the rest can be generated with WEIGHTS_N().
int filter_sizes[] =
    {2, 4, 6, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 0};

struct vertex {
    float position[2];
    float texcoord[2];
};

static const struct gl_vao_entry vertex_vao[] = {
    {"vertex_position", 2, GL_FLOAT,         false, offsetof(struct vertex, position)},
    {"vertex_texcoord", 2, GL_FLOAT,         false, offsetof(struct vertex, texcoord)},
    {0}
};

struct texplane {
    int w, h;
    int tex_w, tex_h;
    GLint gl_internal_format;
    GLenum gl_format;
    GLenum gl_type;
    GLuint gl_texture;
    int gl_buffer;
    int buffer_size;
    void *buffer_ptr;
};

struct video_image {
    struct texplane planes[4];
    bool image_flipped;
    struct mp_image *mpi;       // original input image
};

struct scaler {
    int index;
    const char *name;
    float params[2];
    float antiring;
    struct filter_kernel *kernel;
    GLuint gl_lut;
    const char *lut_name;
    bool insufficient;

    // kernel points here
    struct filter_kernel kernel_storage;
};

struct fbosurface {
    struct fbotex fbotex;
    int64_t pts;
    bool valid;
};

#define FBOSURFACES_MAX 2

struct gl_video {
    GL *gl;

    struct mp_log *log;
    struct gl_video_opts opts;
    bool gl_debug;

    int depth_g;
    int texture_16bit_depth;    // actual bits available in 16 bit textures

    GLenum gl_target; // texture target (GL_TEXTURE_2D, ...) for video and FBOs

    struct gl_vao vao;

    GLuint osd_programs[SUBBITMAP_COUNT];
    GLuint indirect_program, scale_sep_program, final_program, inter_program;

    struct osd_state *osd_state;
    struct mpgl_osd *osd;
    double osd_pts;

    GLuint lut_3d_texture;
    bool use_lut_3d;

    GLuint dither_texture;
    float dither_quantization;
    float dither_center;
    int dither_size;

    struct mp_image_params real_image_params;   // configured format
    struct mp_image_params image_params;        // texture format (mind hwdec case)
    struct mp_imgfmt_desc image_desc;
    int plane_count;
    int image_w, image_h;

    bool is_yuv, is_rgb, is_packed_yuv;
    bool has_alpha;
    char color_swizzle[5];
    float chroma_fix[2];

    float input_gamma, conv_gamma;
    float user_gamma;
    bool user_gamma_enabled; // shader handles user_gamma
    bool sigmoid_enabled;

    struct video_image image;

    struct fbotex indirect_fbo;         // RGB target
    struct fbotex scale_sep_fbo;        // first pass when doing 2 pass scaling
    struct fbosurface surfaces[FBOSURFACES_MAX];
    size_t surface_idx;

    // state for luma (0) and chroma (1) scalers
    struct scaler scalers[2];

    // true if scaler is currently upscaling
    bool upscaling;

    // reinit_rendering must be called
    bool need_reinit_rendering;

    bool is_interpolated;

    struct mp_csp_equalizer video_eq;

    // Source and destination color spaces for the CMS matrix
    struct mp_csp_primaries csp_src, csp_dest;

    struct mp_rect src_rect;    // displayed part of the source video
    struct mp_rect dst_rect;    // video rectangle on output window
    struct mp_osd_res osd_rect; // OSD size/margins
    int vp_x, vp_y, vp_w, vp_h; // GL viewport
    bool vp_vflipped;

    int frames_rendered;

    // Cached because computing it can take relatively long
    int last_dither_matrix_size;
    float *last_dither_matrix;

    struct gl_hwdec *hwdec;
    bool hwdec_active;

    void *scratch;
};

struct fmt_entry {
    int mp_format;
    GLint internal_format;
    GLenum format;
    GLenum type;
};

// Very special formats, for which OpenGL happens to have direct support
static const struct fmt_entry mp_to_gl_formats[] = {
    {IMGFMT_BGR555,  GL_RGBA,  GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV},
    {IMGFMT_BGR565,  GL_RGB,   GL_RGB,  GL_UNSIGNED_SHORT_5_6_5_REV},
    {IMGFMT_RGB555,  GL_RGBA,  GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV},
    {IMGFMT_RGB565,  GL_RGB,   GL_RGB,  GL_UNSIGNED_SHORT_5_6_5},
    {0},
};

static const struct fmt_entry gl_byte_formats[] = {
    {0, GL_RED,     GL_RED,     GL_UNSIGNED_BYTE},      // 1 x 8
    {0, GL_RG,      GL_RG,      GL_UNSIGNED_BYTE},      // 2 x 8
    {0, GL_RGB,     GL_RGB,     GL_UNSIGNED_BYTE},      // 3 x 8
    {0, GL_RGBA,    GL_RGBA,    GL_UNSIGNED_BYTE},      // 4 x 8
    {0, GL_R16,     GL_RED,     GL_UNSIGNED_SHORT},     // 1 x 16
    {0, GL_RG16,    GL_RG,      GL_UNSIGNED_SHORT},     // 2 x 16
    {0, GL_RGB16,   GL_RGB,     GL_UNSIGNED_SHORT},     // 3 x 16
    {0, GL_RGBA16,  GL_RGBA,    GL_UNSIGNED_SHORT},     // 4 x 16
};

static const struct fmt_entry gl_byte_formats_gles3[] = {
    {0, GL_R8,       GL_RED,    GL_UNSIGNED_BYTE},      // 1 x 8
    {0, GL_RG8,      GL_RG,     GL_UNSIGNED_BYTE},      // 2 x 8
    {0, GL_RGB8,     GL_RGB,    GL_UNSIGNED_BYTE},      // 3 x 8
    {0, GL_RGBA8,    GL_RGBA,   GL_UNSIGNED_BYTE},      // 4 x 8
    // There are no filterable texture formats that can be uploaded as
    // GL_UNSIGNED_SHORT, so apparently we're out of luck.
    {0, 0,           0,         0},                     // 1 x 16
    {0, 0,           0,         0},                     // 2 x 16
    {0, 0,           0,         0},                     // 3 x 16
    {0, 0,           0,         0},                     // 4 x 16
};

static const struct fmt_entry gl_byte_formats_gles2[] = {
    {0, GL_LUMINANCE,           GL_LUMINANCE,       GL_UNSIGNED_BYTE}, // 1 x 8
    {0, GL_LUMINANCE_ALPHA,     GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE}, // 2 x 8
    {0, GL_RGB,                 GL_RGB,             GL_UNSIGNED_BYTE}, // 3 x 8
    {0, GL_RGBA,                GL_RGBA,            GL_UNSIGNED_BYTE}, // 4 x 8
    {0, 0,                      0,                  0},                // 1 x 16
    {0, 0,                      0,                  0},                // 2 x 16
    {0, 0,                      0,                  0},                // 3 x 16
    {0, 0,                      0,                  0},                // 4 x 16
};

static const struct fmt_entry gl_byte_formats_legacy[] = {
    {0, GL_LUMINANCE,           GL_LUMINANCE,       GL_UNSIGNED_BYTE}, // 1 x 8
    {0, GL_LUMINANCE_ALPHA,     GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE}, // 2 x 8
    {0, GL_RGB,                 GL_RGB,             GL_UNSIGNED_BYTE}, // 3 x 8
    {0, GL_RGBA,                GL_RGBA,            GL_UNSIGNED_BYTE}, // 4 x 8
    {0, GL_LUMINANCE16,         GL_LUMINANCE,       GL_UNSIGNED_SHORT},// 1 x 16
    {0, GL_LUMINANCE16_ALPHA16, GL_LUMINANCE_ALPHA, GL_UNSIGNED_SHORT},// 2 x 16
    {0, GL_RGB16,               GL_RGB,             GL_UNSIGNED_SHORT},// 3 x 16
    {0, GL_RGBA16,              GL_RGBA,            GL_UNSIGNED_SHORT},// 4 x 16
};

static const struct fmt_entry gl_float16_formats[] = {
    {0, GL_R16F,    GL_RED,     GL_FLOAT},              // 1 x f
    {0, GL_RG16F,   GL_RG,      GL_FLOAT},              // 2 x f
    {0, GL_RGB16F,  GL_RGB,     GL_FLOAT},              // 3 x f
    {0, GL_RGBA16F, GL_RGBA,    GL_FLOAT},              // 4 x f
};

static const struct fmt_entry gl_apple_formats[] = {
    {IMGFMT_UYVY, GL_RGB, GL_RGB_422_APPLE, GL_UNSIGNED_SHORT_8_8_APPLE},
    {IMGFMT_YUYV, GL_RGB, GL_RGB_422_APPLE, GL_UNSIGNED_SHORT_8_8_REV_APPLE},
    {0}
};

struct packed_fmt_entry {
    int fmt;
    int8_t component_size;
    int8_t components[4]; // source component - 0 means unmapped
};

static const struct packed_fmt_entry mp_packed_formats[] = {
    //                  w   R  G  B  A
    {IMGFMT_Y8,         1, {1, 0, 0, 0}},
    {IMGFMT_Y16,        2, {1, 0, 0, 0}},
    {IMGFMT_YA8,        1, {1, 0, 0, 2}},
    {IMGFMT_YA16,       2, {1, 0, 0, 2}},
    {IMGFMT_ARGB,       1, {2, 3, 4, 1}},
    {IMGFMT_0RGB,       1, {2, 3, 4, 0}},
    {IMGFMT_BGRA,       1, {3, 2, 1, 4}},
    {IMGFMT_BGR0,       1, {3, 2, 1, 0}},
    {IMGFMT_ABGR,       1, {4, 3, 2, 1}},
    {IMGFMT_0BGR,       1, {4, 3, 2, 0}},
    {IMGFMT_RGBA,       1, {1, 2, 3, 4}},
    {IMGFMT_RGB0,       1, {1, 2, 3, 0}},
    {IMGFMT_BGR24,      1, {3, 2, 1, 0}},
    {IMGFMT_RGB24,      1, {1, 2, 3, 0}},
    {IMGFMT_RGB48,      2, {1, 2, 3, 0}},
    {IMGFMT_RGBA64,     2, {1, 2, 3, 4}},
    {IMGFMT_BGRA64,     2, {3, 2, 1, 4}},
    {0},
};

static const char *const osd_shaders[SUBBITMAP_COUNT] = {
    [SUBBITMAP_LIBASS] = "frag_osd_libass",
    [SUBBITMAP_RGBA] =   "frag_osd_rgba",
};

const struct gl_video_opts gl_video_opts_def = {
    .npot = 1,
    .dither_depth = -1,
    .dither_size = 6,
    .fbo_format = GL_RGBA,
    .sigmoid_center = 0.75,
    .sigmoid_slope = 6.5,
    .scalers = { "bilinear", "bilinear" },
    .scaler_params = {{NAN, NAN}, {NAN, NAN}},
    .scaler_radius = {3, 3},
    .alpha_mode = 2,
    .background = {0, 0, 0, 255},
    .gamma = 1.0f,
};

const struct gl_video_opts gl_video_opts_hq_def = {
    .npot = 1,
    .dither_depth = 0,
    .dither_size = 6,
    .fbo_format = GL_RGBA16,
    .fancy_downscaling = 1,
    .sigmoid_center = 0.75,
    .sigmoid_slope = 6.5,
    .sigmoid_upscaling = 1,
    .scalers = { "spline36", "bilinear" },
    .dscaler = "mitchell",
    .scaler_params = {{NAN, NAN}, {NAN, NAN}},
    .scaler_radius = {3, 3},
    .alpha_mode = 2,
    .background = {0, 0, 0, 255},
    .gamma = 1.0f,
};

static int validate_scaler_opt(struct mp_log *log, const m_option_t *opt,
                               struct bstr name, struct bstr param);

#define OPT_BASE_STRUCT struct gl_video_opts
const struct m_sub_options gl_video_conf = {
    .opts = (const m_option_t[]) {
        OPT_FLOATRANGE("gamma", gamma, 0, 0.1, 2.0),
        OPT_FLAG("srgb", srgb, 0),
        OPT_FLAG("npot", npot, 0),
        OPT_FLAG("pbo", pbo, 0),
        OPT_STRING_VALIDATE("scale", scalers[0], 0, validate_scaler_opt),
        OPT_STRING_VALIDATE("cscale", scalers[1], 0, validate_scaler_opt),
        OPT_STRING_VALIDATE("scale-down", dscaler, 0, validate_scaler_opt),
        OPT_FLOAT("scale-param1", scaler_params[0][0], 0),
        OPT_FLOAT("scale-param2", scaler_params[0][1], 0),
        OPT_FLOAT("cscale-param1", scaler_params[1][0], 0),
        OPT_FLOAT("cscale-param2", scaler_params[1][1], 0),
        OPT_FLOATRANGE("scale-radius", scaler_radius[0], 0, 1.0, 16.0),
        OPT_FLOATRANGE("cscale-radius", scaler_radius[1], 0, 1.0, 16.0),
        OPT_FLOATRANGE("scale-antiring", scaler_antiring[0], 0, 0.0, 1.0),
        OPT_FLOATRANGE("cscale-antiring", scaler_antiring[1], 0, 0.0, 1.0),
        OPT_FLAG("scaler-resizes-only", scaler_resizes_only, 0),
        OPT_FLAG("linear-scaling", linear_scaling, 0),
        OPT_FLAG("fancy-downscaling", fancy_downscaling, 0),
        OPT_FLAG("sigmoid-upscaling", sigmoid_upscaling, 0),
        OPT_FLOATRANGE("sigmoid-center", sigmoid_center, 0, 0.0, 1.0),
        OPT_FLOATRANGE("sigmoid-slope", sigmoid_slope, 0, 1.0, 20.0),
        OPT_CHOICE("fbo-format", fbo_format, 0,
                   ({"rgb",    GL_RGB},
                    {"rgba",   GL_RGBA},
                    {"rgb8",   GL_RGB8},
                    {"rgb10",  GL_RGB10},
                    {"rgb10_a2", GL_RGB10_A2},
                    {"rgb16",  GL_RGB16},
                    {"rgb16f", GL_RGB16F},
                    {"rgb32f", GL_RGB32F},
                    {"rgba12", GL_RGBA12},
                    {"rgba16", GL_RGBA16},
                    {"rgba16f", GL_RGBA16F},
                    {"rgba32f", GL_RGBA32F})),
        OPT_CHOICE_OR_INT("dither-depth", dither_depth, 0, -1, 16,
                          ({"no", -1}, {"auto", 0})),
        OPT_CHOICE("dither", dither_algo, 0,
                   ({"fruit", 0}, {"ordered", 1}, {"no", -1})),
        OPT_INTRANGE("dither-size-fruit", dither_size, 0, 2, 8),
        OPT_FLAG("temporal-dither", temporal_dither, 0),
        OPT_CHOICE("chroma-location", chroma_location, 0,
                   ({"auto",   MP_CHROMA_AUTO},
                    {"center", MP_CHROMA_CENTER},
                    {"left",   MP_CHROMA_LEFT})),
        OPT_CHOICE("alpha", alpha_mode, M_OPT_OPTIONAL_PARAM,
                   ({"no", 0},
                    {"yes", 1}, {"", 1},
                    {"blend", 2})),
        OPT_FLAG("rectangle-textures", use_rectangle, 0),
        OPT_COLOR("background", background, 0),
        OPT_FLAG("smoothmotion", smoothmotion, 0),
        OPT_FLOAT("smoothmotion-threshold", smoothmotion_threshold,
                   CONF_RANGE, .min = 0, .max = 0.5),
        OPT_REMOVED("approx-gamma", "this is always enabled now"),
        OPT_REMOVED("cscale-down", "chroma is never downscaled"),
        OPT_REMOVED("scale-sep", "this is set automatically whenever sane"),
        OPT_REMOVED("indirect", "this is set automatically whenever sane"),

        OPT_REPLACED("lscale", "scale"),
        OPT_REPLACED("lscale-down", "scale-down"),
        OPT_REPLACED("lparam1", "scale-param1"),
        OPT_REPLACED("lparam2", "scale-param2"),
        OPT_REPLACED("lradius", "scale-radius"),
        OPT_REPLACED("lantiring", "scale-antiring"),
        OPT_REPLACED("cparam1", "cscale-param1"),
        OPT_REPLACED("cparam2", "cscale-param2"),
        OPT_REPLACED("cradius", "cscale-radius"),
        OPT_REPLACED("cantiring", "cscale-antiring"),

        {0}
    },
    .size = sizeof(struct gl_video_opts),
    .defaults = &gl_video_opts_def,
};

static void uninit_rendering(struct gl_video *p);
static void delete_shaders(struct gl_video *p);
static void check_gl_features(struct gl_video *p);
static bool init_format(int fmt, struct gl_video *init);
static double get_scale_factor(struct gl_video *p);

static const struct fmt_entry *find_tex_format(GL *gl, int bytes_per_comp,
                                               int n_channels)
{
    assert(bytes_per_comp == 1 || bytes_per_comp == 2);
    assert(n_channels >= 1 && n_channels <= 4);
    const struct fmt_entry *fmts = gl_byte_formats;
    if (gl->es >= 300) {
        fmts = gl_byte_formats_gles3;
    } else if (gl->es) {
        fmts = gl_byte_formats_gles2;
    } else if (!(gl->mpgl_caps & MPGL_CAP_TEX_RG)) {
        fmts = gl_byte_formats_legacy;
    }
    return &fmts[n_channels - 1 + (bytes_per_comp - 1) * 4];
}

static void debug_check_gl(struct gl_video *p, const char *msg)
{
    if (p->gl_debug)
        glCheckError(p->gl, p->log, msg);
}

void gl_video_set_debug(struct gl_video *p, bool enable)
{
    GL *gl = p->gl;

    p->gl_debug = enable;
    if (p->gl->debug_context)
        gl_set_debug_logger(gl, enable ? p->log : NULL);
}

// Draw a textured quad.
// x0, y0, x1, y1 = destination coordinates of the quad in pixels
// tx0, ty0, tx1, ty1 = source texture coordinates in pixels
// tex_w, tex_h = size of the texture in pixels
// flags = bits 0-1: rotate, bits 2: flip vertically
static void draw_quad(struct gl_video *p,
                      float x0, float y0, float x1, float y1,
                      float tx0, float ty0, float tx1, float ty1,
                      float tex_w, float tex_h, int flags)
{
    if (p->gl_target != GL_TEXTURE_2D)
        tex_w = tex_h = 1.0f;

    if (flags & 4) {
        float tmp = ty0;
        ty0 = ty1;
        ty1 = tmp;
    }

    struct vertex va[4] = {
        { {x0, y0}, {tx0 / tex_w, ty0 / tex_h} },
        { {x0, y1}, {tx0 / tex_w, ty1 / tex_h} },
        { {x1, y0}, {tx1 / tex_w, ty0 / tex_h} },
        { {x1, y1}, {tx1 / tex_w, ty1 / tex_h} },
    };

    int rot = flags & 3;
    while (rot--) {
        static const int perm[4] = {1, 3, 0, 2};
        struct vertex vb[4];
        memcpy(vb, va, sizeof(vb));
        for (int n = 0; n < 4; n++)
            memcpy(va[n].texcoord, vb[perm[n]].texcoord, sizeof(float[2]));
    }

    gl_vao_draw_data(&p->vao, GL_TRIANGLE_STRIP, va, 4);

    debug_check_gl(p, "after rendering");
}

static void transpose3x3(float r[3][3])
{
    MPSWAP(float, r[0][1], r[1][0]);
    MPSWAP(float, r[0][2], r[2][0]);
    MPSWAP(float, r[1][2], r[2][1]);
}

static void update_uniforms(struct gl_video *p, GLuint program)
{
    GL *gl = p->gl;
    GLint loc;

    if (program == 0)
        return;

    gl->UseProgram(program);

    struct mp_csp_params cparams = MP_CSP_PARAMS_DEFAULTS;
    cparams.gray = p->is_yuv && !p->is_packed_yuv && p->plane_count == 1;
    cparams.input_bits = p->image_desc.component_bits;
    cparams.texture_bits = (cparams.input_bits + 7) & ~7;
    mp_csp_set_image_params(&cparams, &p->image_params);
    mp_csp_copy_equalizer_values(&cparams, &p->video_eq);
    if (p->image_desc.flags & MP_IMGFLAG_XYZ) {
        cparams.colorspace = MP_CSP_XYZ;
        cparams.input_bits = 8;
        cparams.texture_bits = 8;
    }

    loc = gl->GetUniformLocation(program, "transform");
    if (loc >= 0 && p->vp_w > 0 && p->vp_h > 0) {
        float matrix[3][3];
        int vvp[2] = {p->vp_h, 0};
        if (p->vp_vflipped)
            MPSWAP(int, vvp[0], vvp[1]);
        gl_matrix_ortho2d(matrix, 0, p->vp_w, vvp[0], vvp[1]);
        gl->UniformMatrix3fv(loc, 1, GL_FALSE, &matrix[0][0]);
    }

    loc = gl->GetUniformLocation(program, "colormatrix");
    if (loc >= 0) {
        struct mp_cmat m = {{{0}}};
        if (p->image_desc.flags & MP_IMGFLAG_XYZ) {
            // Hard-coded as relative colorimetric for now, since this transforms
            // from the source file's D55 material to whatever color space our
            // projector/display lives in, which should be D55 for a proper
            // home cinema setup either way.
            mp_get_xyz2rgb_coeffs(&cparams, p->csp_src,
                                  MP_INTENT_RELATIVE_COLORIMETRIC, &m);
        } else {
            mp_get_yuv2rgb_coeffs(&cparams, &m);
        }
        transpose3x3(m.m); // GLES2 can not transpose in glUniformMatrix3fv
        gl->UniformMatrix3fv(loc, 1, GL_FALSE, &m.m[0][0]);
        loc = gl->GetUniformLocation(program, "colormatrix_c");
        gl->Uniform3f(loc, m.c[0], m.c[1], m.c[2]);
    }

    gl->Uniform1f(gl->GetUniformLocation(program, "input_gamma"),
                  p->input_gamma);

    gl->Uniform1f(gl->GetUniformLocation(program, "conv_gamma"),
                  p->conv_gamma);

    // Coefficients for the sigmoidal transform are taken from the
    // formula here: http://www.imagemagick.org/Usage/color_mods/#sigmoidal
    float sig_center = p->opts.sigmoid_center;
    float sig_slope = p->opts.sigmoid_slope;

    // This function needs to go through (0,0) and (1,1) so we compute the
    // values at 1 and 0, and then scale/shift them, respectively.
    float sig_offset = 1.0/(1+expf(sig_slope * sig_center));
    float sig_scale  = 1.0/(1+expf(sig_slope * (sig_center-1))) - sig_offset;

    gl->Uniform1f(gl->GetUniformLocation(program, "sig_center"), sig_center);
    gl->Uniform1f(gl->GetUniformLocation(program, "sig_slope"), sig_slope);
    gl->Uniform1f(gl->GetUniformLocation(program, "sig_scale"), sig_scale);
    gl->Uniform1f(gl->GetUniformLocation(program, "sig_offset"), sig_offset);

    gl->Uniform1f(gl->GetUniformLocation(program, "inv_gamma"),
                  1.0f / p->user_gamma);

    for (int n = 0; n < p->plane_count; n++) {
        char textures_n[32];
        char textures_size_n[32];
        snprintf(textures_n, sizeof(textures_n), "texture%d", n);
        snprintf(textures_size_n, sizeof(textures_size_n), "textures_size[%d]", n);

        gl->Uniform1i(gl->GetUniformLocation(program, textures_n), n);
        if (p->gl_target == GL_TEXTURE_2D) {
            gl->Uniform2f(gl->GetUniformLocation(program, textures_size_n),
                          p->image.planes[n].tex_w, p->image.planes[n].tex_h);
        } else {
            // Makes the pixel size calculation code think they are 1x1
            gl->Uniform2f(gl->GetUniformLocation(program, textures_size_n), 1, 1);
        }
    }

    loc = gl->GetUniformLocation(program, "chroma_div");
    if (loc >= 0) {
        int xs = p->image_desc.chroma_xs;
        int ys = p->image_desc.chroma_ys;
        gl->Uniform2f(loc, 1.0 / (1 << xs), 1.0 / (1 << ys));
    }

    gl->Uniform2f(gl->GetUniformLocation(program, "chroma_fix"),
                  p->chroma_fix[0], p->chroma_fix[1]);

    loc = gl->GetUniformLocation(program, "chroma_center_offset");
    if (loc >= 0) {
        int chr = p->opts.chroma_location;
        if (!chr)
            chr = p->image_params.chroma_location;
        int cx, cy;
        mp_get_chroma_location(chr, &cx, &cy);
        // By default texture coordinates are such that chroma is centered with
        // any chroma subsampling. If a specific direction is given, make it
        // so that the luma and chroma sample line up exactly.
        // For 4:4:4, setting chroma location should have no effect at all.
        // luma sample size (in chroma coord. space)
        float ls_w = 1.0 / (1 << p->image_desc.chroma_xs);
        float ls_h = 1.0 / (1 << p->image_desc.chroma_ys);
        // move chroma center to luma center (in chroma coord. space)
        float o_x = ls_w < 1 ? ls_w * -cx / 2 : 0;
        float o_y = ls_h < 1 ? ls_h * -cy / 2 : 0;
        int c = p->gl_target == GL_TEXTURE_2D ? 1 : 0;
        gl->Uniform2f(loc, o_x / FFMAX(p->image.planes[1].w * c, 1),
                           o_y / FFMAX(p->image.planes[1].h * c, 1));
    }

    gl->Uniform2f(gl->GetUniformLocation(program, "dither_size"),
                  p->dither_size, p->dither_size);

    gl->Uniform1i(gl->GetUniformLocation(program, "lut_3d"), TEXUNIT_3DLUT);

    loc = gl->GetUniformLocation(program, "cms_matrix");
    if (loc >= 0) {
        float cms_matrix[3][3] = {{0}};
        // Hard-coded to relative colorimetric - for a BT.2020 3DLUT we expect
        // the input to be actual BT.2020 and not something red- or blueshifted,
        // and for sRGB monitors we most likely want relative scaling either way.
        mp_get_cms_matrix(p->csp_src, p->csp_dest, MP_INTENT_RELATIVE_COLORIMETRIC, cms_matrix);
        gl->UniformMatrix3fv(loc, 1, GL_TRUE, &cms_matrix[0][0]);
    }

    for (int n = 0; n < 2; n++) {
        const char *lut = p->scalers[n].lut_name;
        if (lut)
            gl->Uniform1i(gl->GetUniformLocation(program, lut),
                          TEXUNIT_SCALERS + n);
    }

    gl->Uniform1i(gl->GetUniformLocation(program, "dither"), TEXUNIT_DITHER);
    gl->Uniform1f(gl->GetUniformLocation(program, "dither_quantization"),
                  p->dither_quantization);
    gl->Uniform1f(gl->GetUniformLocation(program, "dither_center"),
                  p->dither_center);

    float sparam1_l = p->opts.scaler_params[0][0];
    float sparam1_c = p->opts.scaler_params[1][0];
    gl->Uniform1f(gl->GetUniformLocation(program, "filter_param1_l"),
                  isnan(sparam1_l) ? 0.5f : sparam1_l);
    gl->Uniform1f(gl->GetUniformLocation(program, "filter_param1_c"),
                  isnan(sparam1_c) ? 0.5f : sparam1_c);

    gl->Uniform3f(gl->GetUniformLocation(program, "translation"), 0, 0, 0);

    gl->UseProgram(0);

    debug_check_gl(p, "update_uniforms()");
}

static void update_all_uniforms(struct gl_video *p)
{
    for (int n = 0; n < SUBBITMAP_COUNT; n++)
        update_uniforms(p, p->osd->programs[n]);
    update_uniforms(p, p->indirect_program);
    update_uniforms(p, p->scale_sep_program);
    update_uniforms(p, p->final_program);
    update_uniforms(p, p->inter_program);
}

#define SECTION_HEADER "#!section "

static char *get_section(void *talloc_ctx, struct bstr source,
                         const char *section)
{
    char *res = talloc_strdup(talloc_ctx, "");
    bool copy = false;
    while (source.len) {
        struct bstr line = bstr_strip_linebreaks(bstr_getline(source, &source));
        if (bstr_eatstart(&line, bstr0(SECTION_HEADER))) {
            copy = bstrcmp0(line, section) == 0;
        } else if (copy) {
            res = talloc_asprintf_append_buffer(res, "%.*s\n", BSTR_P(line));
        }
    }
    return res;
}

static char *t_concat(void *talloc_ctx, const char *s1, const char *s2)
{
    return talloc_asprintf(talloc_ctx, "%s%s", s1, s2);
}

static GLuint create_shader(struct gl_video *p, GLenum type, const char *header,
                            const char *source)
{
    GL *gl = p->gl;

    void *tmp = talloc_new(NULL);
    const char *full_source = t_concat(tmp, header, source);

    GLuint shader = gl->CreateShader(type);
    gl->ShaderSource(shader, 1, &full_source, NULL);
    gl->CompileShader(shader);
    GLint status;
    gl->GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    GLint log_length;
    gl->GetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

    int pri = status ? (log_length > 1 ? MSGL_V : MSGL_DEBUG) : MSGL_ERR;
    const char *typestr = type == GL_VERTEX_SHADER ? "vertex" : "fragment";
    if (mp_msg_test(p->log, pri)) {
        MP_MSG(p, pri, "%s shader source:\n", typestr);
        mp_log_source(p->log, pri, full_source);
    }
    if (log_length > 1) {
        GLchar *logstr = talloc_zero_size(tmp, log_length + 1);
        gl->GetShaderInfoLog(shader, log_length, NULL, logstr);
        MP_MSG(p, pri, "%s shader compile log (status=%d):\n%s\n",
               typestr, status, logstr);
    }

    talloc_free(tmp);

    return shader;
}

static void prog_create_shader(struct gl_video *p, GLuint program, GLenum type,
                               const char *header,  const char *source)
{
    GL *gl = p->gl;
    GLuint shader = create_shader(p, type, header, source);
    gl->AttachShader(program, shader);
    gl->DeleteShader(shader);
}

static void link_shader(struct gl_video *p, GLuint program)
{
    GL *gl = p->gl;
    gl->LinkProgram(program);
    GLint status;
    gl->GetProgramiv(program, GL_LINK_STATUS, &status);
    GLint log_length;
    gl->GetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);

    int pri = status ? (log_length > 1 ? MSGL_V : MSGL_DEBUG) : MSGL_ERR;
    if (mp_msg_test(p->log, pri)) {
        GLchar *logstr = talloc_zero_size(NULL, log_length + 1);
        gl->GetProgramInfoLog(program, log_length, NULL, logstr);
        MP_MSG(p, pri, "shader link log (status=%d): %s\n", status, logstr);
        talloc_free(logstr);
    }
}

#define PRELUDE_END "// -- prelude end\n"

static GLuint create_program(struct gl_video *p, const char *name,
                             const char *header, const char *vertex,
                             const char *frag, struct gl_vao *vao)
{
    GL *gl = p->gl;
    MP_VERBOSE(p, "compiling shader program '%s', header:\n", name);
    const char *real_header = strstr(header, PRELUDE_END);
    real_header = real_header ? real_header + strlen(PRELUDE_END) : header;
    mp_log_source(p->log, MSGL_V, real_header);
    GLuint prog = gl->CreateProgram();
    prog_create_shader(p, prog, GL_VERTEX_SHADER, header, vertex);
    prog_create_shader(p, prog, GL_FRAGMENT_SHADER, header, frag);
    gl_vao_bind_attribs(vao, prog);
    link_shader(p, prog);
    return prog;
}

static void shader_def(char **shader, const char *name,
                       const char *value)
{
    *shader = talloc_asprintf_append(*shader, "#define %s %s\n", name, value);
}

static void shader_def_opt(char **shader, const char *name, bool b)
{
    if (b)
        shader_def(shader, name, "1");
}

#define APPENDF(s_ptr, ...) \
    *(s_ptr) = talloc_asprintf_append(*(s_ptr), __VA_ARGS__)

static void shader_setup_scaler(char **shader, struct scaler *scaler, int pass)
{
    int unit = scaler->index;
    const char *target = unit == 0 ? "SAMPLE" : "SAMPLE_C";
    if (!scaler->kernel) {
        APPENDF(shader, "#define %s(p0, p1, p2) "
                "sample_%s(p0, p1, p2, filter_param1_%c)\n",
                target, scaler->name, "lc"[unit]);
    } else {
        int size = scaler->kernel->size;
        const char *lut_tex = scaler->lut_name;
        char name[40];
        snprintf(name, sizeof(name), "sample_scaler%d", unit);
        APPENDF(shader, "#define DEF_SCALER%d \\\n    ", unit);
        char lut_fn[40];
        if (scaler->kernel->polar) {
            int radius = (int)scaler->kernel->radius;
            // SAMPLE_CONVOLUTION_POLAR_R(NAME, R, LUT, WEIGHTS_FN, ANTIRING)
            APPENDF(shader, "SAMPLE_CONVOLUTION_POLAR_R(%s, %d, %s, WEIGHTS%d, %f)\n",
                    name, radius, lut_tex, unit, scaler->antiring);

            // Pre-compute unrolled weights matrix
            APPENDF(shader, "#define WEIGHTS%d(LUT) \\\n    ", unit);
            for (int y = 1-radius; y <= radius; y++) {
                for (int x = 1-radius; x <= radius; x++) {
                    // Since we can't know the subpixel position in advance,
                    // assume a worst case scenario.
                    int yy = y > 0 ? y-1 : y;
                    int xx = x > 0 ? x-1 : x;
                    double d = sqrt(xx*xx + yy*yy);

                    // Samples outside the radius are unnecessary
                    if (d < radius) {
                        APPENDF(shader, "SAMPLE_POLAR_%s(LUT, %f, %d, %d) \\\n    ",
                                // The center 4 coefficients are the primary
                                // contributors, used to clamp the result for
                                // anti-ringing
                                (x >= 0 && y >= 0 && x <= 1 && y <= 1)
                                  ? "PRIMARY" : "HELPER",
                                (double)radius, x, y);
                    }
                }
            }
            APPENDF(shader, "\n");
        } else {
            if (size == 2 || size == 6) {
                snprintf(lut_fn, sizeof(lut_fn), "weights%d", size);
            } else {
                snprintf(lut_fn, sizeof(lut_fn), "weights_scaler%d", unit);
                APPENDF(shader, "WEIGHTS_N(%s, %d) \\\n    ", lut_fn, size);
            }
            if (pass != -1) {
                // The direction/pass assignment is rather arbitrary, but fixed in
                // other parts of the code (like FBO setup).
                const char *direction = pass == 0 ? "0, 1" : "1, 0";
                // SAMPLE_CONVOLUTION_SEP_N(NAME, DIR, N, LUT, WEIGHTS_FUNC)
                APPENDF(shader, "SAMPLE_CONVOLUTION_SEP_N(%s, vec2(%s), %d, %s, %s)\n",
                        name, direction, size, lut_tex, lut_fn);
            } else {
                // SAMPLE_CONVOLUTION_N(NAME, N, LUT, WEIGHTS_FUNC)
                APPENDF(shader, "SAMPLE_CONVOLUTION_N(%s, %d, %s, %s)\n",
                        name, size, lut_tex, lut_fn);
            }
        }
        APPENDF(shader, "#define %s %s\n", target, name);
    }
}

// return false if RGB or 4:4:4 YUV
static bool input_is_subsampled(struct gl_video *p)
{
    for (int i = 0; i < p->plane_count; i++)
        if (p->image_desc.xs[i] || p->image_desc.ys[i])
            return true;
    return false;
}

static void compile_shaders(struct gl_video *p)
{
    GL *gl = p->gl;

    debug_check_gl(p, "before shaders");

    delete_shaders(p);

    void *tmp = talloc_new(NULL);

    struct bstr src = bstr0(vo_opengl_shaders);
    char *vertex_shader = get_section(tmp, src, "vertex_all");
    char *shader_prelude = get_section(tmp, src, "prelude");
    char *s_video = get_section(tmp, src, "frag_video");

    bool rg = gl->mpgl_caps & MPGL_CAP_TEX_RG;
    bool tex1d = gl->mpgl_caps & MPGL_CAP_1D_TEX;
    bool tex3d = gl->mpgl_caps & MPGL_CAP_3D_TEX;
    bool arrays = gl->mpgl_caps & MPGL_CAP_1ST_CLASS_ARRAYS;
    char *header =
        talloc_asprintf(tmp, "#version %d%s\n"
                             "#define HAVE_RG %d\n"
                             "#define HAVE_1DTEX %d\n"
                             "#define HAVE_3DTEX %d\n"
                             "#define HAVE_ARRAYS %d\n"
                             "%s%s",
                             gl->glsl_version, gl->es >= 300 ? " es" : "",
                             rg, tex1d, tex3d, arrays, shader_prelude, PRELUDE_END);

    bool use_cms = p->opts.srgb || p->use_lut_3d;
    // 3DLUT overrides sRGB
    bool use_srgb = p->opts.srgb && !p->use_lut_3d;

    float input_gamma = 1.0;
    float conv_gamma = 1.0;

    bool is_xyz = p->image_desc.flags & MP_IMGFLAG_XYZ;
    if (is_xyz) {
        input_gamma *= 2.6;
        // Note that this results in linear light, so we make sure to enable
        // use_linear_light for XYZ inputs as well.
    }

    p->input_gamma = input_gamma;
    p->conv_gamma = conv_gamma;

    bool use_input_gamma = p->input_gamma != 1.0;
    bool use_conv_gamma = p->conv_gamma != 1.0;
    bool use_const_luma = p->image_params.colorspace == MP_CSP_BT_2020_C;

    enum mp_csp_trc gamma_fun = MP_CSP_TRC_NONE;

    // If either color correction option (3dlut or srgb) is enabled, or if
    // sigmoidal upscaling is requested, or if the source is linear XYZ, we
    // always scale in linear light
    bool use_linear_light = p->opts.linear_scaling || p->opts.sigmoid_upscaling
                            || use_cms || is_xyz;

    if (use_linear_light) {
        // We use the color level range to distinguish between PC
        // content like images, which are most likely sRGB, and TV content
        // like movies, which are most likely BT.1886. XYZ input is always
        // treated as linear.
        if (is_xyz) {
            gamma_fun = MP_CSP_TRC_LINEAR;
        } else if (p->image_params.colorlevels == MP_CSP_LEVELS_PC) {
            gamma_fun = MP_CSP_TRC_SRGB;
        } else {
            gamma_fun = MP_CSP_TRC_BT_1886;
        }
    }

    // The inverse of the above transformation is normally handled by
    // the CMS cases, but if CMS is disabled we need to go back manually
    bool use_inv_bt1886 = false;
    if (use_linear_light && !use_cms) {
        if (gamma_fun == MP_CSP_TRC_SRGB) {
            use_srgb = true;
        } else {
            use_inv_bt1886 = true;
        }
    }

    // Optionally transform to sigmoidal color space if requested.
    p->sigmoid_enabled = p->opts.sigmoid_upscaling;
    bool use_sigmoid = p->sigmoid_enabled && p->upscaling;

    // Figure out the right color spaces we need to convert, if any
    enum mp_csp_prim prim_src = p->image_params.primaries, prim_dest;
    if (use_cms) {
        // sRGB mode wants sRGB aka BT.709 primaries, but the 3DLUT is
        // always built against BT.2020.
        prim_dest = p->opts.srgb ? MP_CSP_PRIM_BT_709 : MP_CSP_PRIM_BT_2020;
    } else {
        // If no CMS is being done we just want to output stuff as-is,
        // in the native colorspace of the source.
        prim_dest = prim_src;
    }

    // XYZ input has no defined input color space, so we can directly convert
    // it to whatever output space we actually need.
    if (p->image_desc.flags & MP_IMGFLAG_XYZ)
        prim_src = prim_dest;

    // Set the colorspace primaries and figure out whether we need to perform
    // an extra conversion.
    p->csp_src  = mp_get_csp_primaries(prim_src);
    p->csp_dest = mp_get_csp_primaries(prim_dest);

    bool use_cms_matrix = prim_src != prim_dest;

    if (p->gl_target == GL_TEXTURE_RECTANGLE) {
        shader_def(&header, "VIDEO_SAMPLER", "sampler2DRect");
        shader_def_opt(&header, "USE_RECTANGLE", true);
    } else {
        shader_def(&header, "VIDEO_SAMPLER", "sampler2D");
    }

    // Need to pass alpha through the whole chain. (Not needed for OSD shaders.)
    if (p->opts.alpha_mode == 1)
        shader_def_opt(&header, "USE_ALPHA", p->has_alpha);

    char *header_osd = talloc_strdup(tmp, header);
    shader_def_opt(&header_osd, "USE_OSD_LINEAR_CONV_BT1886",
                   use_cms && gamma_fun == MP_CSP_TRC_BT_1886);
    shader_def_opt(&header_osd, "USE_OSD_LINEAR_CONV_SRGB",
                   use_cms && gamma_fun == MP_CSP_TRC_SRGB);
    shader_def_opt(&header_osd, "USE_OSD_CMS_MATRIX", use_cms_matrix);
    shader_def_opt(&header_osd, "USE_OSD_3DLUT", p->use_lut_3d);
    shader_def_opt(&header_osd, "USE_OSD_SRGB", use_cms && use_srgb);

    for (int n = 0; n < SUBBITMAP_COUNT; n++) {
        const char *name = osd_shaders[n];
        if (name) {
            char *s_osd = get_section(tmp, src, name);
            p->osd_programs[n] = create_program(p, name, header_osd,
                                                vertex_shader, s_osd,
                                                &p->osd->vao);
        }
    }

    struct gl_vao *v = &p->vao; // VAO to use to draw primitives

    char *header_conv = talloc_strdup(tmp, "");
    char *header_final = talloc_strdup(tmp, "");
    char *header_inter = talloc_strdup(tmp, "");
    char *header_sep = NULL;

    if (p->image_desc.id == IMGFMT_NV12 || p->image_desc.id == IMGFMT_NV21) {
        shader_def(&header_conv, "USE_CONV", "CONV_NV12");
    } else if (p->plane_count > 1) {
        shader_def(&header_conv, "USE_CONV", "CONV_PLANAR");
    }

    if (p->color_swizzle[0])
        shader_def(&header_conv, "USE_COLOR_SWIZZLE", p->color_swizzle);
    shader_def_opt(&header_conv, "USE_INPUT_GAMMA", use_input_gamma);
    shader_def_opt(&header_conv, "USE_COLORMATRIX", !p->is_rgb);
    shader_def_opt(&header_conv, "USE_CONV_GAMMA", use_conv_gamma);
    shader_def_opt(&header_conv, "USE_CONST_LUMA", use_const_luma);
    shader_def_opt(&header_conv, "USE_LINEAR_LIGHT_BT1886",
                   gamma_fun == MP_CSP_TRC_BT_1886);
    shader_def_opt(&header_conv, "USE_LINEAR_LIGHT_SRGB",
                   gamma_fun == MP_CSP_TRC_SRGB);
    shader_def_opt(&header_conv, "USE_SIGMOID", use_sigmoid);
    if (p->opts.alpha_mode > 0 && p->has_alpha && p->plane_count > 3)
        shader_def(&header_conv, "USE_ALPHA_PLANE", "3");
    if (p->opts.alpha_mode == 2 && p->has_alpha)
        shader_def(&header_conv, "USE_ALPHA_BLEND", "1");
    shader_def_opt(&header_conv, "USE_CHROMA_FIX",
                   p->chroma_fix[0] != 1.0f || p->chroma_fix[1] != 1.0f);

    shader_def_opt(&header_final, "USE_SIGMOID_INV", use_sigmoid);
    shader_def_opt(&header_final, "USE_INV_GAMMA", p->user_gamma_enabled);
    shader_def_opt(&header_final, "USE_CMS_MATRIX", use_cms_matrix);
    shader_def_opt(&header_final, "USE_3DLUT", p->use_lut_3d);
    shader_def_opt(&header_final, "USE_SRGB", use_srgb);
    shader_def_opt(&header_final, "USE_INV_BT1886", use_inv_bt1886);
    shader_def_opt(&header_final, "USE_DITHER", p->dither_texture != 0);
    shader_def_opt(&header_final, "USE_TEMPORAL_DITHER", p->opts.temporal_dither);

    if (p->scalers[0].kernel && !p->scalers[0].kernel->polar) {
        header_sep = talloc_strdup(tmp, "");
        shader_def_opt(&header_sep, "FIXED_SCALE", true);
        shader_setup_scaler(&header_sep, &p->scalers[0], 0);
        shader_setup_scaler(&header_inter, &p->scalers[0], 1);
    } else {
        shader_setup_scaler(&header_inter, &p->scalers[0], -1);
    }

    bool use_interpolation = p->opts.smoothmotion;

    if (use_interpolation) {
        shader_def_opt(&header_inter, "FIXED_SCALE", true);
        shader_def_opt(&header_final, "USE_LINEAR_INTERPOLATION", 1);
    }

    // The indirect pass is used to preprocess the image before scaling.
    bool use_indirect = false;

    // Don't sample from input video textures before converting the input to
    // its proper gamma.
    if (use_input_gamma || use_conv_gamma || use_linear_light || use_const_luma)
        use_indirect = true;

    // Trivial scalers are implemented directly and efficiently by the GPU.
    // This only includes bilinear and nearest neighbour in OpenGL, but we
    // don't support nearest neighbour upsampling.
    bool trivial_scaling = strcmp(p->scalers[0].name, "bilinear") == 0 &&
                           strcmp(p->scalers[1].name, "bilinear") == 0;

    // If the video is subsampled, chroma information needs to be pulled up to
    // the input size before scaling can be done. Even for 4:4:4 or planar RGB
    // this is also faster because it means the scalers can operate on all
    // channels simultaneously. This is unnecessary for trivial scaling.
    if (p->plane_count > 1 && !trivial_scaling)
        use_indirect = true;

    if (input_is_subsampled(p)) {
        shader_setup_scaler(&header_conv, &p->scalers[1], -1);
    } else {
        // Force using the normal scaler on chroma. If the "indirect" stage is
        // used, the actual scaling will happen in the next stage.
        shader_def(&header_conv, "SAMPLE_C",
                   use_indirect ? "SAMPLE_TRIVIAL" : "SAMPLE");
    }

    if (use_indirect) {
        // We don't use filtering for the Y-plane (luma), because it's never
        // scaled in this scenario.
        shader_def(&header_conv, "SAMPLE", "SAMPLE_TRIVIAL");
        shader_def_opt(&header_conv, "FIXED_SCALE", true);
        header_conv = t_concat(tmp, header, header_conv);
        p->indirect_program =
            create_program(p, "indirect", header_conv, vertex_shader, s_video, v);
    } else if (header_sep) {
        header_sep = t_concat(tmp, header_sep, header_conv);
    } else {
        header_inter = t_concat(tmp, header_inter, header_conv);
    }

    if (header_sep) {
        header_sep = t_concat(tmp, header, header_sep);
        p->scale_sep_program =
            create_program(p, "scale_sep", header_sep, vertex_shader, s_video, v);
    }

    if (use_interpolation) {
        header_inter = t_concat(tmp, header, header_inter);
        p->inter_program =
            create_program(p, "inter", header_inter, vertex_shader, s_video, v);
    } else {
        header_final = t_concat(tmp, header_final, header_inter);
    }

    header_final = t_concat(tmp, header, header_final);
    p->final_program =
        create_program(p, "final", header_final, vertex_shader, s_video, v);

    debug_check_gl(p, "shader compilation");

    talloc_free(tmp);
}

static void delete_program(GL *gl, GLuint *prog)
{
    gl->DeleteProgram(*prog);
    *prog = 0;
}

static void delete_shaders(struct gl_video *p)
{
    GL *gl = p->gl;

    for (int n = 0; n < SUBBITMAP_COUNT; n++)
        delete_program(gl, &p->osd->programs[n]);
    delete_program(gl, &p->indirect_program);
    delete_program(gl, &p->scale_sep_program);
    delete_program(gl, &p->final_program);
    delete_program(gl, &p->inter_program);
}

static void get_scale_factors(struct gl_video *p, double xy[2])
{
    xy[0] = (p->dst_rect.x1 - p->dst_rect.x0) /
            (double)(p->src_rect.x1 - p->src_rect.x0);
    xy[1] = (p->dst_rect.y1 - p->dst_rect.y0) /
            (double)(p->src_rect.y1 - p->src_rect.y0);
}

static double get_scale_factor(struct gl_video *p)
{
    double xy[2];
    get_scale_factors(p, xy);
    return FFMIN(xy[0], xy[1]);
}

static void update_scale_factor(struct gl_video *p, struct scaler *scaler)
{
    double scale = 1.0;
    double xy[2];
    get_scale_factors(p, xy);
    double f = MPMIN(xy[0], xy[1]);
    if (p->opts.fancy_downscaling && scaler->index == 0 && f < 1.0 &&
        fabs(xy[0] - f) < 0.01 && fabs(xy[1] - f) < 0.01)
    {
        MP_VERBOSE(p, "Using fancy-downscaling (scaler %d).\n", scaler->index);
        scale = FFMAX(1.0, 1.0 / f);
    }
    scaler->insufficient = !mp_init_filter(scaler->kernel, filter_sizes, scale);
}

static void init_scaler(struct gl_video *p, struct scaler *scaler)
{
    GL *gl = p->gl;

    assert(scaler->name);

    scaler->kernel = NULL;
    scaler->insufficient = false;

    const struct filter_kernel *t_kernel = mp_find_filter_kernel(scaler->name);
    if (!t_kernel)
        return;

    scaler->kernel_storage = *t_kernel;
    scaler->kernel = &scaler->kernel_storage;

    for (int n = 0; n < 2; n++) {
        if (!isnan(p->opts.scaler_params[scaler->index][n]))
            scaler->kernel->params[n] = p->opts.scaler_params[scaler->index][n];
    }

    scaler->antiring = p->opts.scaler_antiring[scaler->index];

    if (scaler->kernel->radius < 0)
        scaler->kernel->radius = p->opts.scaler_radius[scaler->index];

    update_scale_factor(p, scaler);

    int size = scaler->kernel->size;
    int elems_per_pixel = 4;
    if (size == 1) {
        elems_per_pixel = 1;
    } else if (size == 2) {
        elems_per_pixel = 2;
    } else if (size == 6) {
        elems_per_pixel = 3;
    }
    int width = size / elems_per_pixel;
    assert(size == width * elems_per_pixel);
    const struct fmt_entry *fmt = &gl_float16_formats[elems_per_pixel - 1];
    int target;

    if (scaler->kernel->polar) {
        target = GL_TEXTURE_1D;
        scaler->lut_name = scaler->index == 0 ? "lut_1d_l" : "lut_1d_c";
    } else {
        target = GL_TEXTURE_2D;
        scaler->lut_name = scaler->index == 0 ? "lut_2d_l" : "lut_2d_c";
    }

    gl->ActiveTexture(GL_TEXTURE0 + TEXUNIT_SCALERS + scaler->index);

    if (!scaler->gl_lut)
        gl->GenTextures(1, &scaler->gl_lut);

    gl->BindTexture(target, scaler->gl_lut);

    float *weights = talloc_array(NULL, float, LOOKUP_TEXTURE_SIZE * size);
    mp_compute_lut(scaler->kernel, LOOKUP_TEXTURE_SIZE, weights);

    if (target == GL_TEXTURE_1D) {
        gl->TexImage1D(target, 0, fmt->internal_format, LOOKUP_TEXTURE_SIZE,
                       0, fmt->format, GL_FLOAT, weights);
    } else {
        gl->TexImage2D(target, 0, fmt->internal_format, width, LOOKUP_TEXTURE_SIZE,
                       0, fmt->format, GL_FLOAT, weights);
    }

    talloc_free(weights);

    gl->TexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    if (target != GL_TEXTURE_1D)
        gl->TexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl->ActiveTexture(GL_TEXTURE0);

    debug_check_gl(p, "after initializing scaler");
}

static void init_dither(struct gl_video *p)
{
    GL *gl = p->gl;

    // Assume 8 bits per component if unknown.
    int dst_depth = p->depth_g ? p->depth_g : 8;
    if (p->opts.dither_depth > 0)
        dst_depth = p->opts.dither_depth;

    if (p->opts.dither_depth < 0 || p->opts.dither_algo < 0)
        return;

    MP_VERBOSE(p, "Dither to %d.\n", dst_depth);

    int tex_size;
    void *tex_data;
    GLint tex_iformat;
    GLint tex_format;
    GLenum tex_type;
    unsigned char temp[256];

    if (p->opts.dither_algo == 0) {
        int sizeb = p->opts.dither_size;
        int size = 1 << sizeb;

        if (p->last_dither_matrix_size != size) {
            p->last_dither_matrix = talloc_realloc(p, p->last_dither_matrix,
                                                   float, size * size);
            mp_make_fruit_dither_matrix(p->last_dither_matrix, sizeb);
            p->last_dither_matrix_size = size;
        }

        tex_size = size;
        tex_iformat = gl_float16_formats[0].internal_format;
        tex_format = gl_float16_formats[0].format;
        tex_type = GL_FLOAT;
        tex_data = p->last_dither_matrix;
    } else {
        assert(sizeof(temp) >= 8 * 8);
        mp_make_ordered_dither_matrix(temp, 8);

        const struct fmt_entry *fmt = find_tex_format(gl, 1, 1);
        tex_size = 8;
        tex_iformat = fmt->internal_format;
        tex_format = fmt->format;
        tex_type = fmt->type;
        tex_data = temp;
    }

    // This defines how many bits are considered significant for output on
    // screen. The superfluous bits will be used for rounding according to the
    // dither matrix. The precision of the source implicitly decides how many
    // dither patterns can be visible.
    p->dither_quantization = (1 << dst_depth) - 1;
    p->dither_center = 0.5 / (tex_size * tex_size);
    p->dither_size = tex_size;

    gl->ActiveTexture(GL_TEXTURE0 + TEXUNIT_DITHER);
    gl->GenTextures(1, &p->dither_texture);
    gl->BindTexture(GL_TEXTURE_2D, p->dither_texture);
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->TexImage2D(GL_TEXTURE_2D, 0, tex_iformat, tex_size, tex_size, 0,
                   tex_format, tex_type, tex_data);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->ActiveTexture(GL_TEXTURE0);

    debug_check_gl(p, "dither setup");
}

static void recreate_osd(struct gl_video *p)
{
    if (p->osd)
        mpgl_osd_destroy(p->osd);
    p->osd = mpgl_osd_init(p->gl, p->log, p->osd_state, p->osd_programs);
    p->osd->use_pbo = p->opts.pbo;
}

static bool does_resize(struct mp_rect src, struct mp_rect dst)
{
    return src.x1 - src.x0 != dst.x1 - dst.x0 ||
           src.y1 - src.y0 != dst.y1 - dst.y0;
}

static const char *expected_scaler(struct gl_video *p, int unit)
{
    if (p->opts.scaler_resizes_only && unit == 0 &&
        !does_resize(p->src_rect, p->dst_rect))
    {
        return "bilinear";
    }
    if (unit == 0 && p->opts.dscaler && get_scale_factor(p) < 1.0)
        return p->opts.dscaler;
    return p->opts.scalers[unit];
}

static void update_settings(struct gl_video *p)
{
    struct mp_csp_params params;
    mp_csp_copy_equalizer_values(&params, &p->video_eq);

    p->user_gamma = params.gamma * p->opts.gamma;

    // Lazy gamma shader initialization (a microoptimization)
    if (p->user_gamma != 1.0f && !p->user_gamma_enabled) {
        p->user_gamma_enabled = true;
        p->need_reinit_rendering = true;
    }
}

static void reinit_rendering(struct gl_video *p)
{
    GL *gl = p->gl;

    MP_VERBOSE(p, "Reinit rendering.\n");

    debug_check_gl(p, "before scaler initialization");

    uninit_rendering(p);

    if (!p->image_params.imgfmt)
        return;

    update_settings(p);

    for (int n = 0; n < 2; n++)
        p->scalers[n].name = expected_scaler(p, n);

    init_dither(p);

    init_scaler(p, &p->scalers[0]);
    init_scaler(p, &p->scalers[1]);

    compile_shaders(p);
    update_all_uniforms(p);

    int w = p->image_w;
    int h = p->image_h;

    // Convolution filters don't need linear sampling, so using nearest is
    // often faster.
    GLenum filter = p->scalers[0].kernel ? GL_NEAREST : GL_LINEAR;

    if (p->indirect_program) {
        fbotex_init(&p->indirect_fbo, gl, p->log, w, h, p->gl_target, filter,
                    p->opts.fbo_format);
    }

    recreate_osd(p);

    p->need_reinit_rendering = false;
}

static void uninit_rendering(struct gl_video *p)
{
    GL *gl = p->gl;

    delete_shaders(p);

    for (int n = 0; n < 2; n++) {
        gl->DeleteTextures(1, &p->scalers[n].gl_lut);
        p->scalers[n].gl_lut = 0;
        p->scalers[n].lut_name = NULL;
        p->scalers[n].kernel = NULL;
    }

    gl->DeleteTextures(1, &p->dither_texture);
    p->dither_texture = 0;

    fbotex_uninit(&p->indirect_fbo);

    for (int i = 0; i < FBOSURFACES_MAX; i++) {
        fbotex_uninit(&p->surfaces[i].fbotex);
        p->surfaces[i].valid = false;
    }

    fbotex_uninit(&p->scale_sep_fbo);
}

void gl_video_set_lut3d(struct gl_video *p, struct lut3d *lut3d)
{
    GL *gl = p->gl;

    if (!lut3d) {
        if (p->use_lut_3d) {
            p->use_lut_3d = false;
            reinit_rendering(p);
        }
        return;
    }

    if (!(gl->mpgl_caps & MPGL_CAP_3D_TEX))
        return;

    if (!p->lut_3d_texture)
        gl->GenTextures(1, &p->lut_3d_texture);

    gl->ActiveTexture(GL_TEXTURE0 + TEXUNIT_3DLUT);
    gl->BindTexture(GL_TEXTURE_3D, p->lut_3d_texture);
    gl->TexImage3D(GL_TEXTURE_3D, 0, GL_RGB16, lut3d->size[0], lut3d->size[1],
                   lut3d->size[2], 0, GL_RGB, GL_UNSIGNED_SHORT, lut3d->data);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    gl->ActiveTexture(GL_TEXTURE0);

    p->use_lut_3d = true;
    check_gl_features(p);

    debug_check_gl(p, "after 3d lut creation");

    reinit_rendering(p);
}

static void set_image_textures(struct gl_video *p, struct video_image *vimg,
                               GLuint imgtex[4])
{
    GL *gl = p->gl;
    GLuint dummy[4] = {0};
    if (!imgtex)
        imgtex = dummy;

    assert(vimg->mpi);

    if (p->hwdec_active) {
        p->hwdec->driver->map_image(p->hwdec, vimg->mpi, imgtex);
    } else {
        for (int n = 0; n < p->plane_count; n++)
            imgtex[n] = vimg->planes[n].gl_texture;
    }

    for (int n = 0; n < 4; n++) {
        gl->ActiveTexture(GL_TEXTURE0 + n);
        gl->BindTexture(p->gl_target, imgtex[n]);
    }
    gl->ActiveTexture(GL_TEXTURE0);
}

static void unset_image_textures(struct gl_video *p)
{
    GL *gl = p->gl;

    for (int n = 0; n < 4; n++) {
        gl->ActiveTexture(GL_TEXTURE0 + n);
        gl->BindTexture(p->gl_target, 0);
    }
    gl->ActiveTexture(GL_TEXTURE0);

    if (p->hwdec_active)
        p->hwdec->driver->unmap_image(p->hwdec);
}

static int align_pow2(int s)
{
    int r = 1;
    while (r < s)
        r *= 2;
    return r;
}

static void init_video(struct gl_video *p)
{
    GL *gl = p->gl;

    check_gl_features(p);

    init_format(p->image_params.imgfmt, p);
    p->gl_target = p->opts.use_rectangle ? GL_TEXTURE_RECTANGLE : GL_TEXTURE_2D;

    if (p->hwdec_active) {
        if (p->hwdec->driver->reinit(p->hwdec, &p->image_params) < 0)
            MP_ERR(p, "Initializing texture for hardware decoding failed.\n");
        init_format(p->image_params.imgfmt, p);
        p->gl_target = p->hwdec->gl_texture_target;
    }

    mp_image_params_guess_csp(&p->image_params);

    p->image_w = p->image_params.w;
    p->image_h = p->image_params.h;

    int eq_caps = MP_CSP_EQ_CAPS_GAMMA;
    if (p->is_yuv && p->image_params.colorspace != MP_CSP_BT_2020_C)
        eq_caps |= MP_CSP_EQ_CAPS_COLORMATRIX;
    if (p->image_desc.flags & MP_IMGFLAG_XYZ)
        eq_caps |= MP_CSP_EQ_CAPS_BRIGHTNESS;
    p->video_eq.capabilities = eq_caps;

    debug_check_gl(p, "before video texture creation");

    struct video_image *vimg = &p->image;

    for (int n = 0; n < p->plane_count; n++) {
        struct texplane *plane = &vimg->planes[n];

        plane->w = mp_chroma_div_up(p->image_w, p->image_desc.xs[n]);
        plane->h = mp_chroma_div_up(p->image_h, p->image_desc.ys[n]);

        plane->tex_w = plane->w;
        plane->tex_h = plane->h;

        if (!p->hwdec_active) {
            if (!p->opts.npot) {
                plane->tex_w = align_pow2(plane->tex_w);
                plane->tex_h = align_pow2(plane->tex_h);
            }

            gl->ActiveTexture(GL_TEXTURE0 + n);
            gl->GenTextures(1, &plane->gl_texture);
            gl->BindTexture(p->gl_target, plane->gl_texture);

            gl->TexImage2D(p->gl_target, 0, plane->gl_internal_format,
                           plane->tex_w, plane->tex_h, 0,
                           plane->gl_format, plane->gl_type, NULL);

            gl->TexParameteri(p->gl_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->TexParameteri(p->gl_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->TexParameteri(p->gl_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->TexParameteri(p->gl_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        MP_VERBOSE(p, "Texture for plane %d: %dx%d\n",
                   n, plane->tex_w, plane->tex_h);
    }
    gl->ActiveTexture(GL_TEXTURE0);

    // If the dimensions of the Y plane are not aligned on the luma.
    // Assume 4:2:0 with size (3,3). The last luma pixel is (2,2).
    // The last chroma pixel is (1,1), not (0,0). So for luma, the
    // coordinate range is [0,3), for chroma it is [0,2). This means the
    // texture coordinates for chroma are stretched by adding 1 luma pixel
    // to the range. Undo this.
    p->chroma_fix[0] = p->image.planes[0].tex_w / (double)p->image.planes[1].tex_w
                       / (1 << p->image_desc.chroma_xs);
    p->chroma_fix[1] = p->image.planes[0].tex_h / (double)p->image.planes[1].tex_h
                       / (1 << p->image_desc.chroma_ys);

    debug_check_gl(p, "after video texture creation");

    reinit_rendering(p);
}

static void uninit_video(struct gl_video *p)
{
    GL *gl = p->gl;

    uninit_rendering(p);

    struct video_image *vimg = &p->image;

    for (int n = 0; n < 3; n++) {
        struct texplane *plane = &vimg->planes[n];

        gl->DeleteTextures(1, &plane->gl_texture);
        plane->gl_texture = 0;
        gl->DeleteBuffers(1, &plane->gl_buffer);
        plane->gl_buffer = 0;
        plane->buffer_ptr = NULL;
        plane->buffer_size = 0;
    }
    mp_image_unrefp(&vimg->mpi);

    // Invalidate image_params to ensure that gl_video_config() will call
    // init_video() on uninitialized gl_video.
    p->real_image_params = (struct mp_image_params){0};
    p->image_params = p->real_image_params;
}

static void change_dither_trafo(struct gl_video *p)
{
    GL *gl = p->gl;
    int program = p->final_program;

    int phase = p->frames_rendered % 8u;
    float r = phase * (M_PI / 2); // rotate
    float m = phase < 4 ? 1 : -1; // mirror

    gl->UseProgram(program);

    float matrix[2][2] = {{cos(r),     -sin(r)    },
                          {sin(r) * m,  cos(r) * m}};
    gl->UniformMatrix2fv(gl->GetUniformLocation(program, "dither_trafo"),
                         1, GL_TRUE, &matrix[0][0]);

    gl->UseProgram(0);
}

struct pass {
    int num;
    // Not necessarily a FBO; we just abuse this struct because it's convenient.
    // It specifies the source texture/sub-rectangle for the next pass.
    struct fbotex f;
    // If true, render source (f) to dst, instead of the full dest. fbo viewport
    bool use_dst;
    struct mp_rect dst;
    int flags; // for write_quad
};

// *chain contains the source, and is overwritten with a copy of the result
// fbo is used as destination texture/render target.
static void handle_pass(struct gl_video *p, struct pass *chain,
                        struct fbotex *fbo, GLuint program)
{
    GL *gl = p->gl;

    if (!program)
        return;

    gl->BindTexture(p->gl_target, chain->f.texture);
    gl->UseProgram(program);

    gl->Viewport(fbo->vp_x, fbo->vp_y, fbo->vp_w, fbo->vp_h);
    gl->BindFramebuffer(GL_FRAMEBUFFER, fbo->fbo);

    int tex_w = chain->f.tex_w;
    int tex_h = chain->f.tex_h;
    struct mp_rect src = {
        .x0 = chain->f.vp_x,
        .y0 = chain->f.vp_y,
        .x1 = chain->f.vp_x + chain->f.vp_w,
        .y1 = chain->f.vp_y + chain->f.vp_h,
    };

    struct mp_rect dst = {-1, -1, 1, 1};
    if (chain->use_dst)
        dst = chain->dst;

    MP_TRACE(p, "Pass %d: [%d,%d,%d,%d] -> [%d,%d,%d,%d][%d,%d@%dx%d/%dx%d] (%d)\n",
             chain->num, src.x0, src.y0, src.x1, src.y1,
             dst.x0, dst.y0, dst.x1, dst.y1,
             fbo->vp_x, fbo->vp_y, fbo->vp_w, fbo->vp_h,
             fbo->tex_w, fbo->tex_h, chain->flags);

    draw_quad(p,
              dst.x0, dst.y0, dst.x1, dst.y1,
              src.x0, src.y0, src.x1, src.y1,
              tex_w, tex_h, chain->flags);

    *chain = (struct pass){
        .num = chain->num + 1,
        .f = *fbo,
    };
}

static size_t fbosurface_next(struct gl_video *p)
{
    return (p->surface_idx + 1) % FBOSURFACES_MAX;
}

// Handle all of the frame passes upto and including upscaling, assuming
// upscaling is not part of the final pass
static void gl_video_upscale_frame(struct gl_video *p, struct pass *chain, struct fbotex *inter_fbo)
{
    // Order of processing: [indirect -> [scale_sep ->]] inter
    handle_pass(p, chain, &p->indirect_fbo, p->indirect_program);

    // compensated for optional rotation
    struct mp_rect src_rect_rot = p->src_rect;
    if ((p->image_params.rotate % 180) == 90) {
        MPSWAP(int, src_rect_rot.x0, src_rect_rot.y0);
        MPSWAP(int, src_rect_rot.x1, src_rect_rot.y1);
    }

    // Clip to visible height so that separate scaling scales the visible part
    // only (and the target FBO texture can have a bounded size).
    // Don't clamp width; too hard to get correct final scaling on l/r borders.
    chain->f.vp_y = src_rect_rot.y0;
    chain->f.vp_h = src_rect_rot.y1 - src_rect_rot.y0;

    handle_pass(p, chain, &p->scale_sep_fbo, p->scale_sep_program);

    // For Y direction, use the whole source viewport; it has been fit to the
    // correct origin/height before.
    // For X direction, assume the texture wasn't scaled yet, so we can
    // select the correct portion, which will be scaled to screen.
    chain->f.vp_x = src_rect_rot.x0;
    chain->f.vp_w = src_rect_rot.x1 - src_rect_rot.x0;

    if (inter_fbo)
        handle_pass(p, chain, inter_fbo, p->inter_program);
}

static double gl_video_interpolate_frame(struct gl_video *p,
                                       struct pass *chain,
                                       struct frame_timing *t)
{
    GL *gl = p->gl;
    double inter_coeff = 0.0;
    int64_t prev_pts = p->surfaces[fbosurface_next(p)].pts;

    // Make sure all surfaces are actually valid, and redraw them manually
    // if this is not the case
    for (int i = 0; i < FBOSURFACES_MAX; i++) {
        if (!p->surfaces[i].valid) {
            struct pass frame = { .f = chain->f };
            gl_video_upscale_frame(p, &frame, &p->surfaces[i].fbotex);
            p->surfaces[i].valid = true;
        }
    }

    if (t && prev_pts < t->pts) {
        MP_STATS(p, "new-pts");
        gl_video_upscale_frame(p, chain, &p->surfaces[p->surface_idx].fbotex);
        p->surfaces[p->surface_idx].valid = true;
        p->surfaces[p->surface_idx].pts = t->pts;
        p->surface_idx = fbosurface_next(p);
    } else {
        // re-use the previously rendered surface as source
        chain->f = p->surfaces[fbosurface_next(p)].fbotex;
    }

    // fbosurface 0 is bound by handle_pass
    gl->ActiveTexture(GL_TEXTURE0 + 1);
    gl->BindTexture(p->gl_target, p->surfaces[p->surface_idx].fbotex.texture);
    gl->ActiveTexture(GL_TEXTURE0);

    if (!t) {
        p->is_interpolated = false;
        return 0.0;
    }

    int64_t vsync_interval = t->next_vsync - t->prev_vsync;

    if (t->pts > t->next_vsync && t->pts < t->next_vsync + vsync_interval) {
        // current frame overlaps PTS boundary, blend
        double R = t->pts - t->next_vsync;
        float ts = p->opts.smoothmotion_threshold;
        inter_coeff = R / vsync_interval;
        inter_coeff = inter_coeff < 0.0 + ts ? 0.0 : inter_coeff;
        inter_coeff = inter_coeff > 1.0 - ts ? 1.0 : inter_coeff;
        MP_DBG(p, "inter frame ppts: %lld, pts: %lld, "
               "vsync: %lld, mix: %f\n",
               (long long)prev_pts, (long long)t->pts,
               (long long)t->next_vsync, inter_coeff);
        MP_STATS(p, "frame-mix");

        // the value is scaled to fit in the graph with the completely
        // unrelated "phase" value (which is stupid)
        MP_STATS(p, "value-timed %lld %f mix-value",
                 (long long)t->pts, inter_coeff * 10000);
    } else if (t->pts > t->next_vsync) {
        // there's a new frame, but we haven't displayed or blended it yet,
        // so we still draw the old frame
        inter_coeff = 1.0;
    }

    p->is_interpolated = inter_coeff > 0.0;
    return inter_coeff;
}

// (fbo==0 makes BindFramebuffer select the screen backbuffer)
void gl_video_render_frame(struct gl_video *p, int fbo, struct frame_timing *t)
{
    GL *gl = p->gl;
    struct video_image *vimg = &p->image;

    p->is_interpolated = false;

    gl->BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl->Viewport(p->vp_x, p->vp_y, p->vp_w, p->vp_h);

    if (p->opts.temporal_dither)
        change_dither_trafo(p);

    if (p->dst_rect.x0 > p->vp_x || p->dst_rect.y0 > p->vp_y
        || p->dst_rect.x1 < p->vp_x + p->vp_w
        || p->dst_rect.y1 < p->vp_y + p->vp_h)
    {
        gl->Clear(GL_COLOR_BUFFER_BIT);
    }

    if (!vimg->mpi) {
        gl->Clear(GL_COLOR_BUFFER_BIT);
        goto draw_osd;
    }

    GLuint imgtex[4] = {0};
    set_image_textures(p, vimg, imgtex);

    struct pass chain = {
        .f = {
            .vp_w = p->image_w,
            .vp_h = p->image_h,
            .tex_w = vimg->planes[0].tex_w,
            .tex_h = vimg->planes[0].tex_h,
            .texture = imgtex[0],
        },
    };

    double inter_coeff = 0.0;
    if (p->opts.smoothmotion) {
        inter_coeff = gl_video_interpolate_frame(p, &chain, t);
    } else {
        gl_video_upscale_frame(p, &chain, NULL);
    }

    struct fbotex screen = {
        .vp_x = p->vp_x,
        .vp_y = p->vp_y,
        .vp_w = p->vp_w,
        .vp_h = p->vp_h,
        .fbo = fbo,
    };

    chain.use_dst = true;
    chain.dst = p->dst_rect;
    chain.flags = (p->image_params.rotate % 90 ? 0 : p->image_params.rotate / 90)
                | (vimg->image_flipped ? 4 : 0);

    gl->UseProgram(p->final_program);
    GLint loc = gl->GetUniformLocation(p->final_program, "inter_coeff");
    gl->Uniform1f(loc, inter_coeff);
    handle_pass(p, &chain, &screen, p->final_program);

    gl->UseProgram(0);

    unset_image_textures(p);

    p->frames_rendered++;

    debug_check_gl(p, "after video rendering");

draw_osd:
    mpgl_osd_draw(p->osd, p->osd_rect, p->osd_pts, p->image_params.stereo_out);

    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void update_window_sized_objects(struct gl_video *p)
{
    int w = p->dst_rect.x1 - p->dst_rect.x0;
    int h = p->dst_rect.y1 - p->dst_rect.y0;
    if ((p->image_params.rotate % 180) == 90)
        MPSWAP(int, w, h);

    // Round up to an arbitrary alignment to make window resizing or
    // panscan controls smoother (less texture reallocations).
    int width  = FFALIGN(w, 256);
    int height = FFALIGN(h, 256);

    if (p->scale_sep_program) {
        if (h > p->scale_sep_fbo.tex_h) {
            fbotex_uninit(&p->scale_sep_fbo);
            fbotex_init(&p->scale_sep_fbo, p->gl, p->log, p->image_w, height,
                        p->gl_target, GL_NEAREST, p->opts.fbo_format);
        }
        p->scale_sep_fbo.vp_w = p->image_w;
        p->scale_sep_fbo.vp_h = h;
    }

    if (p->opts.smoothmotion) {
        for (int i = 0; i < FBOSURFACES_MAX; i++) {
            struct fbotex *fbo = &p->surfaces[i].fbotex;
            if (w > fbo->tex_w || h > fbo->tex_h) {
                fbotex_uninit(fbo);
                fbotex_init(fbo, p->gl, p->log, width, height,
                            p->gl_target, GL_NEAREST, p->opts.fbo_format);
            }
            fbo->vp_w = w;
            fbo->vp_h = h;
            p->surfaces[i].valid = false;
        }
    }
}

static void check_resize(struct gl_video *p)
{
    bool need_scaler_reinit = false;    // filter size change needed
    bool need_scaler_update = false;    // filter LUT change needed
    bool too_small = false;
    for (int n = 0; n < 2; n++) {
        if (p->scalers[n].kernel) {
            struct filter_kernel old = *p->scalers[n].kernel;
            update_scale_factor(p, &p->scalers[n]);
            struct filter_kernel new = *p->scalers[n].kernel;
            need_scaler_reinit |= (new.size != old.size);
            need_scaler_update |= (new.inv_scale != old.inv_scale);
            too_small |= p->scalers[n].insufficient;
        }
    }
    for (int n = 0; n < 2; n++) {
        if (strcmp(p->scalers[n].name, expected_scaler(p, n)) != 0)
            need_scaler_reinit = true;
    }
    if (p->upscaling != (get_scale_factor(p) > 1.0)) {
        p->upscaling = !p->upscaling;
        // Switching between upscaling and downscaling also requires sigmoid
        // to be toggled
        need_scaler_reinit |= p->sigmoid_enabled;
    }
    if (need_scaler_reinit) {
        reinit_rendering(p);
    } else if (need_scaler_update) {
        init_scaler(p, &p->scalers[0]);
        init_scaler(p, &p->scalers[1]);
    }
    if (too_small) {
        MP_WARN(p, "Can't downscale that much, window "
                   "output may look suboptimal.\n");
    }

    update_window_sized_objects(p);
    update_all_uniforms(p);
}

void gl_video_resize(struct gl_video *p, struct mp_rect *window,
                     struct mp_rect *src, struct mp_rect *dst,
                     struct mp_osd_res *osd, bool vflip)
{
    p->src_rect = *src;
    p->dst_rect = *dst;
    p->osd_rect = *osd;

    p->vp_x = window->x0;
    p->vp_y = window->y0;
    p->vp_w = window->x1 - window->x0;
    p->vp_h = window->y1 - window->y0;

    p->vp_vflipped = vflip;

    check_resize(p);
}

static bool get_image(struct gl_video *p, struct mp_image *mpi)
{
    GL *gl = p->gl;

    if (!p->opts.pbo)
        return false;

    struct video_image *vimg = &p->image;

    // See comments in init_video() about odd video sizes.
    // The normal upload path does this too, but less explicit.
    mp_image_set_size(mpi, vimg->planes[0].w, vimg->planes[0].h);

    for (int n = 0; n < p->plane_count; n++) {
        struct texplane *plane = &vimg->planes[n];
        mpi->stride[n] = mpi->plane_w[n] * p->image_desc.bytes[n];
        int needed_size = mpi->plane_h[n] * mpi->stride[n];
        if (!plane->gl_buffer)
            gl->GenBuffers(1, &plane->gl_buffer);
        gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, plane->gl_buffer);
        if (needed_size > plane->buffer_size) {
            plane->buffer_size = needed_size;
            gl->BufferData(GL_PIXEL_UNPACK_BUFFER, plane->buffer_size,
                           NULL, GL_DYNAMIC_DRAW);
        }
        if (!plane->buffer_ptr)
            plane->buffer_ptr = gl->MapBuffer(GL_PIXEL_UNPACK_BUFFER,
                                              GL_WRITE_ONLY);
        mpi->planes[n] = plane->buffer_ptr;
        gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    return true;
}

void gl_video_upload_image(struct gl_video *p, struct mp_image *mpi)
{
    GL *gl = p->gl;

    struct video_image *vimg = &p->image;

    p->osd_pts = mpi->pts;

    talloc_free(vimg->mpi);
    vimg->mpi = mpi;

    if (p->hwdec_active)
        return;

    assert(mpi->num_planes == p->plane_count);

    mp_image_t mpi2 = *mpi;
    bool pbo = false;
    if (!vimg->planes[0].buffer_ptr && get_image(p, &mpi2)) {
        for (int n = 0; n < p->plane_count; n++) {
            int line_bytes = mpi->plane_w[n] * p->image_desc.bytes[n];
            memcpy_pic(mpi2.planes[n], mpi->planes[n], line_bytes, mpi->plane_h[n],
                       mpi2.stride[n], mpi->stride[n]);
        }
        pbo = true;
    }
    vimg->image_flipped = mpi2.stride[0] < 0;
    for (int n = 0; n < p->plane_count; n++) {
        struct texplane *plane = &vimg->planes[n];
        void *plane_ptr = mpi2.planes[n];
        if (pbo) {
            gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, plane->gl_buffer);
            if (!gl->UnmapBuffer(GL_PIXEL_UNPACK_BUFFER))
                MP_FATAL(p, "Video PBO upload failed. "
                         "Remove the 'pbo' suboption.\n");
            plane->buffer_ptr = NULL;
            plane_ptr = NULL; // PBO offset 0
        }
        gl->ActiveTexture(GL_TEXTURE0 + n);
        gl->BindTexture(p->gl_target, plane->gl_texture);
        glUploadTex(gl, p->gl_target, plane->gl_format, plane->gl_type,
                    plane_ptr, mpi2.stride[n], 0, 0, plane->w, plane->h, 0);
    }
    gl->ActiveTexture(GL_TEXTURE0);
    if (pbo)
        gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

static bool test_fbo(struct gl_video *p, bool *success)
{
    if (!*success)
        return false;

    GL *gl = p->gl;
    *success = false;
    MP_VERBOSE(p, "Testing user-set FBO format (0x%x)\n",
                   (unsigned)p->opts.fbo_format);
    struct fbotex fbo = {0};
    if (fbotex_init(&fbo, p->gl, p->log, 16, 16, p->gl_target, GL_LINEAR,
                    p->opts.fbo_format))
    {
        gl->BindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
        gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
        *success = true;
    }
    fbotex_uninit(&fbo);
    glCheckError(gl, p->log, "FBO test");
    return *success;
}

// Disable features that are not supported with the current OpenGL version.
static void check_gl_features(struct gl_video *p)
{
    GL *gl = p->gl;
    bool have_float_tex = gl->mpgl_caps & MPGL_CAP_FLOAT_TEX;
    bool have_fbo = gl->mpgl_caps & MPGL_CAP_FB;
    bool have_arrays = gl->mpgl_caps & MPGL_CAP_1ST_CLASS_ARRAYS;
    bool have_1d_tex = gl->mpgl_caps & MPGL_CAP_1D_TEX;
    bool have_3d_tex = gl->mpgl_caps & MPGL_CAP_3D_TEX;
    bool have_mix = gl->glsl_version >= 130;

    char *disabled[10];
    int n_disabled = 0;

    // Normally, we want to disable them by default if FBOs are unavailable,
    // because they will be slow (not critically slow, but still slower).
    // Without FP textures, we must always disable them.
    // I don't know if luminance alpha float textures exist, so disregard them.
    for (int n = 0; n < 2; n++) {
        const struct filter_kernel *kernel = mp_find_filter_kernel(p->opts.scalers[n]);
        if (kernel) {
            char *reason = NULL;
            if (!test_fbo(p, &have_fbo))
                reason = "scaler (FBO)";
            if (!have_float_tex)
                reason = "scaler (float tex.)";
            if (!have_arrays)
                reason = "scaler (no GLSL support)";
            if (!have_1d_tex && kernel->polar)
                reason = "scaler (1D tex.)";
            if (reason) {
                p->opts.scalers[n] = "bilinear";
                disabled[n_disabled++] = reason;
            }
        }
    }

    // GLES3 doesn't provide filtered 16 bit integer textures
    // GLES2 doesn't even provide 3D textures
    if (p->use_lut_3d && !(have_3d_tex && have_float_tex)) {
        p->use_lut_3d = false;
        disabled[n_disabled++] = "color management (GLES unsupported)";
    }

    // Missing float textures etc. (maybe ordered would actually work)
    if (p->opts.dither_algo >= 0 && gl->es) {
        p->opts.dither_algo = -1;
        disabled[n_disabled++] = "dithering (GLES unsupported)";
    }

    int use_cms = p->opts.srgb || p->use_lut_3d;

    // srgb_compand() not available
    if (!have_mix && p->opts.srgb) {
        p->opts.srgb = false;
        disabled[n_disabled++] = "sRGB output (GLSL version)";
    }
    if (use_cms && !test_fbo(p, &have_fbo)) {
        p->opts.srgb = false;
        p->use_lut_3d = false;
        disabled[n_disabled++] = "color management (FBO)";
    }
    if (p->opts.smoothmotion && !test_fbo(p, &have_fbo)) {
        p->opts.smoothmotion = false;
        disabled[n_disabled++] = "smoothmotion (FBO)";
    }
    // because of bt709_expand()
    if (!have_mix && p->use_lut_3d) {
        p->use_lut_3d = false;
        disabled[n_disabled++] = "color management (GLSL version)";
    }
    if (gl->es && p->opts.pbo) {
        p->opts.pbo = 0;
        disabled[n_disabled++] = "PBOs (GLES unsupported)";
    }

    if (n_disabled) {
        MP_ERR(p, "Some OpenGL extensions not detected, disabling: ");
        for (int n = 0; n < n_disabled; n++) {
            if (n)
                MP_ERR(p, ", ");
            MP_ERR(p, "%s", disabled[n]);
        }
        MP_ERR(p, ".\n");
    }
}

static int init_gl(struct gl_video *p)
{
    GL *gl = p->gl;

    debug_check_gl(p, "before init_gl");

    check_gl_features(p);

    gl->Disable(GL_DITHER);

    gl_vao_init(&p->vao, gl, sizeof(struct vertex), vertex_vao);

    gl_video_set_gl_state(p);

    // Test whether we can use 10 bit. Hope that testing a single format/channel
    // is good enough (instead of testing all 1-4 channels variants etc.).
    const struct fmt_entry *fmt = find_tex_format(gl, 2, 1);
    if (gl->GetTexLevelParameteriv && fmt->format) {
        GLuint tex;
        gl->GenTextures(1, &tex);
        gl->BindTexture(GL_TEXTURE_2D, tex);
        gl->TexImage2D(GL_TEXTURE_2D, 0, fmt->internal_format, 64, 64, 0,
                       fmt->format, fmt->type, NULL);
        GLenum pname = 0;
        switch (fmt->format) {
        case GL_RED:        pname = GL_TEXTURE_RED_SIZE; break;
        case GL_LUMINANCE:  pname = GL_TEXTURE_LUMINANCE_SIZE; break;
        }
        GLint param = 0;
        if (pname)
            gl->GetTexLevelParameteriv(GL_TEXTURE_2D, 0, pname, &param);
        if (param) {
            MP_VERBOSE(p, "16 bit texture depth: %d.\n", (int)param);
            p->texture_16bit_depth = param;
        }
        gl->DeleteTextures(1, &tex);
    }

    debug_check_gl(p, "after init_gl");

    return 1;
}

void gl_video_uninit(struct gl_video *p)
{
    if (!p)
        return;

    GL *gl = p->gl;

    uninit_video(p);

    gl_vao_uninit(&p->vao);

    gl->DeleteTextures(1, &p->lut_3d_texture);

    mpgl_osd_destroy(p->osd);

    gl_set_debug_logger(gl, NULL);

    talloc_free(p);
}

void gl_video_set_gl_state(struct gl_video *p)
{
    GL *gl = p->gl;

    struct m_color c = p->opts.background;
    gl->ClearColor(c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0);
    gl->ActiveTexture(GL_TEXTURE0);
    if (gl->mpgl_caps & MPGL_CAP_ROW_LENGTH)
        gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void gl_video_unset_gl_state(struct gl_video *p)
{
    /* nop */
}

void gl_video_reset(struct gl_video *p)
{
    for (int i = 0; i < FBOSURFACES_MAX; i++)
        p->surfaces[i].pts = 0;
    p->surface_idx = 0;
}

bool gl_video_showing_interpolated_frame(struct gl_video *p)
{
    return p->is_interpolated;
}

// dest = src.<w> (always using 4 components)
static void packed_fmt_swizzle(char w[5], const struct fmt_entry *texfmt,
                               const struct packed_fmt_entry *fmt)
{
    const char *comp = "rgba";

    // Normally, we work with GL_RG
    if (texfmt && texfmt->internal_format == GL_LUMINANCE_ALPHA)
        comp = "ragb";

    for (int c = 0; c < 4; c++)
        w[c] = comp[MPMAX(fmt->components[c] - 1, 0)];
    w[4] = '\0';
}

static bool init_format(int fmt, struct gl_video *init)
{
    struct GL *gl = init->gl;

    init->hwdec_active = false;
    if (init->hwdec && init->hwdec->driver->imgfmt == fmt) {
        fmt = init->hwdec->converted_imgfmt;
        init->hwdec_active = true;
    }

    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(fmt);
    if (!desc.id)
        return false;

    if (desc.num_planes > 4)
        return false;

    const struct fmt_entry *plane_format[4] = {0};

    init->color_swizzle[0] = '\0';
    init->has_alpha = false;

    // YUV/planar formats
    if (desc.flags & MP_IMGFLAG_YUV_P) {
        int bits = desc.component_bits;
        if ((desc.flags & MP_IMGFLAG_NE) && bits >= 8 && bits <= 16) {
            init->has_alpha = desc.num_planes > 3;
            plane_format[0] = find_tex_format(gl, (bits + 7) / 8, 1);
            for (int p = 1; p < desc.num_planes; p++)
                plane_format[p] = plane_format[0];
            goto supported;
        }
    }

    // YUV/half-packed
    if (fmt == IMGFMT_NV12 || fmt == IMGFMT_NV21) {
        if (!(init->gl->mpgl_caps & MPGL_CAP_TEX_RG))
            return false;
        plane_format[0] = find_tex_format(gl, 1, 1);
        plane_format[1] = find_tex_format(gl, 1, 2);
        if (fmt == IMGFMT_NV21)
            snprintf(init->color_swizzle, sizeof(init->color_swizzle), "rbga");
        goto supported;
    }

    // RGB/planar
    if (fmt == IMGFMT_GBRP) {
        snprintf(init->color_swizzle, sizeof(init->color_swizzle), "brga");
        plane_format[0] = find_tex_format(gl, 1, 1);
        for (int p = 1; p < desc.num_planes; p++)
            plane_format[p] = plane_format[0];
        goto supported;
    }

    // XYZ (same organization as RGB packed, but requires conversion matrix)
    if (fmt == IMGFMT_XYZ12) {
        plane_format[0] = find_tex_format(gl, 2, 3);
        goto supported;
    }

    // Packed RGB special formats
    for (const struct fmt_entry *e = mp_to_gl_formats; e->mp_format; e++) {
        if (!gl->es && e->mp_format == fmt) {
            plane_format[0] = e;
            goto supported;
        }
    }

    // Packed RGB(A) formats
    for (const struct packed_fmt_entry *e = mp_packed_formats; e->fmt; e++) {
        if (e->fmt == fmt) {
            int n_comp = desc.bytes[0] / e->component_size;
            plane_format[0] = find_tex_format(gl, e->component_size, n_comp);
            packed_fmt_swizzle(init->color_swizzle, plane_format[0], e);
            init->has_alpha = e->components[3] != 0;
            goto supported;
        }
    }

    // Packed YUV Apple formats
    if (init->gl->mpgl_caps & MPGL_CAP_APPLE_RGB_422) {
        for (const struct fmt_entry *e = gl_apple_formats; e->mp_format; e++) {
            if (e->mp_format == fmt) {
                init->is_packed_yuv = true;
                snprintf(init->color_swizzle, sizeof(init->color_swizzle),
                         "gbra");
                plane_format[0] = e;
                goto supported;
            }
        }
    }

    // Unsupported format
    return false;

supported:

    // Stuff like IMGFMT_420AP10. Untested, most likely insane.
    if (desc.num_planes == 4 && (desc.component_bits % 8) != 0)
        return false;

    if (desc.component_bits > 8 && desc.component_bits < 16) {
        if (init->texture_16bit_depth < 16)
            return false;
    }

    for (int p = 0; p < desc.num_planes; p++) {
        if (!plane_format[p]->format)
            return false;
    }

    for (int p = 0; p < desc.num_planes; p++) {
        struct texplane *plane = &init->image.planes[p];
        const struct fmt_entry *format = plane_format[p];
        assert(format);
        plane->gl_format = format->format;
        plane->gl_internal_format = format->internal_format;
        plane->gl_type = format->type;
    }

    init->is_yuv = desc.flags & MP_IMGFLAG_YUV;
    init->is_rgb = desc.flags & MP_IMGFLAG_RGB;
    init->plane_count = desc.num_planes;
    init->image_desc = desc;

    return true;
}

bool gl_video_check_format(struct gl_video *p, int mp_format)
{
    struct gl_video tmp = *p;
    return init_format(mp_format, &tmp);
}

void gl_video_config(struct gl_video *p, struct mp_image_params *params)
{
    mp_image_unrefp(&p->image.mpi);

    if (!mp_image_params_equal(&p->real_image_params, params)) {
        uninit_video(p);
        p->real_image_params = *params;
        p->image_params = *params;
        if (params->imgfmt)
            init_video(p);
    }

    check_resize(p);
}

void gl_video_set_output_depth(struct gl_video *p, int r, int g, int b)
{
    MP_VERBOSE(p, "Display depth: R=%d, G=%d, B=%d\n", r, g, b);
    p->depth_g = g;
}

struct gl_video *gl_video_init(GL *gl, struct mp_log *log, struct osd_state *osd)
{
    if (gl->version < 210 && gl->es < 200) {
        mp_err(log, "At least OpenGL 2.1 or OpenGL ES 2.0 required.\n");
        return NULL;
    }

    struct gl_video *p = talloc_ptrtype(NULL, p);
    *p = (struct gl_video) {
        .gl = gl,
        .log = log,
        .osd_state = osd,
        .opts = gl_video_opts_def,
        .gl_target = GL_TEXTURE_2D,
        .texture_16bit_depth = 16,
        .user_gamma = 1.0f,
        .scalers = {
            { .index = 0, .name = "bilinear" },
            { .index = 1, .name = "bilinear" },
        },
        .scratch = talloc_zero_array(p, char *, 1),
    };
    gl_video_set_debug(p, true);
    init_gl(p);
    recreate_osd(p);
    return p;
}

// Get static string for scaler shader.
static const char *handle_scaler_opt(const char *name)
{
    if (name && name[0]) {
        const struct filter_kernel *kernel = mp_find_filter_kernel(name);
        if (kernel)
            return kernel->name;

        for (const char *const *filter = fixed_scale_filters; *filter; filter++) {
            if (strcmp(*filter, name) == 0)
                return *filter;
        }
    }
    return NULL;
}

// Set the options, and possibly update the filter chain too.
// Note: assumes all options are valid and verified by the option parser.
void gl_video_set_options(struct gl_video *p, struct gl_video_opts *opts)
{
    p->opts = *opts;
    for (int n = 0; n < 2; n++) {
        p->opts.scalers[n] = (char *)handle_scaler_opt(p->opts.scalers[n]);
        p->opts.dscaler = (char *)handle_scaler_opt(p->opts.dscaler);
    }

    check_gl_features(p);
    reinit_rendering(p);
    check_resize(p);
}

void gl_video_get_colorspace(struct gl_video *p, struct mp_image_params *params)
{
    *params = p->image_params; // supports everything
}

struct mp_csp_equalizer *gl_video_eq_ptr(struct gl_video *p)
{
    return &p->video_eq;
}

// Call when the mp_csp_equalizer returned by gl_video_eq_ptr() was changed.
void gl_video_eq_update(struct gl_video *p)
{
    update_settings(p);

    if (p->need_reinit_rendering) {
        reinit_rendering(p);
        check_resize(p);
    } else {
        update_all_uniforms(p);
    }
}

static int validate_scaler_opt(struct mp_log *log, const m_option_t *opt,
                               struct bstr name, struct bstr param)
{
    char s[20] = {0};
    int r = 1;
    if (bstr_equals0(param, "help")) {
        r = M_OPT_EXIT - 1;
    } else {
        snprintf(s, sizeof(s), "%.*s", BSTR_P(param));
        if (!handle_scaler_opt(s))
            r = M_OPT_INVALID;
    }
    if (r < 1) {
        mp_info(log, "Available scalers:\n");
        for (const char *const *filter = fixed_scale_filters; *filter; filter++)
            mp_info(log, "    %s\n", *filter);
        for (int n = 0; mp_filter_kernels[n].name; n++)
            mp_info(log, "    %s\n", mp_filter_kernels[n].name);
        if (s[0])
            mp_fatal(log, "No scaler named '%s' found!\n", s);
    }
    return r;
}

// Resize and redraw the contents of the window without further configuration.
// Intended to be used in situations where the frontend can't really be
// involved with reconfiguring the VO properly.
// gl_video_resize() should be called when user interaction is done.
void gl_video_resize_redraw(struct gl_video *p, int w, int h)
{
    p->vp_w = w;
    p->vp_h = h;
    gl_video_render_frame(p, 0, NULL);
}

void gl_video_set_hwdec(struct gl_video *p, struct gl_hwdec *hwdec)
{
    p->hwdec = hwdec;
    mp_image_unrefp(&p->image.mpi);
}
