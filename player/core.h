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

#ifndef MPLAYER_MP_CORE_H
#define MPLAYER_MP_CORE_H

#include <stdbool.h>
#include <pthread.h>

#include "libmpv/client.h"

#include "common/common.h"
#include "options/options.h"
#include "sub/osd.h"
#include "audio/audio.h"
#include "video/mp_image.h"
#include "video/out/vo.h"

#include "lavfi.h"

// definitions used internally by the core player code

enum stop_play_reason {
    KEEP_PLAYING = 0,   // must be 0, numeric values of others do not matter
    AT_END_OF_FILE,     // file has ended, prepare to play next
                        // also returned on unrecoverable playback errors
    PT_NEXT_ENTRY,      // prepare to play next entry in playlist
    PT_CURRENT_ENTRY,   // prepare to play mpctx->playlist->current
    PT_STOP,            // stop playback, clear playlist
    PT_RELOAD_FILE,     // restart playback
    PT_QUIT,            // stop playback, quit player
    PT_ERROR,           // play next playlist entry (due to an error)
};

enum mp_osd_seek_info {
    OSD_SEEK_INFO_BAR           = 1,
    OSD_SEEK_INFO_TEXT          = 2,
    OSD_SEEK_INFO_CHAPTER_TEXT  = 4,
    OSD_SEEK_INFO_EDITION       = 8,
    OSD_SEEK_INFO_CURRENT_FILE  = 16,
};


enum {
    // other constants
    MAX_OSD_LEVEL = 3,
    OSD_LEVEL_INVISIBLE = 4,
    OSD_BAR_SEEK = 256,

    MAX_NUM_VO_PTS = 100,
};

enum seek_type {
    MPSEEK_NONE = 0,
    MPSEEK_RELATIVE,
    MPSEEK_ABSOLUTE,
    MPSEEK_FACTOR,
    MPSEEK_BACKSTEP,
};

enum seek_precision {
    MPSEEK_DEFAULT = 0,
    // The following values are numerically sorted by increasing precision
    MPSEEK_KEYFRAME,
    MPSEEK_EXACT,
    MPSEEK_VERY_EXACT,
};

enum video_sync {
    VS_DEFAULT = 0,
    VS_DISP_RESAMPLE,
    VS_DISP_RESAMPLE_VDROP,
    VS_DISP_RESAMPLE_NONE,
    VS_DISP_ADROP,
    VS_DISP_VDROP,
    VS_DISP_NONE,
    VS_NONE,
};

#define VS_IS_DISP(x) ((x) == VS_DISP_RESAMPLE ||       \
                       (x) == VS_DISP_RESAMPLE_VDROP || \
                       (x) == VS_DISP_RESAMPLE_NONE ||  \
                       (x) == VS_DISP_ADROP ||          \
                       (x) == VS_DISP_VDROP ||          \
                       (x) == VS_DISP_NONE)

// Information about past video frames that have been sent to the VO.
struct frame_info {
    double pts;
    double duration;        // PTS difference to next frame
    double approx_duration; // possibly fixed/smoothed out duration
    double av_diff;         // A/V diff at time of scheduling
    int num_vsyncs;         // scheduled vsyncs, if using display-sync
};

struct track {
    enum stream_type type;

    // Currently used for decoding.
    bool selected;

    // The type specific ID, also called aid (audio), sid (subs), vid (video).
    // For UI purposes only; this ID doesn't have anything to do with any
    // IDs coming from demuxers or container files.
    int user_tid;

    int demuxer_id; // same as stream->demuxer_id. -1 if not set.
    int ff_index; // same as stream->ff_index, or 0.

    char *title;
    bool default_track, forced_track;
    bool attached_picture;
    char *lang;

    // If this track is from an external file (e.g. subtitle file).
    bool is_external;
    bool no_default;            // pretend it's not external for auto-selection
    char *external_filename;
    bool auto_loaded;

    struct demuxer *demuxer;
    // Invariant: !stream || stream->demuxer == demuxer
    struct sh_stream *stream;

    // Current subtitle state (or cached state if selected==false).
    struct dec_sub *d_sub;

