/*
 * This file is part of mplayer2.
 *
 * mplayer2 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mplayer2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mplayer2.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <assert.h>
#include <math.h>

#include <libavcodec/avcodec.h>
#include <libavutil/common.h>
#include <libavutil/intreadwrite.h>

#include "talloc.h"
#include "common/msg.h"
#include "common/av_common.h"
#include "options/options.h"
#include "video/mp_image.h"
#include "video/csputils.h"
#include "sd.h"
#include "dec_sub.h"

struct sd_lavc_priv {
    AVCodecContext *avctx;
    AVSubtitle sub;
    bool have_sub;
    int count;
    struct sub_bitmap *inbitmaps;
    struct sub_bitmap *outbitmaps;
    struct osd_bmp_indexed *imgs;
    bool bitmaps_changed;
    double pts;
    double endpts;
    struct mp_image_params video_params;
};

static bool supports_format(const char *format)
{
    enum AVCodecID cid = mp_codec_to_av_codec_id(format);
    // Supported codecs must be known to decode to paletted bitmaps
    switch (cid) {
    case AV_CODEC_ID_DVB_SUBTITLE:
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
    case AV_CODEC_ID_XSUB:
    // lavc dvdsubdec doesn't read color/resolution on Libav 9.1 and below,
    // so fall back to sd_spu in this case. Never use sd_spu with new ffmpeg;
    // spudec can't handle ffmpeg .idx demuxing (added to lavc in 54.79.100).
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(54, 40, 0)
    case AV_CODEC_ID_DVD_SUBTITLE:
#endif
        return true;
    default:
        return false;
    }
}

static void guess_resolution(enum AVCodecID type, int *w, int *h)
{
    if (type == AV_CODEC_ID_DVD_SUBTITLE) {
        /* XXX Although the video frame is some size, the SPU frame is
           always maximum size i.e. 720 wide and 576 or 480 high */
        // For HD files in MKV the VobSub resolution can be higher though,
        // see largeres_vobsub.mkv
        if (*w <= 720 && *h <= 576) {
            *w = 720;
            *h = (*h == 480 || *h == 240) ? 480 : 576;
        }
    } else {
        // Hope that PGS subs set these and 720/576 works for dvb subs
        if (!*w)
            *w = 720;
        if (!*h)
            *h = 576;
    }
}

static void get_resolution(struct sd *sd, int wh[2])
{
    struct sd_lavc_priv *priv = sd->priv;
    wh[0] = priv->avctx->width;
    wh[1] = priv->avctx->height;
    if (wh[0] <= 0 || wh[1] <= 0) {
        wh[0] = priv->video_params.w;
        wh[1] = priv->video_params.h;
    }
    guess_resolution(priv->avctx->codec_id, &wh[0], &wh[1]);
}

static void set_mp4_vobsub_idx(AVCodecContext *avctx, char *src, int w, int h)
{
    char pal_s[128];
    int pal_s_pos = 0;
    for (int i = 0; i < 16; i++) {
        unsigned int e = AV_RB32(src + i * 4);

        // lavc doesn't accept YUV palette - "does god hate me?"
        struct mp_csp_params csp = MP_CSP_PARAMS_DEFAULTS;
        csp.int_bits_in = 8;
        csp.int_bits_out = 8;
        float cmatrix[3][4];
        mp_get_yuv2rgb_coeffs(&csp, cmatrix);
        int c[3] = {(e >> 16) & 0xff, (e >> 8) & 0xff, e & 0xff};
        mp_map_int_color(cmatrix, 8, c);
        e = (c[2] << 16) | (c[1] << 8) | c[0];

        snprintf(pal_s + pal_s_pos, sizeof(pal_s) - pal_s_pos, "%06x%s", e,
                 i != 15 ? ", " : "");
        pal_s_pos = strlen(pal_s);
        if (pal_s_pos >= sizeof(pal_s))
            break;
    }

    char buf[256] = "";
    snprintf(buf, sizeof(buf), "size: %dx%d\npalette: %s\n", w, h, pal_s);
    mp_lavc_set_extradata(avctx, buf, strlen(buf));
}

