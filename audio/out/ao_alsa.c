/*
 * ALSA 0.9.x-1.x audio output driver
 *
 * Copyright (C) 2004 Alex Beregszaszi
 * Zsolt Barat <joy@streamminister.de>
 *
 * modified for real ALSA 0.9.0 support by Zsolt Barat <joy@streamminister.de>
 * additional AC-3 passthrough support by Andy Lo A Foe <andy@alsaplayer.org>
 * 08/22/2002 iec958-init rewritten and merged with common init, zsolt
 * 04/13/2004 merged with ao_alsa1.x, fixes provided by Jindrich Makovicka
 * 04/25/2004 printfs converted to mp_msg, Zsolt.
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "options/options.h"
#include "options/m_option.h"
#include "common/msg.h"
#include "osdep/endian.h"

#include <alsa/asoundlib.h>

#define HAVE_CHMAP_API \
    (defined(SND_CHMAP_API_VERSION) && SND_CHMAP_API_VERSION >= (1 << 16))

#include "ao.h"
#include "internal.h"
#include "audio/format.h"

struct priv {
    snd_pcm_t *alsa;
    bool device_lost;
    snd_pcm_format_t alsa_fmt;
    bool can_pause;
    snd_pcm_sframes_t prepause_frames;
    double delay_before_pause;
    snd_pcm_uframes_t buffersize;
    snd_pcm_uframes_t outburst;

    char *cfg_device;
    char *cfg_mixer_device;
    char *cfg_mixer_name;
    int cfg_mixer_index;
    int cfg_resample;
    int cfg_ni;
    int cfg_ignore_chmap;
};

#define BUFFER_TIME 250000  // 250ms
#define FRAGCOUNT 16

#define CHECK_ALSA_ERROR(message) \
    do { \
        if (err < 0) { \
            MP_ERR(ao, "%s: %s\n", (message), snd_strerror(err)); \
            goto alsa_error; \
        } \
    } while (0)

#define CHECK_ALSA_WARN(message) \
    do { \
        if (err < 0) \
            MP_WARN(ao, "%s: %s\n", (message), snd_strerror(err)); \
    } while (0)

// Common code for handling ENODEV, which happens if a device gets "lost", and
// can't be used anymore. Returns true if alsa_err is not ENODEV.
static bool check_device_present(struct ao *ao, int alsa_err)
{
    struct priv *p = ao->priv;
    if (alsa_err != -ENODEV)
        return true;
    if (!p->device_lost) {
        MP_WARN(ao, "Device lost, trying to recover...\n");
        ao_request_reload(ao);
        p->device_lost = true;
    }
    return false;
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    struct priv *p = ao->priv;
    snd_mixer_t *handle = NULL;
    switch (cmd) {
    case AOCONTROL_GET_MUTE:
    case AOCONTROL_SET_MUTE:
    case AOCONTROL_GET_VOLUME:
    case AOCONTROL_SET_VOLUME:
    {
        int err;
        snd_mixer_elem_t *elem;
        snd_mixer_selem_id_t *sid;

        long pmin, pmax;
        long get_vol, set_vol;
        float f_multi;

        if (!af_fmt_is_pcm(ao->format))
            return CONTROL_FALSE;

        snd_mixer_selem_id_alloca(&sid);

        snd_mixer_selem_id_set_index(sid, p->cfg_mixer_index);
        snd_mixer_selem_id_set_name(sid, p->cfg_mixer_name);

        err = snd_mixer_open(&handle, 0);
        CHECK_ALSA_ERROR("Mixer open error");

        err = snd_mixer_attach(handle, p->cfg_mixer_device);
        CHECK_ALSA_ERROR("Mixer attach error");

        err = snd_mixer_selem_register(handle, NULL, NULL);
        CHECK_ALSA_ERROR("Mixer register error");

        err = snd_mixer_load(handle);
        CHECK_ALSA_ERROR("Mixer load error");

        elem = snd_mixer_find_selem(handle, sid);
        if (!elem) {
            MP_VERBOSE(ao, "Unable to find simple control '%s',%i.\n",
                       snd_mixer_selem_id_get_name(sid),
                       snd_mixer_selem_id_get_index(sid));
            goto alsa_error;
        }

        snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);
        f_multi = (100 / (float)(pmax - pmin));

        switch (cmd) {
        case AOCONTROL_SET_VOLUME: {
            ao_control_vol_t *vol = arg;
            set_vol = vol->left / f_multi + pmin + 0.5;

            err = snd_mixer_selem_set_playback_volume
                    (elem, SND_MIXER_SCHN_FRONT_LEFT, set_vol);
            CHECK_ALSA_ERROR("Error setting left channel");
            MP_DBG(ao, "left=%li, ", set_vol);

            set_vol = vol->right / f_multi + pmin + 0.5;

            err = snd_mixer_selem_set_playback_volume
                    (elem, SND_MIXER_SCHN_FRONT_RIGHT, set_vol);
            CHECK_ALSA_ERROR("Error setting right channel");
            MP_DBG(ao, "right=%li, pmin=%li, pmax=%li, mult=%f\n",
                   set_vol, pmin, pmax, f_multi);
            break;
        }
        case AOCONTROL_GET_VOLUME: {
            ao_control_vol_t *vol = arg;
            snd_mixer_selem_get_playback_volume
                (elem, SND_MIXER_SCHN_FRONT_LEFT, &get_vol);
            vol->left = (get_vol - pmin) * f_multi;
            snd_mixer_selem_get_playback_volume
                (elem, SND_MIXER_SCHN_FRONT_RIGHT, &get_vol);
            vol->right = (get_vol - pmin) * f_multi;
            MP_DBG(ao, "left=%f, right=%f\n", vol->left, vol->right);
            break;
        }
        case AOCONTROL_SET_MUTE: {
            bool *mute = arg;
            if (!snd_mixer_selem_has_playback_switch(elem))
                goto alsa_error;
            if (!snd_mixer_selem_has_playback_switch_joined(elem)) {
                snd_mixer_selem_set_playback_switch
                    (elem, SND_MIXER_SCHN_FRONT_RIGHT, !*mute);
            }
            snd_mixer_selem_set_playback_switch
                (elem, SND_MIXER_SCHN_FRONT_LEFT, !*mute);
            break;
        }
        case AOCONTROL_GET_MUTE: {
            bool *mute = arg;
            if (!snd_mixer_selem_has_playback_switch(elem))
                goto alsa_error;
            int tmp = 1;
            snd_mixer_selem_get_playback_switch
                (elem, SND_MIXER_SCHN_FRONT_LEFT, &tmp);
            *mute = !tmp;
            if (!snd_mixer_selem_has_playback_switch_joined(elem)) {
                snd_mixer_selem_get_playback_switch
                    (elem, SND_MIXER_SCHN_FRONT_RIGHT, &tmp);
                *mute &= !tmp;
            }
            break;
        }
        }
        snd_mixer_close(handle);
        return CONTROL_OK;
    }

    } //end switch
    return CONTROL_UNKNOWN;

alsa_error:
    if (handle)
        snd_mixer_close(handle);
    return CONTROL_ERROR;
}

static const int mp_to_alsa_format[][2] = {
    {AF_FORMAT_U8,          SND_PCM_FORMAT_U8},
    {AF_FORMAT_S16,         SND_PCM_FORMAT_S16},
    {AF_FORMAT_S32,         SND_PCM_FORMAT_S32},
    {AF_FORMAT_S24,
            MP_SELECT_LE_BE(SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S24_3BE)},
    {AF_FORMAT_FLOAT,       SND_PCM_FORMAT_FLOAT},
    {AF_FORMAT_DOUBLE,      SND_PCM_FORMAT_FLOAT64},
    {AF_FORMAT_S_MP3,       SND_PCM_FORMAT_MPEG},
    {AF_FORMAT_UNKNOWN,     SND_PCM_FORMAT_UNKNOWN},
};

static int find_alsa_format(int af_format)
{
    af_format = af_fmt_from_planar(af_format);
    for (int n = 0; mp_to_alsa_format[n][0] != AF_FORMAT_UNKNOWN; n++) {
        if (mp_to_alsa_format[n][0] == af_format)
            return mp_to_alsa_format[n][1];
    }
    if (af_fmt_is_spdif(af_format))
        return SND_PCM_FORMAT_S16;
    return SND_PCM_FORMAT_UNKNOWN;
}

#if HAVE_CHMAP_API

static const int alsa_to_mp_channels[][2] = {
    {SND_CHMAP_FL,      MP_SP(FL)},
    {SND_CHMAP_FR,      MP_SP(FR)},
    {SND_CHMAP_RL,      MP_SP(BL)},
    {SND_CHMAP_RR,      MP_SP(BR)},
    {SND_CHMAP_FC,      MP_SP(FC)},
    {SND_CHMAP_LFE,     MP_SP(LFE)},
    {SND_CHMAP_SL,      MP_SP(SL)},
    {SND_CHMAP_SR,      MP_SP(SR)},
    {SND_CHMAP_RC,      MP_SP(BC)},
    {SND_CHMAP_FLC,     MP_SP(FLC)},
    {SND_CHMAP_FRC,     MP_SP(FRC)},
    {SND_CHMAP_FLW,     MP_SP(WL)},
    {SND_CHMAP_FRW,     MP_SP(WR)},
    {SND_CHMAP_TC,      MP_SP(TC)},
    {SND_CHMAP_TFL,     MP_SP(TFL)},
    {SND_CHMAP_TFR,     MP_SP(TFR)},
    {SND_CHMAP_TFC,     MP_SP(TFC)},
    {SND_CHMAP_TRL,     MP_SP(TBL)},
    {SND_CHMAP_TRR,     MP_SP(TBR)},
    {SND_CHMAP_TRC,     MP_SP(TBC)},
    {SND_CHMAP_RRC,     MP_SP(SDR)},
    {SND_CHMAP_RLC,     MP_SP(SDL)},
    {SND_CHMAP_MONO,    MP_SP(FC)},
    {SND_CHMAP_NA,      MP_SPEAKER_ID_NA},
    {SND_CHMAP_UNKNOWN, MP_SPEAKER_ID_NA},
    {SND_CHMAP_LAST,    MP_SPEAKER_ID_COUNT}
};

static int find_mp_channel(int alsa_channel)
{
    for (int i = 0; alsa_to_mp_channels[i][1] != MP_SPEAKER_ID_COUNT; i++) {
        if (alsa_to_mp_channels[i][0] == alsa_channel)
            return alsa_to_mp_channels[i][1];
    }

    return MP_SPEAKER_ID_COUNT;
}

#define CHMAP(n, ...) &(struct mp_chmap) MP_CONCAT(MP_CHMAP, n) (__VA_ARGS__)

// Replace each channel in a with b (a->num == b->num)
static void replace_submap(struct mp_chmap *dst, struct mp_chmap *a,
                           struct mp_chmap *b)
{
    struct mp_chmap t = *dst;
    if (!mp_chmap_is_valid(&t) || mp_chmap_diffn(a, &t) != 0)
        return;
    assert(a->num == b->num);
    for (int n = 0; n < t.num; n++) {
        for (int i = 0; i < a->num; i++) {
            if (t.speaker[n] == a->speaker[i]) {
                t.speaker[n] = b->speaker[i];
                break;
            }
        }
    }
    if (mp_chmap_is_valid(&t))
        *dst = t;
}

static bool mp_chmap_from_alsa(struct mp_chmap *dst, snd_pcm_chmap_t *src)
{
    *dst = (struct mp_chmap) {0};

    if (src->channels > MP_NUM_CHANNELS)
        return false;

    dst->num = src->channels;
    for (int c = 0; c < dst->num; c++)
        dst->speaker[c] = find_mp_channel(src->pos[c]);

    // Assume anything with 1 channel is mono.
    if (dst->num == 1)
        dst->speaker[0] = MP_SP(FC);

    // Remap weird Intel HDA HDMI 7.1 layouts correctly.
    replace_submap(dst, CHMAP(6, FL, FR, BL, BR, SDL, SDR),
                        CHMAP(6, FL, FR, SL, SR, BL,  BR));

    return mp_chmap_is_valid(dst);
}

static bool query_chmaps(struct ao *ao, struct mp_chmap *chmap)
{
    struct priv *p = ao->priv;
    struct mp_chmap_sel chmap_sel = {.tmp = p};

    snd_pcm_chmap_query_t **maps = snd_pcm_query_chmaps(p->alsa);
    if (!maps)
        return false;

    for (int i = 0; maps[i] != NULL; i++) {
        char aname[128];
        if (snd_pcm_chmap_print(&maps[i]->map, sizeof(aname), aname) <= 0)
            aname[0] = '\0';

        struct mp_chmap entry;
        if (mp_chmap_from_alsa(&entry, &maps[i]->map)) {
            struct mp_chmap reorder = entry;
            if (maps[i]->type == SND_CHMAP_TYPE_VAR)
                mp_chmap_reorder_norm(&reorder);
            MP_DBG(ao, "Got supported channel map: %s (type %s) -> %s -> %s\n",
                   aname, snd_pcm_chmap_type_name(maps[i]->type),
                   mp_chmap_to_str(&entry), mp_chmap_to_str(&reorder));
            mp_chmap_sel_add_map(&chmap_sel, &reorder);
        } else {
            MP_VERBOSE(ao, "skipping unknown ALSA channel map: %s\n", aname);
        }
    }

    snd_pcm_free_chmaps(maps);

    return ao_chmap_sel_adjust(ao, &chmap_sel, chmap);
}

// Map back our selected channel layout to an ALSA one. This is done this way so
// that our ALSA->mp_chmap mapping function only has to go one way.
// The return value is to be freed with free().
static snd_pcm_chmap_t *map_back_chmap(struct ao *ao, struct mp_chmap *chmap)
{
    struct priv *p = ao->priv;
    if (!mp_chmap_is_valid(chmap))
        return NULL;

    snd_pcm_chmap_query_t **maps = snd_pcm_query_chmaps(p->alsa);
    if (!maps)
        return NULL;

    snd_pcm_chmap_t *alsa_chmap = NULL;

    for (int i = 0; maps[i] != NULL; i++) {
        struct mp_chmap entry;
        if (!mp_chmap_from_alsa(&entry, &maps[i]->map))
            continue;

        if (mp_chmap_equals(chmap, &entry) ||
            (mp_chmap_equals_reordered(chmap, &entry) &&
                maps[i]->type == SND_CHMAP_TYPE_VAR))
        {
            alsa_chmap = calloc(1, sizeof(*alsa_chmap) +
                                   sizeof(alsa_chmap->pos[0]) * entry.num);
            if (!alsa_chmap)
                break;
            alsa_chmap->channels = entry.num;

            // Undo if mp_chmap_reorder() was called on the result.
            int reorder[MP_NUM_CHANNELS];
            mp_chmap_get_reorder(reorder, chmap, &entry);
            for (int n = 0; n < entry.num; n++)
                alsa_chmap->pos[n] = maps[i]->map.pos[reorder[n]];
            break;
        }
    }

    snd_pcm_free_chmaps(maps);
    return alsa_chmap;
}


static int set_chmap(struct ao *ao, struct mp_chmap *dev_chmap, int num_channels)
{
    struct priv *p = ao->priv;
    int err;

    snd_pcm_chmap_t *alsa_chmap = map_back_chmap(ao, dev_chmap);
    if (alsa_chmap) {
        char tmp[128];
        if (snd_pcm_chmap_print(alsa_chmap, sizeof(tmp), tmp) > 0)
            MP_VERBOSE(ao, "trying to set ALSA channel map: %s\n", tmp);

        err = snd_pcm_set_chmap(p->alsa, alsa_chmap);
        if (err == -ENXIO) {
            // A device my not be able to set any channel map, even channel maps
            // that were reported as supported. This is either because the ALSA
            // device is broken (dmix), or because the driver has only 1
            // channel map per channel count, and setting the map is not needed.
            MP_VERBOSE(ao, "device returned ENXIO when setting channel map %s\n",
                       mp_chmap_to_str(dev_chmap));
        } else {
            CHECK_ALSA_WARN("Channel map setup failed");
        }

        free(alsa_chmap);
    }

    alsa_chmap = snd_pcm_get_chmap(p->alsa);
    if (alsa_chmap) {
        char tmp[128];
        if (snd_pcm_chmap_print(alsa_chmap, sizeof(tmp), tmp) > 0)
            MP_VERBOSE(ao, "channel map reported by ALSA: %s\n", tmp);

        struct mp_chmap chmap;
        mp_chmap_from_alsa(&chmap, alsa_chmap);

        MP_VERBOSE(ao, "which we understand as: %s\n", mp_chmap_to_str(&chmap));

        if (p->cfg_ignore_chmap) {
            MP_VERBOSE(ao, "user set ignore-chmap; ignoring the channel map.\n");
        } else if (af_fmt_is_spdif(ao->format)) {
            MP_VERBOSE(ao, "using spdif passthrough; ignoring the channel map.\n");
        } else if (!mp_chmap_is_valid(&chmap)) {
            MP_WARN(ao, "Got unknown channel map from ALSA.\n");
        } else if (chmap.num != num_channels) {
            MP_WARN(ao, "ALSA channel map conflicts with channel count!\n");
        } else {
            MP_VERBOSE(ao, "using the ALSA channel map.\n");
            if (mp_chmap_equals(&chmap, &ao->channels))
                MP_VERBOSE(ao, "which is what we requested.\n");
            ao->channels = chmap;
        }

        free(alsa_chmap);
    }

    return 0;
}

#else /* HAVE_CHMAP_API */

