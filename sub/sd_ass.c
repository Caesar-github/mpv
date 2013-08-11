/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <libavutil/common.h>
#include <ass/ass.h>

#include "talloc.h"

#include "mpvcore/options.h"
#include "mpvcore/mp_common.h"
#include "mpvcore/mp_msg.h"
#include "video/csputils.h"
#include "video/mp_image.h"
#include "sub.h"
#include "dec_sub.h"
#include "ass_mp.h"
#include "sd.h"

// Enable code that treats subtitle events with duration 0 specially, and
// adjust their duration so that they will disappear with the next event.
#define INCOMPLETE_EVENTS 0

struct sd_ass_priv {
    struct ass_track *ass_track;
    bool is_converted;
    bool incomplete_event;
    struct sub_bitmap *parts;
    bool flush_on_seek;
    char last_text[500];
    struct mp_image_params video_params;
    struct mp_image_params last_params;
};

static void mangle_colors(struct sd *sd, struct sub_bitmaps *parts);

static bool supports_format(const char *format)
{
    // ass-text is produced by converters and the subreader.c ssa parser; this
    // format has ASS tags, but doesn't start with any prelude, nor does it
    // have extradata.
    return format && (strcmp(format, "ass") == 0 ||
                      strcmp(format, "ssa") == 0 ||
                      strcmp(format, "ass-text") == 0);
}

static void free_last_event(ASS_Track *track)
{
    assert(track->n_events > 0);
    ass_free_event(track, track->n_events - 1);
    track->n_events--;
}

static int init(struct sd *sd)
{
    struct MPOpts *opts = sd->opts;
    if (!sd->ass_library || !sd->ass_renderer || !sd->codec)
        return -1;

    struct sd_ass_priv *ctx = talloc_zero(NULL, struct sd_ass_priv);
    sd->priv = ctx;

    ctx->is_converted = sd->converted_from != NULL;

    if (sd->ass_track) {
        ctx->ass_track = sd->ass_track;
    } else {
        ctx->ass_track = ass_new_track(sd->ass_library);
        if (!ctx->is_converted)
            ctx->ass_track->track_type = TRACK_TYPE_ASS;
    }

    if (sd->extradata) {
        ass_process_codec_private(ctx->ass_track, sd->extradata,
                                  sd->extradata_len);
    }

    mp_ass_add_default_styles(ctx->ass_track, opts);

    return 0;
}

static void decode(struct sd *sd, struct demux_packet *packet)
{
    void *data = packet->buffer;
    int data_len = packet->len;
    double pts = packet->pts;
    double duration = packet->duration;
    unsigned char *text = data;
    struct sd_ass_priv *ctx = sd->priv;
    ASS_Track *track = ctx->ass_track;
    if (strcmp(sd->codec, "ass") == 0) {
        ass_process_chunk(track, data, data_len,
                          (long long)(pts*1000 + 0.5),
                          (long long)(duration*1000 + 0.5));
        return;
    } else if (strcmp(sd->codec, "ssa") == 0) {
        // broken ffmpeg ASS packet format
        ctx->flush_on_seek = true;
        ass_process_data(track, data, data_len);
        return;
    }
    // plaintext subs
    if (pts == MP_NOPTS_VALUE) {
        mp_msg(MSGT_SUBREADER, MSGL_WARN, "Subtitle without pts, ignored\n");
        return;
    }
    long long ipts = pts * 1000 + 0.5;
    long long iduration = duration * 1000 + 0.5;
#if INCOMPLETE_EVENTS
    if (ctx->incomplete_event) {
        ctx->incomplete_event = false;
        ASS_Event *event = track->events + track->n_events - 1;
        if (ipts <= event->Start)
            free_last_event(track);
        else
            event->Duration = ipts - event->Start;
    }
    // Note: we rely on there being guaranteed 0 bytes after data packets
    int len = strlen(text);
    if (len < 5) {
        // Some tracks use a whitespace (but not empty) packet to mark end
        // of previous subtitle.
        for (int i = 0; i < len; i++)
            if (!strchr(" \f\n\r\t\v", text[i]))
                goto not_all_whitespace;
        return;
    }
 not_all_whitespace:;
    if (!sd->no_remove_duplicates) {
        for (int i = 0; i < track->n_events; i++)
            if (track->events[i].Start == ipts
                && (duration <= 0 || track->events[i].Duration == iduration)
                && strcmp(track->events[i].Text, text) == 0)
                return;   // We've already added this subtitle
    }
    if (duration <= 0) {
        iduration = 10000;
        ctx->incomplete_event = true;
    }
#else
    if (duration <= 0) {
        mp_msg(MSGT_SUBREADER, MSGL_WARN, "Subtitle without duration or "
               "duration set to 0 at pts %f, ignored\n", pts);
        return;
    }
    if (!sd->no_remove_duplicates) {
        for (int i = 0; i < track->n_events; i++) {
            if (track->events[i].Start == ipts
                && (track->events[i].Duration == iduration)
                && strcmp(track->events[i].Text, text) == 0)
                return;   // We've already added this subtitle
        }
    }
#endif
    int eid = ass_alloc_event(track);
    ASS_Event *event = track->events + eid;
    event->Start = ipts;
    event->Duration = iduration;
    event->Style = track->default_style;
    event->Text = strdup(text);
}

