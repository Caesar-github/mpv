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

#ifndef MPLAYER_MP_CORE_H
#define MPLAYER_MP_CORE_H

#include <stdbool.h>

#include "libmpv/client.h"

#include "common/common.h"
#include "options/options.h"
#include "sub/osd.h"

// definitions used internally by the core player code

#define INITIALIZED_VO      1
#define INITIALIZED_AO      2
#define INITIALIZED_PLAYBACK 16
#define INITIALIZED_LIBASS  32
#define INITIALIZED_STREAM  64
#define INITIALIZED_DEMUXER 512
#define INITIALIZED_ACODEC  1024
#define INITIALIZED_VCODEC  2048
#define INITIALIZED_SUB     4096
#define INITIALIZED_SUB2    8192
#define INITIALIZED_ALL     0xFFFF


enum stop_play_reason {
    KEEP_PLAYING = 0,   // must be 0, numeric values of others do not matter
    AT_END_OF_FILE,     // file has ended, prepare to play next
                        // also returned on unrecoverable playback errors
    PT_NEXT_ENTRY,      // prepare to play next entry in playlist
    PT_CURRENT_ENTRY,   // prepare to play mpctx->playlist->current
    PT_STOP,            // stop playback, clear playlist
    PT_RESTART,         // restart previous file
    PT_RELOAD_DEMUXER,  // restart playback, but keep stream open
    PT_QUIT,            // stop playback, quit player
};

enum exit_reason {
  EXIT_NONE,
  EXIT_QUIT,
  EXIT_PLAYED,
  EXIT_ERROR,
  EXIT_NOTPLAYED,
  EXIT_SOMENOTPLAYED
};

struct timeline_part {
    double start;
    double source_start;
    struct demuxer *source;
};

struct chapter {
    double start;
    char *name;
};

enum mp_osd_seek_info {
    OSD_SEEK_INFO_BAR           = 1,
    OSD_SEEK_INFO_TEXT          = 2,
    OSD_SEEK_INFO_CHAPTER_TEXT  = 4,
    OSD_SEEK_INFO_EDITION       = 8,
};


enum {
    // other constants
    MAX_OSD_LEVEL = 3,
    MAX_TERM_OSD_LEVEL = 1,
    OSD_LEVEL_INVISIBLE = 4,
    OSD_BAR_SEEK = 256,
};

enum seek_type {
    MPSEEK_NONE = 0,
    MPSEEK_RELATIVE,
    MPSEEK_ABSOLUTE,
    MPSEEK_FACTOR,
};

struct track {
    enum stream_type type;

    // Currently used for decoding.
    bool selected;

    // The type specific ID, also called aid (audio), sid (subs), vid (video).
    // For UI purposes only; this ID doesn't have anything to do with any
    // IDs coming from demuxers or container files.
    int user_tid;

    // Same as stream->demuxer_id. -1 if not set.
    int demuxer_id;

    char *title;
    bool default_track;
    bool attached_picture;
    char *lang;

    // If this track is from an external file (e.g. subtitle file).
    bool is_external;
    bool no_default;            // pretend it's not external for auto-selection
    char *external_filename;
    bool auto_loaded;

    // If the track's stream changes with the timeline (ordered chapters).
    bool under_timeline;

    // Value can change if under_timeline==true.
    struct demuxer *demuxer;
    // Invariant: !stream || stream->demuxer == demuxer
    struct sh_stream *stream;

    // For external subtitles, which are read fully on init. Do not attempt
    // to read packets from them.
    bool preloaded;
};

enum {
    MAX_NUM_VO_PTS = 100,
};

#define NUM_PTRACKS 2