static bool query_chmaps(struct ao *ao, struct mp_chmap *chmap)
{
    return false;
}

static int set_chmap(struct ao *ao, struct mp_chmap *dev_chmap, int num_channels)
{
    return 0;
}

#endif /* else HAVE_CHMAP_API */

static int map_iec958_srate(int srate)
{
    switch (srate) {
    case 44100:     return IEC958_AES3_CON_FS_44100;
    case 48000:     return IEC958_AES3_CON_FS_48000;
    case 32000:     return IEC958_AES3_CON_FS_32000;
    case 22050:     return IEC958_AES3_CON_FS_22050;
    case 24000:     return IEC958_AES3_CON_FS_24000;
    case 88200:     return IEC958_AES3_CON_FS_88200;
    case 768000:    return IEC958_AES3_CON_FS_768000;
    case 96000:     return IEC958_AES3_CON_FS_96000;
    case 176400:    return IEC958_AES3_CON_FS_176400;
    case 192000:    return IEC958_AES3_CON_FS_192000;
    default:        return IEC958_AES3_CON_FS_NOTID;
    }
}

// ALSA device strings can have parameters. They are usually appended to the
// device name. There can be various forms, and we (sometimes) want to append
// them to unknown device strings, which possibly already include params.
static char *append_params(void *ta_parent, const char *device, const char *p)
{
    if (!p || !p[0])
        return talloc_strdup(ta_parent, device);

    int len = strlen(device);
    char *end = strchr(device, ':');
    if (!end) {
        /* no existing parameters: add it behind device name */
        return talloc_asprintf(ta_parent, "%s:%s", device, p);
    } else if (end[1] == '\0') {
        /* ":" but no parameters */
        return talloc_asprintf(ta_parent, "%s%s", device, p);
    } else if (end[1] == '{' && device[len - 1] == '}') {
        /* parameters in config syntax: add it inside the { } block */
        return talloc_asprintf(ta_parent, "%.*s %s}", len - 1, device, p);
    } else {
        /* a simple list of parameters: add it at the end of the list */
        return talloc_asprintf(ta_parent, "%s,%s", device, p);
    }
    abort();
}

