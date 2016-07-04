#ifndef MPGL_HWDEC_H_
#define MPGL_HWDEC_H_

#include "common.h"
#include "video/hwdec.h"

struct gl_hwdec {
    const struct gl_hwdec_driver *driver;
    struct mp_log *log;
    struct mpv_global *global;
    GL *gl;
    struct mp_hwdec_devices *devs;
    // GLSL extensions required to sample textures from this.
    const char **glsl_extensions;
    // For free use by hwdec driver
    void *priv;
    // For working around the vdpau vs. vaapi mess.
    bool probing;
};

struct gl_hwdec_plane {
    GLuint gl_texture;
    GLenum gl_target;
    int tex_w, tex_h; // allocated texture size
    char swizzle[5]; // component order (if length is 0, use defaults)
};

struct gl_hwdec_frame {
    struct gl_hwdec_plane planes[4];
    bool vdpau_fields;
};

struct gl_hwdec_driver {
    // Name of the interop backend. This is used for informational purposes only.
    const char *name;
    // Used to explicitly request a specific API.
    enum hwdec_type api;
    // The hardware surface IMGFMT_ that must be passed to map_image later.
    int imgfmt;
    // Create the hwdec device. It must add it to hw->devs, if applicable.
    int (*create)(struct gl_hwdec *hw);
    // Prepare for rendering video. (E.g. create textures.)
    // Called on initialization, and every time the video size changes.
    // *params must be set to the format the hw textures return.
    int (*reinit)(struct gl_hwdec *hw, struct mp_image_params *params);
    // Return textures that contain a copy or reference of the given hw_image.
    // The textures mirror the format returned by the reinit params argument.
    // The textures must remain valid until unmap is called.
    // hw_image remains referenced by the caller until unmap is called.
    int (*map_frame)(struct gl_hwdec *hw, struct mp_image *hw_image,
                     struct gl_hwdec_frame *out_frame);
    // Must be idempotent.
    void (*unmap)(struct gl_hwdec *hw);

    void (*destroy)(struct gl_hwdec *hw);
};

struct gl_hwdec *gl_hwdec_load_api(struct mp_log *log, GL *gl,
                                   struct mpv_global *g,
                                   struct mp_hwdec_devices *devs,
                                   enum hwdec_type api);

void gl_hwdec_uninit(struct gl_hwdec *hwdec);

#endif
