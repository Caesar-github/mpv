/*
 * Copyright (C) Aaron Holtzman - Aug 1999
 * Strongly modified, most parts rewritten: A'rpi/ESP-team - 2000-2001
 * (C) MPlayer developers
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

#ifndef MPLAYER_VIDEO_OUT_H
#define MPLAYER_VIDEO_OUT_H

#include <inttypes.h>
#include <stdbool.h>

#include "video/img_format.h"
#include "common/common.h"
#include "options/options.h"

#define VO_EVENT_EXPOSE 1
#define VO_EVENT_RESIZE 2

enum mp_voctrl {
    /* signal a device reset seek */
    VOCTRL_RESET = 1,
    /* Handle input and redraw events, called by vo_check_events() */
    VOCTRL_CHECK_EVENTS,
    /* used to switch to fullscreen */
    VOCTRL_FULLSCREEN,
    /* signal a device pause */
    VOCTRL_PAUSE,
    /* start/resume playback */
    VOCTRL_RESUME,

    VOCTRL_GET_PANSCAN,
    VOCTRL_SET_PANSCAN,
    VOCTRL_SET_EQUALIZER,               // struct voctrl_set_equalizer_args*
    VOCTRL_GET_EQUALIZER,               // struct voctrl_get_equalizer_args*

    /* for hardware decoding */
    VOCTRL_GET_HWDEC_INFO,              // struct mp_hwdec_info*

    VOCTRL_NEWFRAME,
    VOCTRL_SKIPFRAME,
    VOCTRL_REDRAW_FRAME,

    VOCTRL_ONTOP,
    VOCTRL_BORDER,
    VOCTRL_UPDATE_WINDOW_TITLE,         // char*

    VOCTRL_SET_CURSOR_VISIBILITY,       // bool*

    VOCTRL_KILL_SCREENSAVER,
    VOCTRL_RESTORE_SCREENSAVER,

    VOCTRL_SET_DEINTERLACE,
    VOCTRL_GET_DEINTERLACE,

    VOCTRL_UPDATE_SCREENINFO,
    VOCTRL_WINDOW_TO_OSD_COORDS,        // float[2] (x/y)
    VOCTRL_GET_WINDOW_SIZE,             // int[2] (w/h)
    VOCTRL_SET_WINDOW_SIZE,             // int[2] (w/h)

    VOCTRL_SET_YUV_COLORSPACE,          // struct mp_csp_details*
    VOCTRL_GET_YUV_COLORSPACE,          // struct mp_csp_details*

    VOCTRL_SCREENSHOT,                  // struct voctrl_screenshot_args*

    VOCTRL_SET_COMMAND_LINE,            // char**
};

// VOCTRL_SET_EQUALIZER
struct voctrl_set_equalizer_args {
    const char *name;
    int value;
};

// VOCTRL_GET_EQUALIZER
struct voctrl_get_equalizer_args {
    const char *name;
    int *valueptr;
};

// VOCTRL_SCREENSHOT
struct voctrl_screenshot_args {
    // 0: Save image of the currently displayed video frame, in original
    //    resolution.
    // 1: Save full screenshot of the window. Should contain OSD, EOSD, and the
    //    scaled video.
    // The value of this variable can be ignored if only a single method is
    // implemented.
    int full_window;
    // Will be set to a newly allocated image, that contains the screenshot.
    // The caller has to free the image with talloc_free().
    // It is not specified whether the image data is a copy or references the
    // image data directly.
    // Is never NULL. (Failure has to be indicated by returning VO_FALSE.)
    struct mp_image *out_image;
    // Whether the VO rendered OSD/subtitles into out_image
    bool has_osd;
};

#define VO_TRUE         true
#define VO_FALSE        false
#define VO_ERROR	-1
#define VO_NOTAVAIL	-2
#define VO_NOTIMPL	-3

#define VOFLAG_FLIPPING		0x08
#define VOFLAG_HIDDEN		0x10  //< Use to create a hidden window
#define VOFLAG_STEREO		0x20  //< Use to create a stereo-capable window
#define VOFLAG_GL_DEBUG         0x40  // Hint to request debug OpenGL context
#define VOFLAG_ALPHA            0x80  // Hint to request alpha framebuffer

struct vo;
struct osd_state;
struct mp_image;
struct mp_image_params;

struct vo_driver {
    // Driver buffers or adds (deinterlace) frames and will keep track
    // of pts values itself
    bool buffer_frames;

    // Encoding functionality, which can be invoked via --o only.
    bool encode;

    const char *name;
    const char *description;

    /*
     *   returns: zero on successful initialization, non-zero on error.
     */
    int (*preinit)(struct vo *vo);

    /*
     * Whether the given image format is supported and config() will succeed.
     * format: one of IMGFMT_*
     * returns: 0 on not supported, otherwise a bitmask of VFCAP_* values
     */
    int (*query_format)(struct vo *vo, uint32_t format);

    /*
     * Initialize or reconfigure the display driver.
     *   width,height: image source size
     *   d_width,d_height: requested window size, just a hint
     *   flags: combination of VOFLAG_ values
     *   title: window title, if available
     *   format: fourcc of pixel format
     * returns : zero on successful initialization, non-zero on error.
     */
    int (*config)(struct vo *vo, uint32_t width, uint32_t height,
                  uint32_t d_width, uint32_t d_height, uint32_t flags,
                  uint32_t format);