static int try_open_device(struct ao *ao, const char *device)
{
    struct priv *p = ao->priv;
    int err;

    if (af_fmt_is_spdif(ao->format)) {
        void *tmp = talloc_new(NULL);
        char *params = talloc_asprintf(tmp,
                        "AES0=%d,AES1=%d,AES2=0,AES3=%d",
                        IEC958_AES0_NONAUDIO | IEC958_AES0_PRO_EMPHASIS_NONE,
                        IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER,
                        map_iec958_srate(ao->samplerate));
        const char *ac3_device = append_params(tmp, device, params);
        MP_VERBOSE(ao, "opening device '%s' => '%s'\n", device, ac3_device);
        err = snd_pcm_open(&p->alsa, ac3_device, SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            // Some spdif-capable devices do not accept the AES0 parameter,
            // and instead require the iec958 pseudo-device (they will play
            // noise otherwise). Unfortunately, ALSA gives us no way to map
            // these devices, so try it for the default device only.
            bstr dev;
            bstr_split_tok(bstr0(device), ":", &dev, &(bstr){0});
            if (bstr_equals0(dev, "default")) {
                ac3_device = append_params(tmp, "iec958", params);
                MP_VERBOSE(ao, "got error %d; opening iec fallback device '%s'\n",
                           err, ac3_device);
                err = snd_pcm_open
                            (&p->alsa, ac3_device, SND_PCM_STREAM_PLAYBACK, 0);
            }
        }
        talloc_free(tmp);
    } else {
        MP_VERBOSE(ao, "opening device '%s'\n", device);
        err = snd_pcm_open(&p->alsa, device, SND_PCM_STREAM_PLAYBACK, 0);
    }

    return err;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->alsa) {
        int err;

        err = snd_pcm_close(p->alsa);
        p->alsa = NULL;
        CHECK_ALSA_ERROR("pcm close error");
    }

