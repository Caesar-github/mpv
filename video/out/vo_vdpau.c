/*
 * VDPAU video output driver
 *
 * Copyright (C) 2008 NVIDIA (Rajib Mahapatra <rmahapatra@nvidia.com>)
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

/*
 * Actual decoding is done in video/decode/vdpau.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <assert.h>

#include <libavutil/common.h>

#include "config.h"
#include "video/vdpau.h"
#include "video/hwdec.h"
#include "common/msg.h"
#include "options/options.h"
#include "talloc.h"
#include "vo.h"
#include "x11_common.h"
#include "video/csputils.h"
#include "sub/osd.h"
#include "options/m_option.h"
#include "video/vfcap.h"
#include "video/mp_image.h"
#include "osdep/timer.h"
#include "bitmap_packer.h"

// Returns x + a, but wrapped around to the range [0, m)
// a must be within [-m, m], x within [0, m)
#define WRAP_ADD(x, a, m) ((a) < 0 \
                           ? ((x)+(a)+(m) < (m) ? (x)+(a)+(m) : (x)+(a)) \
                           : ((x)+(a) < (m) ? (x)+(a) : (x)+(a)-(m)))


/* number of video and output surfaces */
#define MAX_OUTPUT_SURFACES                15
#define NUM_BUFFERED_VIDEO                 5

/* Pixelformat used for output surfaces */
#define OUTPUT_RGBA_FORMAT VDP_RGBA_FORMAT_B8G8R8A8

/*
 * Global variable declaration - VDPAU specific
 */

struct vdpctx {
    struct mp_vdpau_ctx               *mpvdp;
    struct vdp_functions              *vdp;
    VdpDevice                          vdp_device;
    uint64_t                           preemption_counter;

    struct m_color                     colorkey;

    VdpPresentationQueueTarget         flip_target;
    VdpPresentationQueue               flip_queue;

    VdpOutputSurface                   output_surfaces[MAX_OUTPUT_SURFACES];
    VdpOutputSurface                   screenshot_surface;
    int                                num_output_surfaces;
    VdpOutputSurface                   rgb_surfaces[NUM_BUFFERED_VIDEO];
    VdpOutputSurface                   black_pixel;
    struct buffered_video_surface {
        // Either surface or rgb_surface is used (never both)
        VdpVideoSurface surface;
        VdpOutputSurface rgb_surface;
        double pts;
        mp_image_t *mpi;
    } buffered_video[NUM_BUFFERED_VIDEO];
    int                                deint_queue_pos;

    // State for redrawing the screen after seek-reset
    int                                prev_deint_queue_pos;

    int                                output_surface_width, output_surface_height;

    int                                force_yuv;
    VdpVideoMixer                      video_mixer;
    struct mp_csp_details              colorspace;
    int                                user_deint;
    int                                deint;
    int                                deint_type;
    int                                pullup;
    float                              denoise;
    float                              sharpen;
    int                                hqscaling;
    int                                chroma_deint;
    int                                flip_offset_window;
    int                                flip_offset_fs;
    int                                top_field_first;
    bool                               flip;

    VdpRect                            src_rect_vid;
    VdpRect                            out_rect_vid;
    struct mp_osd_res                  osd_rect;

    int                                surface_num; // indexes output_surfaces
    int                                query_surface_num;
    VdpTime                            recent_vsync_time;
    float                              user_fps;
    int                                composite_detect;
    unsigned int                       vsync_interval;
    uint64_t                           last_queue_time;
    uint64_t                           queue_time[MAX_OUTPUT_SURFACES];
    uint64_t                           last_ideal_time;
    bool                               dropped_frame;
    uint64_t                           dropped_time;
    uint32_t                           vid_width, vid_height;
    uint32_t                           image_format;
    VdpChromaType                      vdp_chroma_type;
    VdpYCbCrFormat                     vdp_pixel_format;
    bool                               rgb_mode;

    // OSD
    struct osd_bitmap_surface {
        VdpRGBAFormat format;
        VdpBitmapSurface surface;
        uint32_t max_width;
        uint32_t max_height;
        struct bitmap_packer *packer;
        // List of surfaces to be rendered
        struct osd_target {
            VdpRect source;
            VdpRect dest;
            VdpColor color;
        } *targets;
        int targets_size;
        int render_count;
        int bitmap_id;
        int bitmap_pos_id;
    } osd_surfaces[MAX_OSD_PARTS];

    // Video equalizer
    struct mp_csp_equalizer video_eq;
};

static bool status_ok(struct vo *vo);

static int render_video_to_output_surface(struct vo *vo,
                                          VdpOutputSurface output_surface,
                                          VdpRect *output_rect,
                                          VdpRect *video_rect)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
    VdpTime dummy;
    VdpStatus vdp_st;
    struct buffered_video_surface *bv = vc->buffered_video;
    int dp = vc->deint_queue_pos;

    // Redraw frame from before seek reset?
    if (dp < 0)
        dp = vc->prev_deint_queue_pos;
    if (dp < 0) {
        // At least clear the screen if there is nothing to render
        int flags = VDP_OUTPUT_SURFACE_RENDER_ROTATE_0;
        vdp_st = vdp->output_surface_render_output_surface(output_surface,
                                                           NULL, vc->black_pixel,
                                                           NULL, NULL, NULL,
                                                           flags);
        return -1;
    }

    vdp_st = vdp->presentation_queue_block_until_surface_idle(vc->flip_queue,
                                                              output_surface,
                                                              &dummy);
    CHECK_VDP_WARNING(vo, "Error when calling "
                      "vdp_presentation_queue_block_until_surface_idle");

    if (vc->rgb_mode) {
        int flags = VDP_OUTPUT_SURFACE_RENDER_ROTATE_0;
        vdp_st = vdp->output_surface_render_output_surface(output_surface,
                                                           NULL, vc->black_pixel,
                                                           NULL, NULL, NULL,
                                                           flags);
        CHECK_VDP_WARNING(vo, "Error clearing screen");
        vdp_st = vdp->output_surface_render_output_surface(output_surface,
                                                           output_rect,
                                                           bv[dp/2].rgb_surface,
                                                           video_rect,
                                                           NULL, NULL, flags);
        CHECK_VDP_WARNING(vo, "Error when calling "
                          "vdp_output_surface_render_output_surface");
        return 0;
    }

    int field = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME;
    // dp==0 means last field of latest frame, 1 earlier field of latest frame,
    // 2 last field of previous frame and so on
    if (vc->deint) {
        field = vc->top_field_first ^ (dp & 1) ?
            VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD:
            VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD;
    }
    const VdpVideoSurface *past_fields = (const VdpVideoSurface []){
        bv[(dp+1)/2].surface, bv[(dp+2)/2].surface};
    const VdpVideoSurface *future_fields = (const VdpVideoSurface []){
        dp >= 1 ? bv[(dp-1)/2].surface : VDP_INVALID_HANDLE};

    vdp_st = vdp->video_mixer_render(vc->video_mixer, VDP_INVALID_HANDLE,
                                     0, field, 2, past_fields,
                                     bv[dp/2].surface, 1, future_fields,
                                     video_rect, output_surface,
                                     NULL, output_rect, 0, NULL);
    CHECK_VDP_WARNING(vo, "Error when calling vdp_video_mixer_render");
    return 0;
}

