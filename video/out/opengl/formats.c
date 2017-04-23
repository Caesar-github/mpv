#include "common/common.h"
#include "formats.h"

enum {
    // --- GL type aliases (for readability)
    T_U8        = GL_UNSIGNED_BYTE,
    T_U16       = GL_UNSIGNED_SHORT,
    T_FL        = GL_FLOAT,
};

// List of allowed formats, and their usability for bilinear filtering and FBOs.
// This is limited to combinations that are useful for our renderer.
static const struct gl_format gl_formats[] = {
    // These are used for desktop GL 3+, and GLES 3+ with GL_EXT_texture_norm16.
    {GL_R8,                  GL_RED,             T_U8,  F_CF | F_GL3 | F_GL2F | F_ES3},
    {GL_RG8,                 GL_RG,              T_U8,  F_CF | F_GL3 | F_GL2F | F_ES3},
    {GL_RGB8,                GL_RGB,             T_U8,  F_CF | F_GL3 | F_GL2F | F_ES3},
    {GL_RGBA8,               GL_RGBA,            T_U8,  F_CF | F_GL3 | F_GL2F | F_ES3},
    {GL_R16,                 GL_RED,             T_U16, F_CF | F_GL3 | F_GL2F | F_EXT16},
    {GL_RG16,                GL_RG,              T_U16, F_CF | F_GL3 | F_GL2F | F_EXT16},
    {GL_RGB16,               GL_RGB,             T_U16, F_CF | F_GL3 | F_GL2F},
    {GL_RGBA16,              GL_RGBA,            T_U16, F_CF | F_GL3 | F_GL2F | F_EXT16},

    // Specifically not color-renderable.
    {GL_RGB16,               GL_RGB,             T_U16, F_TF | F_EXT16},

    // GL2 legacy. Ignores possibly present FBO extensions (no CF flag set).
    {GL_LUMINANCE8,          GL_LUMINANCE,       T_U8,  F_TF | F_GL2},
    {GL_LUMINANCE8_ALPHA8,   GL_LUMINANCE_ALPHA, T_U8,  F_TF | F_GL2},
    {GL_RGB8,                GL_RGB,             T_U8,  F_TF | F_GL2},
    {GL_RGBA8,               GL_RGBA,            T_U8,  F_TF | F_GL2},
    {GL_LUMINANCE16,         GL_LUMINANCE,       T_U16, F_TF | F_GL2},
    {GL_LUMINANCE16_ALPHA16, GL_LUMINANCE_ALPHA, T_U16, F_TF | F_GL2},
    {GL_RGB16,               GL_RGB,             T_U16, F_TF | F_GL2},
    {GL_RGBA16,              GL_RGBA,            T_U16, F_TF | F_GL2},

    // ES2 legacy
    {GL_LUMINANCE,           GL_LUMINANCE,       T_U8,  F_TF | F_ES2},
    {GL_LUMINANCE_ALPHA,     GL_LUMINANCE_ALPHA, T_U8,  F_TF | F_ES2},
    {GL_RGB,                 GL_RGB,             T_U8,  F_TF | F_ES2},
    {GL_RGBA,                GL_RGBA,            T_U8,  F_TF | F_ES2},

    // Non-normalized integer formats.
    // Follows ES 3.0 as to which are color-renderable.
    {GL_R8UI,                GL_RED_INTEGER,     T_U8,  F_CR | F_GL3 | F_ES3},
    {GL_RG8UI,               GL_RG_INTEGER,      T_U8,  F_CR | F_GL3 | F_ES3},
    {GL_RGB8UI,              GL_RGB_INTEGER,     T_U8,         F_GL3 | F_ES3},
    {GL_RGBA8UI,             GL_RGBA_INTEGER,    T_U8,  F_CR | F_GL3 | F_ES3},
    {GL_R16UI,               GL_RED_INTEGER,     T_U16, F_CR | F_GL3 | F_ES3},
    {GL_RG16UI,              GL_RG_INTEGER,      T_U16, F_CR | F_GL3 | F_ES3},
    {GL_RGB16UI,             GL_RGB_INTEGER,     T_U16,        F_GL3 | F_ES3},
    {GL_RGBA16UI,            GL_RGBA_INTEGER,    T_U16, F_CR | F_GL3 | F_ES3},

    // On GL3+ or GL2.1 with GL_ARB_texture_float, floats work fully.
    {GL_R16F,                GL_RED,             T_FL,  F_F16 | F_CF | F_GL3 | F_GL2F},
    {GL_RG16F,               GL_RG,              T_FL,  F_F16 | F_CF | F_GL3 | F_GL2F},
    {GL_RGB16F,              GL_RGB,             T_FL,  F_F16 | F_CF | F_GL3 | F_GL2F},
    {GL_RGBA16F,             GL_RGBA,            T_FL,  F_F16 | F_CF | F_GL3 | F_GL2F},
    {GL_R32F,                GL_RED,             T_FL,          F_CF | F_GL3 | F_GL2F},
    {GL_RG32F,               GL_RG,              T_FL,          F_CF | F_GL3 | F_GL2F},
    {GL_RGB32F,              GL_RGB,             T_FL,          F_CF | F_GL3 | F_GL2F},
    {GL_RGBA32F,             GL_RGBA,            T_FL,          F_CF | F_GL3 | F_GL2F},

    // Note: we simply don't support float anything on ES2, despite extensions.
    // We also don't bother with non-filterable float formats, and we ignore
    // 32 bit float formats that are not blendable when rendering to them.

    // On ES3.2+, both 16 bit floats work fully (except 3-component formats).
    // F_EXTF16 implies extensions that also enable 16 bit floats fully.
    {GL_R16F,                GL_RED,             T_FL,  F_F16 | F_CF | F_ES32 | F_EXTF16},
    {GL_RG16F,               GL_RG,              T_FL,  F_F16 | F_CF | F_ES32 | F_EXTF16},
    {GL_RGB16F,              GL_RGB,             T_FL,  F_F16 | F_TF | F_ES32 | F_EXTF16},
    {GL_RGBA16F,             GL_RGBA,            T_FL,  F_F16 | F_CF | F_ES32 | F_EXTF16},

    // On ES3.0+, 16 bit floats are texture-filterable.
    // Don't bother with 32 bit floats; they exist but are neither CR nor TF.
    {GL_R16F,                GL_RED,             T_FL,  F_F16 | F_TF | F_ES3},
    {GL_RG16F,               GL_RG,              T_FL,  F_F16 | F_TF | F_ES3},
    {GL_RGB16F,              GL_RGB,             T_FL,  F_F16 | F_TF | F_ES3},
    {GL_RGBA16F,             GL_RGBA,            T_FL,  F_F16 | F_TF | F_ES3},

    // These might be useful as FBO formats.
    {GL_RGB10_A2,            GL_RGBA,
     GL_UNSIGNED_INT_2_10_10_10_REV,                    F_CF | F_GL3 | F_ES3},
    {GL_RGBA12,              GL_RGBA,            T_U16, F_CF | F_GL2 | F_GL3},
    {GL_RGB10,               GL_RGB,             T_U16, F_CF | F_GL2 | F_GL3},

    // Special formats.
    {GL_RGB8,                GL_RGB,
     GL_UNSIGNED_SHORT_5_6_5,                           F_TF | F_GL2 | F_GL3},
    {GL_RGB,                 GL_RGB_422_APPLE,
     GL_UNSIGNED_SHORT_8_8_APPLE,                       F_TF | F_APPL},
    {GL_RGB,                 GL_RGB_422_APPLE,
     GL_UNSIGNED_SHORT_8_8_REV_APPLE,                   F_TF | F_APPL},

    {0}
};

