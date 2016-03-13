/*
 * Copyright (C) 2006 Michael Niedermayer <michaelni@gmx.at>
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

#include <stdlib.h>

#include <libavfilter/version.h>

#include "options/options.h"

#include "common/msg.h"
#include "vf.h"

#include "vf_lavfi.h"

struct vf_priv_s {
    int mode;
    int interlaced_only;
    struct vf_lw_opts *lw_opts;
};

static int vf_open(vf_instance_t *vf)
{
    struct vf_priv_s *p = vf->priv;

#if LIBAVFILTER_VERSION_MICRO >= 100
    const char *mode[] = {"send_frame", "send_field", "send_frame_nospatial",
                          "send_field_nospatial"};

    if (vf_lw_set_graph(vf, p->lw_opts, "yadif", "mode=%s:deint=%s", mode[p->mode],
                        p->interlaced_only ? "interlaced" : "all") >= 0)
    {
        return 1;
    }
#else
    // Libav numeric modes happen to match ours, but keep it explicit.
    const char *mode[] = {"0", "1", "2", "3"};
    if (vf_lw_set_graph(vf, p->lw_opts, "yadif", "mode=%s:auto=%d", mode[p->mode],
                        p->interlaced_only) >= 0)
    {
        return 1;
    }
#endif

    MP_FATAL(vf, "This version of libavfilter has no 'yadif' filter.\n");
    return 0;
}

#define OPT_BASE_STRUCT struct vf_priv_s
static const m_option_t vf_opts_fields[] = {
    OPT_CHOICE("mode", mode, 0,
               ({"frame", 0},
                {"field", 1},
                {"frame-nospatial", 2},
                {"field-nospatial", 3})),
    OPT_FLAG("interlaced-only", interlaced_only, 0),
    OPT_SUBSTRUCT("", lw_opts, vf_lw_conf, 0),
    {0}
};

const vf_info_t vf_info_yadif = {
    .description = "Yet Another DeInterlacing Filter",
    .name = "yadif",
    .open = vf_open,
    .priv_size = sizeof(struct vf_priv_s),
    .priv_defaults = &(const struct vf_priv_s){
        .mode = 1,
        .interlaced_only = 1,
    },
    .options = vf_opts_fields,
};