static int video_to_output_surface(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;

    return render_video_to_output_surface(vo,
                                          vc->output_surfaces[vc->surface_num],
                                          &vc->out_rect_vid, &vc->src_rect_vid);
}

static int next_deint_queue_pos(struct vo *vo, bool eof)
{
    struct vdpctx *vc = vo->priv;

    int dqp = vc->deint_queue_pos;
    if (dqp < 0)
        dqp += 1000;
    else
        dqp = vc->deint >= 2 ? dqp - 1 : dqp - 2 | 1;
    if (dqp < (eof ? 0 : 3))
        return -1;
    return dqp;
}

static void set_next_frame_info(struct vo *vo, bool eof)
{
    struct vdpctx *vc = vo->priv;

    vo->frame_loaded = false;
    int dqp = next_deint_queue_pos(vo, eof);
    if (dqp < 0)
        return;
    vo->frame_loaded = true;

    // Set pts values
    struct buffered_video_surface *bv = vc->buffered_video;
    int idx = dqp >> 1;
    if (idx == 0) {  // no future frame/pts available
        vo->next_pts = bv[0].pts;
        vo->next_pts2 = MP_NOPTS_VALUE;
    } else if (!(vc->deint >= 2)) {    // no field-splitting deinterlace
        vo->next_pts = bv[idx].pts;
        vo->next_pts2 = bv[idx - 1].pts;
    } else {  // deinterlace with separate fields
        double intermediate_pts;
        double diff = bv[idx - 1].pts - bv[idx].pts;
        if (diff > 0 && diff < 0.5)
            intermediate_pts = (bv[idx].pts + bv[idx - 1].pts) / 2;
        else
            intermediate_pts =  bv[idx].pts;
        if (dqp & 1) { // first field
            vo->next_pts = bv[idx].pts;
            vo->next_pts2 = intermediate_pts;
        } else {
            vo->next_pts = intermediate_pts;
            vo->next_pts2 = bv[idx - 1].pts;
        }
    }
}

static void add_new_video_surface(struct vo *vo, VdpVideoSurface surface,
                                  VdpOutputSurface rgb_surface,
                                  struct mp_image *reserved_mpi, double pts)
{
    struct vdpctx *vc = vo->priv;
    struct buffered_video_surface *bv = vc->buffered_video;

    mp_image_unrefp(&bv[NUM_BUFFERED_VIDEO - 1].mpi);

    for (int i = NUM_BUFFERED_VIDEO - 1; i > 0; i--)
        bv[i] = bv[i - 1];
    bv[0] = (struct buffered_video_surface){
        .mpi = reserved_mpi,
        .surface = surface,
        .rgb_surface = rgb_surface,
        .pts = pts,
    };

    vc->deint_queue_pos = FFMIN(vc->deint_queue_pos + 2,
                                NUM_BUFFERED_VIDEO * 2 - 3);
    set_next_frame_info(vo, false);
}

static void forget_frames(struct vo *vo, bool seek_reset)
{
    struct vdpctx *vc = vo->priv;

    if (seek_reset) {
        if (vc->deint_queue_pos >= 0)
            vc->prev_deint_queue_pos = vc->deint_queue_pos;
    } else {
        vc->prev_deint_queue_pos = -1001;
    }

    vc->deint_queue_pos = -1001;
    vc->dropped_frame = false;
    if (vc->prev_deint_queue_pos < 0) {
        for (int i = 0; i < NUM_BUFFERED_VIDEO; i++) {
            struct buffered_video_surface *p = vc->buffered_video + i;
            mp_image_unrefp(&p->mpi);
            *p = (struct buffered_video_surface){
                .surface = VDP_INVALID_HANDLE,
                .rgb_surface = VDP_INVALID_HANDLE,
            };
        }
    }
}

static void resize(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
    VdpStatus vdp_st;
    struct mp_rect src_rect;
    struct mp_rect dst_rect;
    vo_get_src_dst_rects(vo, &src_rect, &dst_rect, &vc->osd_rect);
    vc->out_rect_vid.x0 = dst_rect.x0;
    vc->out_rect_vid.x1 = dst_rect.x1;
    vc->out_rect_vid.y0 = dst_rect.y0;
    vc->out_rect_vid.y1 = dst_rect.y1;
    vc->src_rect_vid.x0 = src_rect.x0;
    vc->src_rect_vid.x1 = src_rect.x1;
    vc->src_rect_vid.y0 = vc->flip ? src_rect.y1 : src_rect.y0;
    vc->src_rect_vid.y1 = vc->flip ? src_rect.y0 : src_rect.y1;

    int flip_offset_ms = vo->opts->fullscreen ?
                         vc->flip_offset_fs :
                         vc->flip_offset_window;

    vo->flip_queue_offset = flip_offset_ms / 1000.;

    if (vc->output_surface_width < vo->dwidth
        || vc->output_surface_height < vo->dheight) {
        if (vc->output_surface_width < vo->dwidth) {
            vc->output_surface_width += vc->output_surface_width >> 1;
            vc->output_surface_width = FFMAX(vc->output_surface_width,
                                             vo->dwidth);
        }
        if (vc->output_surface_height < vo->dheight) {
            vc->output_surface_height += vc->output_surface_height >> 1;
            vc->output_surface_height = FFMAX(vc->output_surface_height,
                                              vo->dheight);
        }
        // Creation of output_surfaces
        for (int i = 0; i < vc->num_output_surfaces; i++)
            if (vc->output_surfaces[i] != VDP_INVALID_HANDLE) {
                vdp_st = vdp->output_surface_destroy(vc->output_surfaces[i]);
                CHECK_VDP_WARNING(vo, "Error when calling "
                                  "vdp_output_surface_destroy");
            }
        for (int i = 0; i < vc->num_output_surfaces; i++) {
            vdp_st = vdp->output_surface_create(vc->vdp_device,
                                                OUTPUT_RGBA_FORMAT,
                                                vc->output_surface_width,
                                                vc->output_surface_height,
                                                &vc->output_surfaces[i]);
            CHECK_VDP_WARNING(vo, "Error when calling vdp_output_surface_create");
            MP_DBG(vo, "vdpau out create: %u\n",
                   vc->output_surfaces[i]);
        }
    }
    vo->want_redraw = true;
}

