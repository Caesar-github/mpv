/*
 * HTTP Cookies
 * Reads Netscape and Mozilla cookies.txt files
 *
 * Copyright (c) 2003 Dave Lambley <mplayer@davel.me.uk>
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
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <inttypes.h>
#include <limits.h>

#include "osdep/io.h"

#include "mpvcore/options.h"
#include "cookies.h"
#include "mpvcore/mp_msg.h"

#define MAX_COOKIES 20

char *cookies_file = NULL;

typedef struct cookie_list_type {
    char *name;
    char *value;
    char *domain;
    char *path;

    int secure;

    struct cookie_list_type *next;
} cookie_list_t;

/* Pointer to the linked list of cookies */
static struct cookie_list_type *cookie_list = NULL;


/* Like strdup, but stops at anything <31. */
static char *col_dup(const char *src)
{
    char *dst;
    int length = 0;

    while (src[length] > 31)
	length++;

    dst = malloc(length + 1);
    strncpy(dst, src, length);
    dst[length] = 0;

    return dst;
}

/* Finds the start of all the columns */
static int parse_line(char **ptr, char *cols[6])
{
    int col;
    cols[0] = *ptr;

    for (col = 1; col < 7; col++) {
	for (; (**ptr) > 31; (*ptr)++);
	if (**ptr == 0)
	    return 0;
	(*ptr)++;
	if ((*ptr)[-1] != 9)
	    return 0;
	cols[col] = (*ptr);
    }

    return 1;
}

/* Loads a file into RAM */
static char *load_file(const char *filename, int64_t * length)
{
    int fd;
    char *buffer = NULL;

    mp_msg(MSGT_NETWORK, MSGL_V, "Loading cookie file: %s\n", filename);

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
	mp_msg(MSGT_NETWORK, MSGL_V, "Could not open");
	goto err_out;
    }

    *length = lseek(fd, 0, SEEK_END);

    if (*length < 0) {
	mp_msg(MSGT_NETWORK, MSGL_V, "Could not find EOF");
	goto err_out;
    }

    if (*length > SIZE_MAX - 1) {
	mp_msg(MSGT_NETWORK, MSGL_V, "File too big, could not malloc.");
	goto err_out;
    }

    lseek(fd, 0, SEEK_SET);

    if (!(buffer = malloc(*length + 1))) {
	mp_msg(MSGT_NETWORK, MSGL_V, "Could not malloc.");
	goto err_out;
    }

    if (read(fd, buffer, *length) != *length) {
	mp_msg(MSGT_NETWORK, MSGL_V, "Read is behaving funny.");
	goto err_out;
    }
    close(fd);
    buffer[*length] = 0;

    return buffer;

err_out:
    if (fd != -1) close(fd);
    free(buffer);
    return NULL;
}

/* Loads a cookies.txt file into a linked list. */
static struct cookie_list_type *load_cookies_from(const char *filename,
						  struct cookie_list_type
						  *list)
{
    char *ptr, *file;
    int64_t length;

    ptr = file = load_file(filename, &length);
    if (!ptr)
	return list;

    while (*ptr) {
	char *cols[7];
	if (parse_line(&ptr, cols)) {
	    struct cookie_list_type *new;
	    new = malloc(sizeof(cookie_list_t));
	    new->name = col_dup(cols[5]);
	    new->value = col_dup(cols[6]);
	    new->path = col_dup(cols[2]);
	    new->domain = col_dup(cols[0]);
	    new->secure = (*(cols[3]) == 't') || (*(cols[3]) == 'T');
	    new->next = list;
	    list = new;
	}
    }
    free(file);
    return list;
}

/* Attempt to load cookies.txt. Returns a pointer to the linked list contain the cookies. */
static struct cookie_list_type *load_cookies(void)
{
    if (cookies_file)
	return load_cookies_from(cookies_file, NULL);

    return NULL;
}

// Return a cookies string as expected by lavf (libavformat/http.c). The format
// is like a Set-Cookie header (http://curl.haxx.se/rfc/cookie_spec.html),
// separated by newlines.
char *cookies_lavf(void)
{
    if (!cookie_list)
        cookie_list = load_cookies();

    struct cookie_list_type *list = cookie_list;
    char *res = talloc_strdup(NULL, "");

    while (list) {
        res = talloc_asprintf_append_buffer(res,
                    "%s=%s; path=%s; domain=%s; %s\n", list->name, list->value,
                    list->path, list->domain, list->secure ? "secure" : "");
        list = list->next;
    }

    return res;
}
