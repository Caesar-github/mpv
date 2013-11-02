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

#include <libavcodec/avcodec.h>
#include <libavutil/common.h>

#include "talloc.h"
#include "mpvcore/mp_msg.h"
#include "mpvcore/av_common.h"
#include "mpvcore/options.h"
#include "demux/stheader.h"
#include "sd.h"
#include "dec_sub.h"
#include "sub.h"

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
    ctx->extradata_size = sd->extradata_len;
    ctx->extradata = sd->extradata;
    if (avcodec_open2(ctx, sub_codec, NULL) < 0)
        goto error;
    priv->avctx = ctx;
    sd->priv = priv;
    return 0;

 error:
    mp_msg(MSGT_SUBREADER, MSGL_ERR,
           "Could not open libavcodec subtitle decoder\n");
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
                struct sub_bitmap *b = &priv->inbitmaps[i];
                struct osd_bmp_indexed *img = &priv->imgs[i];
                if (!(r->flags & AV_SUBTITLE_FLAG_FORCED) &&
                    opts->forced_subs_only)
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
            mp_msg(MSGT_SUBREADER, MSGL_ERR, "sd_lavc: unsupported subtitle "
                   "type from libavcodec\n");
            break;
        }
    }
}

static void get_bitmaps(struct sd *sd, struct mp_osd_res d, double pts,
                        struct sub_bitmaps *res)
{
    struct sd_lavc_priv *priv = sd->priv;

    if (priv->pts != MP_NOPTS_VALUE && pts < priv->pts)
        return;
    if (priv->endpts != MP_NOPTS_VALUE && (pts >= priv->endpts ||
                                           pts < priv->endpts - 300))
        clear(priv);
    if (priv->bitmaps_changed && priv->count > 0)
        priv->outbitmaps = talloc_memdup(priv, priv->inbitmaps,
                                         talloc_get_size(priv->inbitmaps));
    int inw = priv->avctx->width;
    int inh = priv->avctx->height;
    guess_resolution(priv->avctx->codec_id, &inw, &inh);
    int vidw = d.w - d.ml - d.mr;
    int vidh = d.h - d.mt - d.mb;
    double xscale = (double)vidw / inw;
    double yscale = (double)vidh / inh;
    if (priv->avctx->codec_id == AV_CODEC_ID_DVD_SUBTITLE) {
        // For DVD subs, try to keep the subtitle PAR at display PAR.
        if (d.video_par > 1.0) {
            xscale /= d.video_par;
        } else {
            yscale *= d.video_par;
        }
    }
    int cx = vidw / 2 - (int)(inw * xscale) / 2;
    int cy = vidh / 2 - (int)(inh * yscale) / 2;
    for (int i = 0; i < priv->count; i++) {
        struct sub_bitmap *bi = &priv->inbitmaps[i];
        struct sub_bitmap *bo = &priv->outbitmaps[i];
        bo->x = bi->x * xscale + cx + d.ml;
        bo->y = bi->y * yscale + cy + d.mt;
        bo->dw = bi->w * xscale;
        bo->dh = bi->h * yscale;
    }
    res->parts = priv->outbitmaps;
    res->num_parts = priv->count;
    if (priv->bitmaps_changed)
        res->bitmap_id = ++res->bitmap_pos_id;
    priv->bitmaps_changed = false;
    res->format = SUBBITMAP_INDEXED;
    res->scaled = xscale != 1 || yscale != 1;
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
    av_free(priv->avctx);
    talloc_free(priv);
}

const struct sd_functions sd_lavc = {
    .name = "lavc",
    .supports_format = supports_format,
    .init = init,
    .decode = decode,
    .get_bitmaps = get_bitmaps,
    .reset = reset,
    .uninit = uninit,
};