static int init(struct sd *sd)
{
    struct sd_lavc_priv *priv = talloc_zero(NULL, struct sd_lavc_priv);
    enum AVCodecID cid = mp_codec_to_av_codec_id(sd->codec);
    AVCodecContext *ctx = NULL;
    AVCodec *sub_codec = avcodec_find_decoder(cid);
    if (!sub_codec)
        goto error;
    ctx = avcodec_alloc_context3(sub_codec);
    if (!ctx)
        goto error;
    mp_lavc_set_extradata(ctx, sd->extradata, sd->extradata_len);
    if (sd->extradata_len == 64 && sd->sub_stream_w && sd->sub_stream_h &&
        cid == AV_CODEC_ID_DVD_SUBTITLE)
    {
        set_mp4_vobsub_idx(ctx, sd->extradata, sd->sub_stream_w, sd->sub_stream_h);
    }
    if (avcodec_open2(ctx, sub_codec, NULL) < 0)
        goto error;
    priv->avctx = ctx;
    sd->priv = priv;
    return 0;

 error:
    MP_FATAL(sd, "Could not open libavcodec subtitle decoder\n");
    av_free(ctx);
    talloc_free(priv);
    return -1;
}

static void clear(struct sd_lavc_priv *priv)
{
    priv->count = 0;
    talloc_free(priv->inbitmaps);
    talloc_free(priv->outbitmaps);
    priv->inbitmaps = priv->outbitmaps = NULL;
    talloc_free(priv->imgs);
    priv->imgs = NULL;
    priv->bitmaps_changed = true;
    priv->pts = MP_NOPTS_VALUE;
    priv->endpts = MP_NOPTS_VALUE;
    if (priv->have_sub)
        avsubtitle_free(&priv->sub);
    priv->have_sub = false;
}

static void decode(struct sd *sd, struct demux_packet *packet)
{
    struct MPOpts *opts = sd->opts;
    struct sd_lavc_priv *priv = sd->priv;
    AVCodecContext *ctx = priv->avctx;
    double pts = packet->pts;
    double duration = packet->duration;
    AVSubtitle sub;
    AVPacket pkt;

    // libavformat sets duration==0, even if the duration is unknown.
    // Assume there are no bitmap subs that actually use duration==0 for
    // hidden subtitle events.
    if (duration == 0)
        duration = -1;

    clear(priv);
    av_init_packet(&pkt);
    pkt.data = packet->buffer;
    pkt.size = packet->len;
    pkt.pts = pts * 1000;
    if (duration >= 0)
        pkt.convergence_duration = duration * 1000;
    int got_sub;
    int res = avcodec_decode_subtitle2(ctx, &sub, &got_sub, &pkt);
    if (res < 0 || !got_sub)
        return;
    priv->sub = sub;
    priv->have_sub = true;
    if (pts != MP_NOPTS_VALUE) {
        if (sub.end_display_time > sub.start_display_time)
            duration = (sub.end_display_time - sub.start_display_time) / 1000.0;
        pts += sub.start_display_time / 1000.0;
    }
    double endpts = MP_NOPTS_VALUE;
    if (pts != MP_NOPTS_VALUE && duration >= 0)
        endpts = pts + duration;
    if (sub.num_rects > 0) {
        switch (sub.rects[0]->type) {
        case SUBTITLE_BITMAP:
            priv->count = 0;
            priv->pts = pts;
            priv->endpts = endpts;
            priv->inbitmaps = talloc_array(priv, struct sub_bitmap,
                                           sub.num_rects);
            priv->imgs = talloc_array(priv, struct osd_bmp_indexed,
                                      sub.num_rects);
            for (int i = 0; i < sub.num_rects; i++) {
                struct AVSubtitleRect *r = sub.rects[i];
                struct sub_bitmap *b = &priv->inbitmaps[priv->count];
                struct osd_bmp_indexed *img = &priv->imgs[priv->count];
                if (!(r->flags & AV_SUBTITLE_FLAG_FORCED) &&
                    opts->forced_subs_only)
                    continue;
                if (r->w == 0 || r->h == 0)
                    continue;
                img->bitmap = r->pict.data[0];
                assert(r->nb_colors > 0);
                assert(r->nb_colors * 4 <= sizeof(img->palette));
                memcpy(img->palette, r->pict.data[1], r->nb_colors * 4);
                b->bitmap = img;
                b->stride = r->pict.linesize[0];
                b->w = r->w;
                b->h = r->h;
                b->x = r->x;
                b->y = r->y;
                priv->count++;
            }
            break;
        default:
            MP_ERR(sd, "unsupported subtitle type from libavcodec\n");
            break;
        }
    }
}