// Pairs of mpv formats and OpenGL types that match directly. Code using this
// is supposed to look through the gl_formats table, and there is supposed to
// be exactly 1 matching entry (which tells you format/internal format).
static const int special_formats[][2] = {
    {IMGFMT_RGB565,     GL_UNSIGNED_SHORT_5_6_5},
    {IMGFMT_UYVY,       GL_UNSIGNED_SHORT_8_8_APPLE},
    {IMGFMT_YUYV,       GL_UNSIGNED_SHORT_8_8_REV_APPLE},
    {0}
};

struct packed_fmt_entry {
    int fmt;
    int8_t component_size;
    int8_t components[4]; // source component - 0 means unmapped
};

// Regular packed formats, which can be mapped to GL formats by finding a
// texture format with same component count/size, and swizzling the result.
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

// Return an or-ed combination of all F_ flags that apply.
int gl_format_feature_flags(GL *gl)
{
    return (gl->version == 210 ? F_GL2 : 0)
         | (gl->version >= 300 ? F_GL3 : 0)
         | (gl->es == 200 ? F_ES2 : 0)
         | (gl->es >= 300 ? F_ES3 : 0)
         | (gl->es >= 320 ? F_ES32 : 0)
         | (gl->mpgl_caps & MPGL_CAP_EXT16 ? F_EXT16 : 0)
         | ((gl->es >= 300 &&
            (gl->mpgl_caps & MPGL_CAP_EXT_CR_HFLOAT)) ? F_EXTF16 : 0)
         | ((gl->version == 210 &&
            (gl->mpgl_caps & MPGL_CAP_ARB_FLOAT) &&
            (gl->mpgl_caps & MPGL_CAP_TEX_RG) &&
            (gl->mpgl_caps & MPGL_CAP_FB)) ? F_GL2F : 0)
         | (gl->mpgl_caps & MPGL_CAP_APPLE_RGB_422 ? F_APPL : 0);
}