alsa_error: ;
}

static int init_device(struct ao *ao)
{
    struct priv *p = ao->priv;
    int err;

    const char *device = "default";
    if (ao->device)
        device = ao->device;
    if (p->cfg_device && p->cfg_device[0])
        device = p->cfg_device;

    err = try_open_device(ao, device);
    CHECK_ALSA_ERROR("Playback open error");

    err = snd_pcm_nonblock(p->alsa, 0);
    CHECK_ALSA_WARN("Unable to set blocking mode");

    snd_pcm_hw_params_t *alsa_hwparams;
    snd_pcm_hw_params_alloca(&alsa_hwparams);

    err = snd_pcm_hw_params_any(p->alsa, alsa_hwparams);
    CHECK_ALSA_ERROR("Unable to get initial parameters");

    // Some ALSA drivers have broken delay reporting, so disable the ALSA
    // resampling plugin by default.
    if (!p->cfg_resample) {
        err = snd_pcm_hw_params_set_rate_resample(p->alsa, alsa_hwparams, 0);
        CHECK_ALSA_ERROR("Unable to disable resampling");
    }

    snd_pcm_access_t access = af_fmt_is_planar(ao->format)
                                    ? SND_PCM_ACCESS_RW_NONINTERLEAVED
                                    : SND_PCM_ACCESS_RW_INTERLEAVED;
    err = snd_pcm_hw_params_set_access(p->alsa, alsa_hwparams, access);
    if (err < 0 && af_fmt_is_planar(ao->format)) {
        ao->format = af_fmt_from_planar(ao->format);
        access = SND_PCM_ACCESS_RW_INTERLEAVED;
        err = snd_pcm_hw_params_set_access(p->alsa, alsa_hwparams, access);
    }
    CHECK_ALSA_ERROR("Unable to set access type");

    bool found_format = false;
    int try_formats[AF_FORMAT_COUNT];
    af_get_best_sample_formats(ao->format, try_formats);
    for (int n = 0; try_formats[n]; n++) {
        if (af_fmt_is_planar(ao->format) != af_fmt_is_planar(try_formats[n]))
            continue; // implied SND_PCM_ACCESS mismatches
        p->alsa_fmt = find_alsa_format(try_formats[n]);
        MP_VERBOSE(ao, "trying format %s\n", af_fmt_to_str(try_formats[n]));
        if (snd_pcm_hw_params_test_format(p->alsa, alsa_hwparams, p->alsa_fmt) >= 0) {
            ao->format = try_formats[n];
            found_format = true;
            break;
        }
    }

    if (!found_format) {
        MP_ERR(ao, "Can't find appropriate sample format.\n");
        goto alsa_error;
    }

    err = snd_pcm_hw_params_set_format(p->alsa, alsa_hwparams, p->alsa_fmt);
    CHECK_ALSA_ERROR("Unable to set format");

    struct mp_chmap dev_chmap = ao->channels;
    if (af_fmt_is_spdif(ao->format) || p->cfg_ignore_chmap) {
        dev_chmap.num = 0; // disable chmap API
    } else if (dev_chmap.num == 1 && dev_chmap.speaker[0] == MP_SPEAKER_ID_FC) {
        // As yet another ALSA API inconsistency, mono is not reported correctly.
        dev_chmap.num = 0;
    } else if (query_chmaps(ao, &dev_chmap)) {
        ao->channels = dev_chmap;
    } else {
        // Assume only stereo and mono are supported.
        mp_chmap_from_channels(&ao->channels, MPMIN(2, dev_chmap.num));
        dev_chmap.num = 0;
    }

    int num_channels = ao->channels.num;
    err = snd_pcm_hw_params_set_channels_near
            (p->alsa, alsa_hwparams, &num_channels);
    CHECK_ALSA_ERROR("Unable to set channels");

    if (num_channels > MP_NUM_CHANNELS) {
        MP_FATAL(ao, "Too many audio channels (%d).\n", num_channels);
        goto alsa_error;
    }

    err = snd_pcm_hw_params_set_rate_near
            (p->alsa, alsa_hwparams, &ao->samplerate, NULL);
    CHECK_ALSA_ERROR("Unable to set samplerate-2");

    snd_pcm_hw_params_t *hwparams_backup;
    snd_pcm_hw_params_alloca(&hwparams_backup);
    snd_pcm_hw_params_copy(hwparams_backup, alsa_hwparams);

    // Cargo-culted buffer settings; might still be useful for PulseAudio.
    err = snd_pcm_hw_params_set_buffer_time_near
            (p->alsa, alsa_hwparams, &(unsigned int){BUFFER_TIME}, NULL);
    CHECK_ALSA_WARN("Unable to set buffer time near");
    if (err >= 0) {
        err = snd_pcm_hw_params_set_periods_near
                    (p->alsa, alsa_hwparams, &(unsigned int){FRAGCOUNT}, NULL);
        CHECK_ALSA_WARN("Unable to set periods");
    }
    if (err < 0)
        snd_pcm_hw_params_copy(alsa_hwparams, hwparams_backup);

    /* finally install hardware parameters */
    err = snd_pcm_hw_params(p->alsa, alsa_hwparams);
    CHECK_ALSA_ERROR("Unable to set hw-parameters");

    if (set_chmap(ao, &dev_chmap, num_channels) < 0)
        goto alsa_error;

    if (num_channels != ao->channels.num) {
        int req = ao->channels.num;
        mp_chmap_from_channels(&ao->channels, MPMIN(2, num_channels));
        MP_ERR(ao, "Asked for %d channels, got %d - fallback to %s.\n", req,
               num_channels, mp_chmap_to_str(&ao->channels));
    }

    err = snd_pcm_hw_params_get_buffer_size(alsa_hwparams, &p->buffersize);
    CHECK_ALSA_ERROR("Unable to get buffersize");

    err = snd_pcm_hw_params_get_period_size(alsa_hwparams, &p->outburst, NULL);
    CHECK_ALSA_ERROR("Unable to get period size");

    p->can_pause = snd_pcm_hw_params_can_pause(alsa_hwparams);

    snd_pcm_sw_params_t *alsa_swparams;
    snd_pcm_sw_params_alloca(&alsa_swparams);

    err = snd_pcm_sw_params_current(p->alsa, alsa_swparams);
    CHECK_ALSA_ERROR("Unable to get sw-parameters");

    snd_pcm_uframes_t boundary;
    err = snd_pcm_sw_params_get_boundary(alsa_swparams, &boundary);
    CHECK_ALSA_ERROR("Unable to get boundary");

    /* start playing when one period has been written */
    err = snd_pcm_sw_params_set_start_threshold
            (p->alsa, alsa_swparams, p->outburst);
    CHECK_ALSA_ERROR("Unable to set start threshold");

    /* disable underrun reporting */
    err = snd_pcm_sw_params_set_stop_threshold
            (p->alsa, alsa_swparams, boundary);
    CHECK_ALSA_ERROR("Unable to set stop threshold");

    /* play silence when there is an underrun */
    err = snd_pcm_sw_params_set_silence_size
            (p->alsa, alsa_swparams, boundary);
    CHECK_ALSA_ERROR("Unable to set silence size");

    err = snd_pcm_sw_params(p->alsa, alsa_swparams);
    CHECK_ALSA_ERROR("Unable to set sw-parameters");

    MP_VERBOSE(ao, "hw pausing supported: %s\n", p->can_pause ? "yes" : "no");
    MP_VERBOSE(ao, "buffersize: %d samples\n", (int)p->buffersize);
    MP_VERBOSE(ao, "period size: %d samples\n", (int)p->outburst);

    return 0;

alsa_error:
    uninit(ao);
    return -1;
}

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (!p->cfg_ni)
        ao->format = af_fmt_from_planar(ao->format);

    MP_VERBOSE(ao, "using ALSA version: %s\n", snd_asoundlib_version());

    int r = init_device(ao);

    // Sometimes, ALSA will advertise certain chmaps, but it's not possible to
    // set them. This can happen with dmix: as of alsa 1.0.29, dmix can do
    // stereo only, but advertises the surround chmaps of the underlying device.
    // In this case, e.g. setting 6 channels will succeed, but requesting  5.1
    // afterwards will fail. Then it will return something like "FL FR NA NA NA NA"
    // as channel map. This means we would have to pad stereo output to 6
    // channels with silence, which would require lots of extra processing. You
    // can't change the number of channels to 2 either, because the hw params
    // are already set! So just fuck it and reopen the device with the chmap
    // "cleaned out" of NA entries.
    if (r >= 0) {
        struct mp_chmap without_na = ao->channels;
        mp_chmap_remove_na(&without_na);

        if (mp_chmap_is_valid(&without_na) && without_na.num <= 2 &&
            ao->channels.num > 2)
        {
            MP_VERBOSE(ao, "Working around braindead dmix multichannel behavior.\n");
            uninit(ao);
            ao->channels = without_na;
            r = init_device(ao);
        }
    }

    return r;
}