    // Current decoding state (NULL if selected==false)
    struct dec_video *d_video;
    struct dec_audio *d_audio;

    // Where the decoded result goes to (one of them is not NULL if active)
    struct vo_chain *vo_c;
    struct ao_chain *ao_c;
    struct lavfi_pad *sink;
};

// Summarizes video filtering and output.
struct vo_chain {
    struct mp_log *log;

    struct mp_hwdec_info *hwdec_info;
    double container_fps;

    struct vf_chain *vf;
    struct vo *vo;

    // 1-element input frame queue.
    struct mp_image *input_mpi;

    // Last known input_mpi format (so vf can be reinitialized any time).
    struct mp_image_params input_format;

    struct track *track;
    struct lavfi_pad *filter_src;
    struct dec_video *video_src;

    // - video consists of a single picture, which should be shown only once
    // - do not sync audio to video in any way
    bool is_coverart;
};

// Like vo_chain, for audio.
struct ao_chain {
    struct mp_log *log;

    double pts; // timestamp of first sample output by decoder
    bool spdif_passthrough, spdif_failed;
    bool pts_reset;

    struct af_stream *af;
    struct ao *ao;
    struct mp_audio_buffer *ao_buffer;

    // 1-element input frame queue.
    struct mp_audio *input_frame;

    // Last known input_mpi format (so vf can be reinitialized any time).
    struct mp_audio input_format;

    struct track *track;
    struct lavfi_pad *filter_src;
    struct dec_audio *audio_src;
};

/* Note that playback can be paused, stopped, etc. at any time. While paused,
 * playback restart is still active, because you want seeking to work even
 * if paused.
 * The main purpose of distinguishing these states is proper reinitialization
 * of A/V sync.
 */
enum playback_status {
    // code may compare status values numerically
    STATUS_SYNCING,     // seeking for a position to resume
    STATUS_FILLING,     // decoding more data (so you start with full buffers)
    STATUS_READY,       // buffers full, playback can be started any time
    STATUS_PLAYING,     // normal playback
    STATUS_DRAINING,    // decoding has ended; still playing out queued buffers
    STATUS_EOF,         // playback has ended, or is disabled
};

#define NUM_PTRACKS 2

