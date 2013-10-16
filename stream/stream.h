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

#ifndef MPLAYER_STREAM_H
#define MPLAYER_STREAM_H

#include "config.h"
#include "mpvcore/mp_msg.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <fcntl.h>

#include "mpvcore/bstr.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

enum streamtype {
    STREAMTYPE_GENERIC = 0,
    STREAMTYPE_FILE,
    STREAMTYPE_RADIO,
    STREAMTYPE_DVB,
    STREAMTYPE_DVD,
    STREAMTYPE_PVR,
    STREAMTYPE_TV,
    STREAMTYPE_MF,
    STREAMTYPE_AVDEVICE,
};

#define STREAM_BUFFER_SIZE 2048
#define STREAM_MAX_SECTOR_SIZE (8 * 1024)

// Max buffer for initial probe.
#define STREAM_MAX_BUFFER_SIZE (2 * 1024 * 1024)


// stream->mode
#define STREAM_READ  0
#define STREAM_WRITE 1

// flags for stream_open_ext (this includes STREAM_READ and STREAM_WRITE)
#define STREAM_NO_FILTERS 2

// stream->flags
#define MP_STREAM_FAST_SKIPPING 1 // allow forward seeks by skipping
#define MP_STREAM_SEEK_BW  2
#define MP_STREAM_SEEK_FW  4
#define MP_STREAM_SEEK  (MP_STREAM_SEEK_BW | MP_STREAM_SEEK_FW)

#define STREAM_NO_MATCH -2
#define STREAM_UNSUPPORTED -1
#define STREAM_ERROR 0
#define STREAM_OK    1

#define MAX_STREAM_PROTOCOLS 20

enum stream_ctrl {
    STREAM_CTRL_GET_TIME_LENGTH = 1,
    STREAM_CTRL_SEEK_TO_CHAPTER,
    STREAM_CTRL_GET_CURRENT_CHAPTER,
    STREAM_CTRL_GET_NUM_CHAPTERS,
    STREAM_CTRL_GET_CURRENT_TIME,
    STREAM_CTRL_SEEK_TO_TIME,
    STREAM_CTRL_GET_SIZE,
    STREAM_CTRL_GET_ASPECT_RATIO,
    STREAM_CTRL_GET_NUM_ANGLES,
    STREAM_CTRL_GET_ANGLE,
    STREAM_CTRL_SET_ANGLE,
    STREAM_CTRL_GET_NUM_TITLES,
    STREAM_CTRL_GET_LANG,
    STREAM_CTRL_GET_CURRENT_TITLE,
    STREAM_CTRL_GET_CACHE_SIZE,
    STREAM_CTRL_GET_CACHE_FILL,
    STREAM_CTRL_GET_CACHE_IDLE,
    STREAM_CTRL_RECONNECT,
    // DVD/Bluray, signal general support for GET_CURRENT_TIME etc.
    STREAM_CTRL_MANAGES_TIMELINE,
    STREAM_CTRL_GET_START_TIME,
    STREAM_CTRL_GET_CHAPTER_TIME,
    STREAM_CTRL_GET_DVD_INFO,
    STREAM_CTRL_SET_CONTENTS,
    STREAM_CTRL_GET_METADATA,
};

struct stream_lang_req {
    int type;     // STREAM_AUDIO, STREAM_SUB
    int id;
    char name[50];
};

struct stream_dvd_info_req {
    unsigned int palette[16];
    int num_subs;
};

struct stream;
typedef struct stream_info_st {
    const char *name;
    // opts is set from ->opts
    int (*open)(struct stream *st, int mode);
    const char **protocols;
    int priv_size;
    const void *priv_defaults;
    const struct m_option *options;
    const char **url_options;
    bool stream_filter;
} stream_info_t;

typedef struct stream {
    const struct stream_info_st *info;

    // Read
    int (*fill_buffer)(struct stream *s, char *buffer, int max_len);
    // Write
    int (*write_buffer)(struct stream *s, char *buffer, int len);
    // Seek
    int (*seek)(struct stream *s, int64_t pos);
    // Control
    // Will be later used to let streams like dvd and cdda report
    // their structure (ie tracks, chapters, etc)
    int (*control)(struct stream *s, int cmd, void *arg);
    // Close
    void (*close)(struct stream *s);

    enum streamtype type; // see STREAMTYPE_*
    enum streamtype uncached_type; // if stream is cache, type of wrapped str.
    int flags; // MP_STREAM_SEEK_* or'ed flags
    int sector_size; // sector size (seek will be aligned on this size if non 0)
    int read_chunk; // maximum amount of data to read at once to limit latency
    unsigned int buf_pos, buf_len;
    int64_t pos, start_pos, end_pos;
    int eof;
    int mode; //STREAM_READ or STREAM_WRITE
    bool streaming;     // known to be a network stream if true
    void *priv; // used for DVD, TV, RTSP etc
    char *url;  // filename/url (possibly including protocol prefix)
    char *path; // filename (url without protocol prefix)
    char *mime_type; // when HTTP streaming is used
    char *demuxer; // request demuxer to be used
    char *lavf_type; // name of expected demuxer type for lavf
    bool safe_origin; // used for playlists that can be opened safely
    struct MPOpts *opts;

    FILE *capture_file;
    char *capture_filename;

    struct stream *uncached_stream; // underlying stream for cache wrapper
    struct stream *source;

    // Includes additional padding in case sizes get rounded up by sector size.
    unsigned char buffer[];
} stream_t;