static void drain(struct ao *ao)
{
    struct priv *p = ao->priv;
    snd_pcm_drain(p->alsa);
}

static int get_space(struct ao *ao)
{
    struct priv *p = ao->priv;
    snd_pcm_status_t *status;
    int err;

    snd_pcm_status_alloca(&status);

    err = snd_pcm_status(p->alsa, status);
    if (!check_device_present(ao, err))
        goto alsa_error;
    CHECK_ALSA_ERROR("cannot get pcm status");

    unsigned space = snd_pcm_status_get_avail(status);
    if (space > p->buffersize) // Buffer underrun?
        space = p->buffersize;
    return space / p->outburst * p->outburst;

alsa_error:
    return 0;
}

/* delay in seconds between first and last sample in buffer */
static double get_delay(struct ao *ao)
{
    struct priv *p = ao->priv;
    snd_pcm_sframes_t delay;

    if (snd_pcm_state(p->alsa) == SND_PCM_STATE_PAUSED)
        return p->delay_before_pause;

    if (snd_pcm_delay(p->alsa, &delay) < 0)
        return 0;

    if (delay < 0) {
        /* underrun - move the application pointer forward to catch up */
        snd_pcm_forward(p->alsa, -delay);
        delay = 0;
    }
    return delay / (double)ao->samplerate;
}

