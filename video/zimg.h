#pragma once

#include <stdbool.h>

#include <zimg.h>

#include "mp_image.h"

#define ZIMG_ALIGN 64

struct mpv_global;

bool mp_zimg_supports_in_format(int imgfmt);
bool mp_zimg_supports_out_format(int imgfmt);

struct mp_zimg_context {
    // Can be set for verbose error printing.
    struct mp_log *log;

    // User configuration. Note: changing these requires calling mp_zimg_config()
    // to update the filter graph. The first mp_zimg_convert() call (or if the
    // image format changes) will do this automatically.
    zimg_resample_filter_e scaler;
    double scaler_params[2];
    zimg_resample_filter_e scaler_chroma;
    double scaler_chroma_params[2];
    zimg_dither_type_e dither;
    bool fast; // reduce quality for better performance

    // Input/output parameters. Note: if these mismatch with the
    // mp_zimg_convert() parameters, mp_zimg_config() will be called
    // automatically.
    struct mp_image_params src, dst;

    // Cached zimg state (if any). Private, do not touch.
    zimg_filter_graph *zimg_graph;
    void *zimg_tmp;
    struct mp_zimg_repack *zimg_src;
    struct mp_zimg_repack *zimg_dst;
};

// Allocate a zimg context. Always succeeds. Returns a talloc pointer (use
// talloc_free() to release it).
struct mp_zimg_context *mp_zimg_alloc(void);

// Try to build the conversion chain using the parameters currently set in ctx.
// If this succeeds, mp_zimg_convert() will always succeed (probably), as long
// as the input has the same parameters.
// Returns false on error.
bool mp_zimg_config(struct mp_zimg_context *ctx);

// Similar to mp_zimg_config(), but assume none of the user parameters changed,
// except possibly .src and .dst. This essentially checks whether src/dst
// changed, and if so, calls mp_zimg_config().
bool mp_zimg_config_image_params(struct mp_zimg_context *ctx);

// Convert/scale src to dst. On failure, the data in dst is not touched.
bool mp_zimg_convert(struct mp_zimg_context *ctx, struct mp_image *dst,
                     struct mp_image *src);

// Set the global zimg command line parameters on this context. Use this if you
// want the user to be able to change the scaler etc.
void mp_zimg_set_from_cmdline(struct mp_zimg_context *ctx, struct mpv_global *g);