static int win_x11_init_vdpau_flip_queue(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
    struct vo_x11_state *x11 = vo->x11;
    VdpStatus vdp_st;

    if (vc->flip_target == VDP_INVALID_HANDLE) {
        vdp_st = vdp->presentation_queue_target_create_x11(vc->vdp_device,
                                                           x11->window,
                                                           &vc->flip_target);
        CHECK_VDP_ERROR(vo, "Error when calling "
                        "vdp_presentation_queue_target_create_x11");
    }

    /* Emperically this seems to be the first call which fails when we
     * try to reinit after preemption while the user is still switched
     * from X to a virtual terminal (creating the vdp_device initially
     * succeeds, as does creating the flip_target above). This is
     * probably not guaranteed behavior, but we'll assume it as a simple
     * way to reduce warnings while trying to recover from preemption.
     */
    if (vc->flip_queue == VDP_INVALID_HANDLE) {
        vdp_st = vdp->presentation_queue_create(vc->vdp_device, vc->flip_target,
                                                &vc->flip_queue);
        if (vc->mpvdp->is_preempted && vdp_st != VDP_STATUS_OK) {
            MP_DBG(vo, "Failed to create flip queue while preempted: %s\n",
                   vdp->get_error_string(vdp_st));
            return -1;
        } else
            CHECK_VDP_ERROR(vo, "Error when calling vdp_presentation_queue_create");
    }

    if (vc->colorkey.a > 0) {
        VdpColor color = {
            .red = vc->colorkey.r / 255.0,
            .green = vc->colorkey.g / 255.0,
            .blue = vc->colorkey.b / 255.0,
            .alpha = 0,
        };
        vdp_st = vdp->presentation_queue_set_background_color(vc->flip_queue,
                                                              &color);
        CHECK_VDP_WARNING(vo, "Error setting colorkey");
    }

    vc->vsync_interval = 1;
    if (vc->composite_detect && vo_x11_screen_is_composited(vo)) {
        MP_INFO(vo, "Compositing window manager detected. Assuming timing info "
                "is inaccurate.\n");
    } else if (vc->user_fps > 0) {
        vc->vsync_interval = 1e9 / vc->user_fps;
        MP_INFO(vo, "Assuming user-specified display refresh rate of %.3f Hz.\n",
                vc->user_fps);
    } else if (vc->user_fps == 0) {
#if HAVE_XF86VM
        double fps = vo_x11_vm_get_fps(vo);
        if (!fps)
            MP_WARN(vo, "Failed to get display FPS\n");
        else {
            vc->vsync_interval = 1e9 / fps;
            // This is verbose, but I'm not yet sure how common wrong values are
            MP_INFO(vo, "Got display refresh rate %.3f Hz.\n", fps);
            MP_INFO(vo, "If that value looks wrong give the "
                    "-vo vdpau:fps=X suboption manually.\n");
        }
#else
        MP_INFO(vo, "This binary has been compiled without XF86VidMode support.\n");
        MP_INFO(vo, "Can't use vsync-aware timing without manually provided "
                "-vo vdpau:fps=X suboption.\n");
#endif
    } else
        MP_VERBOSE(vo, "framedrop/timing logic disabled by user.\n");

    return 0;
}

static int set_video_attribute(struct vo *vo, VdpVideoMixerAttribute attr,
                               const void *value, char *attr_name)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
    VdpStatus vdp_st;

    if (vc->rgb_mode)
        return -1;

    vdp_st = vdp->video_mixer_set_attribute_values(vc->video_mixer, 1, &attr,
                                                   &value);
    if (vdp_st != VDP_STATUS_OK) {
        MP_ERR(vo, "Error setting video mixer attribute %s: %s\n", attr_name,
               vdp->get_error_string(vdp_st));
        return -1;
    }
    return 0;
}

static void update_csc_matrix(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;

    MP_VERBOSE(vo, "Updating CSC matrix\n");

    // VdpCSCMatrix happens to be compatible with mplayer's CSC matrix type
    // both are float[3][4]
    VdpCSCMatrix matrix;

    struct mp_csp_params cparams = {
        .colorspace = vc->colorspace, .input_bits = 8, .texture_bits = 8 };
    mp_csp_copy_equalizer_values(&cparams, &vc->video_eq);
    mp_get_yuv2rgb_coeffs(&cparams, matrix);

    set_video_attribute(vo, VDP_VIDEO_MIXER_ATTRIBUTE_CSC_MATRIX,
                        &matrix, "CSC matrix");
}

#define SET_VIDEO_ATTR(attr_name, attr_type, value) set_video_attribute(vo, \
                 VDP_VIDEO_MIXER_ATTRIBUTE_ ## attr_name, &(attr_type){value},\
                 # attr_name)
static int create_vdp_mixer(struct vo *vo, VdpChromaType vdp_chroma_type)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
#define VDP_NUM_MIXER_PARAMETER 3
#define MAX_NUM_FEATURES 6
    int i;
    VdpStatus vdp_st;

    if (vc->video_mixer != VDP_INVALID_HANDLE)
        return 0;

    int feature_count = 0;
    VdpVideoMixerFeature features[MAX_NUM_FEATURES];
    VdpBool feature_enables[MAX_NUM_FEATURES];
    static const VdpVideoMixerParameter parameters[VDP_NUM_MIXER_PARAMETER] = {
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH,
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT,
        VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE,
    };
    const void *const parameter_values[VDP_NUM_MIXER_PARAMETER] = {
        &vc->vid_width,
        &vc->vid_height,
        &vdp_chroma_type,
    };
    features[feature_count++] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL;
    if (vc->deint_type == 4)
        features[feature_count++] =
            VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL;
    if (vc->pullup)
        features[feature_count++] = VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE;
    if (vc->denoise)
        features[feature_count++] = VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION;
    if (vc->sharpen)
        features[feature_count++] = VDP_VIDEO_MIXER_FEATURE_SHARPNESS;
    if (vc->hqscaling) {
        VdpVideoMixerFeature hqscaling_feature =
            VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + vc->hqscaling-1;
        VdpBool hqscaling_available;
        vdp_st = vdp->video_mixer_query_feature_support(vc->vdp_device,
                                                        hqscaling_feature,
                                                        &hqscaling_available);
        CHECK_VDP_ERROR(vo, "Error when calling video_mixer_query_feature_support");
        if (hqscaling_available)
            features[feature_count++] = hqscaling_feature;
        else
            MP_ERR(vo, "Your hardware or VDPAU library does not support "
                   "requested hqscaling.\n");
    }

    vdp_st = vdp->video_mixer_create(vc->vdp_device, feature_count, features,
                                     VDP_NUM_MIXER_PARAMETER,
                                     parameters, parameter_values,
                                     &vc->video_mixer);
    CHECK_VDP_ERROR(vo, "Error when calling vdp_video_mixer_create");

    for (i = 0; i < feature_count; i++)
        feature_enables[i] = VDP_TRUE;
    if (vc->deint < 3)
        feature_enables[0] = VDP_FALSE;
    if (vc->deint_type == 4 && vc->deint < 4)
        feature_enables[1] = VDP_FALSE;
    if (feature_count) {
        vdp_st = vdp->video_mixer_set_feature_enables(vc->video_mixer,
                                                      feature_count, features,
                                                      feature_enables);
        CHECK_VDP_WARNING(vo, "Error calling vdp_video_mixer_set_feature_enables");
    }
    if (vc->denoise)
        SET_VIDEO_ATTR(NOISE_REDUCTION_LEVEL, float, vc->denoise);
    if (vc->sharpen)
        SET_VIDEO_ATTR(SHARPNESS_LEVEL, float, vc->sharpen);
    if (!vc->chroma_deint)
        SET_VIDEO_ATTR(SKIP_CHROMA_DEINTERLACE, uint8_t, 1);

    update_csc_matrix(vo);
    return 0;
}