typedef struct MPContext {
    bool initialized;
    bool is_cplayer;
    bool autodetach;
    struct mpv_global *global;
    struct MPOpts *opts;
    struct mp_log *log;
    struct m_config *mconfig;
    struct input_ctx *input;
    struct mp_client_api *clients;
    struct mp_dispatch_queue *dispatch;

    struct mp_log *statusline;
    struct osd_state *osd;
    struct mp_osd_msg *osd_msg_stack;
    char *term_osd_text;
    char *term_osd_status;
    char *term_osd_subs;
    char *term_osd_contents;
    char *last_window_title;

    int add_osd_seek_info; // bitfield of enum mp_osd_seek_info
    double osd_visible; // for the osd bar only
    int osd_function;
    double osd_function_visible;
    double osd_last_update;
    struct osd_progbar_state osd_progbar;

    struct playlist *playlist;
    char *filename; // currently playing file
    struct mp_resolve_result *resolve_result;
    enum stop_play_reason stop_play;
    unsigned int initialized_flags;  // which subsystems have been initialized

    // Return code to use with PT_QUIT
    enum exit_reason quit_player_rc;
    int quit_custom_rc;
    bool has_quit_custom_rc;
    bool error_playing;
    char **resume_defaults;

    int64_t shown_vframes, shown_aframes;

    struct demuxer **sources;
    int num_sources;

    struct timeline_part *timeline;
    int num_timeline_parts;
    int timeline_part;
    struct chapter *chapters;
    int num_chapters;
    double video_offset;

    struct stream *stream;
    struct demuxer *demuxer;

    struct track **tracks;
    int num_tracks;

    char *track_layout_hash;

    // Selected tracks. NULL if no track selected.
    // There can be NUM_PTRACKS of the same STREAM_TYPE selected at once.
    // Currently, this is used for the secondary subtitle track only.
    struct track *current_track[NUM_PTRACKS][STREAM_TYPE_COUNT];

    struct dec_video *d_video;
    struct dec_audio *d_audio;
    struct dec_sub *d_sub[2];

    // Uses: accessing metadata (consider ordered chapters case, where the main
    // demuxer defines metadata), or special purpose demuxers like TV.
    struct demuxer *master_demuxer;

    struct mixer *mixer;
    struct ao *ao;
    double ao_pts;
    struct mp_audio *ao_decoder_fmt; // for weak gapless audio check
    struct mp_audio_buffer *ao_buffer;  // queued audio; passed to ao_play() later

    struct vo *video_out;

    /* We're starting playback from scratch or after a seek. Show first
     * video frame immediately and reinitialize sync. */
    bool restart_playback;
    /* Set if audio should be timed to start with video frame after seeking,
     * not set when e.g. playing cover art */
    bool sync_audio_to_video;
    /* After playback restart (above) or audio stream change, adjust audio
     * stream by cutting samples or adding silence at the beginning to make
     * audio playback position match video position. */
    bool syncing_audio;
    bool hrseek_active;
    bool hrseek_framedrop;
    double hrseek_pts;
    // AV sync: the next frame should be shown when the audio out has this
    // much (in seconds) buffered data left. Increased when more data is
    // written to the ao, decreased when moving to the next frame.
    // In the audio-only case used as a timer since the last seek
    // by the audio CPU usage meter.
    double delay;
    // AV sync: time until next frame should be shown
    double time_frame;
    // How long the last vo flip() call took. Used to adjust timing with
    // the goal of making flip() calls finish (rather than start) at the
    // specified time.
    double last_vo_flip_duration;
    // Display duration (as "intended") of the last flipped frame.
    double last_frame_duration;
    // Set to true some time after a new frame has been shown, and it turns out
    // that this frame was the last one before video ends.
    bool playing_last_frame;
    // How much video timing has been changed to make it match the audio
    // timeline. Used for status line information only.
    double total_avsync_change;
    // Total number of dropped frames that were "approved" to be dropped.
    // Actual dropping depends on --framedrop and decoder internals.
    int drop_frame_cnt;
    // Number of frames dropped in a row.
    int dropped_frames;
    // A-V sync difference when last frame was displayed. Kept to display
    // the same value if the status line is updated at a time where no new
    // video frame is shown.
    double last_av_difference;
    /* Timestamp of the latest image that was queued on the VO, but not yet
     * to be flipped. */
    double video_next_pts;
    /* timestamp of video frame currently visible on screen
     * (or at least queued to be flipped by VO) */
    double video_pts;
    double last_seek_pts;
    // As video_pts, but is not reset when seeking away. (For the very short
    // period of time until a new frame is decoded and shown.)
    double last_vo_pts;
    // Video PTS, or audio PTS if video has ended.
    double playback_pts;

    int last_chapter;

    // History of video frames timestamps that were queued in the VO
    // This includes even skipped frames during hr-seek
    double vo_pts_history_pts[MAX_NUM_VO_PTS];
    // Whether the PTS at vo_pts_history[n] is after a seek reset
    uint64_t vo_pts_history_seek[MAX_NUM_VO_PTS];
    uint64_t vo_pts_history_seek_ts;
    uint64_t backstep_start_seek_ts;
    bool backstep_active;

    double audio_delay;

    double last_heartbeat;
    double last_metadata_update;
    double last_idle_tick;

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
        double amount;
        int exact;  // -1 = disable, 0 = default, 1 = enable
        bool immediate; // disable seek delay logic
        // currently not set by commands, only used internally by seek()
        int direction; // -1 = backward, 0 = default, 1 = forward
    } seek;

    /* Heuristic for relative chapter seeks: keep track which chapter
     * the user wanted to go to, even if we aren't exactly within the
     * boundaries of that chapter due to an inaccurate seek. */
    int last_chapter_seek;
    double last_chapter_pts;

    /* Subtitle renderer. This is separate, because we want to keep fonts
     * loaded across ordered chapters, instead of reloading and rescanning
     * them on each transition. (Both of these objects contain this state.)
     */
    struct ass_renderer *ass_renderer;
    struct ass_library *ass_library;
    struct mp_log *ass_log;

    int last_dvb_step;

    bool paused;
    bool eof_reached;
    // step this many frames, then pause
    int step_frames;
    // Counted down each frame, stop playback if 0 is reached. (-1 = disable)
    int max_frames;
    bool playing_msg_shown;

    bool paused_for_cache;

    // Set after showing warning about decoding being too slow for realtime
    // playback rate. Used to avoid showing it multiple times.
    bool drop_message_shown;

    struct screenshot_ctx *screenshot_ctx;
    struct command_ctx *command_ctx;
    struct encode_lavc_context *encode_lavc_ctx;
    struct mp_nav_state *nav_state;
} MPContext;

