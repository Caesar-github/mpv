/*
 * This file is part of mpv.
 * Parts based on MPlayer code by Reimar Döffinger.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include "common/common.h"
#include "utils.h"

// GLU has this as gluErrorString (we don't use GLU, as it is legacy-OpenGL)
static const char *gl_error_to_string(GLenum error)
{
    switch (error) {
    case GL_INVALID_ENUM: return "INVALID_ENUM";
    case GL_INVALID_VALUE: return "INVALID_VALUE";
    case GL_INVALID_OPERATION: return "INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    default: return "unknown";
    }
}

void glCheckError(GL *gl, struct mp_log *log, const char *info)
{
    for (;;) {
        GLenum error = gl->GetError();
        if (error == GL_NO_ERROR)
            break;
        mp_msg(log, MSGL_ERR, "%s: OpenGL error %s.\n", info,
               gl_error_to_string(error));
    }
}

// return the number of bytes per pixel for the given format
// does not handle all possible variants, just those used by mpv
int glFmt2bpp(GLenum format, GLenum type)
{
    int component_size = 0;
    switch (type) {
    case GL_UNSIGNED_BYTE_3_3_2:
    case GL_UNSIGNED_BYTE_2_3_3_REV:
        return 1;
    case GL_UNSIGNED_SHORT_5_5_5_1:
    case GL_UNSIGNED_SHORT_1_5_5_5_REV:
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_5_6_5_REV:
        return 2;
    case GL_UNSIGNED_BYTE:
        component_size = 1;
        break;
    case GL_UNSIGNED_SHORT:
        component_size = 2;
        break;
    }
    switch (format) {
    case GL_LUMINANCE:
    case GL_ALPHA:
        return component_size;
    case GL_RGB_422_APPLE:
        return 2;
    case GL_RGB:
    case GL_BGR:
    case GL_RGB_INTEGER:
        return 3 * component_size;
    case GL_RGBA:
    case GL_BGRA:
    case GL_RGBA_INTEGER:
        return 4 * component_size;
    case GL_RED:
    case GL_RED_INTEGER:
        return component_size;
    case GL_RG:
    case GL_LUMINANCE_ALPHA:
    case GL_RG_INTEGER:
        return 2 * component_size;
    }
    abort(); // unknown
}

static int get_alignment(int stride)
{
    if (stride % 8 == 0)
        return 8;
    if (stride % 4 == 0)
        return 4;
    if (stride % 2 == 0)
        return 2;
    return 1;
}

// upload a texture, handling things like stride and slices
//  target: texture target, usually GL_TEXTURE_2D
//  format, type: texture parameters
//  dataptr, stride: image data
//  x, y, width, height: part of the image to upload
//  slice: height of an upload slice, 0 for all at once
void glUploadTex(GL *gl, GLenum target, GLenum format, GLenum type,
                 const void *dataptr, int stride,
                 int x, int y, int w, int h, int slice)
{
    const uint8_t *data = dataptr;
    int y_max = y + h;
    if (w <= 0 || h <= 0)
        return;
    if (slice <= 0)
        slice = h;
    if (stride < 0) {
        data += (h - 1) * stride;
        stride = -stride;
    }
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, get_alignment(stride));
    bool use_rowlength = slice > 1 && (gl->mpgl_caps & MPGL_CAP_ROW_LENGTH);
    if (use_rowlength) {
        // this is not always correct, but should work for MPlayer
        gl->PixelStorei(GL_UNPACK_ROW_LENGTH, stride / glFmt2bpp(format, type));
    } else {
        if (stride != glFmt2bpp(format, type) * w)
            slice = 1; // very inefficient, but at least it works
    }
    for (; y + slice <= y_max; y += slice) {
        gl->TexSubImage2D(target, 0, x, y, w, slice, format, type, data);
        data += stride * slice;
    }
    if (y < y_max)
        gl->TexSubImage2D(target, 0, x, y, w, y_max - y, format, type, data);
    if (use_rowlength)
        gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

// Like glUploadTex, but upload a byte array with all elements set to val.
// If scratch is not NULL, points to a resizeable talloc memory block than can
// be freely used by the function (for avoiding temporary memory allocations).
void glClearTex(GL *gl, GLenum target, GLenum format, GLenum type,
                int x, int y, int w, int h, uint8_t val, void **scratch)
{
    int bpp = glFmt2bpp(format, type);
    int stride = w * bpp;
    int size = h * stride;
    if (size < 1)
        return;
    void *data = scratch ? *scratch : NULL;
    if (talloc_get_size(data) < size)
        data = talloc_realloc(NULL, data, char *, size);
    memset(data, val, size);
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, get_alignment(stride));
    gl->TexSubImage2D(target, 0, x, y, w, h, format, type, data);
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (scratch) {
        *scratch = data;
    } else {
        talloc_free(data);
    }
}

mp_image_t *glGetWindowScreenshot(GL *gl)
{
    if (gl->es)
        return NULL; // ES can't read from front buffer
    GLint vp[4]; //x, y, w, h
    gl->GetIntegerv(GL_VIEWPORT, vp);
    mp_image_t *image = mp_image_alloc(IMGFMT_RGB24, vp[2], vp[3]);
    if (!image)
        return NULL;
    gl->PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->ReadBuffer(GL_FRONT);
    //flip image while reading (and also avoid stride-related trouble)
    for (int y = 0; y < vp[3]; y++) {
        gl->ReadPixels(vp[0], vp[1] + vp[3] - y - 1, vp[2], 1,
                       GL_RGB, GL_UNSIGNED_BYTE,
                       image->planes[0] + y * image->stride[0]);
    }
    gl->PixelStorei(GL_PACK_ALIGNMENT, 4);
    return image;
}

void mp_log_source(struct mp_log *log, int lev, const char *src)
{
    int line = 1;
    if (!src)
        return;
    while (*src) {
        const char *end = strchr(src, '\n');
        const char *next = end + 1;
        if (!end)
            next = end = src + strlen(src);
        mp_msg(log, lev, "[%3d] %.*s\n", line, (int)(end - src), src);
        line++;
        src = next;
    }
}

static void gl_vao_enable_attribs(struct gl_vao *vao)
{
    GL *gl = vao->gl;

    for (int n = 0; vao->entries[n].name; n++) {
        const struct gl_vao_entry *e = &vao->entries[n];

        gl->EnableVertexAttribArray(n);
        gl->VertexAttribPointer(n, e->num_elems, e->type, e->normalized,
                                vao->stride, (void *)(intptr_t)e->offset);
    }
}

void gl_vao_init(struct gl_vao *vao, GL *gl, int stride,
                 const struct gl_vao_entry *entries)
{
    assert(!vao->vao);
    assert(!vao->buffer);

    *vao = (struct gl_vao){
        .gl = gl,
        .stride = stride,
        .entries = entries,
    };

    gl->GenBuffers(1, &vao->buffer);

    if (gl->BindVertexArray) {
        gl->BindBuffer(GL_ARRAY_BUFFER, vao->buffer);

        gl->GenVertexArrays(1, &vao->vao);
        gl->BindVertexArray(vao->vao);
        gl_vao_enable_attribs(vao);
        gl->BindVertexArray(0);

        gl->BindBuffer(GL_ARRAY_BUFFER, 0);
    }
}

void gl_vao_uninit(struct gl_vao *vao)
{
    GL *gl = vao->gl;
    if (!gl)
        return;

    if (gl->DeleteVertexArrays)
        gl->DeleteVertexArrays(1, &vao->vao);
    gl->DeleteBuffers(1, &vao->buffer);

    *vao = (struct gl_vao){0};
}

void gl_vao_bind(struct gl_vao *vao)
{
    GL *gl = vao->gl;

    if (gl->BindVertexArray) {
        gl->BindVertexArray(vao->vao);
    } else {
        gl->BindBuffer(GL_ARRAY_BUFFER, vao->buffer);
        gl_vao_enable_attribs(vao);
        gl->BindBuffer(GL_ARRAY_BUFFER, 0);
    }
}

void gl_vao_unbind(struct gl_vao *vao)
{
    GL *gl = vao->gl;

    if (gl->BindVertexArray) {
        gl->BindVertexArray(0);
    } else {
        for (int n = 0; vao->entries[n].name; n++)
            gl->DisableVertexAttribArray(n);
    }
}

// Draw the vertex data (as described by the gl_vao_entry entries) in ptr
// to the screen. num is the number of vertexes. prim is usually GL_TRIANGLES.
// If ptr is NULL, then skip the upload, and use the data uploaded with the
// previous call.
void gl_vao_draw_data(struct gl_vao *vao, GLenum prim, void *ptr, size_t num)
{
    GL *gl = vao->gl;

    if (ptr) {
        gl->BindBuffer(GL_ARRAY_BUFFER, vao->buffer);
        gl->BufferData(GL_ARRAY_BUFFER, num * vao->stride, ptr, GL_DYNAMIC_DRAW);
        gl->BindBuffer(GL_ARRAY_BUFFER, 0);
    }

    gl_vao_bind(vao);

    gl->DrawArrays(prim, 0, num);

    gl_vao_unbind(vao);
}

struct gl_format {
    GLenum format;
    GLenum type;
    GLint internal_format;
};

static const struct gl_format gl_formats[] = {
    // GLES 3.0
    {GL_RGB,    GL_UNSIGNED_BYTE,               GL_RGB},
    {GL_RGBA,   GL_UNSIGNED_BYTE,               GL_RGBA},
    {GL_RGB,    GL_UNSIGNED_BYTE,               GL_RGB8},
    {GL_RGBA,   GL_UNSIGNED_BYTE,               GL_RGBA8},
    {GL_RGB,    GL_UNSIGNED_SHORT,              GL_RGB16},
    {GL_RGBA,   GL_UNSIGNED_INT_2_10_10_10_REV, GL_RGB10_A2},
    // not texture filterable in GLES 3.0
    {GL_RGB,    GL_FLOAT,                       GL_RGB16F},
    {GL_RGBA,   GL_FLOAT,                       GL_RGBA16F},
    {GL_RGB,    GL_FLOAT,                       GL_RGB32F},
    {GL_RGBA,   GL_FLOAT,                       GL_RGBA32F},
    // Desktop GL
    {GL_RGB,    GL_UNSIGNED_SHORT,              GL_RGB10},
    {GL_RGBA,   GL_UNSIGNED_SHORT,              GL_RGBA12},
    {GL_RGBA,   GL_UNSIGNED_SHORT,              GL_RGBA16},
    {0}
};

// Create a texture and a FBO using the texture as color attachments.
//  iformat: texture internal format
// Returns success.
bool fbotex_init(struct fbotex *fbo, GL *gl, struct mp_log *log, int w, int h,
                 GLenum iformat)
{
    assert(!fbo->fbo);
    assert(!fbo->texture);
    return fbotex_change(fbo, gl, log, w, h, iformat, 0);
}

// Like fbotex_init(), except it can be called on an already initialized FBO;
// and if the parameters are the same as the previous call, do not touch it.
// flags can be 0, or a combination of FBOTEX_FUZZY_W and FBOTEX_FUZZY_H.
// Enabling FUZZY for W or H means the w or h does not need to be exact.
bool fbotex_change(struct fbotex *fbo, GL *gl, struct mp_log *log, int w, int h,
                   GLenum iformat, int flags)
{
    bool res = true;

    int cw = w, ch = h;

    if ((flags & FBOTEX_FUZZY_W) && cw < fbo->rw)
        cw = fbo->rw;
    if ((flags & FBOTEX_FUZZY_H) && ch < fbo->rh)
        ch = fbo->rh;

    if (fbo->rw == cw && fbo->rh == ch && fbo->iformat == iformat) {
        fbo->lw = w;
        fbo->lh = h;
        return true;
    }

    int lw = w, lh = h;

    if (flags & FBOTEX_FUZZY_W)
        w = MP_ALIGN_UP(w, 256);
    if (flags & FBOTEX_FUZZY_H)
        h = MP_ALIGN_UP(h, 256);

    GLenum filter = fbo->tex_filter;

    struct gl_format format = {
        .format = GL_RGBA,
        .type = GL_UNSIGNED_BYTE,
        .internal_format = iformat,
    };
    for (int n = 0; gl_formats[n].format; n++) {
        if (gl_formats[n].internal_format == format.internal_format) {
            format = gl_formats[n];
            break;
        }
    }

    *fbo = (struct fbotex) {
        .gl = gl,
        .rw = w,
        .rh = h,
        .lw = lw,
        .lh = lh,
        .iformat = iformat,
    };

    mp_verbose(log, "Create FBO: %dx%d -> %dx%d\n", fbo->lw, fbo->lh,
                                                    fbo->rw, fbo->rh);

    if (!(gl->mpgl_caps & MPGL_CAP_FB))
        return false;

    gl->GenFramebuffers(1, &fbo->fbo);
    gl->GenTextures(1, &fbo->texture);
    gl->BindTexture(GL_TEXTURE_2D, fbo->texture);
    gl->TexImage2D(GL_TEXTURE_2D, 0, format.internal_format, fbo->rw, fbo->rh, 0,
                   format.format, format.type, NULL);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_2D, 0);

    fbotex_set_filter(fbo, filter ? filter : GL_LINEAR);

    glCheckError(gl, log, "after creating framebuffer texture");

    gl->BindFramebuffer(GL_FRAMEBUFFER, fbo->fbo);
    gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, fbo->texture, 0);

    GLenum err = gl->CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (err != GL_FRAMEBUFFER_COMPLETE) {
        mp_err(log, "Error: framebuffer completeness check failed (error=%d).\n",
               (int)err);
        res = false;
    }

    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);

    glCheckError(gl, log, "after creating framebuffer");

    return res;
}

void fbotex_set_filter(struct fbotex *fbo, GLenum tex_filter)
{
    GL *gl = fbo->gl;

    if (fbo->tex_filter != tex_filter && fbo->texture) {
        gl->BindTexture(GL_TEXTURE_2D, fbo->texture);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, tex_filter);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, tex_filter);
        gl->BindTexture(GL_TEXTURE_2D, 0);
    }
    fbo->tex_filter = tex_filter;
}

void fbotex_uninit(struct fbotex *fbo)
{
    GL *gl = fbo->gl;

    if (gl && (gl->mpgl_caps & MPGL_CAP_FB)) {
        gl->DeleteFramebuffers(1, &fbo->fbo);
        gl->DeleteTextures(1, &fbo->texture);
        *fbo = (struct fbotex) {0};
    }
}

// Standard parallel 2D projection, except y1 < y0 means that the coordinate
// system is flipped, not the projection.
void gl_transform_ortho(struct gl_transform *t, float x0, float x1,
                        float y0, float y1)
{
    if (y1 < y0) {
        float tmp = y0;
        y0 = tmp - y1;
        y1 = tmp;
    }

    t->m[0][0] = 2.0f / (x1 - x0);
    t->m[0][1] = 0.0f;
    t->m[1][0] = 0.0f;
    t->m[1][1] = 2.0f / (y1 - y0);
    t->t[0] = -(x1 + x0) / (x1 - x0);
    t->t[1] = -(y1 + y0) / (y1 - y0);
}

// Apply the effects of one transformation to another, transforming it in the
// process. In other words: post-composes t onto x
void gl_transform_trans(struct gl_transform t, struct gl_transform *x)
{
    struct gl_transform xt = *x;
    x->m[0][0] = t.m[0][0] * xt.m[0][0] + t.m[0][1] * xt.m[1][0];
    x->m[1][0] = t.m[1][0] * xt.m[0][0] + t.m[1][1] * xt.m[1][0];
    x->m[0][1] = t.m[0][0] * xt.m[0][1] + t.m[0][1] * xt.m[1][1];
    x->m[1][1] = t.m[1][0] * xt.m[0][1] + t.m[1][1] * xt.m[1][1];
    gl_transform_vec(t, &x->t[0], &x->t[1]);
}

static void GLAPIENTRY gl_debug_cb(GLenum source, GLenum type, GLuint id,
                                   GLenum severity, GLsizei length,
                                   const GLchar *message, const void *userParam)
{
    // keep in mind that the debug callback can be asynchronous
    struct mp_log *log = (void *)userParam;
    int level = MSGL_ERR;
    switch (severity) {
    case GL_DEBUG_SEVERITY_NOTIFICATION:level = MSGL_V; break;
    case GL_DEBUG_SEVERITY_LOW:         level = MSGL_INFO; break;
    case GL_DEBUG_SEVERITY_MEDIUM:      level = MSGL_WARN; break;
    case GL_DEBUG_SEVERITY_HIGH:        level = MSGL_ERR; break;
    }
    mp_msg(log, level, "GL: %s\n", message);
}

void gl_set_debug_logger(GL *gl, struct mp_log *log)
{
    if (gl->DebugMessageCallback)
        gl->DebugMessageCallback(log ? gl_debug_cb : NULL, log);
}

#define SC_ENTRIES 32
#define SC_UNIFORM_ENTRIES 20

enum uniform_type {
    UT_invalid,
    UT_i,
    UT_f,
    UT_m,
    UT_buffer,
};

union uniform_val {
    GLfloat f[9];
    GLint i[4];
    struct {
        char* text;
        GLint binding;
    } buffer;
};

struct sc_uniform {
    char *name;
    enum uniform_type type;
    const char *glsl_type;
    int size;
    GLint loc;
    union uniform_val v;
};

struct sc_entry {
    GLuint gl_shader;
    GLint uniform_locs[SC_UNIFORM_ENTRIES];
    union uniform_val cached_v[SC_UNIFORM_ENTRIES];
    bstr frag;
    bstr vert;
    struct gl_vao *vao;
};

struct gl_shader_cache {
    GL *gl;
    struct mp_log *log;

    // this is modified during use (gl_sc_add() etc.)
    bstr prelude_text;
    bstr header_text;
    bstr text;
    struct gl_vao *vao;

    struct sc_entry entries[SC_ENTRIES];
    int num_entries;

    struct sc_uniform uniforms[SC_UNIFORM_ENTRIES];
    int num_uniforms;

    // temporary buffers (avoids frequent reallocations)
    bstr tmp[5];
};

struct gl_shader_cache *gl_sc_create(GL *gl, struct mp_log *log)
{
    struct gl_shader_cache *sc = talloc_ptrtype(NULL, sc);
    *sc = (struct gl_shader_cache){
        .gl = gl,
        .log = log,
    };
    return sc;
}

void gl_sc_reset(struct gl_shader_cache *sc)
{
    sc->prelude_text.len = 0;
    sc->header_text.len = 0;
    sc->text.len = 0;
    for (int n = 0; n < sc->num_uniforms; n++) {
        talloc_free(sc->uniforms[n].name);
        if (sc->uniforms[n].type == UT_buffer)
            talloc_free(sc->uniforms[n].v.buffer.text);
    }
    sc->num_uniforms = 0;
}

static void sc_flush_cache(struct gl_shader_cache *sc)
{
    for (int n = 0; n < sc->num_entries; n++) {
        struct sc_entry *e = &sc->entries[n];
        sc->gl->DeleteProgram(e->gl_shader);
        talloc_free(e->vert.start);
        talloc_free(e->frag.start);
    }
    sc->num_entries = 0;
}

void gl_sc_destroy(struct gl_shader_cache *sc)
{
    if (!sc)
        return;
    gl_sc_reset(sc);
    sc_flush_cache(sc);
    talloc_free(sc);
}

void gl_sc_enable_extension(struct gl_shader_cache *sc, char *name)
{
    bstr_xappend_asprintf(sc, &sc->prelude_text, "#extension %s : enable\n", name);
}

#define bstr_xappend0(sc, b, s) bstr_xappend(sc, b, bstr0(s))

void gl_sc_add(struct gl_shader_cache *sc, const char *text)
{
    bstr_xappend0(sc, &sc->text, text);
}

void gl_sc_addf(struct gl_shader_cache *sc, const char *textf, ...)
{
    va_list ap;
    va_start(ap, textf);
    bstr_xappend_vasprintf(sc, &sc->text, textf, ap);
    va_end(ap);
}

void gl_sc_hadd(struct gl_shader_cache *sc, const char *text)
{
    bstr_xappend0(sc, &sc->header_text, text);
}

void gl_sc_haddf(struct gl_shader_cache *sc, const char *textf, ...)
{
    va_list ap;
    va_start(ap, textf);
    bstr_xappend_vasprintf(sc, &sc->header_text, textf, ap);
    va_end(ap);
}

static struct sc_uniform *find_uniform(struct gl_shader_cache *sc,
                                       const char *name)
{
    for (int n = 0; n < sc->num_uniforms; n++) {
        if (strcmp(sc->uniforms[n].name, name) == 0)
            return &sc->uniforms[n];
    }
    // not found -> add it
    assert(sc->num_uniforms < SC_UNIFORM_ENTRIES); // just don't have too many
    struct sc_uniform *new = &sc->uniforms[sc->num_uniforms++];
    *new = (struct sc_uniform) { .loc = -1, .name = talloc_strdup(NULL, name) };
    return new;
}

const char* mp_sampler_type(GLenum texture_target)
{
    switch (texture_target) {
    case GL_TEXTURE_1D:         return "sampler1D";
    case GL_TEXTURE_2D:         return "sampler2D";
    case GL_TEXTURE_RECTANGLE:  return "sampler2DRect";
    case GL_TEXTURE_3D:         return "sampler3D";
    default: abort();
    }
}

void gl_sc_uniform_sampler(struct gl_shader_cache *sc, char *name, GLenum target,
                           int unit)
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->type = UT_i;
    u->size = 1;
    u->glsl_type = mp_sampler_type(target);
    u->v.i[0] = unit;
}

void gl_sc_uniform_sampler_ui(struct gl_shader_cache *sc, char *name, int unit)
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->type = UT_i;
    u->size = 1;
    u->glsl_type = sc->gl->es ? "highp usampler2D" : "usampler2D";
    u->v.i[0] = unit;
}

void gl_sc_uniform_f(struct gl_shader_cache *sc, char *name, GLfloat f)
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->type = UT_f;
    u->size = 1;
    u->glsl_type = "float";
    u->v.f[0] = f;
}

void gl_sc_uniform_i(struct gl_shader_cache *sc, char *name, GLint i)
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->type = UT_i;
    u->size = 1;
    u->glsl_type = "int";
    u->v.i[0] = i;
}

void gl_sc_uniform_vec2(struct gl_shader_cache *sc, char *name, GLfloat f[2])
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->type = UT_f;
    u->size = 2;
    u->glsl_type = "vec2";
    u->v.f[0] = f[0];
    u->v.f[1] = f[1];
}

void gl_sc_uniform_vec3(struct gl_shader_cache *sc, char *name, GLfloat f[3])
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->type = UT_f;
    u->size = 3;
    u->glsl_type = "vec3";
    u->v.f[0] = f[0];
    u->v.f[1] = f[1];
    u->v.f[2] = f[2];
}

static void transpose2x2(float r[2 * 2])
{
    MPSWAP(float, r[0+2*1], r[1+2*0]);
}

void gl_sc_uniform_mat2(struct gl_shader_cache *sc, char *name,
                        bool transpose, GLfloat *v)
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->type = UT_m;
    u->size = 2;
    u->glsl_type = "mat2";
    for (int n = 0; n < 4; n++)
        u->v.f[n] = v[n];
    if (transpose)
        transpose2x2(&u->v.f[0]);
}

static void transpose3x3(float r[3 * 3])
{
    MPSWAP(float, r[0+3*1], r[1+3*0]);
    MPSWAP(float, r[0+3*2], r[2+3*0]);
    MPSWAP(float, r[1+3*2], r[2+3*1]);
}

void gl_sc_uniform_mat3(struct gl_shader_cache *sc, char *name,
                        bool transpose, GLfloat *v)
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->type = UT_m;
    u->size = 3;
    u->glsl_type = "mat3";
    for (int n = 0; n < 9; n++)
        u->v.f[n] = v[n];
    if (transpose)
        transpose3x3(&u->v.f[0]);
}

void gl_sc_uniform_buffer(struct gl_shader_cache *sc, char *name,
                          const char *text, int binding)
{
    struct sc_uniform *u = find_uniform(sc, name);
    u->type = UT_buffer;
    u->v.buffer.text = talloc_strdup(sc, text);
    u->v.buffer.binding = binding;
}

// This will call glBindAttribLocation() on the shader before it's linked
// (OpenGL requires this to happen before linking). Basically, it associates
// the input variable names with the fields in the vao.
// The vertex shader is setup such that the elements are available as fragment
// shader variables using the names in the vao entries, which "position" being
// set to gl_Position.
void gl_sc_set_vao(struct gl_shader_cache *sc, struct gl_vao *vao)
{
    sc->vao = vao;
}

static const char *vao_glsl_type(const struct gl_vao_entry *e)
{
    // pretty dumb... too dumb, but works for us
    switch (e->num_elems) {
    case 1: return "float";
    case 2: return "vec2";
    case 3: return "vec3";
    case 4: return "vec4";
    default: abort();
    }
}

// Assumes program is current (gl->UseProgram(program)).
static void update_uniform(GL *gl, struct sc_entry *e, struct sc_uniform *u, int n)
{
    if (u->type == UT_buffer) {
        GLuint idx = gl->GetUniformBlockIndex(e->gl_shader, u->name);
        gl->UniformBlockBinding(e->gl_shader, idx, u->v.buffer.binding);
        return;
    }
    GLint loc = e->uniform_locs[n];
    if (loc < 0)
        return;
    switch (u->type) {
    case UT_i:
        assert(u->size == 1);
        if (memcmp(e->cached_v[n].i, u->v.i, sizeof(u->v.i)) != 0) {
            memcpy(e->cached_v[n].i, u->v.i, sizeof(u->v.i));
            gl->Uniform1i(loc, u->v.i[0]);
        }
        break;
    case UT_f:
        if (memcmp(e->cached_v[n].f, u->v.f, sizeof(u->v.f)) != 0) {
            memcpy(e->cached_v[n].f, u->v.f, sizeof(u->v.f));
            switch (u->size) {
            case 1: gl->Uniform1f(loc, u->v.f[0]); break;
            case 2: gl->Uniform2f(loc, u->v.f[0], u->v.f[1]); break;
            case 3: gl->Uniform3f(loc, u->v.f[0], u->v.f[1], u->v.f[2]); break;
            case 4: gl->Uniform4f(loc, u->v.f[0], u->v.f[1], u->v.f[2],
                                  u->v.f[3]); break;
            default: abort();
            }
        }
        break;
    case UT_m:
        if (memcmp(e->cached_v[n].f, u->v.f, sizeof(u->v.f)) != 0) {
            memcpy(e->cached_v[n].f, u->v.f, sizeof(u->v.f));
            switch (u->size) {
            case 2: gl->UniformMatrix2fv(loc, 1, GL_FALSE, &u->v.f[0]); break;
            case 3: gl->UniformMatrix3fv(loc, 1, GL_FALSE, &u->v.f[0]); break;
            default: abort();
            }
        }
        break;
    default:
        abort();
    }
}

static void compile_attach_shader(struct gl_shader_cache *sc, GLuint program,
                                  GLenum type, const char *source)
{
    GL *gl = sc->gl;

    GLuint shader = gl->CreateShader(type);
    gl->ShaderSource(shader, 1, &source, NULL);
    gl->CompileShader(shader);
    GLint status;
    gl->GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    GLint log_length;
    gl->GetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

    int pri = status ? (log_length > 1 ? MSGL_V : MSGL_DEBUG) : MSGL_ERR;
    const char *typestr = type == GL_VERTEX_SHADER ? "vertex" : "fragment";
    if (mp_msg_test(sc->log, pri)) {
        MP_MSG(sc, pri, "%s shader source:\n", typestr);
        mp_log_source(sc->log, pri, source);
    }
    if (log_length > 1) {
        GLchar *logstr = talloc_zero_size(NULL, log_length + 1);
        gl->GetShaderInfoLog(shader, log_length, NULL, logstr);
        MP_MSG(sc, pri, "%s shader compile log (status=%d):\n%s\n",
               typestr, status, logstr);
        talloc_free(logstr);
    }

    gl->AttachShader(program, shader);
    gl->DeleteShader(shader);
}

static void link_shader(struct gl_shader_cache *sc, GLuint program)
{
    GL *gl = sc->gl;
    gl->LinkProgram(program);
    GLint status;
    gl->GetProgramiv(program, GL_LINK_STATUS, &status);
    GLint log_length;
    gl->GetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);

    int pri = status ? (log_length > 1 ? MSGL_V : MSGL_DEBUG) : MSGL_ERR;
    if (mp_msg_test(sc->log, pri)) {
        GLchar *logstr = talloc_zero_size(NULL, log_length + 1);
        gl->GetProgramInfoLog(program, log_length, NULL, logstr);
        MP_MSG(sc, pri, "shader link log (status=%d): %s\n", status, logstr);
        talloc_free(logstr);
    }
}

static GLuint create_program(struct gl_shader_cache *sc, const char *vertex,
                             const char *frag)
{
    GL *gl = sc->gl;
    MP_VERBOSE(sc, "recompiling a shader program:\n");
    if (sc->header_text.len) {
        MP_VERBOSE(sc, "header:\n");
        mp_log_source(sc->log, MSGL_V, sc->header_text.start);
        MP_VERBOSE(sc, "body:\n");
    }
    if (sc->text.len)
        mp_log_source(sc->log, MSGL_V, sc->text.start);
    GLuint prog = gl->CreateProgram();
    compile_attach_shader(sc, prog, GL_VERTEX_SHADER, vertex);
    compile_attach_shader(sc, prog, GL_FRAGMENT_SHADER, frag);
    for (int n = 0; sc->vao->entries[n].name; n++) {
        char vname[80];
        snprintf(vname, sizeof(vname), "vertex_%s", sc->vao->entries[n].name);
        gl->BindAttribLocation(prog, n, vname);
    }
    link_shader(sc, prog);
    return prog;
}

#define ADD(x, ...) bstr_xappend_asprintf(sc, (x), __VA_ARGS__)
#define ADD_BSTR(x, s) bstr_xappend(sc, (x), (s))

// 1. Generate vertex and fragment shaders from the fragment shader text added
//    with gl_sc_add(). The generated shader program is cached (based on the
//    text), so actual compilation happens only the first time.
// 2. Update the uniforms set with gl_sc_uniform_*.
// 3. Make the new shader program current (glUseProgram()).
// 4. Reset the sc state and prepare for a new shader program. (All uniforms
//    and fragment operations needed for the next program have to be re-added.)
void gl_sc_gen_shader_and_reset(struct gl_shader_cache *sc)
{
    GL *gl = sc->gl;

    assert(sc->vao);

    for (int n = 0; n < MP_ARRAY_SIZE(sc->tmp); n++)
        sc->tmp[n].len = 0;

    // set up shader text (header + uniforms + body)
    bstr *header = &sc->tmp[0];
    ADD(header, "#version %d%s\n", gl->glsl_version, gl->es >= 300 ? " es" : "");
    if (gl->es)
        ADD(header, "precision mediump float;\n");
    ADD_BSTR(header, sc->prelude_text);
    char *vert_in = gl->glsl_version >= 130 ? "in" : "attribute";
    char *vert_out = gl->glsl_version >= 130 ? "out" : "varying";
    char *frag_in = gl->glsl_version >= 130 ? "in" : "varying";

    // vertex shader: we don't use the vertex shader, so just setup a dummy,
    // which passes through the vertex array attributes.
    bstr *vert_head = &sc->tmp[1];
    ADD_BSTR(vert_head, *header);
    bstr *vert_body = &sc->tmp[2];
    ADD(vert_body, "void main() {\n");
    bstr *frag_vaos = &sc->tmp[3];
    for (int n = 0; sc->vao->entries[n].name; n++) {
        const struct gl_vao_entry *e = &sc->vao->entries[n];
        const char *glsl_type = vao_glsl_type(e);
        if (strcmp(e->name, "position") == 0) {
            // setting raster pos. requires setting gl_Position magic variable
            assert(e->num_elems == 2 && e->type == GL_FLOAT);
            ADD(vert_head, "%s vec2 vertex_position;\n", vert_in);
            ADD(vert_body, "gl_Position = vec4(vertex_position, 1.0, 1.0);\n");
        } else {
            ADD(vert_head, "%s %s vertex_%s;\n", vert_in, glsl_type, e->name);
            ADD(vert_head, "%s %s %s;\n", vert_out, glsl_type, e->name);
            ADD(vert_body, "%s = vertex_%s;\n", e->name, e->name);
            ADD(frag_vaos, "%s %s %s;\n", frag_in, glsl_type, e->name);
        }
    }
    ADD(vert_body, "}\n");
    bstr *vert = vert_head;
    ADD_BSTR(vert, *vert_body);

    // fragment shader; still requires adding used uniforms and VAO elements
    bstr *frag = &sc->tmp[4];
    ADD_BSTR(frag, *header);
    ADD(frag, "#define RG %s\n", gl->mpgl_caps & MPGL_CAP_TEX_RG ? "rg" : "ra");
    if (gl->glsl_version >= 130) {
        ADD(frag, "#define texture1D texture\n");
        ADD(frag, "#define texture3D texture\n");
        ADD(frag, "out vec4 out_color;\n");
    } else {
        ADD(frag, "#define texture texture2D\n");
    }
    ADD_BSTR(frag, *frag_vaos);
    for (int n = 0; n < sc->num_uniforms; n++) {
        struct sc_uniform *u = &sc->uniforms[n];
        if (u->type == UT_buffer) {
            ADD(frag, "uniform %s { %s };\n", u->name, u->v.buffer.text);
        } else {
            ADD(frag, "uniform %s %s;\n", u->glsl_type, u->name);
        }
    }

    // Additional helpers.
    ADD(frag, "#define LUT_POS(x, lut_size)"
              " mix(0.5 / (lut_size), 1.0 - 0.5 / (lut_size), (x))\n");

    // custom shader header
    if (sc->header_text.len) {
        ADD(frag, "// header\n");
        ADD_BSTR(frag, sc->header_text);
        ADD(frag, "// body\n");
    }
    ADD(frag, "void main() {\n");
    // we require _all_ frag shaders to write to a "vec4 color"
    ADD(frag, "vec4 color = vec4(0.0, 0.0, 0.0, 1.0);\n");
    ADD_BSTR(frag, sc->text);
    if (gl->glsl_version >= 130) {
        ADD(frag, "out_color = color;\n");
    } else {
        ADD(frag, "gl_FragColor = color;\n");
    }
    ADD(frag, "}\n");

    struct sc_entry *entry = NULL;
    for (int n = 0; n < sc->num_entries; n++) {
        struct sc_entry *cur = &sc->entries[n];
        if (bstr_equals(cur->frag, *frag) && bstr_equals(cur->vert, *vert)) {
            entry = cur;
            break;
        }
    }
    if (!entry) {
        if (sc->num_entries == SC_ENTRIES)
            sc_flush_cache(sc);
        entry = &sc->entries[sc->num_entries++];
        *entry = (struct sc_entry){
            .vert = bstrdup(NULL, *vert),
            .frag = bstrdup(NULL, *frag),
        };
    }
    // build vertex shader from vao and cache the locations of the uniform variables
    if (!entry->gl_shader) {
        entry->gl_shader = create_program(sc, vert->start, frag->start);
        for (int n = 0; n < sc->num_uniforms; n++) {
            entry->uniform_locs[n] = gl->GetUniformLocation(entry->gl_shader,
                                                            sc->uniforms[n].name);
        }
    }

    gl->UseProgram(entry->gl_shader);

    for (int n = 0; n < sc->num_uniforms; n++)
        update_uniform(gl, entry, &sc->uniforms[n], n);

    gl_sc_reset(sc);
}