// Free everything specific to a certain video file
static void free_video_specific(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
    VdpStatus vdp_st;

    forget_frames(vo, false);

    if (vc->video_mixer != VDP_INVALID_HANDLE) {
        vdp_st = vdp->video_mixer_destroy(vc->video_mixer);
        CHECK_VDP_WARNING(vo, "Error when calling vdp_video_mixer_destroy");
    }
    vc->video_mixer = VDP_INVALID_HANDLE;

    if (vc->screenshot_surface != VDP_INVALID_HANDLE) {
        vdp_st = vdp->output_surface_destroy(vc->screenshot_surface);
        CHECK_VDP_WARNING(vo, "Error when calling vdp_output_surface_destroy");
    }
    vc->screenshot_surface = VDP_INVALID_HANDLE;

    for (int n = 0; n < NUM_BUFFERED_VIDEO; n++) {
        if (vc->rgb_surfaces[n] != VDP_INVALID_HANDLE) {
            vdp_st = vdp->output_surface_destroy(vc->rgb_surfaces[n]);
            CHECK_VDP_WARNING(vo, "Error when calling vdp_output_surface_destroy");
        }
        vc->rgb_surfaces[n] = VDP_INVALID_HANDLE;
    }

    if (vc->black_pixel != VDP_INVALID_HANDLE) {
        vdp_st = vdp->output_surface_destroy(vc->black_pixel);
        CHECK_VDP_WARNING(vo, "Error when calling vdp_output_surface_destroy");
    }
    vc->black_pixel = VDP_INVALID_HANDLE;
}

static int get_rgb_format(int imgfmt)
{
    switch (imgfmt) {
    case IMGFMT_BGR32: return VDP_RGBA_FORMAT_B8G8R8A8;
    default:           return -1;
    }
}

static int initialize_vdpau_objects(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
    VdpStatus vdp_st;

    mp_vdpau_get_format(vc->image_format, &vc->vdp_chroma_type,
                        &vc->vdp_pixel_format);

    if (win_x11_init_vdpau_flip_queue(vo) < 0)
        return -1;

    if (vc->rgb_mode) {
        int format = get_rgb_format(vc->image_format);
        for (int n = 0; n < NUM_BUFFERED_VIDEO; n++) {
            vdp_st = vdp->output_surface_create(vc->vdp_device,
                                                format,
                                                vc->vid_width, vc->vid_height,
                                                &vc->rgb_surfaces[n]);
            CHECK_VDP_ERROR(vo, "Allocating RGB surface");
        }
        vdp_st = vdp->output_surface_create(vc->vdp_device, OUTPUT_RGBA_FORMAT,
                                            1, 1, &vc->black_pixel);
        CHECK_VDP_ERROR(vo, "Allocating clearing surface");
        const char data[4] = {0};
        vdp_st = vdp->output_surface_put_bits_native(vc->black_pixel,
                                                     (const void*[]){data},
                                                     (uint32_t[]){4}, NULL);
        CHECK_VDP_ERROR(vo, "Initializing clearing surface");
    } else {
        if (create_vdp_mixer(vo, vc->vdp_chroma_type) < 0)
            return -1;
    }

    forget_frames(vo, false);
    resize(vo);
    return 0;
}

static void mark_vdpau_objects_uninitialized(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;

    for (int i = 0; i < NUM_BUFFERED_VIDEO; i++)
        vc->rgb_surfaces[i] = VDP_INVALID_HANDLE;
    forget_frames(vo, false);
    vc->black_pixel = VDP_INVALID_HANDLE;
    vc->video_mixer = VDP_INVALID_HANDLE;
    vc->flip_queue = VDP_INVALID_HANDLE;
    vc->flip_target = VDP_INVALID_HANDLE;
    for (int i = 0; i < MAX_OUTPUT_SURFACES; i++)
        vc->output_surfaces[i] = VDP_INVALID_HANDLE;
    vc->screenshot_surface = VDP_INVALID_HANDLE;
    vc->vdp_device = VDP_INVALID_HANDLE;
    for (int i = 0; i < MAX_OSD_PARTS; i++) {
        struct osd_bitmap_surface *sfc = &vc->osd_surfaces[i];
        talloc_free(sfc->packer);
        sfc->bitmap_id = sfc->bitmap_pos_id = 0;
        *sfc = (struct osd_bitmap_surface){
            .surface = VDP_INVALID_HANDLE,
        };
    }
    vc->output_surface_width = vc->output_surface_height = -1;
}

static int handle_preemption(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;

    if (!mp_vdpau_status_ok(vc->mpvdp)) {
        mark_vdpau_objects_uninitialized(vo);
        return -1;
    }

    if (vc->preemption_counter == vc->mpvdp->preemption_counter)
        return 0;

    mark_vdpau_objects_uninitialized(vo);

    vc->preemption_counter = vc->mpvdp->preemption_counter;
    vc->vdp_device = vc->mpvdp->vdp_device;

    if (initialize_vdpau_objects(vo) < 0)
        return -1;

    return 1;
}

static bool status_ok(struct vo *vo)
{
    return vo->config_ok && handle_preemption(vo) >= 0;
}

/*
 * connect to X server, create and map window, initialize all
 * VDPAU objects, create different surfaces etc.
 */
static int config(struct vo *vo, uint32_t width, uint32_t height,
                  uint32_t d_width, uint32_t d_height, uint32_t flags,
                  uint32_t format)
{
    struct vdpctx *vc = vo->priv;

    if (handle_preemption(vo) < 0)
        return -1;

    vc->flip = flags & VOFLAG_FLIPPING;
    vc->image_format = format;
    vc->vid_width    = width;
    vc->vid_height   = height;

    vc->rgb_mode = get_rgb_format(format) >= 0;

    vc->deint = vc->rgb_mode ? 0 : vc->user_deint;

    free_video_specific(vo);

    vo_x11_config_vo_window(vo, NULL, vo->dx, vo->dy, d_width, d_height,
                            flags, "vdpau");

    if (initialize_vdpau_objects(vo) < 0)
        return -1;

    return 0;
}

static struct bitmap_packer *make_packer(struct vo *vo, VdpRGBAFormat format)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;

    struct bitmap_packer *packer = talloc_zero(vo, struct bitmap_packer);
    uint32_t w_max = 0, h_max = 0;
    VdpStatus vdp_st = vdp->
        bitmap_surface_query_capabilities(vc->vdp_device, format,
                                          &(VdpBool){0}, &w_max, &h_max);
    CHECK_VDP_WARNING(vo, "Query to get max OSD surface size failed");
    packer->w_max = w_max;
    packer->h_max = h_max;
    return packer;
}