    /*
     * Initialize or reconfigure the display driver. Alternative to config(),
     * and can carry more image parameters.
     *   params: video parameters, like pixel format and frame size
     *   flags: combination of VOFLAG_ values
     * returns: < 0 on error, >= 0 on success
     */
    int (*reconfig)(struct vo *vo, struct mp_image_params *params, int flags);

    /*
     * Control interface
     */
    int (*control)(struct vo *vo, uint32_t request, void *data);

    void (*draw_image)(struct vo *vo, struct mp_image *mpi);

    /*
     * Get extra frames from the VO, such as those added by VDPAU
     * deinterlace. Preparing the next such frame if any could be done
     * automatically by the VO after a previous flip_page(), but having
     * it as a separate step seems to allow making code more robust.
     */
    void (*get_buffered_frame)(struct vo *vo, bool eof);

    /*
     * Draws OSD to the screen buffer
     */
    void (*draw_osd)(struct vo *vo, struct osd_state *osd);

    /*
     * Blit/Flip buffer to the screen. Must be called after each frame!
     * pts_us is the frame presentation time, linked to mp_time_us().
     * pts_us is 0 if the frame should be presented immediately.
     * duration is estimated time in us until the next frame is shown.
     * duration is -1 if it is unknown or unset.
     */
    void (*flip_page)(struct vo *vo);
    void (*flip_page_timed)(struct vo *vo, int64_t pts_us, int duration);

    /*
     * Closes driver. Should restore the original state of the system.
     */
    void (*uninit)(struct vo *vo);

    // Size of private struct for automatic allocation (0 doesn't allocate)
    int priv_size;

    // If not NULL, it's copied into the newly allocated private struct.
    const void *priv_defaults;

    // List of options to parse into priv struct (requires privsize to be set)
    const struct m_option *options;
};

struct vo {
    struct {
        struct mp_log *log; // Using "[vo]" as prefix
    } vo_log;
    struct mp_log *log; // Using e.g. "[vo/vdpau]" as prefix
    int config_ok;      // Last config call was successful?
    int config_count;   // Total number of successful config calls
    struct mp_image_params *params; // Configured parameters (as in vo_reconfig)

    bool probing;

    bool untimed;       // non-interactive, don't do sleep calls in playloop

    bool frame_loaded;  // Is there a next frame the VO could flip to?
    struct mp_image *waiting_mpi;
    double next_pts;    // pts value of the next frame if any
    double next_pts2;   // optional pts of frame after that
    bool want_redraw;   // visible frame wrong (window resize), needs refresh
    bool redrawing;     // between redrawing frame and flipping it
    bool hasframe;      // >= 1 frame has been drawn, so redraw is possible
    double wakeup_period; // if > 0, this sets the maximum wakeup period for event polling

    double flip_queue_offset; // queue flip events at most this much in advance

    const struct vo_driver *driver;
    void *priv;
    struct mp_vo_opts *opts;
    struct mpv_global *global;
    struct vo_x11_state *x11;
    struct vo_w32_state *w32;
    struct vo_cocoa_state *cocoa;
    struct vo_wayland_state *wayland;
    struct encode_lavc_context *encode_lavc_ctx;
    struct input_ctx *input_ctx;
    int event_fd;  // check_events() should be called when this has input
    int registered_fd;  // set to event_fd when registered in input system

    // requested position/resolution (usually window position/window size)
    int dx;
    int dy;
    int dwidth;
    int dheight;

    int xinerama_x;
    int xinerama_y;

    struct aspect_data {
        float monitor_par; // out of screen size or from options
        int orgw; // real width
        int orgh; // real height
        int prew; // prescaled width
        int preh; // prescaled height
        float par; // pixel aspect ratio out of orgw/orgh and prew/preh
    } aspdat;

    char *window_title;
};

struct mpv_global;
struct vo *init_best_video_out(struct mpv_global *global,
                               struct input_ctx *input_ctx,
                               struct encode_lavc_context *encode_lavc_ctx);
int vo_reconfig(struct vo *vo, struct mp_image_params *p, int flags);

int vo_control(struct vo *vo, uint32_t request, void *data);
void vo_queue_image(struct vo *vo, struct mp_image *mpi);
int vo_redraw_frame(struct vo *vo);
bool vo_get_want_redraw(struct vo *vo);
int vo_get_buffered_frame(struct vo *vo, bool eof);
void vo_skip_frame(struct vo *vo);
void vo_new_frame_imminent(struct vo *vo);
void vo_draw_osd(struct vo *vo, struct osd_state *osd);
void vo_flip_page(struct vo *vo, int64_t pts_us, int duration);
void vo_check_events(struct vo *vo);
void vo_seek_reset(struct vo *vo);
void vo_destroy(struct vo *vo);

const char *vo_get_window_title(struct vo *vo);

// NULL terminated array of all drivers
extern const struct vo_driver *video_out_drivers[];

struct mp_keymap {
  int from;
  int to;
};
int lookup_keymap_table(const struct mp_keymap *map, int key);

void vo_mouse_movement(struct vo *vo, int posx, int posy);

struct mp_osd_res;
void vo_get_src_dst_rects(struct vo *vo, struct mp_rect *out_src,
                          struct mp_rect *out_dst, struct mp_osd_res *out_osd);

#endif /* MPLAYER_VIDEO_OUT_H */