// Return the entry for the given internal format. Return NULL if unsupported.
const struct gl_format *gl_find_internal_format(GL *gl, GLint internal_format)
{
    int features = gl_format_feature_flags(gl);
    for (int n = 0; gl_formats[n].type; n++) {
        const struct gl_format *f = &gl_formats[n];
        if (f->internal_format == internal_format && (f->flags & features))
            return f;
    }
    return NULL;
}

const struct gl_format *gl_find_special_format(GL *gl, int mpfmt)
{
    int features = gl_format_feature_flags(gl);
    for (int n = 0; special_formats[n][0]; n++) {
        if (special_formats[n][0] == mpfmt) {
            GLenum type = special_formats[n][1];
            for (int i = 0; gl_formats[i].type; i++) {
                const struct gl_format *f = &gl_formats[i];
                if (f->type == type && (f->flags & features))
                    return f;
            }
            break;
        }
    }
    return NULL;
}

// type: one of MPGL_TYPE_*
// flags: bitset of F_*, all flags must be present
const struct gl_format *gl_find_format(GL *gl, int type, int flags,
                                       int bytes_per_component, int n_components)
{
    if (!bytes_per_component || !n_components || !type)
        return NULL;
    int features = gl_format_feature_flags(gl);
    for (int n = 0; gl_formats[n].type; n++) {
        const struct gl_format *f = &gl_formats[n];
        if ((f->flags & features) &&
            ((f->flags & flags) == flags) &&
            gl_format_type(f) == type &&
            gl_component_size(f->type) == bytes_per_component &&
            gl_format_components(f->format) == n_components)
            return f;
    }
    return NULL;
}

// Return a texture-filterable unsigned normalized fixed point format.
const struct gl_format *gl_find_unorm_format(GL *gl, int bytes_per_component,
                                             int n_components)
{
    return gl_find_format(gl, MPGL_TYPE_UNORM, F_TF, bytes_per_component,
                          n_components);
}

// Return an unsigned integer format.
const struct gl_format *gl_find_uint_format(GL *gl, int bytes_per_component,
                                            int n_components)
{
    return gl_find_format(gl, MPGL_TYPE_UINT, 0, bytes_per_component,
                          n_components);
}

// Return a 16 bit float format. Note that this will return a GL_FLOAT format
// with 32 bit per component; just the internal representation is smaller.
// Some GL versions will allow upload with GL_HALF_FLOAT as well.
const struct gl_format *gl_find_float16_format(GL *gl, int n_components)
{
    return gl_find_format(gl, MPGL_TYPE_FLOAT, F_F16, 4, n_components);
}

int gl_format_type(const struct gl_format *format)
{
    if (!format)
        return 0;
    if (format->type == GL_FLOAT)
        return MPGL_TYPE_FLOAT;
    if (gl_integer_format_to_base(format->format))
        return MPGL_TYPE_UINT;
    return MPGL_TYPE_UNORM;
}

// Return base internal format of an integer format, or 0 if it's not integer.
// "format" is like in struct gl_format.
GLenum gl_integer_format_to_base(GLenum format)
{
    switch (format) {
    case GL_RED_INTEGER:        return GL_RED;
    case GL_RG_INTEGER:         return GL_RG;
    case GL_RGB_INTEGER:        return GL_RGB;
    case GL_RGBA_INTEGER:       return GL_RGBA;
    }
    return 0;
}

// Return whether it's a non-normalized integer format.
// "format" is like in struct gl_format.
bool gl_is_integer_format(GLenum format)
{
    return !!gl_integer_format_to_base(format);
}

// Return the number of bytes per component this format implies.
// Returns 0 for formats with non-byte alignments and formats which
// merge multiple components (like GL_UNSIGNED_SHORT_5_6_5).
// "type" is like in struct gl_format.
int gl_component_size(GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:                      return 1;
    case GL_UNSIGNED_SHORT:                     return 2;
    case GL_FLOAT:                              return 4;
    }
    return 0;
}

// Return the number of separate color components.
// "format" is like in struct gl_format.
int gl_format_components(GLenum format)
{
    switch (format) {
    case GL_RED:
    case GL_RED_INTEGER:
    case GL_LUMINANCE:
        return 1;
    case GL_RG:
    case GL_RG_INTEGER:
    case GL_LUMINANCE_ALPHA:
        return 2;
    case GL_RGB:
    case GL_RGB_INTEGER:
        return 3;
    case GL_RGBA:
    case GL_RGBA_INTEGER:
        return 4;
    }
    return 0;
}