static void draw_osd_part(struct vo *vo, int index)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
    VdpStatus vdp_st;
    struct osd_bitmap_surface *sfc = &vc->osd_surfaces[index];
    VdpOutputSurface output_surface = vc->output_surfaces[vc->surface_num];
    int i;

    VdpOutputSurfaceRenderBlendState blend_state = {
        .struct_version = VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION,
        .blend_factor_source_color =
            VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_SRC_ALPHA,
        .blend_factor_source_alpha =
            VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ZERO,
        .blend_factor_destination_color =
            VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .blend_factor_destination_alpha =
            VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ZERO,
        .blend_equation_color = VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
        .blend_equation_alpha = VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
    };

    VdpOutputSurfaceRenderBlendState blend_state_premultiplied = blend_state;
    blend_state_premultiplied.blend_factor_source_color =
            VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE;

    for (i = 0; i < sfc->render_count; i++) {
        VdpOutputSurfaceRenderBlendState *blend = &blend_state;
        if (sfc->format == VDP_RGBA_FORMAT_B8G8R8A8)
            blend = &blend_state_premultiplied;
        vdp_st = vdp->
            output_surface_render_bitmap_surface(output_surface,
                                                 &sfc->targets[i].dest,
                                                 sfc->surface,
                                                 &sfc->targets[i].source,
                                                 &sfc->targets[i].color,
                                                 blend,
                                                 VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
        CHECK_VDP_WARNING(vo, "OSD: Error when rendering");
    }
}

static void generate_osd_part(struct vo *vo, struct sub_bitmaps *imgs)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
    VdpStatus vdp_st;
    struct osd_bitmap_surface *sfc = &vc->osd_surfaces[imgs->render_index];
    bool need_upload = false;

    if (imgs->bitmap_pos_id == sfc->bitmap_pos_id)
        return; // Nothing changed and we still have the old data

    sfc->render_count = 0;

    if (imgs->format == SUBBITMAP_EMPTY || imgs->num_parts == 0)
        return;

    if (imgs->bitmap_id == sfc->bitmap_id)
        goto osd_skip_upload;

    need_upload = true;
    VdpRGBAFormat format;
    int format_size;
    switch (imgs->format) {
    case SUBBITMAP_LIBASS:
        format = VDP_RGBA_FORMAT_A8;
        format_size = 1;
        break;
    case SUBBITMAP_RGBA:
        format = VDP_RGBA_FORMAT_B8G8R8A8;
        format_size = 4;
        break;
    default:
        abort();
    };
    if (sfc->format != format) {
        talloc_free(sfc->packer);
        sfc->packer = NULL;
    };
    sfc->format = format;
    if (!sfc->packer)
        sfc->packer = make_packer(vo, format);
    sfc->packer->padding = imgs->scaled; // assume 2x2 filter on scaling
    int r = packer_pack_from_subbitmaps(sfc->packer, imgs);
    if (r < 0) {
        MP_ERR(vo, "OSD bitmaps do not fit on a surface with the maximum "
               "supported size\n");
        return;
    } else if (r == 1) {
        if (sfc->surface != VDP_INVALID_HANDLE) {
            vdp_st = vdp->bitmap_surface_destroy(sfc->surface);
            CHECK_VDP_WARNING(vo, "Error when calling vdp_bitmap_surface_destroy");
        }
        MP_VERBOSE(vo, "Allocating a %dx%d surface for OSD bitmaps.\n",
                   sfc->packer->w, sfc->packer->h);
        vdp_st = vdp->bitmap_surface_create(vc->vdp_device, format,
                                            sfc->packer->w, sfc->packer->h,
                                            true, &sfc->surface);
        if (vdp_st != VDP_STATUS_OK)
            sfc->surface = VDP_INVALID_HANDLE;
        CHECK_VDP_WARNING(vo, "OSD: error when creating surface");
    }
    if (imgs->scaled) {
        char zeros[sfc->packer->used_width * format_size];
        memset(zeros, 0, sizeof(zeros));
        vdp_st = vdp->bitmap_surface_put_bits_native(sfc->surface,
                &(const void *){zeros}, &(uint32_t){0},
                &(VdpRect){0, 0, sfc->packer->used_width,
                                 sfc->packer->used_height});
    }

osd_skip_upload:
    if (sfc->surface == VDP_INVALID_HANDLE)
        return;
    if (sfc->packer->count > sfc->targets_size) {
        talloc_free(sfc->targets);
        sfc->targets_size = sfc->packer->count;
        sfc->targets = talloc_size(vc, sfc->targets_size
                                       * sizeof(*sfc->targets));
    }

    for (int i = 0 ;i < sfc->packer->count; i++) {
        struct sub_bitmap *b = &imgs->parts[i];
        struct osd_target *target = sfc->targets + sfc->render_count;
        int x = sfc->packer->result[i].x;
        int y = sfc->packer->result[i].y;
        target->source = (VdpRect){x, y, x + b->w, y + b->h};
        target->dest = (VdpRect){b->x, b->y, b->x + b->dw, b->y + b->dh};
        target->color = (VdpColor){1, 1, 1, 1};
        if (imgs->format == SUBBITMAP_LIBASS) {
            uint32_t color = b->libass.color;
            target->color.alpha = 1.0 - ((color >> 0) & 0xff) / 255.0;
            target->color.blue  = ((color >>  8) & 0xff) / 255.0;
            target->color.green = ((color >> 16) & 0xff) / 255.0;
            target->color.red   = ((color >> 24) & 0xff) / 255.0;
        }
        if (need_upload) {
            vdp_st = vdp->
                bitmap_surface_put_bits_native(sfc->surface,
                                               &(const void *){b->bitmap},
                                               &(uint32_t){b->stride},
                                               &target->source);
                CHECK_VDP_WARNING(vo, "OSD: putbits failed");
        }
        sfc->render_count++;
    }

    sfc->bitmap_id = imgs->bitmap_id;
    sfc->bitmap_pos_id = imgs->bitmap_pos_id;
}

static void draw_osd_cb(void *ctx, struct sub_bitmaps *imgs)
{
    struct vo *vo = ctx;
    generate_osd_part(vo, imgs);
    draw_osd_part(vo, imgs->render_index);
}

static void draw_osd(struct vo *vo, struct osd_state *osd)
{
    struct vdpctx *vc = vo->priv;

    if (!status_ok(vo))
        return;

    static const bool formats[SUBBITMAP_COUNT] = {
        [SUBBITMAP_LIBASS] = true,
        [SUBBITMAP_RGBA] = true,
    };

    osd_draw(osd, vc->osd_rect, osd->vo_pts, 0, formats, draw_osd_cb, vo);
}