static void audio_pause(struct ao *ao)
{
    struct priv *p = ao->priv;
    int err;

    if (p->can_pause) {
        if (snd_pcm_state(p->alsa) == SND_PCM_STATE_RUNNING) {
            p->delay_before_pause = get_delay(ao);
            err = snd_pcm_pause(p->alsa, 1);
            CHECK_ALSA_ERROR("pcm pause error");
        }
    } else {
        if (snd_pcm_delay(p->alsa, &p->prepause_frames) < 0
            || p->prepause_frames < 0)
            p->prepause_frames = 0;
        p->delay_before_pause = p->prepause_frames / (double)ao->samplerate;

        err = snd_pcm_drop(p->alsa);
        CHECK_ALSA_ERROR("pcm drop error");
    }

alsa_error: ;
}

static void resume_device(struct ao *ao)
{
    struct priv *p = ao->priv;
    int err;

    if (snd_pcm_state(p->alsa) == SND_PCM_STATE_SUSPENDED) {
        MP_INFO(ao, "PCM in suspend mode, trying to resume.\n");

        while ((err = snd_pcm_resume(p->alsa)) == -EAGAIN)
            sleep(1);
    }
}

static void audio_resume(struct ao *ao)
{
    struct priv *p = ao->priv;
    int err;

    resume_device(ao);

    if (p->can_pause) {
        if (snd_pcm_state(p->alsa) == SND_PCM_STATE_PAUSED) {
            err = snd_pcm_pause(p->alsa, 0);
            CHECK_ALSA_ERROR("pcm resume error");
        }
    } else {
        MP_VERBOSE(ao, "resume not supported by hardware\n");
        err = snd_pcm_prepare(p->alsa);
        CHECK_ALSA_ERROR("pcm prepare error");
        if (p->prepause_frames)
            ao_play_silence(ao, p->prepause_frames);
    }

alsa_error: ;
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;
    int err;

    p->prepause_frames = 0;
    p->delay_before_pause = 0;
    err = snd_pcm_drop(p->alsa);
    CHECK_ALSA_ERROR("pcm prepare error");
    err = snd_pcm_prepare(p->alsa);
    CHECK_ALSA_ERROR("pcm prepare error");

alsa_error: ;
}

