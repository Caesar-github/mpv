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
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>

#include "config.h"
#include "talloc.h"

#include "osdep/io.h"
#include "osdep/priority.h"
#include "osdep/terminal.h"
#include "osdep/timer.h"

#include "common/av_log.h"
#include "common/codecs.h"
#include "common/cpudetect.h"
#include "common/encode.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "options/m_property.h"
#include "common/common.h"
#include "common/msg.h"
#include "common/global.h"
#include "options/parse_configfile.h"
#include "options/parse_commandline.h"
#include "common/playlist.h"
#include "common/playlist_parser.h"
#include "options/options.h"
#include "input/input.h"

#include "audio/decode/dec_audio.h"
#include "audio/out/ao.h"
#include "audio/mixer.h"
#include "demux/demux.h"
#include "stream/stream.h"
#include "sub/ass_mp.h"
#include "sub/osd.h"
#include "video/decode/dec_video.h"
#include "video/out/vo.h"

#include "core.h"
#include "lua.h"
#include "command.h"
#include "screenshot.h"

#if HAVE_X11
#include "video/out/x11_common.h"
#endif

#if HAVE_COCOA
#include "osdep/macosx_application.h"
#endif

#ifdef PTW32_STATIC_LIB
#include <pthread.h>
#endif

#if defined(__MINGW32__) || defined(__CYGWIN__)
#include <windows.h>

#ifndef BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE
#define BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE (0x0001)
#endif

#ifndef BASE_SEARCH_PATH_PERMANENT
#define BASE_SEARCH_PATH_PERMANENT (0x8000)
#endif
#endif

const char mp_help_text[] =
"Usage:   mpv [options] [url|path/]filename\n"
"\n"
"Basic options:\n"
" --start=<time>    seek to given (percent, seconds, or hh:mm:ss) position\n"
" --no-audio        do not play sound\n"
" --no-video        do not play video\n"
" --fs              fullscreen playback\n"
" --sub=<file>      specify subtitle file to use\n"
" --playlist=<file> specify playlist file\n"
"\n"
" --list-options    list all mpv options\n"
"\n";

void mp_print_version(struct mp_log *log, int always)
{
    int v = always ? MSGL_INFO : MSGL_V;
    mp_msg(log, v,
           "%s (C) 2000-2013 mpv/MPlayer/mplayer2 projects\n built on %s\n",
           mpv_version, mpv_builddate);
    print_libav_versions(log, v);
    mp_msg(log, v, "\n");
}

static MP_NORETURN void exit_player(struct MPContext *mpctx,
                                    enum exit_reason how)
{
    int rc;
    uninit_player(mpctx, INITIALIZED_ALL);

#if HAVE_ENCODING
    encode_lavc_finish(mpctx->encode_lavc_ctx);
    encode_lavc_free(mpctx->encode_lavc_ctx);
#endif

    mpctx->encode_lavc_ctx = NULL;

#if HAVE_LUA
    mp_lua_uninit(mpctx);
#endif

#if defined(__MINGW32__)
    timeEndPeriod(1);
#endif

#if HAVE_COCOA
    cocoa_set_input_context(NULL);
#endif

    command_uninit(mpctx);

    mp_input_uninit(mpctx->input);

    osd_free(mpctx->osd);

#if HAVE_LIBASS
    ass_library_done(mpctx->ass_library);
    mpctx->ass_library = NULL;
#endif

    getch2_disable();
    uninit_libav(mpctx->global);

    if (how != EXIT_NONE) {
        const char *reason;
        switch (how) {
        case EXIT_SOMENOTPLAYED:
        case EXIT_PLAYED:
            reason = "End of file";
            break;
        case EXIT_NOTPLAYED:
            reason = "No files played";
            break;
        case EXIT_ERROR:
            reason = "Fatal error";
            break;
        default:
            reason = "Quit";
        }
        MP_INFO(mpctx, "\nExiting... (%s)\n", reason);
    }

    if (mpctx->has_quit_custom_rc) {
        rc = mpctx->quit_custom_rc;
    } else {
        switch (how) {
            case EXIT_ERROR:
                rc = 1; break;
            case EXIT_NOTPLAYED:
                rc = 2; break;
            case EXIT_SOMENOTPLAYED:
                rc = 3; break;
            default:
                rc = 0;
        }
    }

    // must be last since e.g. mp_msg uses option values
    // that will be freed by this.

    mp_msg_uninit(mpctx->global);
    talloc_free(mpctx);

#if HAVE_COCOA
    terminate_cocoa_application();
    // never reach here:
    // terminate calls exit itself, just silence compiler warning
    exit(0);
#else
    exit(rc);
#endif
}