static int update_presentation_queue_status(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
    VdpStatus vdp_st;

    while (vc->query_surface_num != vc->surface_num) {
        VdpTime vtime;
        VdpPresentationQueueStatus status;
        VdpOutputSurface surface = vc->output_surfaces[vc->query_surface_num];
        vdp_st = vdp->presentation_queue_query_surface_status(vc->flip_queue,
                                                              surface,
                                                              &status, &vtime);
        CHECK_VDP_WARNING(vo, "Error calling "
                         "presentation_queue_query_surface_status");
        if (mp_msg_test(vo->log, MSGL_TRACE)) {
            VdpTime current;
            vdp_st = vdp->presentation_queue_get_time(vc->flip_queue, &current);
            CHECK_VDP_WARNING(vo, "Error when calling "
                              "vdp_presentation_queue_get_time");
            MP_TRACE(vo, "Vdpau time: %"PRIu64"\n", (uint64_t)current);
            MP_TRACE(vo, "Surface %d status: %d time: %"PRIu64"\n",
                     (int)surface, (int)status, (uint64_t)vtime);
        }
        if (status == VDP_PRESENTATION_QUEUE_STATUS_QUEUED)
            break;
        if (vc->vsync_interval > 1) {
            uint64_t qtime = vc->queue_time[vc->query_surface_num];
            int diff = ((int64_t)vtime - (int64_t)qtime) / 1e6;
            MP_TRACE(vo, "Queue time difference: %d ms\n", diff);
            if (vtime < qtime + vc->vsync_interval / 2)
                MP_VERBOSE(vo, "Frame shown too early (%d ms)\n", diff);
            if (vtime > qtime + vc->vsync_interval)
                MP_VERBOSE(vo, "Frame shown late (%d ms)\n", diff);
        }
        vc->query_surface_num = WRAP_ADD(vc->query_surface_num, 1,
                                         vc->num_output_surfaces);
        vc->recent_vsync_time = vtime;
    }
    int num_queued = WRAP_ADD(vc->surface_num, -vc->query_surface_num,
                              vc->num_output_surfaces);
    MP_DBG(vo, "Queued surface count (before add): %d\n", num_queued);
    return num_queued;
}

// Return the timestamp of the vsync that must have happened before ts.
static inline uint64_t prev_vsync(struct vdpctx *vc, uint64_t ts)
{
    int64_t diff = (int64_t)(ts - vc->recent_vsync_time);
    int64_t offset = diff % vc->vsync_interval;
    if (offset < 0)
        offset += vc->vsync_interval;
    return ts - offset;
}

static void flip_page_timed(struct vo *vo, int64_t pts_us, int duration)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
    VdpStatus vdp_st;
    uint32_t vsync_interval = vc->vsync_interval;

    if (handle_preemption(vo) < 0)
        return;

    if (duration > INT_MAX / 1000)
        duration = -1;
    else
        duration *= 1000;

    if (vc->vsync_interval == 1)
        duration = -1;  // Make sure drop logic is disabled

    VdpTime vdp_time = 0;
    vdp_st = vdp->presentation_queue_get_time(vc->flip_queue, &vdp_time);
    CHECK_VDP_WARNING(vo, "Error when calling vdp_presentation_queue_get_time");

    int64_t rel_pts_ns = (pts_us - mp_time_us()) * 1000;
    if (!pts_us || rel_pts_ns < 0)
        rel_pts_ns = 0;

    uint64_t now = vdp_time;
    uint64_t pts = now + rel_pts_ns;
    uint64_t ideal_pts = pts;
    uint64_t npts = duration >= 0 ? pts + duration : UINT64_MAX;

    /* This should normally never happen.
     * - The last queued frame can't have a PTS that goes more than 50ms in the
     *   future. This is guaranteed by the playloop, which currently actually
     *   roughly queues 50ms ahead, plus the flip queue offset. Just to be sure
     *   give some additional room by doubling the time.
     * - The last vsync can never be in the future.
     */
    int64_t max_pts_ahead = (vo->flip_queue_offset + 0.050) * 2 * 1e9;
    if (vc->last_queue_time > now + max_pts_ahead ||
        vc->recent_vsync_time > now)
    {
        vc->last_queue_time = 0;
        vc->recent_vsync_time = 0;
        MP_WARN(vo, "Inconsistent timing detected.\n");
    }

#define PREV_VSYNC(ts) prev_vsync(vc, ts)

    /* We hope to be here at least one vsync before the frame should be shown.
     * If we are running late then don't drop the frame unless there is
     * already one queued for the next vsync; even if we _hope_ to show the
     * next frame soon enough to mean this one should be dropped we might
     * not make the target time in reality. Without this check we could drop
     * every frame, freezing the display completely if video lags behind.
     */
    if (now > PREV_VSYNC(FFMAX(pts, vc->last_queue_time + vsync_interval)))
        npts = UINT64_MAX;

    /* Allow flipping a frame at a vsync if its presentation time is a
     * bit after that vsync and the change makes the flip time delta
     * from previous frame better match the target timestamp delta.
     * This avoids instability with frame timestamps falling near vsyncs.
     * For example if the frame timestamps were (with vsyncs at
     * integer values) 0.01, 1.99, 4.01, 5.99, 8.01, ... then
     * straightforward timing at next vsync would flip the frames at
     * 1, 2, 5, 6, 9; this changes it to 1, 2, 4, 6, 8 and so on with
     * regular 2-vsync intervals.
     *
     * Also allow moving the frame forward if it looks like we dropped
     * the previous frame incorrectly (now that we know better after
     * having final exact timestamp information for this frame) and
     * there would unnecessarily be a vsync without a frame change.
     */
    uint64_t vsync = PREV_VSYNC(pts);
    if (pts < vsync + vsync_interval / 4
        && (vsync - PREV_VSYNC(vc->last_queue_time)
            > pts - vc->last_ideal_time + vsync_interval / 2
            || vc->dropped_frame && vsync > vc->dropped_time))
        pts -= vsync_interval / 2;

    vc->dropped_frame = true; // changed at end if false
    vc->dropped_time = ideal_pts;

    pts = FFMAX(pts, vc->last_queue_time + vsync_interval);
    pts = FFMAX(pts, now);
    if (npts < PREV_VSYNC(pts) + vsync_interval)
        return;

    int num_flips = update_presentation_queue_status(vo);
    vsync = vc->recent_vsync_time + num_flips * vc->vsync_interval;
    pts = FFMAX(pts, now);
    pts = FFMAX(pts, vsync + (vsync_interval >> 2));
    vsync = PREV_VSYNC(pts);
    if (npts < vsync + vsync_interval)
        return;
    pts = vsync + (vsync_interval >> 2);
    VdpOutputSurface frame = vc->output_surfaces[vc->surface_num];
    vdp_st = vdp->presentation_queue_display(vc->flip_queue, frame,
                                             vo->dwidth, vo->dheight, pts);
    CHECK_VDP_WARNING(vo, "Error when calling vdp_presentation_queue_display");

    MP_TRACE(vo, "Queue new surface %d: Vdpau time: %"PRIu64" "
             "pts: %"PRIu64"\n", (int)frame, now, pts);

    vc->last_queue_time = pts;
    vc->queue_time[vc->surface_num] = pts;
    vc->last_ideal_time = ideal_pts;
    vc->dropped_frame = false;
    vc->surface_num = WRAP_ADD(vc->surface_num, 1, vc->num_output_surfaces);
}

static VdpOutputSurface get_rgb_surface(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;

    assert(vc->rgb_mode);

    for (int n = 0; n < NUM_BUFFERED_VIDEO; n++) {
        VdpOutputSurface surface = vc->rgb_surfaces[n];
        // Note: we expect to be called before add_new_video_surface(), which
        //       will lead to vc->buffered_video[NUM_BUFFERED_VIDEO - 1] to be
        //       marked unused. So this entries rgb_surface can be reused
        //       freely.
        for (int i = 0; i < NUM_BUFFERED_VIDEO - 1; i++) {
            if (vc->buffered_video[i].rgb_surface == surface)
                goto in_use;
        }
        return surface;
    in_use:;
    }

    MP_ERR(vo, "no surfaces available in get_rgb_surface\n");
    return VDP_INVALID_HANDLE;
}