static int play(struct ao *ao, void **data, int samples, int flags)
{
    struct priv *p = ao->priv;
    snd_pcm_sframes_t res = 0;
    if (!(flags & AOPLAY_FINAL_CHUNK))
        samples = samples / p->outburst * p->outburst;

    if (samples == 0)
        return 0;

    do {
        if (af_fmt_is_planar(ao->format)) {
            res = snd_pcm_writen(p->alsa, data, samples);
        } else {
            res = snd_pcm_writei(p->alsa, data[0], samples);
        }

        if (res == -EINTR || res == -EAGAIN) { /* retry */
            res = 0;
        } else if (!check_device_present(ao, res)) {
            goto alsa_error;
        } else if (res < 0) {
            if (res == -ESTRPIPE) {  /* suspend */
                resume_device(ao);
            } else {
                MP_ERR(ao, "Write error: %s\n", snd_strerror(res));
            }
            res = snd_pcm_prepare(p->alsa);
            int err = res;
            CHECK_ALSA_ERROR("pcm prepare error");
            res = 0;
        }
    } while (res == 0);

    return res < 0 ? -1 : res;

alsa_error:
    return -1;
}

#define MAX_POLL_FDS 20
static int audio_wait(struct ao *ao, pthread_mutex_t *lock)
{
    struct priv *p = ao->priv;
    int err;

    int num_fds = snd_pcm_poll_descriptors_count(p->alsa);
    if (num_fds <= 0 || num_fds >= MAX_POLL_FDS)
        goto alsa_error;

    struct pollfd fds[MAX_POLL_FDS];
    err = snd_pcm_poll_descriptors(p->alsa, fds, num_fds);
    CHECK_ALSA_ERROR("cannot get pollfds");

    while (1) {
        int r = ao_wait_poll(ao, fds, num_fds, lock);
        if (r)
            return r;

        unsigned short revents;
        snd_pcm_poll_descriptors_revents(p->alsa, fds, num_fds, &revents);
        CHECK_ALSA_ERROR("cannot read poll events");

        if (revents & POLLERR)
            return -1;
        if (revents & POLLOUT)
            return 0;
    }
    return 0;

alsa_error:
    return -1;
}

