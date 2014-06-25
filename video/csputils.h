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
 *
 * You can alternatively redistribute this file and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef MPLAYER_CSPUTILS_H
#define MPLAYER_CSPUTILS_H

#include <stdbool.h>
#include <stdint.h>

/* NOTE: the csp and levels AUTO values are converted to specific ones
 * above vf/vo level. At least vf_scale relies on all valid settings being
 * nonzero at vf/vo level.
 */

enum mp_csp {
    MP_CSP_AUTO,
    MP_CSP_BT_601,
    MP_CSP_BT_709,
    MP_CSP_SMPTE_240M,
    MP_CSP_BT_2020_NC,
    MP_CSP_BT_2020_C,
    MP_CSP_RGB,
    MP_CSP_XYZ,
    MP_CSP_YCGCO,
    MP_CSP_COUNT
};

// Any enum mp_csp value is a valid index (except MP_CSP_COUNT)
extern const char *const mp_csp_names[MP_CSP_COUNT];

enum mp_csp_levels {
    MP_CSP_LEVELS_AUTO,
    MP_CSP_LEVELS_TV,
    MP_CSP_LEVELS_PC,
    MP_CSP_LEVELS_COUNT,
};

// Any enum mp_csp_levels value is a valid index (except MP_CSP_LEVELS_COUNT)
extern const char *const mp_csp_levels_names[MP_CSP_LEVELS_COUNT];

enum mp_csp_prim {
    MP_CSP_PRIM_AUTO,
    MP_CSP_PRIM_BT_601_525,
    MP_CSP_PRIM_BT_601_625,
    MP_CSP_PRIM_BT_709,
    MP_CSP_PRIM_BT_2020,
    MP_CSP_PRIM_COUNT
};

// Any enum mp_csp_prim value is a valid index (except MP_CSP_PRIM_COUNT)
extern const char *const mp_csp_prim_names[MP_CSP_PRIM_COUNT];

// These constants are based on the ICC specification (Table 23) and match
// up with the API of LittleCMS, which treats them as integers.
enum mp_render_intent {
    MP_INTENT_PERCEPTUAL = 0,
    MP_INTENT_RELATIVE_COLORIMETRIC = 1,
    MP_INTENT_SATURATION = 2,
    MP_INTENT_ABSOLUTE_COLORIMETRIC = 3
};

struct mp_csp_details {
    enum mp_csp format;
    enum mp_csp_levels levels_in;      // encoded video
    enum mp_csp_levels levels_out;     // output device
};

// initializer for struct mp_csp_details that contains reasonable defaults
#define MP_CSP_DETAILS_DEFAULTS {MP_CSP_BT_601, MP_CSP_LEVELS_TV, MP_CSP_LEVELS_PC}

struct mp_csp_params {
    struct mp_csp_details colorspace;
    float brightness;
    float contrast;
    float hue;
    float saturation;
    float rgamma;
    float ggamma;
    float bgamma;
    // texture_bits/input_bits is for rescaling fixed point input to range [0,1]
    int texture_bits;
    int input_bits;
    // for scaling integer input and output (if 0, assume range [0,1])
    int int_bits_in;
    int int_bits_out;
};

#define MP_CSP_PARAMS_DEFAULTS {                                \
    .colorspace = MP_CSP_DETAILS_DEFAULTS,                      \
    .brightness = 0, .contrast = 1, .hue = 0, .saturation = 1,  \
    .rgamma = 1, .ggamma = 1, .bgamma = 1,                      \
    .texture_bits = 8, .input_bits = 8}

enum mp_chroma_location {
    MP_CHROMA_AUTO,
    MP_CHROMA_LEFT,     // mpeg2/4, h264
    MP_CHROMA_CENTER,   // mpeg1, jpeg
    MP_CHROMA_COUNT,
};

extern const char *const mp_chroma_names[MP_CHROMA_COUNT];

enum mp_csp_equalizer_param {
    MP_CSP_EQ_BRIGHTNESS,
    MP_CSP_EQ_CONTRAST,
    MP_CSP_EQ_HUE,
    MP_CSP_EQ_SATURATION,
    MP_CSP_EQ_GAMMA,
    MP_CSP_EQ_COUNT,
};

