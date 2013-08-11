/*
 * VDPAU video output driver
 *
 * Copyright (C) 2008 NVIDIA
 * Copyright (C) 2009 Uoti Urpala
 *
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

#include <stddef.h>
#include <assert.h>

#include <libavcodec/vdpau.h>
#include <libavutil/common.h>

#include "lavc.h"
#include "video/fmt-conversion.h"
#include "video/vdpau.h"
#include "video/decode/dec_video.h"

struct priv {
    struct mp_vdpau_ctx        *mpvdp;
    struct vdp_functions       *vdp;
    VdpDevice                   vdp_device;
    uint64_t                    preemption_counter;

    int                         image_format;
    int                         vid_width;
    int                         vid_height;

    VdpDecoder                  decoder;
    int                         decoder_max_refs;
};

static void mark_uninitialized(struct lavc_ctx *ctx)
{
    struct priv *p = ctx->hwdec_priv;

    p->vdp_device = VDP_INVALID_HANDLE;
    p->decoder = VDP_INVALID_HANDLE;
}

static int handle_preemption(struct lavc_ctx *ctx)
{
    struct priv *p = ctx->hwdec_priv;

    if (!mp_vdpau_status_ok(p->mpvdp))
        return -1;

    // Mark objects as destroyed if preemption+reinit occured
    if (p->preemption_counter < p->mpvdp->preemption_counter) {
        p->preemption_counter = p->mpvdp->preemption_counter;
        mark_uninitialized(ctx);
    }

    p->vdp_device = p->mpvdp->vdp_device;
    p->vdp = p->mpvdp->vdp;

    return 0;
}

static bool create_vdp_decoder(struct lavc_ctx *ctx, int max_refs)
{
    struct priv *p = ctx->hwdec_priv;
    struct vdp_functions *vdp = p->mpvdp->vdp;
    VdpStatus vdp_st;
    VdpDecoderProfile vdp_decoder_profile;

    if (handle_preemption(ctx) < 0)
        return false;

    if (p->decoder != VDP_INVALID_HANDLE)
        vdp->decoder_destroy(p->decoder);

    switch (p->image_format) {
    case IMGFMT_VDPAU_MPEG1:
        vdp_decoder_profile = VDP_DECODER_PROFILE_MPEG1;
        break;
    case IMGFMT_VDPAU_MPEG2:
        vdp_decoder_profile = VDP_DECODER_PROFILE_MPEG2_MAIN;
        break;
    case IMGFMT_VDPAU_H264:
        vdp_decoder_profile = VDP_DECODER_PROFILE_H264_HIGH;
        mp_msg(MSGT_VO, MSGL_V, "[vdpau] Creating H264 hardware decoder "
               "for %d reference frames.\n", max_refs);
        break;
    case IMGFMT_VDPAU_WMV3:
        vdp_decoder_profile = VDP_DECODER_PROFILE_VC1_MAIN;
        break;
    case IMGFMT_VDPAU_VC1:
        vdp_decoder_profile = VDP_DECODER_PROFILE_VC1_ADVANCED;
        break;
    case IMGFMT_VDPAU_MPEG4:
        vdp_decoder_profile = VDP_DECODER_PROFILE_MPEG4_PART2_ASP;
        break;
    default:
        mp_msg(MSGT_VO, MSGL_ERR, "[vdpau] Unknown image format!\n");
        goto fail;
    }
    vdp_st = vdp->decoder_create(p->vdp_device, vdp_decoder_profile,
                                 p->vid_width, p->vid_height, max_refs,
                                 &p->decoder);
    CHECK_ST_WARNING("Failed creating VDPAU decoder");
    if (vdp_st != VDP_STATUS_OK)
        goto fail;
    p->decoder_max_refs = max_refs;
    return true;

fail:
    p->decoder = VDP_INVALID_HANDLE;
    p->decoder_max_refs = 0;
    return false;
}

static void draw_slice_hwdec(struct AVCodecContext *s,
                             const AVFrame *src, int offset[4],
                             int y, int type, int height)
{
    sh_video_t *sh = s->opaque;
    struct lavc_ctx *ctx = sh->context;
    struct priv *p = ctx->hwdec_priv;
    struct vdp_functions *vdp = p->vdp;
    VdpStatus vdp_st;

    if (handle_preemption(ctx) < 0)
        return;

    struct vdpau_render_state *rndr = (void *)src->data[0];

    int max_refs = p->image_format == IMGFMT_VDPAU_H264 ?
                   rndr->info.h264.num_ref_frames : 2;
    if ((p->decoder == VDP_INVALID_HANDLE || p->decoder_max_refs < max_refs)
        && !create_vdp_decoder(ctx, max_refs))
        return;

    vdp_st = vdp->decoder_render(p->decoder, rndr->surface,
                                 (void *)&rndr->info,
                                 rndr->bitstream_buffers_used,
                                 rndr->bitstream_buffers);
    CHECK_ST_WARNING("Failed VDPAU decoder rendering");
}

static void release_surface(void *ptr)
{
    struct vdpau_render_state *state = ptr;
    // Free bitstream buffers allocated by libavcodec
    av_freep(&state->bitstream_buffers);
    talloc_free(state);
}

static struct mp_image *allocate_image(struct lavc_ctx *ctx, AVFrame *frame)
{
    struct priv *p = ctx->hwdec_priv;
    int imgfmt = pixfmt2imgfmt(frame->format);

    if (!IMGFMT_IS_VDPAU(imgfmt))
        return NULL;

    // frame->width/height lie. Using them breaks with non-mod 16 video.
    int w = ctx->avctx->width;
    int h = ctx->avctx->height;

    if (w != p->vid_width || h != p->vid_height || imgfmt != p->image_format) {
        p->vid_width = w;
        p->vid_height = h;
        p->image_format = imgfmt;
        if (!create_vdp_decoder(ctx, 2))
            return NULL;
    }

    VdpChromaType chroma;
    mp_vdpau_get_format(p->image_format, &chroma, NULL);

    struct mp_image *img =
        mp_vdpau_get_video_surface(p->mpvdp, imgfmt, chroma, w, h);

    if (!img)
        return NULL;

    // Create chained reference for vdpau_render_state. This will track the
    // lifetime of the actual reference too.
    // This is quite roundabout, but at least it allows us to share the
    // surface allocator in vo_vdpau.c with the new vdpau code.

    struct vdpau_render_state *state = talloc_ptrtype(NULL, state);
    memset(state, 0, sizeof(*state));
    state->surface = (VdpVideoSurface)(intptr_t)img->planes[3];

    talloc_steal(state, img);

    struct mp_image *new = mp_image_new_custom_ref(img, state, release_surface);
    new->planes[0] = (void *)state;
    return new;
}

static void uninit(struct lavc_ctx *ctx)
{
    struct priv *p = ctx->hwdec_priv;

    if (!p)
        return;

    if (p->decoder != VDP_INVALID_HANDLE)
        p->vdp->decoder_destroy(p->decoder);

    talloc_free(p);
    ctx->hwdec_priv = NULL;
}

static int init(struct lavc_ctx *ctx)
{
    if (!ctx->hwdec_info || !ctx->hwdec_info->vdpau_ctx)
        return -1;

    struct priv *p = talloc_ptrtype(NULL, p);
    *p = (struct priv) {
        .mpvdp = ctx->hwdec_info->vdpau_ctx,
    };
    ctx->hwdec_priv = p;

    p->preemption_counter = p->mpvdp->preemption_counter;
    mark_uninitialized(ctx);

    if (handle_preemption(ctx) < 0)
        return -1;

    AVCodecContext *avctx = ctx->avctx;

    avctx->draw_horiz_band = draw_slice_hwdec;
    avctx->slice_flags = SLICE_FLAG_CODED_ORDER | SLICE_FLAG_ALLOW_FIELD;

    return 0;
}

static void fix_image(struct lavc_ctx *ctx, struct mp_image *img)
{
    // Make it follow the convention of the "new" vdpau decoder
    struct vdpau_render_state *rndr = (void *)img->planes[0];
    img->planes[0] = (void *)"dummy"; // must be non-NULL, otherwise arbitrary
    img->planes[3] = (void *)(intptr_t)rndr->surface;
}

const struct vd_lavc_hwdec_functions mp_vd_lavc_vdpau_old = {
    .image_formats = (const int[]) {
        IMGFMT_VDPAU_MPEG1, IMGFMT_VDPAU_MPEG2, IMGFMT_VDPAU_H264,
        IMGFMT_VDPAU_WMV3, IMGFMT_VDPAU_VC1, IMGFMT_VDPAU_MPEG4,
        0
    },
    .init = init,
    .uninit = uninit,
    .allocate_image = allocate_image,
    .fix_image = fix_image,
};