static bool handle_help_options(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    struct mp_log *log = mpctx->log;
    int opt_exit = 0;
    if (opts->audio_decoders && strcmp(opts->audio_decoders, "help") == 0) {
        struct mp_decoder_list *list = audio_decoder_list();
        mp_print_decoders(log, MSGL_INFO, "Audio decoders:", list);
        talloc_free(list);
        opt_exit = 1;
    }
    if (opts->video_decoders && strcmp(opts->video_decoders, "help") == 0) {
        struct mp_decoder_list *list = video_decoder_list();
        mp_print_decoders(log, MSGL_INFO, "Video decoders:", list);
        talloc_free(list);
        opt_exit = 1;
    }
#if HAVE_X11
    if (opts->vo.fstype_list && strcmp(opts->vo.fstype_list[0], "help") == 0) {
        fstype_help(log);
        MP_INFO(mpctx, "\n");
        opt_exit = 1;
    }
#endif
    if ((opts->demuxer_name && strcmp(opts->demuxer_name, "help") == 0) ||
        (opts->audio_demuxer_name && strcmp(opts->audio_demuxer_name, "help") == 0) ||
        (opts->sub_demuxer_name && strcmp(opts->sub_demuxer_name, "help") == 0)) {
        demuxer_help(log);
        MP_INFO(mpctx, "\n");
        opt_exit = 1;
    }
#if HAVE_ENCODING
    if (encode_lavc_showhelp(log, &opts->encode_output))
        opt_exit = 1;
#endif
    return opt_exit;
}

#ifdef PTW32_STATIC_LIB
static void detach_ptw32(void)
{
    pthread_win32_thread_detach_np();
    pthread_win32_process_detach_np();
}
#endif

static void osdep_preinit(int *p_argc, char ***p_argv)
{
    char *enable_talloc = getenv("MPV_LEAK_REPORT");
    if (*p_argc > 1 && (strcmp((*p_argv)[1], "-leak-report") == 0 ||
                        strcmp((*p_argv)[1], "--leak-report") == 0))
        enable_talloc = "1";
    if (enable_talloc && strcmp(enable_talloc, "1") == 0)
        talloc_enable_leak_report();

#ifdef __MINGW32__
    mp_get_converted_argv(p_argc, p_argv);
#endif

#ifdef PTW32_STATIC_LIB
    pthread_win32_process_attach_np();
    pthread_win32_thread_attach_np();
    atexit(detach_ptw32);
#endif

#if defined(__MINGW32__) || defined(__CYGWIN__)
    // stop Windows from showing all kinds of annoying error dialogs
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    // Enable heap corruption detection
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    WINBOOL (WINAPI *pSetDllDirectory)(LPCWSTR lpPathName) =
        (WINBOOL (WINAPI *)(LPCWSTR))GetProcAddress(kernel32, "SetDllDirectoryW");
    WINBOOL (WINAPI *pSetSearchPathMode)(DWORD Flags) =
        (WINBOOL (WINAPI *)(DWORD))GetProcAddress(kernel32, "SetSearchPathMode");

    // Always use safe search paths for DLLs and other files, ie. never use the
    // current directory
    if (pSetSearchPathMode)
        pSetDllDirectory(L"");
    if (pSetSearchPathMode)
        pSetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE);
#endif

    terminal_init();

    mp_time_init();
}

static int cfg_include(void *ctx, char *filename, int flags)
{
    struct MPContext *mpctx = ctx;
    return m_config_parse_config_file(mpctx->mconfig, filename, flags);
}