static void get_bitmaps(struct sd *sd, struct mp_osd_res dim, double pts,
                        struct sub_bitmaps *res)
{
    struct sd_ass_priv *ctx = sd->priv;
    struct MPOpts *opts = sd->opts;

    if (pts == MP_NOPTS_VALUE || !sd->ass_renderer)
        return;

    ASS_Renderer *renderer = sd->ass_renderer;
    double scale = dim.display_par;
    if (!ctx->is_converted && (!opts->ass_style_override ||
                               opts->ass_vsfilter_aspect_compat))
    {
        scale = scale * dim.video_par;
    }
    mp_ass_configure(renderer, opts, &dim);
    ass_set_aspect_ratio(renderer, scale, 1);
#if LIBASS_VERSION >= 0x01020000
    if (!ctx->is_converted && (!opts->ass_style_override ||
                               opts->ass_vsfilter_blur_compat))
    {
        ass_set_storage_size(renderer, ctx->video_params.w, ctx->video_params.h);
    } else {
        ass_set_storage_size(renderer, 0, 0);
    }
#endif
    mp_ass_render_frame(renderer, ctx->ass_track, pts * 1000 + .5,
                        &ctx->parts, res);
    talloc_steal(ctx, ctx->parts);

    if (!ctx->is_converted)
        mangle_colors(sd, res);
}

struct buf {
    char *start;
    int size;
    int len;
};

static void append(struct buf *b, char c)
{
    if (b->len < b->size) {
        b->start[b->len] = c;
        b->len++;
    }
}

static void ass_to_plaintext(struct buf *b, const char *in)
{
    bool in_tag = false;
    bool in_drawing = false;
    while (*in) {
        if (in_tag) {
            if (in[0] == '}') {
                in += 1;
                in_tag = false;
            } else if (in[0] == '\\' && in[1] == 'p') {
                in += 2;
                // skip text between \pN and \p0 tags
                if (in[0] == '0') {
                    in_drawing = false;
                } else if (in[0] >= '1' && in[0] <= '9') {
                    in_drawing = true;
                }
            } else {
                in += 1;
            }
        } else {
            if (in[0] == '\\' && (in[1] == 'N' || in[1] == 'n')) {
                in += 2;
                append(b, '\n');
            } else if (in[0] == '\\' && in[1] == 'h') {
                in += 2;
                append(b, ' ');
            } else if (in[0] == '{') {
                in += 1;
                in_tag = true;
            } else {
                if (!in_drawing)
                    append(b, in[0]);
                in += 1;
            }
        }
    }
}

// Empty string counts as whitespace. Reads s[len-1] even if there are \0s.
static bool is_whitespace_only(char *s, int len)
{
    for (int n = 0; n < len; n++) {
        if (s[n] != ' ' && s[n] != '\t')
            return false;
    }
    return true;
}