#define MP_CSP_EQ_CAPS_COLORMATRIX \
    ( (1 << MP_CSP_EQ_BRIGHTNESS) \
    | (1 << MP_CSP_EQ_CONTRAST) \
    | (1 << MP_CSP_EQ_HUE) \
    | (1 << MP_CSP_EQ_SATURATION) )

#define MP_CSP_EQ_CAPS_GAMMA (1 << MP_CSP_EQ_GAMMA)
#define MP_CSP_EQ_CAPS_BRIGHTNESS (1 << MP_CSP_EQ_BRIGHTNESS)

extern const char *const mp_csp_equalizer_names[MP_CSP_EQ_COUNT];

// Default initialization with 0 is enough, except for the capabilities field
struct mp_csp_equalizer {
    // Bit field of capabilities. For example (1 << MP_CSP_EQ_HUE) means hue
    // support is available.
    int capabilities;
    // Value for each property is in the range [-100, 100].
    // 0 is default, meaning neutral or no change.
    int values[MP_CSP_EQ_COUNT];
};

struct mp_csp_col_xy {
    float x, y;
};

struct mp_csp_primaries {
    struct mp_csp_col_xy red, green, blue, white;
};

void mp_csp_copy_equalizer_values(struct mp_csp_params *params,
                                  const struct mp_csp_equalizer *eq);

int mp_csp_equalizer_set(struct mp_csp_equalizer *eq, const char *property,
                         int value);

int mp_csp_equalizer_get(struct mp_csp_equalizer *eq, const char *property,
                         int *out_value);

enum mp_csp avcol_spc_to_mp_csp(int avcolorspace);

enum mp_csp_levels avcol_range_to_mp_csp_levels(int avrange);

enum mp_csp_prim avcol_pri_to_mp_csp_prim(int avpri);

int mp_csp_to_avcol_spc(enum mp_csp colorspace);

int mp_csp_levels_to_avcol_range(enum mp_csp_levels range);

int mp_csp_prim_to_avcol_pri(enum mp_csp_prim prim);

enum mp_csp mp_csp_guess_colorspace(int width, int height);
enum mp_csp_prim mp_csp_guess_primaries(int width, int height);

enum mp_chroma_location avchroma_location_to_mp(int avloc);
int mp_chroma_location_to_av(enum mp_chroma_location mploc);

void mp_get_chroma_location(enum mp_chroma_location loc, int *x, int *y);

void mp_gen_gamma_map(unsigned char *map, int size, float gamma);
#define ROW_R 0
#define ROW_G 1
#define ROW_B 2
#define COL_Y 0
#define COL_U 1
#define COL_V 2
#define COL_C 3
struct mp_csp_primaries mp_get_csp_primaries(enum mp_csp_prim csp);

void mp_apply_chromatic_adaptation(struct mp_csp_col_xy src, struct mp_csp_col_xy dest, float m[3][3]);
void mp_get_cms_matrix(struct mp_csp_primaries src, struct mp_csp_primaries dest,
                       enum mp_render_intent intent, float cms_matrix[3][3]);
void mp_get_rgb2xyz_matrix(struct mp_csp_primaries space, float m[3][3]);

void mp_get_xyz2rgb_coeffs(struct mp_csp_params *params, struct mp_csp_primaries prim,
                           enum mp_render_intent intent, float xyz2rgb[3][4]);
void mp_get_yuv2rgb_coeffs(struct mp_csp_params *params, float yuv2rgb[3][4]);
void mp_gen_yuv2rgb_map(struct mp_csp_params *params, uint8_t *map, int size);

void mp_mul_matrix3x3(float a[3][3], float b[3][3]);
void mp_invert_matrix3x3(float m[3][3]);
void mp_invert_yuv2rgb(float out[3][4], float in[3][4]);
void mp_map_int_color(float matrix[3][4], int clip_bits, int c[3]);

#endif /* MPLAYER_CSPUTILS_H */