typedef struct MPContext {
    bool initialized;
    bool autodetach;
    struct mpv_global *global;
    struct MPOpts *opts;
    struct mp_log *log;
    struct m_config *mconfig;
    struct input_ctx *input;
    struct mp_client_api *clients;
    struct mp_dispatch_queue *dispatch;
    struct mp_cancel *playback_abort;

    struct mp_log *statusline;
    struct osd_state *osd;
    char *term_osd_text;
    char *term_osd_status;
    char *term_osd_subs;
    char *term_osd_contents;
    char *last_window_title;
    struct voctrl_playback_state vo_playback_state;

    int add_osd_seek_info; // bitfield of enum mp_osd_seek_info
    double osd_visible; // for the osd bar only
    int osd_function;
    double osd_function_visible;
    double osd_msg_visible;
    double osd_msg_next_duration;
    double osd_last_update;
    bool osd_force_update;
    char *osd_msg_text;
    bool osd_show_pos;
    struct osd_progbar_state osd_progbar;

    struct playlist *playlist;
    struct playlist_entry *playing; // currently playing file
    char *filename; // immutable copy of playing->filename (or NULL)
    char *stream_open_filename;
    enum stop_play_reason stop_play;
    bool playback_initialized; // playloop can be run/is running
    int error_playing;

    // Return code to use with PT_QUIT
    int quit_custom_rc;
    bool has_quit_custom_rc;
    char **resume_defaults;

    // Global file statistics
    int files_played;       // played without issues (even if stopped by user)
    int files_errored;      // played, but errors happened at one point
    int files_broken;       // couldn't be played at all

    // Current file statistics
    int64_t shown_vframes, shown_aframes;

    struct demux_chapter *chapters;
    int num_chapters;

    struct demuxer *demuxer;
    struct mp_tags *filtered_tags;

    struct track **tracks;
    int num_tracks;

    char *track_layout_hash;

    // Selected tracks. NULL if no track selected.
    // There can be NUM_PTRACKS of the same STREAM_TYPE selected at once.
    // Currently, this is used for the secondary subtitle track only.
    struct track *current_track[NUM_PTRACKS][STREAM_TYPE_COUNT];

    struct lavfi *lavfi;

    struct mixer *mixer;
    struct ao *ao;
    struct mp_audio *ao_decoder_fmt; // for weak gapless audio check
    struct ao_chain *ao_chain;

    struct vo_chain *vo_chain;

    struct vo *video_out;
    // next_frame[0] is the next frame, next_frame[1] the one after that.
    // The +1 is for adding 1 additional frame in backstep mode.
    struct mp_image *next_frames[VO_MAX_REQ_FRAMES + 1];
    int num_next_frames;
    struct mp_image *saved_frame;   // for hrseek_lastframe and hrseek_backstep

    enum playback_status video_status, audio_status;
    bool restart_complete;
    // Factors to multiply with opts->playback_speed to get the total audio or
    // video speed (usually 1.0, but can be set to by the sync code).
    double speed_factor_v, speed_factor_a;
    // Redundant values set from opts->playback_speed and speed_factor_*.
    // update_playback_speed() updates them from the other fields.
    double audio_speed, video_speed;
    bool display_sync_active;
    bool display_sync_broken;
    int display_sync_drift_dir;
    // Timing error (in seconds) due to rounding on vsync boundaries
    double display_sync_error;
    double audio_drop_throttle;
    // Number of mistimed frames.
    int mistimed_frames_total;
    bool hrseek_active;     // skip all data until hrseek_pts
    bool hrseek_framedrop;  // allow decoder to drop frames before hrseek_pts
    bool hrseek_lastframe;  // drop everything until last frame reached
    bool hrseek_backstep;   // go to frame before seek target
    double hrseek_pts;
    // AV sync: the next frame should be shown when the audio out has this
    // much (in seconds) buffered data left. Increased when more data is
    // written to the ao, decreased when moving to the next video frame.
    double delay;
    // AV sync: time in seconds until next frame should be shown
    double time_frame;
    // How much video timing has been changed to make it match the audio
    // timeline. Used for status line information only.
    double total_avsync_change;
    // Used to compute the number of frames dropped in a row.
    int dropped_frames_start;
    // A-V sync difference when last frame was displayed. Kept to display
    // the same value if the status line is updated at a time where no new
    // video frame is shown.
    double last_av_difference;
    /* timestamp of video frame currently visible on screen
     * (or at least queued to be flipped by VO) */
    double video_pts;
    double last_seek_pts;
    // As video_pts, but is not reset when seeking away. (For the very short
    // period of time until a new frame is decoded and shown.)
    double last_vo_pts;
    // Video PTS, or audio PTS if video has ended.
    double playback_pts;
    // audio stats only
    int64_t audio_stat_start;
    double written_audio;

    int last_chapter;

    // Past timestamps etc.
    // The newest frame is at index 0.
    struct frame_info *past_frames;
    int num_past_frames;

    double next_heartbeat;
    double last_idle_tick;
    double next_cache_update;

    double sleeptime;      // number of seconds to sleep before next iteration

    double mouse_timer;
    unsigned int mouse_event_ts;
    bool mouse_cursor_visible;

    // used to prevent hanging in some error cases
    double start_timestamp;

    // Timestamp from the last time some timing functions read the
    // current time, in microseconds.
    // Used to turn a new time value to a delta from last time.
    int64_t last_time;

    // Used to communicate the parameters of a seek between parts
    struct seek_params {
        enum seek_type type;
        enum seek_precision exact;
        double amount;
        bool immediate; // disable seek delay logic
    } seek;

    /* Heuristic for relative chapter seeks: keep track which chapter
     * the user wanted to go to, even if we aren't exactly within the
     * boundaries of that chapter due to an inaccurate seek. */
    int last_chapter_seek;
    double last_chapter_pts;

    bool paused;
    // step this many frames, then pause
    int step_frames;
    // Counted down each frame, stop playback if 0 is reached. (-1 = disable)
    int max_frames;
    bool playing_msg_shown;

    bool paused_for_cache;
    double cache_stop_time, cache_wait_time;

    // Set after showing warning about decoding being too slow for realtime
    // playback rate. Used to avoid showing it multiple times.
    bool drop_message_shown;

    char *cached_watch_later_configdir;

    struct screenshot_ctx *screenshot_ctx;
    struct command_ctx *command_ctx;
    struct encode_lavc_context *encode_lavc_ctx;

    struct mp_ipc_ctx *ipc_ctx;

    struct mpv_opengl_cb_context *gl_cb_ctx;
} MPContext;