static char *get_text(struct sd *sd, double pts)
{
    struct sd_ass_priv *ctx = sd->priv;
    ASS_Track *track = ctx->ass_track;

    if (pts == MP_NOPTS_VALUE)
        return NULL;
    long long ipts = pts * 1000 + 0.5;

    struct buf b = {ctx->last_text, sizeof(ctx->last_text) - 1};

    for (int i = 0; i < track->n_events; ++i) {
        ASS_Event *event = track->events + i;
        if (ipts >= event->Start && ipts < event->Start + event->Duration) {
            if (event->Text) {
                int start = b.len;
                ass_to_plaintext(&b, event->Text);
                if (is_whitespace_only(&b.start[start], b.len - start)) {
                    b.len = start;
                } else {
                    append(&b, '\n');
                }
            }
        }
    }

    b.start[b.len] = '\0';

    if (b.len > 0 && b.start[b.len - 1] == '\n')
        b.start[b.len - 1] = '\0';

    return ctx->last_text;
}

static void fix_events(struct sd *sd)
{
    struct sd_ass_priv *ctx = sd->priv;
    ctx->flush_on_seek = false;
}

static void reset(struct sd *sd)
{
    struct sd_ass_priv *ctx = sd->priv;
    if (ctx->incomplete_event)
        free_last_event(ctx->ass_track);
    ctx->incomplete_event = false;
    if (ctx->flush_on_seek)
        ass_flush_events(ctx->ass_track);
    ctx->flush_on_seek = false;
}

static void uninit(struct sd *sd)
{
    struct sd_ass_priv *ctx = sd->priv;

    if (sd->ass_track != ctx->ass_track)
        ass_free_track(ctx->ass_track);
    talloc_free(ctx);
}

static int control(struct sd *sd, enum sd_ctrl cmd, void *arg)
{
    struct sd_ass_priv *ctx = sd->priv;
    switch (cmd) {
    case SD_CTRL_SUB_STEP: {
        double *a = arg;
        a[0] = ass_step_sub(ctx->ass_track, a[0] * 1000 + .5, a[1]) / 1000.0;
        return CONTROL_OK;
    case SD_CTRL_SET_VIDEO_PARAMS:
        ctx->video_params = *(struct mp_image_params *)arg;
        return CONTROL_OK;
    }
    default:
        return CONTROL_UNKNOWN;
    }
}

const struct sd_functions sd_ass = {
    .name = "ass",
    .accept_packets_in_advance = true,
    .supports_format = supports_format,
    .init = init,
    .decode = decode,
    .get_bitmaps = get_bitmaps,
    .get_text = get_text,
    .fix_events = fix_events,
    .control = control,
    .reset = reset,
    .uninit = uninit,
};

