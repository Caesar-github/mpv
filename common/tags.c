/*
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

#include <libavutil/dict.h>
#include "tags.h"
#include "misc/bstr.h"

void mp_tags_set_str(struct mp_tags *tags, const char *key, const char *value)
{
    mp_tags_set_bstr(tags, bstr0(key), bstr0(value));
}

void mp_tags_set_bstr(struct mp_tags *tags, bstr key, bstr value)
{
    for (int n = 0; n < tags->num_keys; n++) {
        if (bstrcasecmp0(key, tags->keys[n]) == 0) {
            talloc_free(tags->values[n]);
            tags->values[n] = bstrto0(tags, value);
            return;
        }
    }

    MP_RESIZE_ARRAY(tags, tags->keys,   tags->num_keys + 1);
    MP_RESIZE_ARRAY(tags, tags->values, tags->num_keys + 1);
    tags->keys[tags->num_keys]   = bstrto0(tags, key);
    tags->values[tags->num_keys] = bstrto0(tags, value);
    tags->num_keys++;
}

char *mp_tags_get_str(struct mp_tags *tags, const char *key)
{
    return mp_tags_get_bstr(tags, bstr0(key));
}

char *mp_tags_get_bstr(struct mp_tags *tags, bstr key)
{
    for (int n = 0; n < tags->num_keys; n++) {
        if (bstrcasecmp0(key, tags->keys[n]) == 0)
            return tags->values[n];
    }
    return NULL;
}

void mp_tags_clear(struct mp_tags *tags)
{
    *tags = (struct mp_tags){0};
    talloc_free_children(tags);
}

struct mp_tags *mp_tags_dup(void *tparent, struct mp_tags *tags)
{
    struct mp_tags *new = talloc_zero(tparent, struct mp_tags);
    MP_RESIZE_ARRAY(new, new->keys,   tags->num_keys);
    MP_RESIZE_ARRAY(new, new->values, tags->num_keys);
    new->num_keys = tags->num_keys;
    for (int n = 0; n < tags->num_keys; n++) {
        new->keys[n] = talloc_strdup(new, tags->keys[n]);
        new->values[n] = talloc_strdup(new, tags->values[n]);
    }
    return new;
}

void mp_tags_merge(struct mp_tags *tags, struct mp_tags *src)
{
    for (int n = 0; n < src->num_keys; n++)
        mp_tags_set_str(tags, src->keys[n], src->values[n]);
}

void mp_tags_copy_from_av_dictionary(struct mp_tags *tags,
                                     struct AVDictionary *av_dict)
{
    AVDictionaryEntry *entry = NULL;
    while ((entry = av_dict_get(av_dict, "", entry, AV_DICT_IGNORE_SUFFIX)))
        mp_tags_set_str(tags, entry->key, entry->value);
}