// audio.c
void reset_audio_state(struct MPContext *mpctx);
void reinit_audio_chain(struct MPContext *mpctx);
int init_audio_decoder(struct MPContext *mpctx, struct track *track);
int reinit_audio_filters(struct MPContext *mpctx);
double playing_audio_pts(struct MPContext *mpctx);
void fill_audio_out_buffers(struct MPContext *mpctx);
double written_audio_pts(struct MPContext *mpctx);
void clear_audio_output_buffers(struct MPContext *mpctx);
void update_playback_speed(struct MPContext *mpctx);
void uninit_audio_out(struct MPContext *mpctx);
void uninit_audio_chain(struct MPContext *mpctx);
int init_audio_decoder(struct MPContext *mpctx, struct track *track);
void reinit_audio_chain_src(struct MPContext *mpctx, struct lavfi_pad *src);

// configfiles.c
void mp_parse_cfgfiles(struct MPContext *mpctx);
void mp_load_auto_profiles(struct MPContext *mpctx);
void mp_get_resume_defaults(struct MPContext *mpctx);
void mp_load_playback_resume(struct MPContext *mpctx, const char *file);
void mp_write_watch_later_conf(struct MPContext *mpctx);
struct playlist_entry *mp_check_playlist_resume(struct MPContext *mpctx,
                                                struct playlist *playlist);

// loadfile.c
void uninit_player(struct MPContext *mpctx, unsigned int mask);
struct track *mp_add_external_file(struct MPContext *mpctx, char *filename,
                                   enum stream_type filter);
#define FLAG_MARK_SELECTION 1
void mp_switch_track(struct MPContext *mpctx, enum stream_type type,
                     struct track *track, int flags);
void mp_switch_track_n(struct MPContext *mpctx, int order,
                       enum stream_type type, struct track *track, int flags);
void mp_deselect_track(struct MPContext *mpctx, struct track *track);
struct track *mp_track_by_tid(struct MPContext *mpctx, enum stream_type type,
                              int tid);
void add_demuxer_tracks(struct MPContext *mpctx, struct demuxer *demuxer);
bool mp_remove_track(struct MPContext *mpctx, struct track *track);
struct playlist_entry *mp_next_file(struct MPContext *mpctx, int direction,
                                    bool force);
void mp_set_playlist_entry(struct MPContext *mpctx, struct playlist_entry *e);
void mp_play_files(struct MPContext *mpctx);
void update_demuxer_properties(struct MPContext *mpctx);
void print_track_list(struct MPContext *mpctx, const char *msg);
void reselect_demux_stream(struct MPContext *mpctx, struct track *track);
void prepare_playlist(struct MPContext *mpctx, struct playlist *pl);
void autoload_external_files(struct MPContext *mpctx);
struct track *select_default_track(struct MPContext *mpctx, int order,
                                   enum stream_type type);

// main.c
int mp_initialize(struct MPContext *mpctx, char **argv);
struct MPContext *mp_create(void);
void mp_destroy(struct MPContext *mpctx);
void mp_print_version(struct mp_log *log, int always);
void wakeup_playloop(void *ctx);

// misc.c
double rel_time_to_abs(struct MPContext *mpctx, struct m_rel_time t);
double get_play_end_pts(struct MPContext *mpctx);
void merge_playlist_files(struct playlist *pl);
float mp_get_cache_percent(struct MPContext *mpctx);
bool mp_get_cache_idle(struct MPContext *mpctx);
void update_vo_playback_state(struct MPContext *mpctx);
void update_window_title(struct MPContext *mpctx, bool force);
void error_on_track(struct MPContext *mpctx, struct track *track);
int stream_dump(struct MPContext *mpctx, const char *source_filename);
int mpctx_run_reentrant(struct MPContext *mpctx, void (*thread_fn)(void *arg),
                        void *thread_arg);