int stream_fill_buffer(stream_t *s);

void stream_set_capture_file(stream_t *s, const char *filename);

int stream_enable_cache_percent(stream_t **stream, int64_t stream_cache_size,
                                int64_t stream_cache_def_size,
                                float stream_cache_min_percent,
                                float stream_cache_seek_min_percent);

// Internal
int stream_cache_init(stream_t *cache, stream_t *stream, int64_t size,
                      int64_t min, int64_t seek_limit);

int stream_write_buffer(stream_t *s, unsigned char *buf, int len);

inline static int stream_read_char(stream_t *s)
{
    return (s->buf_pos < s->buf_len) ? s->buffer[s->buf_pos++] :
           (stream_fill_buffer(s) ? s->buffer[s->buf_pos++] : -256);
}

inline static unsigned int stream_read_dword(stream_t *s)
{
    unsigned int y;
    y = stream_read_char(s);
    y = (y << 8) | stream_read_char(s);
    y = (y << 8) | stream_read_char(s);
    y = (y << 8) | stream_read_char(s);
    return y;
}

inline static uint64_t stream_read_qword(stream_t *s)
{
    uint64_t y;
    y = stream_read_char(s);
    y = (y << 8) | stream_read_char(s);
    y = (y << 8) | stream_read_char(s);
    y = (y << 8) | stream_read_char(s);
    y = (y << 8) | stream_read_char(s);
    y = (y << 8) | stream_read_char(s);
    y = (y << 8) | stream_read_char(s);
    y = (y << 8) | stream_read_char(s);
    return y;
}

unsigned char *stream_read_line(stream_t *s, unsigned char *mem, int max,
                                int utf16);
int stream_skip_bom(struct stream *s);

inline static int stream_eof(stream_t *s)
{
    return s->eof;
}

inline static int64_t stream_tell(stream_t *s)
{
    return s->pos + s->buf_pos - s->buf_len;
}

int stream_skip(stream_t *s, int64_t len);
int stream_seek(stream_t *s, int64_t pos);
int stream_read(stream_t *s, char *mem, int total);
int stream_read_partial(stream_t *s, char *buf, int buf_size);
struct bstr stream_peek(stream_t *s, int len);

struct MPOpts;

struct bstr stream_read_complete(struct stream *s, void *talloc_ctx,
                                 int max_size);
int stream_control(stream_t *s, int cmd, void *arg);
void stream_update_size(stream_t *s);
void free_stream(stream_t *s);
struct stream *stream_create(const char *url, int flags, struct MPOpts *options);
struct stream *stream_open(const char *filename, struct MPOpts *options);
stream_t *open_output_stream(const char *filename, struct MPOpts *options);
stream_t *open_memory_stream(void *data, int len);
struct demux_stream;

/// Set the callback to be used by libstream to check for user
/// interruption during long blocking operations (cache filling, etc).
struct input_ctx;
void stream_set_interrupt_callback(int (*cb)(struct input_ctx *, int),
                                   struct input_ctx *ctx);
/// Call the interrupt checking callback if there is one and
/// wait for time milliseconds
int stream_check_interrupt(int time);

bool stream_manages_timeline(stream_t *s);

/* stream/stream_dvd.c */
extern int dvd_title;
extern int dvd_angle;
extern int dvd_speed;
extern char *dvd_device, *cdrom_device;

extern int bluray_angle;
extern char *bluray_device;

typedef struct {
    int id; // 0 - 31 mpeg; 128 - 159 ac3; 160 - 191 pcm
    int language;
    int type;
    int channels;
} stream_language_t;

void mp_url_unescape_inplace(char *buf);
char *mp_url_escape(void *talloc_ctx, const char *s, const char *ok);

#endif /* MPLAYER_STREAM_H */