// Disgusting hack for (xy-)vsfilter color compatibility.
static void mangle_colors(struct sd *sd, struct sub_bitmaps *parts)
{
    struct MPOpts *opts = sd->opts;
    struct sd_ass_priv *ctx = sd->priv;
    enum mp_csp csp = 0;
    enum mp_csp_levels levels = 0;
    if (opts->ass_vsfilter_color_compat == 0) // "no"
        return;
    bool force_601 = opts->ass_vsfilter_color_compat == 3;
#if LIBASS_VERSION >= 0x01020000
    ASS_Track *track = ctx->ass_track;
    static const int ass_csp[] = {
        [YCBCR_BT601_TV]        = MP_CSP_BT_601,
        [YCBCR_BT601_PC]        = MP_CSP_BT_601,
        [YCBCR_BT709_TV]        = MP_CSP_BT_709,
        [YCBCR_BT709_PC]        = MP_CSP_BT_709,
        [YCBCR_SMPTE240M_TV]    = MP_CSP_SMPTE_240M,
        [YCBCR_SMPTE240M_PC]    = MP_CSP_SMPTE_240M,
    };
    static const int ass_levels[] = {
        [YCBCR_BT601_TV]        = MP_CSP_LEVELS_TV,
        [YCBCR_BT601_PC]        = MP_CSP_LEVELS_PC,
        [YCBCR_BT709_TV]        = MP_CSP_LEVELS_TV,
        [YCBCR_BT709_PC]        = MP_CSP_LEVELS_PC,
        [YCBCR_SMPTE240M_TV]    = MP_CSP_LEVELS_TV,
        [YCBCR_SMPTE240M_PC]    = MP_CSP_LEVELS_PC,
    };
    int trackcsp = track->YCbCrMatrix;
    if (force_601)
        trackcsp = YCBCR_BT601_TV;
    // NONE is a bit random, but the intention is: don't modify colors.
    if (trackcsp == YCBCR_NONE)
        return;
    if (trackcsp < sizeof(ass_csp) / sizeof(ass_csp[0]))
        csp = ass_csp[trackcsp];
    if (trackcsp < sizeof(ass_levels) / sizeof(ass_levels[0]))
        levels = ass_levels[trackcsp];
    if (trackcsp == YCBCR_DEFAULT) {
        csp = MP_CSP_BT_601;
        levels = MP_CSP_LEVELS_TV;
    }
    // Unknown colorspace (either YCBCR_UNKNOWN, or a valid value unknown to us)
    if (!csp || !levels)
        return;
#endif

    struct mp_image_params params = ctx->video_params;

    if (force_601) {
        params.colorspace = MP_CSP_BT_709;
        params.colorlevels = MP_CSP_LEVELS_TV;
    }

    if (csp == params.colorspace && levels == params.colorlevels)
        return;

    bool basic_conv = params.colorspace == MP_CSP_BT_709 &&
                      params.colorlevels == MP_CSP_LEVELS_TV &&
                      csp == MP_CSP_BT_601 &&
                      levels == MP_CSP_LEVELS_TV;

    // With "basic", only do as much as needed for basic compatibility.
    if (opts->ass_vsfilter_color_compat == 1 && !basic_conv)
        return;

    if (params.colorspace != ctx->last_params.colorspace ||
        params.colorlevels != ctx->last_params.colorlevels)
    {
        int msgl = basic_conv ? MSGL_V : MSGL_WARN;
        ctx->last_params = params;
        mp_msg(MSGT_SUBREADER, msgl, "[sd_ass] mangling colors like vsfilter: "
               "RGB -> %s %s -> %s %s -> RGB\n", mp_csp_names[csp],
               mp_csp_levels_names[levels], mp_csp_names[params.colorspace],
               mp_csp_levels_names[params.colorlevels]);
    }

    // Conversion that VSFilter would use
    struct mp_csp_params vs_params = MP_CSP_PARAMS_DEFAULTS;
    vs_params.colorspace.format = csp;
    vs_params.colorspace.levels_in = levels;
    vs_params.int_bits_in = 8;
    vs_params.int_bits_out = 8;
    float vs_yuv2rgb[3][4], vs_rgb2yuv[3][4];
    mp_get_yuv2rgb_coeffs(&vs_params, vs_yuv2rgb);
    mp_invert_yuv2rgb(vs_rgb2yuv, vs_yuv2rgb);

    // Proper conversion to RGB
    struct mp_csp_params rgb_params = MP_CSP_PARAMS_DEFAULTS;
    rgb_params.colorspace.format = params.colorspace;
    rgb_params.colorspace.levels_in = params.colorlevels;
    rgb_params.int_bits_in = 8;
    rgb_params.int_bits_out = 8;
    float vs2rgb[3][4];
    mp_get_yuv2rgb_coeffs(&rgb_params, vs2rgb);

    for (int n = 0; n < parts->num_parts; n++) {
        struct sub_bitmap *sb = &parts->parts[n];
        uint32_t color = sb->libass.color;
        int r = (color >> 24u) & 0xff;
        int g = (color >> 16u) & 0xff;
        int b = (color >>  8u) & 0xff;
        int a = color & 0xff;
        int c[3] = {r, g, b};
        mp_map_int_color(vs_rgb2yuv, 8, c);
        mp_map_int_color(vs2rgb, 8, c);
        sb->libass.color = (c[0] << 24u) | (c[1] << 16) | (c[2] << 8) | a;
    }
}