struct mpv_global *create_sub_global(struct MPContext *mpctx);

// osd.c
void set_osd_bar(struct MPContext *mpctx, int type,
                 double min, double max, double neutral, double val);
bool set_osd_msg(struct MPContext *mpctx, int level, int time,
                 const char* fmt, ...) PRINTF_ATTRIBUTE(4,5);
void set_osd_function(struct MPContext *mpctx, int osd_function);
void term_osd_set_subs(struct MPContext *mpctx, const char *text);
void get_current_osd_sym(struct MPContext *mpctx, char *buf, size_t buf_size);
void set_osd_bar_chapters(struct MPContext *mpctx, int type);

// playloop.c
void mp_wait_events(struct MPContext *mpctx, double sleeptime);
void mp_process_input(struct MPContext *mpctx);
double get_relative_time(struct MPContext *mpctx);
void reset_playback_state(struct MPContext *mpctx);
void pause_player(struct MPContext *mpctx);
void unpause_player(struct MPContext *mpctx);
void add_step_frame(struct MPContext *mpctx, int dir);
void queue_seek(struct MPContext *mpctx, enum seek_type type, double amount,
                enum seek_precision exact, bool immediate);
double get_time_length(struct MPContext *mpctx);
double get_current_time(struct MPContext *mpctx);
double get_playback_time(struct MPContext *mpctx);
int get_percent_pos(struct MPContext *mpctx);
double get_current_pos_ratio(struct MPContext *mpctx, bool use_range);
int get_current_chapter(struct MPContext *mpctx);
char *chapter_display_name(struct MPContext *mpctx, int chapter);
char *chapter_name(struct MPContext *mpctx, int chapter);
double chapter_start_time(struct MPContext *mpctx, int chapter);
int get_chapter_count(struct MPContext *mpctx);
double get_cache_buffering_percentage(struct MPContext *mpctx);
void execute_queued_seek(struct MPContext *mpctx);
void run_playloop(struct MPContext *mpctx);
void mp_idle(struct MPContext *mpctx);
void idle_loop(struct MPContext *mpctx);
int handle_force_window(struct MPContext *mpctx, bool force);
void seek_to_last_frame(struct MPContext *mpctx);

// scripting.c
struct mp_scripting {
    const char *file_ext;   // e.g. "lua"
    int (*load)(struct mpv_handle *client, const char *filename);
};
void mp_load_scripts(struct MPContext *mpctx);

// sub.c
void reset_subtitle_state(struct MPContext *mpctx);
void reinit_sub(struct MPContext *mpctx, struct track *track);
void reinit_sub_all(struct MPContext *mpctx);
void uninit_sub(struct MPContext *mpctx, struct track *track);
void uninit_sub_all(struct MPContext *mpctx);
void update_osd_msg(struct MPContext *mpctx);
bool update_subtitles(struct MPContext *mpctx, double video_pts);

// video.c
int video_get_colors(struct vo_chain *vo_c, const char *item, int *value);
int video_set_colors(struct vo_chain *vo_c, const char *item, int value);
int video_vf_vo_control(struct vo_chain *vo_c, int vf_cmd, void *data);
void reset_video_state(struct MPContext *mpctx);
int init_video_decoder(struct MPContext *mpctx, struct track *track);
int reinit_video_chain(struct MPContext *mpctx);
int reinit_video_chain_src(struct MPContext *mpctx, struct lavfi_pad *src);
int reinit_video_filters(struct MPContext *mpctx);
void write_video(struct MPContext *mpctx);
void mp_force_video_refresh(struct MPContext *mpctx);
void uninit_video_out(struct MPContext *mpctx);
void uninit_video_chain(struct MPContext *mpctx);
double calc_average_frame_duration(struct MPContext *mpctx);
int init_video_decoder(struct MPContext *mpctx, struct track *track);

#endif /* MPLAYER_MP_CORE_H */
