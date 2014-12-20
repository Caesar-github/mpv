/*
 * Cocoa OpenGL Backend
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

#import <Cocoa/Cocoa.h>
#import <CoreServices/CoreServices.h> // for CGDisplayHideCursor
#import <IOKit/pwr_mgt/IOPMLib.h>
#include <dlfcn.h>

#import "cocoa_common.h"
#import "video/out/cocoa/window.h"
#import "video/out/cocoa/events_view.h"
#import "video/out/cocoa/video_view.h"
#import "video/out/cocoa/mpvadapter.h"

#include "osdep/threads.h"
#include "osdep/macosx_compat.h"
#include "osdep/macosx_events_objc.h"

#include "config.h"

#if HAVE_COCOA_APPLICATION
# include "osdep/macosx_application.h"
# include "osdep/macosx_application_objc.h"
#endif

#include "options/options.h"
#include "video/out/vo.h"
#include "win_state.h"

#include "input/input.h"
#include "talloc.h"

#include "common/msg.h"

#define CF_RELEASE(a) if ((a) != NULL) CFRelease(a)
#define cocoa_lock(s)    pthread_mutex_lock(&s->mutex)
#define cocoa_unlock(s)  pthread_mutex_unlock(&s->mutex)

static void vo_cocoa_fullscreen(struct vo *vo);
static void cocoa_change_profile(struct vo *vo, char **store, NSScreen *screen);
static void cocoa_rm_fs_screen_profile_observer(struct vo *vo);

struct vo_cocoa_state {
    NSWindow *window;
    NSView *view;
    MpvVideoView *video;
    NSOpenGLContext *gl_ctx;

    NSScreen *current_screen;
    NSScreen *fs_screen;

    NSInteger window_level;

    int pending_events;

    bool waiting_frame;
    bool skip_swap_buffer;
    bool embedded; // wether we are embedding in another GUI

    IOPMAssertionID power_mgmt_assertion;

    pthread_mutex_t mutex;
    struct mp_log *log;

    uint32_t old_dwidth;
    uint32_t old_dheight;

    char *icc_wnd_profile_path;
    char *icc_fs_profile_path;
    id   fs_icc_changed_ns_observer;

    void (*resize_redraw)(struct vo *vo, int w, int h);
};

static void with_cocoa_lock(struct vo *vo, void(^block)(void))
{
    struct vo_cocoa_state *s = vo->cocoa;
    cocoa_lock(s);
    block();
    cocoa_unlock(s);
}

static void with_cocoa_lock_on_main_thread(struct vo *vo, void(^block)(void))
{
    dispatch_async(dispatch_get_main_queue(), ^{
        with_cocoa_lock(vo, block);
    });
}

static void queue_new_video_size(struct vo *vo, int w, int h)
{
    struct vo_cocoa_state *s = vo->cocoa;
    if ([s->window conformsToProtocol: @protocol(MpvSizing)]) {
        id<MpvSizing> win = (id<MpvSizing>) s->window;
        [win queueNewVideoSize:NSMakeSize(w, h)];
    }
}

void *vo_cocoa_glgetaddr(const char *s)
{
    void *ret = NULL;
    void *handle = dlopen(
        "/System/Library/Frameworks/OpenGL.framework/OpenGL",
        RTLD_LAZY | RTLD_LOCAL);
    if (!handle)
        return NULL;
    ret = dlsym(handle, s);
    dlclose(handle);
    return ret;
}

static void enable_power_management(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;
    if (!s->power_mgmt_assertion) return;
    IOPMAssertionRelease(s->power_mgmt_assertion);
    s->power_mgmt_assertion = kIOPMNullAssertionID;
}

static void disable_power_management(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;
    if (s->power_mgmt_assertion) return;
    IOPMAssertionCreateWithName(
            kIOPMAssertionTypePreventUserIdleDisplaySleep,
            kIOPMAssertionLevelOn,
            CFSTR("io.mpv.video_playing_back"),
            &s->power_mgmt_assertion);
}

static const char macosx_icon[] =
#include "osdep/macosx_icon.inc"
;

static void set_application_icon(NSApplication *app)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSData *icon_data = [NSData dataWithBytesNoCopy:(void *)macosx_icon
                                             length:sizeof(macosx_icon)
                                       freeWhenDone:NO];
    NSImage *icon = [[NSImage alloc] initWithData:icon_data];
    [app setApplicationIconImage:icon];
    [icon release];
    [pool release];
}

int vo_cocoa_init(struct vo *vo)
{
    struct vo_cocoa_state *s = talloc_zero(vo, struct vo_cocoa_state);
    *s = (struct vo_cocoa_state){
        .waiting_frame = false,
        .power_mgmt_assertion = kIOPMNullAssertionID,
        .log = mp_log_new(s, vo->log, "cocoa"),
        .embedded = vo->opts->WinID >= 0,
    };
    mpthread_mutex_init_recursive(&s->mutex);
    vo->cocoa = s;
    return 1;
}

static int vo_cocoa_set_cursor_visibility(struct vo *vo, bool *visible)
{
    struct vo_cocoa_state *s = vo->cocoa;

    if (s->embedded)
        return VO_NOTIMPL;

    MpvEventsView *v = (MpvEventsView *) s->view;

    if (*visible) {
        CGDisplayShowCursor(kCGDirectMainDisplay);
    } else if ([v canHideCursor]) {
        CGDisplayHideCursor(kCGDirectMainDisplay);
    } else {
        *visible = true;
    }

    return VO_TRUE;
}

void vo_cocoa_register_resize_callback(struct vo *vo,
                                       void (*cb)(struct vo *vo, int w, int h))
{
    struct vo_cocoa_state *s = vo->cocoa;
    s->resize_redraw = cb;
}

void vo_cocoa_uninit(struct vo *vo)
{
    with_cocoa_lock(vo, ^{
        struct vo_cocoa_state *s = vo->cocoa;
        enable_power_management(vo);
        cocoa_rm_fs_screen_profile_observer(vo);

        [s->gl_ctx release];
        [s->view removeFromSuperview];
        [s->view release];
        if (s->window) { 
            [s->window release];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        }
    });
}

static int get_screen_handle(struct vo *vo, int identifier, NSWindow *window,
                             NSScreen **screen) {
    struct vo_cocoa_state *s = vo->cocoa;
    NSArray *screens  = [NSScreen screens];
    int n_of_displays = [screens count];

    if (identifier >= n_of_displays) { // check if the identifier is out of bounds
        MP_INFO(s, "Screen ID %d does not exist, falling back to main "
                    "device\n", identifier);
        identifier = -1;
    }

    if (identifier < 0) {
        // default behaviour gets either the window screen or the main screen
        // if window is not available
        if (! (*screen = [window screen]) )
            *screen = [screens objectAtIndex:0];
        return 0;
    } else {
        *screen = [screens objectAtIndex:(identifier)];
        return 1;
    }
}

static void vo_cocoa_update_screens_pointers(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;
    struct mp_vo_opts *opts = vo->opts;
    get_screen_handle(vo, opts->screen_id, s->window, &s->current_screen);
    get_screen_handle(vo, opts->fsscreen_id, s->window, &s->fs_screen);
}

static void vo_cocoa_update_screen_info(struct vo *vo, struct mp_rect *out_rc)
{
    struct vo_cocoa_state *s = vo->cocoa;

    if (s->embedded)
        return;

    vo_cocoa_update_screens_pointers(vo);

    if (out_rc) {
        NSRect r = [s->current_screen frame];
        *out_rc = (struct mp_rect){0, 0, r.size.width, r.size.height};
    }
}

static void resize_window(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;
    NSRect frame = [s->video frameInPixels];
    vo->dwidth  = frame.size.width;
    vo->dheight = frame.size.height;
    [s->gl_ctx update];
}

static void vo_set_level(struct vo *vo, int ontop)
{
    struct vo_cocoa_state *s = vo->cocoa;

    if (ontop) {
        // +1 is not enough as that will show the icon layer on top of the
        // menubar when the application is not frontmost. so use +2
        s->window_level = NSMainMenuWindowLevel + 2;
    } else {
        s->window_level = NSNormalWindowLevel;
    }

    [[s->view window] setLevel:s->window_level];
    [s->window        setLevel:s->window_level];
}

static int vo_cocoa_ontop(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;
    if (s->embedded)
        return VO_NOTIMPL;

    struct mp_vo_opts *opts = vo->opts;
    opts->ontop = !opts->ontop;
    vo_set_level(vo, opts->ontop);
    return VO_TRUE;
}

static MpvVideoWindow *create_window(NSRect rect, NSScreen *s, bool border,
                                     MpvCocoaAdapter *adapter)
{
    int window_mask = 0;
    if (border) {
        window_mask = NSTitledWindowMask|NSClosableWindowMask|
                      NSMiniaturizableWindowMask|NSResizableWindowMask;
    } else {
        window_mask = NSBorderlessWindowMask|NSResizableWindowMask;
    }

    MpvVideoWindow *w =
        [[MpvVideoWindow alloc] initWithContentRect:rect
                                          styleMask:window_mask
                                            backing:NSBackingStoreBuffered
                                              defer:NO
                                             screen:s];
    w.adapter = adapter;
    [w setDelegate: w];

    return w;
}

static void create_ui(struct vo *vo, struct mp_rect *win, int geo_flags)
{
    struct vo_cocoa_state *s = vo->cocoa;
    struct mp_vo_opts *opts  = vo->opts;

    MpvCocoaAdapter *adapter = [[MpvCocoaAdapter alloc] init];
    adapter.vout = vo;

    NSView *parent;
    if (s->embedded) {
        parent = (NSView *) (intptr_t) opts->WinID;
    } else {
        const NSRect wr =
            NSMakeRect(win->x0, win->y0, win->x1 - win->x0, win->y1 - win->y0);
        s->window = create_window(wr, s->current_screen, opts->border, adapter);
        parent = [s->window contentView];
    }

    MpvEventsView *view = [[MpvEventsView alloc] initWithFrame:[parent bounds]];
    view.adapter = adapter;
    s->view = view;
    [parent addSubview:s->view];

    // insert ourselves as the next key view so that clients can give key
    // focus to the mpv view by calling -[NSWindow selectNextKeyView:]
    [parent setNextKeyView:s->view];

#if HAVE_COCOA_APPLICATION
    cocoa_register_menu_item_action(MPM_H_SIZE,   @selector(halfSize));
    cocoa_register_menu_item_action(MPM_N_SIZE,   @selector(normalSize));
    cocoa_register_menu_item_action(MPM_D_SIZE,   @selector(doubleSize));
    cocoa_register_menu_item_action(MPM_MINIMIZE, @selector(performMiniaturize:));
    cocoa_register_menu_item_action(MPM_ZOOM,     @selector(performZoom:));
#endif

    s->video = [[MpvVideoView alloc] initWithFrame:[s->view bounds]];
    [s->video setWantsBestResolutionOpenGLSurface:YES];

    [s->view addSubview:s->video];
    [s->gl_ctx setView:s->video];
    [s->video release];

    s->video.adapter = adapter;
    [adapter release];

    if (!s->embedded) {
        [s->window setRestorable:NO];
        [s->window makeMainWindow];
        [s->window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

static int cocoa_set_window_title(struct vo *vo, const char *title)
{
    struct vo_cocoa_state *s = vo->cocoa;
    if (s->embedded)
        return VO_NOTIMPL;

    void *talloc_ctx   = talloc_new(NULL);
    struct bstr btitle = bstr_sanitize_utf8_latin1(talloc_ctx, bstr0(title));
    NSString *nstitle  = [NSString stringWithUTF8String:btitle.start];
    if (nstitle)
        [s->window setTitle: nstitle];
    talloc_free(talloc_ctx);
    return VO_TRUE;
}

static void cocoa_rm_fs_screen_profile_observer(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;
    [[NSNotificationCenter defaultCenter]
        removeObserver:s->fs_icc_changed_ns_observer];
}

static void cocoa_add_fs_screen_profile_observer(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;

    if (s->fs_icc_changed_ns_observer)
        cocoa_rm_fs_screen_profile_observer(vo);

    if (vo->opts->fsscreen_id < 0)
        return;

    void (^nblock)(NSNotification *n) = ^(NSNotification *n) {
        cocoa_change_profile(vo, &s->icc_fs_profile_path, s->fs_screen);
        s->pending_events |= VO_EVENT_ICC_PROFILE_PATH_CHANGED;
    };

    s->fs_icc_changed_ns_observer = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSScreenColorSpaceDidChangeNotification
                    object:s->fs_screen
                     queue:nil
                usingBlock:nblock];
}

void vo_cocoa_create_nsgl_ctx(struct vo *vo, void *ctx)
{
    struct vo_cocoa_state *s = vo->cocoa;
    s->gl_ctx = [[NSOpenGLContext alloc] initWithCGLContextObj:ctx];
    [s->gl_ctx makeCurrentContext];
}

void vo_cocoa_release_nsgl_ctx(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;
    [s->gl_ctx release];
    s->gl_ctx = nil;
}

int vo_cocoa_config_window(struct vo *vo, uint32_t flags, void *gl_ctx)
{
    struct vo_cocoa_state *s = vo->cocoa;
    with_cocoa_lock_on_main_thread(vo, ^{
        struct mp_rect screenrc;
        vo_cocoa_update_screen_info(vo, &screenrc);

        struct vo_win_geometry geo;
        vo_calc_window_geometry(vo, &screenrc, &geo);
        vo_apply_window_geometry(vo, &geo);

        uint32_t width = vo->dwidth;
        uint32_t height = vo->dheight;

        bool reset_size = s->old_dwidth != width || s->old_dheight != height;
        s->old_dwidth  = width;
        s->old_dheight = height;

        if (!(flags & VOFLAG_HIDDEN) && !s->view) {
            create_ui(vo, &geo.win, geo.flags);
        }

        if (!s->embedded && s->window) {
            if (reset_size)
                queue_new_video_size(vo, width, height);
            vo_cocoa_fullscreen(vo);
            cocoa_add_fs_screen_profile_observer(vo);
            cocoa_set_window_title(vo, vo_get_window_title(vo));
            vo_set_level(vo, vo->opts->ontop);
        }

        // trigger a resize -> don't set vo->dwidth and vo->dheight directly
        // since this block is executed asynchrolously to the video
        // reconfiguration code.
        s->pending_events |= VO_EVENT_RESIZE;
    });

    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    set_application_icon(NSApp);
    return 0;
}

void vo_cocoa_set_current_context(struct vo *vo, bool current)
{
    struct vo_cocoa_state *s = vo->cocoa;

    if (current) {
        cocoa_lock(s);
        if (s->gl_ctx) [s->gl_ctx makeCurrentContext];
    } else {
        [NSOpenGLContext clearCurrentContext];
        cocoa_unlock(s);
    }
}

static void vo_cocoa_resize_redraw(struct vo *vo, int width, int height)
{
    struct vo_cocoa_state *s = vo->cocoa;

    if (!s->gl_ctx)
        return;

    if (!s->resize_redraw)
        return;

    vo_cocoa_set_current_context(vo, true);

    [s->gl_ctx update];
    s->resize_redraw(vo, width, height);
    s->skip_swap_buffer = true;

    [s->gl_ctx flushBuffer];
    vo_cocoa_set_current_context(vo, false);
}

static void draw_changes_after_next_frame(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;
    if (!s->waiting_frame) {
        s->waiting_frame = true;
        NSDisableScreenUpdates();
    }
}

void vo_cocoa_swap_buffers(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;

    if (s->skip_swap_buffer && !s->waiting_frame) {
        s->skip_swap_buffer = false;
        return;
    } else {
        [s->gl_ctx flushBuffer];
    }

    if (s->waiting_frame) {
        s->waiting_frame = false;
        NSEnableScreenUpdates();
    }
}

int vo_cocoa_check_events(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;
    int events = s->pending_events;
    s->pending_events = 0;

    if (events & VO_EVENT_RESIZE) {
        resize_window(vo);
    }

    return events;
}

static int vo_cocoa_fullscreen_sync(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;

    if (s->embedded)
        return VO_NOTIMPL;

    with_cocoa_lock_on_main_thread(vo, ^{
        vo_cocoa_fullscreen(vo);
    });

    return VO_TRUE;
}

static void vo_cocoa_fullscreen(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;
    struct mp_vo_opts *opts  = vo->opts;

    if (s->embedded)
        return;

    vo_cocoa_update_screen_info(vo, NULL);

    draw_changes_after_next_frame(vo);
    [(MpvEventsView *)s->view setFullScreen:opts->fullscreen];

    if (s->icc_fs_profile_path != s->icc_wnd_profile_path)
        s->pending_events = VO_EVENT_ICC_PROFILE_PATH_CHANGED;

    s->pending_events |= VO_EVENT_RESIZE;
}

static char *cocoa_get_icc_profile_path(struct vo *vo, NSScreen *screen)
{
    assert(screen);

    struct vo_cocoa_state *s = vo->cocoa;
    char *result = NULL;
    CFDictionaryRef device_info = NULL;

    CGDirectDisplayID displayID = (CGDirectDisplayID)
        [[screen deviceDescription][@"NSScreenNumber"] unsignedLongValue];

    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(displayID);
    if (CFGetTypeID(uuid) == CFNullGetTypeID()) {
        MP_ERR(s, "cannot get display UUID.\n");
        goto get_icc_profile_path_err_out;
    }

    device_info =
        ColorSyncDeviceCopyDeviceInfo(kColorSyncDisplayDeviceClass, uuid);

    CFRelease(uuid);

    if (!device_info) {
        MP_ERR(s, "cannot get display info.\n");
        goto get_icc_profile_path_err_out;
    }

    CFDictionaryRef factory_info =
        CFDictionaryGetValue(device_info, kColorSyncFactoryProfiles);
    if (!factory_info) {
        MP_ERR(s, "cannot get display factory settings.\n");
        goto get_icc_profile_path_err_out;
    }

    CFStringRef default_profile_id =
        CFDictionaryGetValue(factory_info, kColorSyncDeviceDefaultProfileID);
    if (!default_profile_id) {
        MP_ERR(s, "cannot get display default profile ID.\n");
        goto get_icc_profile_path_err_out;
    }

    CFURLRef icc_url;
    CFDictionaryRef custom_profile_info =
        CFDictionaryGetValue(device_info, kColorSyncCustomProfiles);
    if (custom_profile_info) {
        icc_url = CFDictionaryGetValue(custom_profile_info, default_profile_id);
        // If icc_url is NULL, the ICC profile URL could not be retrieved
        // although a custom profile was specified. This points to a
        // configuration error, so we should not fall back to the factory
        // profile, but return an error instead.
        if (!icc_url) {
            MP_ERR(s, "cannot get display profile URL\n");
            goto get_icc_profile_path_err_out;
        }
    } else {
        // No custom profile specified; try factory profile for the device
        CFDictionaryRef factory_profile_info =
            CFDictionaryGetValue(factory_info, default_profile_id);
        if (!factory_profile_info) {
            MP_ERR(s, "cannot get display profile info\n");
            goto get_icc_profile_path_err_out;
        }

        icc_url = CFDictionaryGetValue(factory_profile_info,
                                       kColorSyncDeviceProfileURL);
        if (!icc_url) {
            MP_ERR(s, "cannot get display factory profile URL.\n");
            goto get_icc_profile_path_err_out;
        }
    }

   result = talloc_strdup(vo, (char *)[[(NSURL *)icc_url path] UTF8String]);
   if (!result)
       MP_ERR(s, "cannot get display profile path.\n");

get_icc_profile_path_err_out:
    CF_RELEASE(device_info);
    return result;
}

static void cocoa_change_profile(struct vo *vo, char **store, NSScreen *screen)
{
    if (*store)
        talloc_free(*store);
    *store = cocoa_get_icc_profile_path(vo, screen);
}

static void vo_cocoa_control_get_icc_profile_path(struct vo *vo, void *arg)
{
    struct vo_cocoa_state *s = vo->cocoa;
    char **p = arg;

    vo_cocoa_update_screen_info(vo, NULL);

    NSScreen *screen;
    char **path;

    if (vo->opts->fullscreen) {
        screen = s->fs_screen;
        path   = &s->icc_fs_profile_path;
    } else {
        screen = s->current_screen;
        path   = &s->icc_wnd_profile_path;
    }

    cocoa_change_profile(vo, path, screen);
    *p = *path;
}

int vo_cocoa_control(struct vo *vo, int *events, int request, void *arg)
{
    switch (request) {
    case VOCTRL_CHECK_EVENTS:
        *events |= vo_cocoa_check_events(vo);
        return VO_TRUE;
    case VOCTRL_FULLSCREEN:
        return vo_cocoa_fullscreen_sync(vo);
    case VOCTRL_ONTOP:
        return vo_cocoa_ontop(vo);
    case VOCTRL_GET_UNFS_WINDOW_SIZE: {
        int *s = arg;
        with_cocoa_lock(vo, ^{
            NSSize size = [vo->cocoa->view frame].size;
            s[0] = size.width;
            s[1] = size.height;
        });
        return VO_TRUE;
    }
    case VOCTRL_SET_UNFS_WINDOW_SIZE: {
        int *s = arg;
        int w, h;
        w = s[0];
        h = s[1];
        with_cocoa_lock_on_main_thread(vo, ^{
            queue_new_video_size(vo, w, h);
        });
        return VO_TRUE;
    }
    case VOCTRL_SET_CURSOR_VISIBILITY:
        return vo_cocoa_set_cursor_visibility(vo, arg);
    case VOCTRL_UPDATE_WINDOW_TITLE:
        return cocoa_set_window_title(vo, (const char *) arg);
    case VOCTRL_RESTORE_SCREENSAVER:
        enable_power_management(vo);
        return VO_TRUE;
    case VOCTRL_KILL_SCREENSAVER:
        disable_power_management(vo);
        return VO_TRUE;
    case VOCTRL_GET_ICC_PROFILE_PATH:
        vo_cocoa_control_get_icc_profile_path(vo, arg);
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

void *vo_cocoa_cgl_context(struct vo *vo)
{
    struct vo_cocoa_state *s = vo->cocoa;
    return [s->gl_ctx CGLContextObj];
}

void *vo_cocoa_cgl_pixel_format(struct vo *vo)
{
    return CGLGetPixelFormat(vo_cocoa_cgl_context(vo));
}

@implementation MpvCocoaAdapter
@synthesize vout = _video_output;

- (void)performAsyncResize:(NSSize)size {
    struct vo_cocoa_state *s = self.vout->cocoa;
    if (!s->waiting_frame)
        vo_cocoa_resize_redraw(self.vout, size.width, size.height);
}

- (BOOL)keyboardEnabled {
    return !!mp_input_vo_keyboard_enabled(self.vout->input_ctx);
}

- (BOOL)mouseEnabled {
    return !!mp_input_mouse_enabled(self.vout->input_ctx);
}

- (void)setNeedsResize {
    struct vo_cocoa_state *s = self.vout->cocoa;
    s->pending_events |= VO_EVENT_RESIZE;
    vo_wakeup(self.vout);
}

- (void)recalcMovableByWindowBackground:(NSPoint)p
{
    BOOL movable = NO;
    if (![self isInFullScreenMode]) {
        movable = !mp_input_test_dragging(self.vout->input_ctx, p.x, p.y);
    }

    [self.vout->cocoa->window setMovableByWindowBackground:movable];
}

- (void)signalMouseMovement:(NSPoint)point {
    mp_input_set_mouse_pos(self.vout->input_ctx, point.x, point.y);
    [self recalcMovableByWindowBackground:point];
}

- (void)putKeyEvent:(NSEvent*)event
{
    cocoa_put_key_event(event);
}

- (void)putKey:(int)mpkey withModifiers:(int)modifiers
{
    cocoa_put_key_with_modifiers(mpkey, modifiers);
}

- (void)putAxis:(int)mpkey delta:(float)delta;
{
    mp_input_put_axis(self.vout->input_ctx, mpkey, delta);
}

- (void)putCommand:(char*)cmd
{
    char *cmd_ = ta_strdup(NULL, cmd);
    mp_cmd_t *cmdt = mp_input_parse_cmd(self.vout->input_ctx, bstr0(cmd_), "");
    mp_input_queue_cmd(self.vout->input_ctx, cmdt);
    ta_free(cmd_);
}

- (BOOL)isInFullScreenMode {
    return self.vout->opts->fullscreen;
}

- (NSScreen *)fsScreen {
    struct vo_cocoa_state *s = self.vout->cocoa;
    return s->fs_screen;
}

- (void)handleFilesArray:(NSArray *)files
{
    [[EventsResponder sharedInstance] handleFilesArray:files];
}

- (void)didChangeWindowedScreenProfile:(NSScreen *)screen
{
    struct vo_cocoa_state *s = self.vout->cocoa;
    cocoa_change_profile(self.vout, &s->icc_wnd_profile_path, screen);
    s->pending_events |= VO_EVENT_ICC_PROFILE_PATH_CHANGED;
}
@end