static bool is_useless_device(char *name)
{
    char *crap[] = {"front", "rear", "center_lfe", "side", "surround21",
        "surround40", "surround41", "surround50", "surround51", "surround71",
        "sysdefault", "pulse", "null", "dsnoop", "dmix", "hw", "iec958"};
    for (int i = 0; i < MP_ARRAY_SIZE(crap); i++) {
        int l = strlen(crap[i]);
        if (name && strncmp(name, crap[i], l) == 0 &&
            (!name[l] || name[l] == ':'))
            return true;
    }
    return false;
}

static void list_devs(struct ao *ao, struct ao_device_list *list)
{
    void **hints;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
        return;

    for (int n = 0; hints[n]; n++) {
        char *name = snd_device_name_get_hint(hints[n], "NAME");
        char *desc = snd_device_name_get_hint(hints[n], "DESC");
        char *io = snd_device_name_get_hint(hints[n], "IOID");
        if (!is_useless_device(name) && (!io || strcmp(io, "Output") == 0)) {
            char desc2[1024];
            snprintf(desc2, sizeof(desc2), "%s", desc ? desc : "");
            for (int i = 0; desc2[i]; i++) {
                if (desc2[i] == '\n')
                    desc2[i] = '/';
            }
            ao_device_list_add(list, ao, &(struct ao_device_desc){name, desc2});
        }
        free(name);
        free(desc);
        free(io);
    }

    snd_device_name_free_hint(hints);
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_alsa = {
    .description = "ALSA audio output",
    .name      = "alsa",
    .init      = init,
    .uninit    = uninit,
    .control   = control,
    .get_space = get_space,
    .play      = play,
    .get_delay = get_delay,
    .pause     = audio_pause,
    .resume    = audio_resume,
    .reset     = reset,
    .drain     = drain,
    .wait      = audio_wait,
    .wakeup    = ao_wakeup_poll,
    .list_devs = list_devs,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .cfg_mixer_device = "default",
        .cfg_mixer_name = "Master",
        .cfg_mixer_index = 0,
        .cfg_ni = 0,
    },
    .options = (const struct m_option[]) {
        OPT_STRING("device", cfg_device, 0),
        OPT_FLAG("resample", cfg_resample, 0),
        OPT_STRING("mixer-device", cfg_mixer_device, 0),
        OPT_STRING("mixer-name", cfg_mixer_name, 0),
        OPT_INTRANGE("mixer-index", cfg_mixer_index, 0, 0, 99),
        OPT_FLAG("non-interleaved", cfg_ni, 0),
        OPT_FLAG("ignore-chmap", cfg_ignore_chmap, 0),
        {0}
    },
};
