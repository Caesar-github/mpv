/*
 * based on video_out_null.c from mpeg2dec
 *
 * Copyright (C) Aaron Holtzman - June 2000
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "config.h"
#include "common/msg.h"
#include "vo.h"
#include "video/mp_image.h"

static void draw_image(struct vo *vo, mp_image_t *mpi)
{
    talloc_free(mpi);
}

static void flip_page(struct vo *vo)
{
}

static int query_format(struct vo *vo, int format)
{
    return 1;
}

static int reconfig(struct vo *vo, struct mp_image_params *params, int flags)
{
    return 0;
}

static void uninit(struct vo *vo)
{
}

static int preinit(struct vo *vo)
{
    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    return VO_NOTIMPL;
}

const struct vo_driver video_out_null = {
    .description = "Null video output",
    .name = "null",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_image = draw_image,
    .flip_page = flip_page,
    .uninit = uninit,
};