static void draw_image(struct vo *vo, mp_image_t *mpi)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;
    struct mp_image *reserved_mpi = NULL;
    VdpVideoSurface surface = VDP_INVALID_HANDLE;
    VdpOutputSurface rgb_surface = VDP_INVALID_HANDLE;
    VdpStatus vdp_st;

    // Forget previous frames, as we can display a new one now.
    vc->prev_deint_queue_pos = -1001;

    if (vc->image_format == IMGFMT_VDPAU) {
        surface = (VdpVideoSurface)(intptr_t)mpi->planes[3];
        reserved_mpi = mp_image_new_ref(mpi);
    } else if (vc->rgb_mode) {
        rgb_surface = get_rgb_surface(vo);
        if (rgb_surface != VDP_INVALID_HANDLE) {
            vdp_st = vdp->output_surface_put_bits_native(rgb_surface,
                                            &(const void *){mpi->planes[0]},
                                            &(uint32_t){mpi->stride[0]},
                                            NULL);
            CHECK_VDP_WARNING(vo, "Error when calling "
                              "output_surface_put_bits_native");
        }
    } else {
        reserved_mpi = mp_vdpau_get_video_surface(vc->mpvdp, IMGFMT_VDPAU,
                                                  vc->vdp_chroma_type,
                                                  mpi->w, mpi->h);
        if (!reserved_mpi)
            return;
        surface = (VdpVideoSurface)(intptr_t)reserved_mpi->planes[3];
        if (handle_preemption(vo) >= 0) {
            const void *destdata[3] = {mpi->planes[0], mpi->planes[2],
                                       mpi->planes[1]};
            if (vc->image_format == IMGFMT_NV12)
                destdata[1] = destdata[2];
            vdp_st = vdp->video_surface_put_bits_y_cb_cr(surface,
                    vc->vdp_pixel_format, destdata, mpi->stride);
            CHECK_VDP_WARNING(vo, "Error when calling "
                              "vdp_video_surface_put_bits_y_cb_cr");
        }
    }
    if (mpi->fields & MP_IMGFIELD_ORDERED)
        vc->top_field_first = !!(mpi->fields & MP_IMGFIELD_TOP_FIRST);
    else
        vc->top_field_first = 1;

    add_new_video_surface(vo, surface, rgb_surface, reserved_mpi, mpi->pts);

    return;
}

// warning: the size and pixel format of surface must match that of the
//          surfaces in vc->output_surfaces
static struct mp_image *read_output_surface(struct vo *vo,
                                            VdpOutputSurface surface,
                                            int width, int height)
{
    struct vdpctx *vc = vo->priv;
    VdpStatus vdp_st;
    struct vdp_functions *vdp = vc->vdp;
    struct mp_image *image = mp_image_alloc(IMGFMT_BGR32, width, height);
    image->colorspace = MP_CSP_RGB;
    image->levels = vc->colorspace.levels_out; // hardcoded with conv. matrix

    void *dst_planes[] = { image->planes[0] };
    uint32_t dst_pitches[] = { image->stride[0] };
    vdp_st = vdp->output_surface_get_bits_native(surface, NULL, dst_planes,
                                                 dst_pitches);
    CHECK_VDP_WARNING(vo, "Error when calling vdp_output_surface_get_bits_native");

    return image;
}

static struct mp_image *get_screenshot(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;
    VdpStatus vdp_st;
    struct vdp_functions *vdp = vc->vdp;

    if (vc->screenshot_surface == VDP_INVALID_HANDLE) {
        vdp_st = vdp->output_surface_create(vc->vdp_device,
                                            OUTPUT_RGBA_FORMAT,
                                            vc->vid_width, vc->vid_height,
                                            &vc->screenshot_surface);
        CHECK_VDP_WARNING(vo, "Error when calling vdp_output_surface_create");
    }

    VdpRect rc = { .x1 = vc->vid_width, .y1 = vc->vid_height };
    render_video_to_output_surface(vo, vc->screenshot_surface, &rc, &rc);

    struct mp_image *image = read_output_surface(vo, vc->screenshot_surface,
                                                 vc->vid_width, vc->vid_height);

    mp_image_set_display_size(image, vo->aspdat.prew, vo->aspdat.preh);

    return image;
}

static struct mp_image *get_window_screenshot(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;
    int last_surface = WRAP_ADD(vc->surface_num, -1, vc->num_output_surfaces);
    VdpOutputSurface screen = vc->output_surfaces[last_surface];
    struct mp_image *image = read_output_surface(vo, screen,
                                                 vc->output_surface_width,
                                                 vc->output_surface_height);
    mp_image_set_size(image, vo->dwidth, vo->dheight);
    return image;
}

static int query_format(struct vo *vo, uint32_t format)
{
    struct vdpctx *vc = vo->priv;

    int flags = VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW;
    if (mp_vdpau_get_format(format, NULL, NULL))
        return flags;
    int rgb_format = get_rgb_format(format);
    if (!vc->force_yuv && rgb_format >= 0)
        return flags;
    return 0;
}

static void destroy_vdpau_objects(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;

    VdpStatus vdp_st;

    free_video_specific(vo);

    if (vc->flip_queue != VDP_INVALID_HANDLE) {
        vdp_st = vdp->presentation_queue_destroy(vc->flip_queue);
        CHECK_VDP_WARNING(vo, "Error when calling vdp_presentation_queue_destroy");
    }

    if (vc->flip_target != VDP_INVALID_HANDLE) {
        vdp_st = vdp->presentation_queue_target_destroy(vc->flip_target);
        CHECK_VDP_WARNING(vo, "Error when calling "
                         "vdp_presentation_queue_target_destroy");
    }

    for (int i = 0; i < vc->num_output_surfaces; i++) {
        if (vc->output_surfaces[i] == VDP_INVALID_HANDLE)
            continue;
        vdp_st = vdp->output_surface_destroy(vc->output_surfaces[i]);
        CHECK_VDP_WARNING(vo, "Error when calling vdp_output_surface_destroy");
    }

    for (int i = 0; i < MAX_OSD_PARTS; i++) {
        struct osd_bitmap_surface *sfc = &vc->osd_surfaces[i];
        if (sfc->surface != VDP_INVALID_HANDLE) {
            vdp_st = vdp->bitmap_surface_destroy(sfc->surface);
            CHECK_VDP_WARNING(vo, "Error when calling vdp_bitmap_surface_destroy");
        }
    }

    mp_vdpau_destroy(vc->mpvdp);
    vc->mpvdp = NULL;
}

static void uninit(struct vo *vo)
{
    /* Destroy all vdpau objects */
    destroy_vdpau_objects(vo);

    vo_x11_uninit(vo);
}