// Return the number of bytes per pixel for the given format.
// Parameter names like in struct gl_format.
int gl_bytes_per_pixel(GLenum format, GLenum type)
{
    // Formats with merged components are special.
    switch (type) {
    case GL_UNSIGNED_INT_2_10_10_10_REV:        return 4;
    case GL_UNSIGNED_SHORT_5_6_5:               return 2;
    case GL_UNSIGNED_SHORT_8_8_APPLE:           return 2;
    case GL_UNSIGNED_SHORT_8_8_REV_APPLE:       return 2;
    }

    return gl_component_size(type) * gl_format_components(format);
}

// The format has cleanly separated components (on byte boundaries).
bool gl_format_is_regular(const struct gl_format *fmt)
{
    int bpp = gl_component_size(fmt->type) * gl_format_components(fmt->format);
    return bpp == gl_bytes_per_pixel(fmt->format, fmt->type);
}

// dest = src.<w> (always using 4 components)
static void packed_fmt_swizzle(char w[5], const struct packed_fmt_entry *fmt)
{
    for (int c = 0; c < 4; c++)
        w[c] = "rgba"[MPMAX(fmt->components[c] - 1, 0)];
    w[4] = '\0';
}

// Like gl_find_unorm_format(), but takes bits (not bytes), and if no fixed
// point format is available, return an unsigned integer format.
static const struct gl_format *find_plane_format(GL *gl, int bits, int n_channels)
{
    int bytes = (bits + 7) / 8;
    const struct gl_format *f = gl_find_unorm_format(gl, bytes, n_channels);
    if (f)
        return f;
    return gl_find_uint_format(gl, bytes, n_channels);
}

// Put a mapping of imgfmt to OpenGL textures into *out. Basically it selects
// the correct texture formats needed to represent an imgfmt in OpenGL, with
// textures using the same memory organization as on the CPU.
// Each plane is represented by a texture, and each texture has a RGBA
// component order. out->color_swizzle is set to permute the components back.
// May return integer formats for >8 bit formats, if the driver has no
// normalized 16 bit formats.
// Returns false (and *out is set to all-0) if no format found.
bool gl_get_imgfmt_desc(GL *gl, int imgfmt, struct gl_imgfmt_desc *out)
{
    *out = (struct gl_imgfmt_desc){0};

    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(imgfmt);
    if (!desc.id)
        return false;

    if (desc.num_planes > 4 || (desc.flags & MP_IMGFLAG_HWACCEL))
        return false;

    const struct gl_format *planes[4] = {0};
    char swizzle_tmp[5] = {0};
    char *swizzle = "rgba";

    // YUV/planar formats
    if (desc.flags & (MP_IMGFLAG_YUV_P | MP_IMGFLAG_RGB_P)) {
        int bits = desc.component_bits;
        if ((desc.flags & MP_IMGFLAG_NE) && bits >= 8 && bits <= 16) {
            planes[0] = find_plane_format(gl, bits, 1);
            for (int n = 1; n < desc.num_planes; n++)
                planes[n] = planes[0];
            // RGB/planar
            if (desc.flags & MP_IMGFLAG_RGB_P)
                swizzle = "brga";
            goto supported;
        }
    }

    // YUV/half-packed
    if (desc.flags & MP_IMGFLAG_YUV_NV) {
        int bits = desc.component_bits;
        if ((desc.flags & MP_IMGFLAG_NE) && bits >= 8 && bits <= 16) {
            planes[0] = find_plane_format(gl, bits, 1);
            planes[1] = find_plane_format(gl, bits, 2);
            if (desc.flags & MP_IMGFLAG_YUV_NV_SWAP)
                swizzle = "rbga";
            goto supported;
        }
    }

    // XYZ (same organization as RGB packed, but requires conversion matrix)
    if (imgfmt == IMGFMT_XYZ12) {
        planes[0] = gl_find_unorm_format(gl, 2, 3);
        goto supported;
    }

    // Packed RGB(A) formats
    for (const struct packed_fmt_entry *e = mp_packed_formats; e->fmt; e++) {
        if (e->fmt == imgfmt) {
            int n_comp = desc.bytes[0] / e->component_size;
            planes[0] = gl_find_unorm_format(gl, e->component_size, n_comp);
            swizzle = swizzle_tmp;
            packed_fmt_swizzle(swizzle, e);
            goto supported;
        }
    }

    // Special formats for which OpenGL happens to have direct support.
    planes[0] = gl_find_special_format(gl, imgfmt);
    if (planes[0]) {
        // Packed YUV Apple formats color permutation
        if (planes[0]->format == GL_RGB_422_APPLE)
            swizzle = "gbra";
        goto supported;
    }

    // Unsupported format
    return false;

supported:

    snprintf(out->swizzle, sizeof(out->swizzle), "%s", swizzle);
    out->num_planes = desc.num_planes;
    for (int n = 0; n < desc.num_planes; n++) {
        if (!planes[n])
            return false;
        out->xs[n] = desc.xs[n];
        out->ys[n] = desc.ys[n];
        out->planes[n] = planes[n];
    }
    return true;
}