static int mpv_main(int argc, char *argv[])
{
    osdep_preinit(&argc, &argv);

    if (argc >= 1) {
        argc--;
        argv++;
    }

    struct MPContext *mpctx = talloc(NULL, MPContext);
    *mpctx = (struct MPContext){
        .last_dvb_step = 1,
        .terminal_osd_text = talloc_strdup(mpctx, ""),
        .playlist = talloc_struct(mpctx, struct playlist, {0}),
    };

    mpctx->global = talloc_zero(mpctx, struct mpv_global);

    // Nothing must call mp_msg*() and related before this
    mp_msg_init(mpctx->global);
    mpctx->log = mp_log_new(mpctx, mpctx->global->log, "!cplayer");
    mpctx->statusline = mp_log_new(mpctx, mpctx->log, "!statusline");

    struct MPOpts *def_opts = talloc_ptrtype(mpctx, def_opts);
    *def_opts = mp_default_opts;
    def_opts->network_useragent = (char *)mpv_version;

    // Create the config context and register the options
    mpctx->mconfig = m_config_new(mpctx, mpctx->log, sizeof(struct MPOpts),
                                  def_opts, mp_opts);
    mpctx->opts = mpctx->mconfig->optstruct;
    mpctx->mconfig->includefunc = cfg_include;
    mpctx->mconfig->includefunc_ctx = mpctx;
    mpctx->mconfig->use_profiles = true;
    mpctx->mconfig->is_toplevel = true;

    struct MPOpts *opts = mpctx->opts;
    mpctx->global->opts = opts;

    char *verbose_env = getenv("MPV_VERBOSE");
    if (verbose_env)
        opts->verbose = atoi(verbose_env);
    mp_msg_update_msglevels(mpctx->global);

    init_libav(mpctx->global);
    GetCpuCaps(&gCpuCaps);
    screenshot_init(mpctx);
    mpctx->mixer = mixer_init(mpctx, mpctx->global);
    command_init(mpctx);

    // Preparse the command line
    m_config_preparse_command_line(mpctx->mconfig, mpctx->global, argc, argv);
    mp_msg_update_msglevels(mpctx->global);

    mp_print_version(mpctx->log, false);

    if (!mp_parse_cfgfiles(mpctx))
        exit_player(mpctx, EXIT_ERROR);

    int r = m_config_parse_mp_command_line(mpctx->mconfig, mpctx->playlist,
                                           mpctx->global, argc, argv);
    if (r < 0) {
        if (r <= M_OPT_EXIT) {
            exit_player(mpctx, EXIT_NONE);
        } else {
            exit_player(mpctx, EXIT_ERROR);
        }
    }

    mp_msg_update_msglevels(mpctx->global);

    if (handle_help_options(mpctx))
        exit_player(mpctx, EXIT_NONE);

    MP_VERBOSE(mpctx, "Configuration: " CONFIGURATION "\n");
    MP_VERBOSE(mpctx, "Command line:");
    for (int i = 0; i < argc; i++)
        MP_VERBOSE(mpctx, " '%s'", argv[i]);
    MP_VERBOSE(mpctx, "\n");

    if (!mpctx->playlist->first && !opts->player_idle_mode) {
        mp_print_version(mpctx->log, true);
        MP_INFO(mpctx, "%s", mp_help_text);
        exit_player(mpctx, EXIT_NONE);
    }

#if HAVE_PRIORITY
    set_priority();
#endif

    mpctx->input = mp_input_init(mpctx->global);
    stream_set_interrupt_callback(mp_input_check_interrupt, mpctx->input);
#if HAVE_COCOA
    cocoa_set_input_context(mpctx->input);
#endif

#if HAVE_ENCODING
    if (opts->encode_output.file && *opts->encode_output.file) {
        mpctx->encode_lavc_ctx = encode_lavc_init(&opts->encode_output,
                                                  mpctx->global);
        if(!mpctx->encode_lavc_ctx) {
            MP_INFO(mpctx, "Encoding initialization failed.");
            exit_player(mpctx, EXIT_ERROR);
        }
        m_config_set_option0(mpctx->mconfig, "vo", "lavc");
        m_config_set_option0(mpctx->mconfig, "ao", "lavc");
        m_config_set_option0(mpctx->mconfig, "fixed-vo", "yes");
        m_config_set_option0(mpctx->mconfig, "force-window", "no");
        m_config_set_option0(mpctx->mconfig, "gapless-audio", "yes");
        mp_input_enable_section(mpctx->input, "encode", MP_INPUT_EXCLUSIVE);
    }
#endif

    if (mpctx->opts->slave_mode)
        terminal_setup_stdin_cmd_input(mpctx->input);
    else if (mpctx->opts->consolecontrols)
        terminal_setup_getch(mpctx->input);

    if (opts->consolecontrols)
        getch2_enable();

#if HAVE_LIBASS
    mpctx->ass_log = mp_log_new(mpctx, mpctx->global->log, "!libass");
    mpctx->ass_library = mp_ass_init(mpctx->global, mpctx->ass_log);
#else
    MP_WARN(mpctx, "Compiled without libass.\n");
    MP_WARN(mpctx, "There will be no OSD and no text subtitles.\n");
#endif

    mpctx->osd = osd_create(mpctx->global);

    if (opts->force_vo) {
        opts->fixed_vo = 1;
        mpctx->video_out = init_best_video_out(mpctx->global, mpctx->input,
                                               mpctx->encode_lavc_ctx);
        if (!mpctx->video_out) {
            MP_FATAL(mpctx, "Error opening/initializing "
                    "the selected video_out (-vo) device.\n");
            exit_player(mpctx, EXIT_ERROR);
        }
        mpctx->mouse_cursor_visible = true;
        mpctx->initialized_flags |= INITIALIZED_VO;
    }

#if HAVE_LUA
    // Lua user scripts can call arbitrary functions. Load them at a point
    // where this is safe.
    mp_lua_init(mpctx);
#endif

    if (opts->shuffle)
        playlist_shuffle(mpctx->playlist);

    if (opts->merge_files)
        merge_playlist_files(mpctx->playlist);

    mpctx->playlist->current = mp_check_playlist_resume(mpctx, mpctx->playlist);
    if (!mpctx->playlist->current)
        mpctx->playlist->current = mpctx->playlist->first;

    mp_play_files(mpctx);

    exit_player(mpctx, mpctx->stop_play == PT_QUIT ? EXIT_QUIT : mpctx->quit_player_rc);

    return 1;
}

int main(int argc, char *argv[])
{
#if HAVE_COCOA
    return cocoa_main(mpv_main, argc, argv);
#else
    return mpv_main(argc, argv);
#endif
}