static int preinit(struct vo *vo)
{
    struct vdpctx *vc = vo->priv;

    if (!vo_x11_init(vo))
        return -1;

    vc->mpvdp = mp_vdpau_create_device_x11(vo->log, vo->x11);
    if (!vc->mpvdp) {
        vo_x11_uninit(vo);
        return -1;
    }

    // Mark everything as invalid first so uninit() can tell what has been
    // allocated
    mark_vdpau_objects_uninitialized(vo);

    vc->preemption_counter = vc->mpvdp->preemption_counter;
    vc->vdp_device = vc->mpvdp->vdp_device;
    vc->vdp = vc->mpvdp->vdp;

    vc->colorspace = (struct mp_csp_details) MP_CSP_DETAILS_DEFAULTS;
    vc->video_eq.capabilities = MP_CSP_EQ_CAPS_COLORMATRIX;

    vc->deint_type = vc->deint ? FFABS(vc->deint) : 3;
    if (vc->deint < 0)
        vc->deint = 0;

    return 0;
}

static int get_equalizer(struct vo *vo, const char *name, int *value)
{
    struct vdpctx *vc = vo->priv;

    if (vc->rgb_mode)
        return false;

    return mp_csp_equalizer_get(&vc->video_eq, name, value) >= 0 ?
           VO_TRUE : VO_NOTIMPL;
}

static int set_equalizer(struct vo *vo, const char *name, int value)
{
    struct vdpctx *vc = vo->priv;

    if (vc->rgb_mode)
        return false;

    if (mp_csp_equalizer_set(&vc->video_eq, name, value) < 0)
        return VO_NOTIMPL;

    if (status_ok(vo))
        update_csc_matrix(vo);
    return true;
}

static void checked_resize(struct vo *vo)
{
    if (!status_ok(vo))
        return;
    resize(vo);
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct vdpctx *vc = vo->priv;
    struct vdp_functions *vdp = vc->vdp;

    handle_preemption(vo);

    switch (request) {
    case VOCTRL_GET_DEINTERLACE:
        if (vc->rgb_mode)
            break;
        *(int *)data = vc->deint;
        return VO_TRUE;
    case VOCTRL_SET_DEINTERLACE:
        if (vc->rgb_mode)
            break;
        vc->deint = vc->user_deint = *(int *)data;
        if (vc->deint)
            vc->deint = vc->deint_type;
        if (vc->deint_type > 2 && status_ok(vo)) {
            VdpStatus vdp_st;
            VdpVideoMixerFeature features[1] =
                {vc->deint_type == 3 ?
                 VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL :
                 VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL};
            VdpBool feature_enables[1] = {vc->deint ? VDP_TRUE : VDP_FALSE};
            vdp_st = vdp->video_mixer_set_feature_enables(vc->video_mixer,
                                                          1, features,
                                                          feature_enables);
            CHECK_VDP_WARNING(vo, "Error changing deinterlacing settings");
        }
        vo->want_redraw = true;
        return VO_TRUE;
    case VOCTRL_PAUSE:
        if (vc->dropped_frame)
            vo->want_redraw = true;
        return true;
    case VOCTRL_GET_HWDEC_INFO: {
        struct mp_hwdec_info *arg = data;
        arg->vdpau_ctx = vc->mpvdp;
        return true;
    }
    case VOCTRL_GET_PANSCAN:
        return VO_TRUE;
    case VOCTRL_SET_PANSCAN:
        checked_resize(vo);
        return VO_TRUE;
    case VOCTRL_SET_EQUALIZER: {
        vo->want_redraw = true;
        struct voctrl_set_equalizer_args *args = data;
        return set_equalizer(vo, args->name, args->value);
    }
    case VOCTRL_GET_EQUALIZER: {
        struct voctrl_get_equalizer_args *args = data;
        return get_equalizer(vo, args->name, args->valueptr);
    }
    case VOCTRL_SET_YUV_COLORSPACE:
        if (vc->rgb_mode)
            break;
        vc->colorspace = *(struct mp_csp_details *)data;
        if (status_ok(vo))
            update_csc_matrix(vo);
        vo->want_redraw = true;
        return true;
    case VOCTRL_GET_YUV_COLORSPACE:
        if (vc->rgb_mode)
            break;
        *(struct mp_csp_details *)data = vc->colorspace;
        return true;
    case VOCTRL_NEWFRAME:
        vc->deint_queue_pos = next_deint_queue_pos(vo, true);
        if (status_ok(vo))
            video_to_output_surface(vo);
        return true;
    case VOCTRL_SKIPFRAME:
        vc->deint_queue_pos = next_deint_queue_pos(vo, true);
        return true;
    case VOCTRL_REDRAW_FRAME:
        if (status_ok(vo))
            video_to_output_surface(vo);
        return true;
    case VOCTRL_RESET:
        forget_frames(vo, true);
        return true;
    case VOCTRL_SCREENSHOT: {
        if (!status_ok(vo))
            return false;
        struct voctrl_screenshot_args *args = data;
        if (args->full_window)
            args->out_image = get_window_screenshot(vo);
        else
            args->out_image = get_screenshot(vo);
        return true;
    }
    }

    int events = 0;
    int r = vo_x11_control(vo, &events, request, data);

    if (events & VO_EVENT_RESIZE) {
        checked_resize(vo);
    } else if (events & VO_EVENT_EXPOSE) {
        vo->want_redraw = true;
    }

    return r;
}

#define OPT_BASE_STRUCT struct vdpctx

const struct vo_driver video_out_vdpau = {
    .buffer_frames = true,
    .description = "VDPAU with X11",
    .name = "vdpau",
    .preinit = preinit,
    .query_format = query_format,
    .config = config,
    .control = control,
    .draw_image = draw_image,
    .get_buffered_frame = set_next_frame_info,
    .draw_osd = draw_osd,
    .flip_page_timed = flip_page_timed,
    .uninit = uninit,
    .priv_size = sizeof(struct vdpctx),
    .options = (const struct m_option []){
        OPT_INTRANGE("deint", deint, 0, -4, 4),
        OPT_FLAG("chroma-deint", chroma_deint, 0, OPTDEF_INT(1)),
        OPT_FLAG("pullup", pullup, 0),
        OPT_FLOATRANGE("denoise", denoise, 0, 0, 1),
        OPT_FLOATRANGE("sharpen", sharpen, 0, -1, 1),
        OPT_INTRANGE("hqscaling", hqscaling, 0, 0, 9),
        OPT_FLOAT("fps", user_fps, 0),
        OPT_FLAG("composite-detect", composite_detect, 0, OPTDEF_INT(1)),
        OPT_INT("queuetime_windowed", flip_offset_window, 0, OPTDEF_INT(50)),
        OPT_INT("queuetime_fs", flip_offset_fs, 0, OPTDEF_INT(50)),
        OPT_INTRANGE("output_surfaces", num_output_surfaces, 0,
                     2, MAX_OUTPUT_SURFACES, OPTDEF_INT(3)),
        OPT_COLOR("colorkey", colorkey, 0,
                  .defval = &(const struct m_color) {
                      .r = 2, .g = 5, .b = 7, .a = 255,
                  }),
        OPT_FLAG("force-yuv", force_yuv, 0),
        {NULL},
    }
};