static void get_bitmaps(struct sd *sd, struct mp_osd_res d, double pts,
                        struct sub_bitmaps *res)
{
    struct sd_lavc_priv *priv = sd->priv;
    struct MPOpts *opts = sd->opts;

    if (priv->pts != MP_NOPTS_VALUE && pts < priv->pts)
        return;
    if (priv->endpts != MP_NOPTS_VALUE && (pts >= priv->endpts ||
                                           pts < priv->endpts - 300))
        clear(priv);
    size_t size = talloc_get_size(priv->inbitmaps);
    if (!priv->outbitmaps)
        priv->outbitmaps = talloc_size(priv, size);
    memcpy(priv->outbitmaps, priv->inbitmaps, size);

    res->parts = priv->outbitmaps;
    res->num_parts = priv->count;
    if (priv->bitmaps_changed)
        res->bitmap_id = ++res->bitmap_pos_id;
    priv->bitmaps_changed = false;
    res->format = SUBBITMAP_INDEXED;

    double video_par = -1;
    if (priv->avctx->codec_id == AV_CODEC_ID_DVD_SUBTITLE &&
            opts->stretch_dvd_subs) {
        // For DVD subs, try to keep the subtitle PAR at display PAR.
        double par =
              (priv->video_params.d_w / (double)priv->video_params.d_h)
            / (priv->video_params.w   / (double)priv->video_params.h);
        if (isnormal(par))
            video_par = par;
    }
    int insize[2];
    get_resolution(sd, insize);
    osd_rescale_bitmaps(res, insize[0], insize[1], d, video_par);
}

static void reset(struct sd *sd)
{
    struct sd_lavc_priv *priv = sd->priv;

    if (priv->pts == MP_NOPTS_VALUE)
        clear(priv);
    // lavc might not do this right for all codecs; may need close+reopen
    avcodec_flush_buffers(priv->avctx);
}

static void uninit(struct sd *sd)
{
    struct sd_lavc_priv *priv = sd->priv;

    clear(priv);
    avcodec_close(priv->avctx);
    av_free(priv->avctx->extradata);
    av_free(priv->avctx);
    talloc_free(priv);
}

static int control(struct sd *sd, enum sd_ctrl cmd, void *arg)
{
    struct sd_lavc_priv *priv = sd->priv;
    switch (cmd) {
    case SD_CTRL_SET_VIDEO_PARAMS:
        priv->video_params = *(struct mp_image_params *)arg;
        return CONTROL_OK;
    case SD_CTRL_GET_RESOLUTION:
        get_resolution(sd, arg);
        return CONTROL_OK;
    default:
        return CONTROL_UNKNOWN;
    }
}

const struct sd_functions sd_lavc = {
    .name = "lavc",
    .supports_format = supports_format,
    .init = init,
    .decode = decode,
    .get_bitmaps = get_bitmaps,
    .control = control,
    .reset = reset,
    .uninit = uninit,
};