// audio.c
void reinit_audio_chain(struct MPContext *mpctx);
int reinit_audio_filters(struct MPContext *mpctx);
double playing_audio_pts(struct MPContext *mpctx);
int fill_audio_out_buffers(struct MPContext *mpctx, double endpts);
double written_audio_pts(struct MPContext *mpctx);
void clear_audio_output_buffers(struct MPContext *mpctx);
void clear_audio_decode_buffers(struct MPContext *mpctx);

// configfiles.c
bool mp_parse_cfgfiles(struct MPContext *mpctx);
void mp_load_auto_profiles(struct MPContext *mpctx);
void mp_get_resume_defaults(struct MPContext *mpctx);
void mp_load_playback_resume(struct MPContext *mpctx, const char *file);
void mp_write_watch_later_conf(struct MPContext *mpctx);
struct playlist_entry *mp_check_playlist_resume(struct MPContext *mpctx,
                                                struct playlist *playlist);

// discnav.c
void mp_nav_init(struct MPContext *mpctx);
void mp_nav_reset(struct MPContext *mpctx);
void mp_nav_destroy(struct MPContext *mpctx);
void mp_nav_user_input(struct MPContext *mpctx, char *command);
void mp_handle_nav(struct MPContext *mpctx);
int mp_nav_in_menu(struct MPContext *mpctx);

// loadfile.c
void uninit_player(struct MPContext *mpctx, unsigned int mask);
struct track *mp_add_subtitles(struct MPContext *mpctx, char *filename);
void mp_switch_track(struct MPContext *mpctx, enum stream_type type,
                     struct track *track);
void mp_switch_track_n(struct MPContext *mpctx, int order,
                       enum stream_type type, struct track *track);
void mp_deselect_track(struct MPContext *mpctx, struct track *track);
void mp_mark_user_track_selection(struct MPContext *mpctx, int order,
                                  enum stream_type type);
struct track *mp_track_by_tid(struct MPContext *mpctx, enum stream_type type,
                              int tid);
bool timeline_set_part(struct MPContext *mpctx, int i, bool force);
double timeline_set_from_time(struct MPContext *mpctx, double pts, bool *need_reset);
struct sh_stream *init_demux_stream(struct MPContext *mpctx, struct track *track);
void reselect_demux_streams(struct MPContext *mpctx);
void add_demuxer_tracks(struct MPContext *mpctx, struct demuxer *demuxer);
bool mp_remove_track(struct MPContext *mpctx, struct track *track);
struct playlist_entry *mp_next_file(struct MPContext *mpctx, int direction,
                                    bool force);
