/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "hwdec_vaapi.h"
#include "video/out/placebo/ra_pl.h"

static bool vaapi_vk_map(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    const struct pl_gpu *gpu = ra_pl_get(mapper->ra);

    struct ra_imgfmt_desc desc = {0};
    if (!ra_get_imgfmt_desc(mapper->ra, mapper->dst_params.imgfmt, &desc))
        return false;

    for (int n = 0; n < p->num_planes; n++) {
        if (p->desc.layers[n].num_planes > 1) {
            // Should never happen because we request separate layers
            MP_ERR(mapper, "Multi-plane VA surfaces are not supported\n");
            return false;
        }

        const struct ra_format *format = desc.planes[n];
        int id = p->desc.layers[n].object_index[0];
        int fd = p->desc.objects[id].fd;
        uint32_t size = p->desc.objects[id].size;
        uint32_t offset = p->desc.layers[n].offset[0];

        struct pl_tex_params tex_params = {
            .w = mp_image_plane_w(&p->layout, n),
            .h = mp_image_plane_h(&p->layout, n),
            .d = 0,
            .format = format->priv,
            .sampleable = true,
            .sample_mode = format->linear_filter ? PL_TEX_SAMPLE_LINEAR
                                                    : PL_TEX_SAMPLE_NEAREST,
            .import_handle = PL_HANDLE_DMA_BUF,
            .shared_mem = (struct pl_shared_mem) {
                .handle = {
                    .fd = fd,
                },
                .size = size,
                .offset = offset,
            },
        };

        const struct pl_tex *pltex = pl_tex_create(gpu, &tex_params);
        if (!pltex) {
            return false;
        }

        struct ra_tex *ratex = talloc_ptrtype(NULL, ratex);
        int ret = mppl_wrap_tex(mapper->ra, pltex, ratex);
        if (!ret) {
            pl_tex_destroy(gpu, &pltex);
            talloc_free(ratex);
            return false;
        }
        mapper->tex[n] = ratex;

        MP_TRACE(mapper, "Object %d with fd %d imported as %p\n",
                id, fd, ratex);
    }
    return true;
}

static void vaapi_vk_unmap(struct ra_hwdec_mapper *mapper)
{
    for (int n = 0; n < 4; n++)
        ra_tex_free(mapper->ra, &mapper->tex[n]);
}

bool vaapi_vk_init(const struct ra_hwdec *hw)
{
   struct priv_owner *p = hw->priv;

    const struct pl_gpu *gpu = ra_pl_get(hw->ra);
    if (!gpu) {
        // This is not a Vulkan RA;
        return false;
    }

    if (!(gpu->import_caps.tex & PL_HANDLE_DMA_BUF)) {
        MP_VERBOSE(hw, "VAAPI Vulkan interop requires support for "
                        "dma_buf import in Vulkan.\n");
        return false;
    }

    MP_VERBOSE(hw, "using VAAPI Vulkan interop\n");

    p->interop_map = vaapi_vk_map;
    p->interop_unmap = vaapi_vk_unmap;

    return true;
}