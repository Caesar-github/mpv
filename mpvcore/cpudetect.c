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
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <libavutil/cpu.h>
#include "compat/libav.h"

#include "config.h"
#include "mpvcore/cpudetect.h"
#include "mpvcore/mp_msg.h"

CpuCaps gCpuCaps;

static void dump_flag(const char *name, bool val)
{
    mp_msg(MSGT_CPUDETECT, MSGL_V, "CPU: %s: %s\n", name,
           val ? "enabled" : "disabled");
}

void GetCpuCaps(CpuCaps *c)
{
    memset(c, 0, sizeof(*c));
    int flags = av_get_cpu_flags();
#if ARCH_X86
    c->hasMMX = flags & AV_CPU_FLAG_MMX;
    c->hasMMX2 = flags & AV_CPU_FLAG_MMX2;
    c->hasSSE = flags & AV_CPU_FLAG_SSE;
    c->hasSSE2 = (flags & AV_CPU_FLAG_SSE2) && !(flags & AV_CPU_FLAG_SSE2SLOW);
    c->hasSSE3 = (flags & AV_CPU_FLAG_SSE3) && !(flags & AV_CPU_FLAG_SSE3SLOW);
    c->hasSSSE3 = flags & AV_CPU_FLAG_SSSE3;
#endif
    dump_flag("MMX", c->hasMMX);
    dump_flag("MMX2", c->hasMMX2);
    dump_flag("SSE", c->hasSSE);
    dump_flag("SSE2", c->hasSSE2);
    dump_flag("SSE3", c->hasSSE3);
    dump_flag("SSSE3", c->hasSSSE3);
}