void mp_set_playlist_entry(struct MPContext *mpctx, struct playlist_entry *e);
void mp_play_files(struct MPContext *mpctx);

// main.c
int mpv_main(int argc, char *argv[]);
int mp_initialize(struct MPContext *mpctx);
struct MPContext *mp_create(void);
void mp_destroy(struct MPContext *mpctx);
void mp_print_version(struct mp_log *log, int always);

// misc.c
double get_start_time(struct MPContext *mpctx);
double get_main_demux_pts(struct MPContext *mpctx);
double rel_time_to_abs(struct MPContext *mpctx, struct m_rel_time t);
double get_play_end_pts(struct MPContext *mpctx);
double get_relative_time(struct MPContext *mpctx);
void merge_playlist_files(struct playlist *pl);
int mp_get_cache_percent(struct MPContext *mpctx);
bool mp_get_cache_idle(struct MPContext *mpctx);
void update_window_title(struct MPContext *mpctx, bool force);
void stream_dump(struct MPContext *mpctx);

// osd.c
void print_status(struct MPContext *mpctx);
void set_osd_bar(struct MPContext *mpctx, int type, const char* name,
                 double min, double max, double neutral, double val);
void set_osd_msg(struct MPContext *mpctx, int level, int time,
                 const char* fmt, ...) PRINTF_ATTRIBUTE(4,5);
void set_osd_function(struct MPContext *mpctx, int osd_function);
void set_osd_subtitle(struct MPContext *mpctx, const char *text);

// playloop.c
void pause_player(struct MPContext *mpctx);
void unpause_player(struct MPContext *mpctx);
void add_step_frame(struct MPContext *mpctx, int dir);
void queue_seek(struct MPContext *mpctx, enum seek_type type, double amount,
                int exact, bool immediate);
bool mp_seek_chapter(struct MPContext *mpctx, int chapter);
double get_time_length(struct MPContext *mpctx);
double get_current_time(struct MPContext *mpctx);
int get_percent_pos(struct MPContext *mpctx);
double get_current_pos_ratio(struct MPContext *mpctx, bool use_range);
int get_current_chapter(struct MPContext *mpctx);
char *chapter_display_name(struct MPContext *mpctx, int chapter);
char *chapter_name(struct MPContext *mpctx, int chapter);
double chapter_start_time(struct MPContext *mpctx, int chapter);
int get_chapter_count(struct MPContext *mpctx);
void execute_queued_seek(struct MPContext *mpctx);
void run_playloop(struct MPContext *mpctx);
void idle_loop(struct MPContext *mpctx);
void handle_force_window(struct MPContext *mpctx, bool reconfig);
void add_frame_pts(struct MPContext *mpctx, double pts);

// scripting.c
struct mp_scripting {
    const char *file_ext;   // e.g. "lua"
    int (*load)(struct mpv_handle *client, const char *filename);
};
void mp_load_scripts(struct MPContext *mpctx);

// sub.c
void reset_subtitles(struct MPContext *mpctx, int order);
void uninit_subs(struct demuxer *demuxer);
void reinit_subs(struct MPContext *mpctx, int order);
void update_osd_msg(struct MPContext *mpctx);
void update_subtitles(struct MPContext *mpctx);

// timeline/tl_matroska.c
void build_ordered_chapter_timeline(struct MPContext *mpctx);
// timeline/tl_mpv_edl.c
void build_mpv_edl_timeline(struct MPContext *mpctx);
// timeline/tl_cue.c
void build_cue_timeline(struct MPContext *mpctx);

// video.c
int reinit_video_chain(struct MPContext *mpctx);
int reinit_video_filters(struct MPContext *mpctx);
int update_video(struct MPContext *mpctx, double endpts, bool reconfig_ok,
                 double *frame_duration);
void mp_force_video_refresh(struct MPContext *mpctx);
void update_fps(struct MPContext *mpctx);

#endif /* MPLAYER_MP_CORE_H */
