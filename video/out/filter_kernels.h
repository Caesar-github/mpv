/*
 * This file is part of mpv.
 *
 * This file can be distributed under the 3-clause license ("New BSD License").
 *
 * You can alternatively redistribute the non-Glumpy parts of this file and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef MPLAYER_FILTER_KERNELS_H
#define MPLAYER_FILTER_KERNELS_H

#include <stdbool.h>

struct filter_window {
    const char *name;
    double radius; // Preferred radius, should only be changed if resizable
    double (*weight)(struct filter_window *k, double x);
    bool resizable; // Filter supports any given radius
    double params[2]; // User-defined custom filter parameters. Not used by
                      // all filters
    double blur; // Blur coefficient (sharpens or widens the filter)
};

struct filter_kernel {
    struct filter_window f; // the kernel itself
    struct filter_window w; // window storage
    bool clamp; // clamp to the range [0-1]
    // Constant values
    const char *window; // default window
    bool polar;         // whether or not the filter uses polar coordinates
    // The following values are set by mp_init_filter() at runtime.
    int size;           // number of coefficients (may depend on radius)
    double inv_scale;   // scale factor (<1.0 is upscale, >1.0 downscale)
};

extern const struct filter_window mp_filter_windows[];
extern const struct filter_kernel mp_filter_kernels[];

const struct filter_window *mp_find_filter_window(const char *name);
const struct filter_kernel *mp_find_filter_kernel(const char *name);

bool mp_init_filter(struct filter_kernel *filter, const int *sizes,
                    double scale);
void mp_compute_lut(struct filter_kernel *filter, int count, float *out_array);

#endif /* MPLAYER_FILTER_KERNELS_H */
