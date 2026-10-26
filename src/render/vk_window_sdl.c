/*
 * Vulkan back end: the SDL2 window (POSIX hosts). Included by vk_backend.c,
 * not compiled on its own -- see "the window" there. The Windows sibling is
 * vk_window_win32.c; both provide the win_* hooks vk_backend.c declares,
 * pump_events, ssx3_vk_host_main and ssx3_vk_list_displays.
 */
#include <SDL_vulkan.h>
#include <dlfcn.h>
#include <strings.h>
#include <sys/resource.h>

#include "keyboard_sdl.h"     /* xboxrecomp/src/input */

static VkbWindow window_create(const char *title);
static void window_key(const SDL_Event *ev);
static void scale_key(const SDL_Event *ev);

/* ---- hooks for vk_backend.c ---------------------------------------------- */

static int win_instance_extensions(const char **exts, uint32_t *n)
{
    unsigned count = *n;

    if (!SDL_Vulkan_GetInstanceExtensions(V.window, &count, exts)) {
        fprintf(stderr, "  [VK] SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
        return 0;
    }
    *n = count;
    return 1;
}

static int win_create_surface(void)
{
    if (!SDL_Vulkan_CreateSurface(V.window, V.instance, &V.wsi)) {
        fprintf(stderr, "  [VK] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return 0;
    }
    return 1;
}

static void win_drawable_size(int *w, int *h)
{
    SDL_Vulkan_GetDrawableSize(V.window, w, h);
}

static int win_display_index(void)
{
    return SDL_GetWindowDisplayIndex(V.window);
}

static uint32_t win_ticks_ms(void)
{
    return SDL_GetTicks();
}

static void win_set_title(const char *title)
{
    SDL_SetWindowTitle(V.window, title);
}

static int win_open_lazy(void)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "  [VK] SDL video: %s\n", SDL_GetError());
        return 0;
    }
    V.window = window_create("SSX 3 (Recomp, Vulkan)");
    if (!V.window)
        return 0;
    xbox_InputSdlInit();
    return 1;
}

static void pump_events(void)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            fprintf(stderr, "  [VK] window closed\n");
            fflush(stderr);
            _Exit(0);
        }
        if (ev.type == SDL_WINDOWEVENT
                && (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED
                    || ev.window.event == SDL_WINDOWEVENT_RESIZED))
            V.swap_dirty = 1;
        scale_key(&ev);
        window_key(&ev);
        xbox_InputSdlEvent(&ev);
    }
}

/* SSX3_ASPECT=auto: the desktop's shape (needs SDL video). */
static void wide_auto(int display)
{
    SDL_DisplayMode m;

    if (!R.wide_auto)
        return;
    if (SDL_GetDesktopDisplayMode(display, &m) != 0)
        m.w = m.h = 0;
    wide_from_desktop(m.w, m.h);
}

/* ---- monitor choice ------------------------------------------------------
 *
 * SDL 2 has no notion of a primary monitor on Wayland: display 0 is whichever
 * output the compositor announced first (on a KDE desktop here: a side
 * monitor). The desktops (KDE, GNOME) do tell Xwayland which output is
 * primary, so its RandR monitor list is asked -- libX11/libXrandr loaded at run
 * time, no build dependency -- and the SDL display at the same position is
 * taken. Under SDL's X11 driver that is the same answer; without X (no
 * DISPLAY, no libraries) display 0 stays the fallback. */

/* XRRMonitorInfo (X11/extensions/Xrandr.h, RandR 1.5). */
typedef struct {
    unsigned long name;
    int primary, automatic, noutput;
    int x, y, width, height, mwidth, mheight;
    unsigned long *outputs;
} XrrMonitor;

/* Position of the X server's primary monitor; 0 if there is none to ask. */
static int x11_primary(int *x, int *y)
{
    void *x11 = NULL, *xrr = NULL, *dpy = NULL;
    void *(*open_display)(const char *);
    int (*close_display)(void *);
    unsigned long (*root_window)(void *);
    XrrMonitor *(*get_monitors)(void *, unsigned long, int, int *);
    void (*free_monitors)(XrrMonitor *);
    XrrMonitor *mon;
    int n = 0, i, found = 0;

    if (!getenv("DISPLAY")
            || !(x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL))
            || !(xrr = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL)))
        goto out;
    open_display = (void *(*)(const char *))dlsym(x11, "XOpenDisplay");
    close_display = (int (*)(void *))dlsym(x11, "XCloseDisplay");
    root_window = (unsigned long (*)(void *))dlsym(x11, "XDefaultRootWindow");
    get_monitors = (XrrMonitor *(*)(void *, unsigned long, int, int *))dlsym(xrr, "XRRGetMonitors");
    free_monitors = (void (*)(XrrMonitor *))dlsym(xrr, "XRRFreeMonitors");
    if (!open_display || !close_display || !root_window || !get_monitors || !free_monitors
            || !(dpy = open_display(NULL)))
        goto out;
    if ((mon = get_monitors(dpy, root_window(dpy), 1, &n))) {
        for (i = 0; i < n; i++)
            if (mon[i].primary) {
                *x = mon[i].x;
                *y = mon[i].y;
                found = 1;
                break;
            }
        free_monitors(mon);
    }
    close_display(dpy);
out:
    if (xrr) dlclose(xrr);
    if (x11) dlclose(x11);
    return found;
}

/* The SDL display of the desktop's primary monitor (0 if unknown). */
static int display_primary(void)
{
    int n = SDL_GetNumVideoDisplays(), i, x, y;
    SDL_Rect r;

    if (n > 1 && x11_primary(&x, &y))
        for (i = 0; i < n; i++)
            if (SDL_GetDisplayBounds(i, &r) == 0 && r.x == x && r.y == y)
                return i;
    return 0;
}

/* SSX3_MONITOR: primary (default), a display number or part of a name. */
static int display_pick(void)
{
    const char *e = getenv("SSX3_MONITOR");
    int n = SDL_GetNumVideoDisplays(), i;
    char *end;
    long v;

    if (n <= 1)
        return 0;
    if (e && *e && strcasecmp(e, "primary")) {
        v = strtol(e, &end, 10);
        if (!*end) {
            if (v >= 0 && v < n)
                return (int)v;
        } else {
            for (i = 0; i < n; i++) {
                const char *name = SDL_GetDisplayName(i);
                size_t k, len = strlen(e);
                for (k = 0; name && name[k]; k++)
                    if (!strncasecmp(name + k, e, len))
                        return i;
            }
        }
        fprintf(stderr, "  [VK] SSX3_MONITOR=%s: no such monitor, using the primary one\n", e);
    }
    return display_primary();
}

int ssx3_vk_displays(Ssx3Display *out, int max)
{
    int n, i, primary, inited = SDL_WasInit(SDL_INIT_VIDEO) != 0;

    if (!inited && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL video: %s\n", SDL_GetError());
        return 0;
    }
    n = SDL_GetNumVideoDisplays();
    if (n > max)
        n = max;
    primary = display_primary();
    for (i = 0; i < n; i++) {
        SDL_DisplayMode m;
        const char *name = SDL_GetDisplayName(i);
        if (SDL_GetDesktopDisplayMode(i, &m) != 0)
            memset(&m, 0, sizeof m);
        snprintf(out[i].name, sizeof out[i].name, "%s", name ? name : "?");
        out[i].w = m.w;
        out[i].h = m.h;
        out[i].hz = m.refresh_rate;
        out[i].primary = i == primary;
    }
    if (!inited)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return n < 0 ? 0 : n;
}

/* `ssx3_recomp --list-displays`: one line per monitor,
 * "<number>\t<name>\t<W>x<H>\t<Hz>\t<primary 0/1>". */
int ssx3_vk_list_displays(void)
{
    Ssx3Display d[16];
    int n = ssx3_vk_displays(d, 16), i;

    for (i = 0; i < n; i++)
        printf("%d\t%s\t%dx%d\t%d\t%d\n", i, d[i].name, d[i].w, d[i].h, d[i].hz, d[i].primary);
    return 0;
}

/* SSX3_WINDOW modes as SDL window flags. */
static Uint32 window_flags(int mode)
{
    return mode == WIN_FULLSCREEN ? SDL_WINDOW_FULLSCREEN
         : mode == WIN_BORDERLESS ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
}

/* The game window (needs SDL video), in the mode SSX3_WINDOW asks for.
 * Exclusive fullscreen uses the desktop's own mode, so the display does not
 * switch resolution: the internal resolution is SSX3_RES_SCALE either way and
 * the frame is scaled to the screen. The cursor is hidden in full screen. */
static SDL_Window *window_create(const char *title)
{
    SDL_DisplayMode dm;
    SDL_Window *w;
    int ww, wh, d;

    window_mode_init();
    d = display_pick();
    wide_init();
    wide_auto(d);
    window_size(&ww, &wh);
    if (s_win_mode == WIN_FULLSCREEN && SDL_GetDesktopDisplayMode(d, &dm) == 0) {
        ww = dm.w;
        wh = dm.h;
    }
    w = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED_DISPLAY(d),
                         SDL_WINDOWPOS_CENTERED_DISPLAY(d), ww, wh,
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
                         | window_flags(s_win_mode));
    if (!w) {
        fprintf(stderr, "  [VK] SDL_CreateWindow: %s\n", SDL_GetError());
        return NULL;
    }
    if (s_win_mode == WIN_FULLSCREEN && SDL_GetDesktopDisplayMode(d, &dm) == 0)
        SDL_SetWindowDisplayMode(w, &dm);
    SDL_ShowCursor(s_win_mode == WIN_WINDOWED ? SDL_ENABLE : SDL_DISABLE);
    fprintf(stderr, "  [VK] window: %s on display %d (%s)\n", window_mode_name(s_win_mode),
            d, SDL_GetDisplayName(d) ? SDL_GetDisplayName(d) : "?");
    return w;
}

/* Alt+Enter / F11: windowed <-> the full-screen mode SSX3_WINDOW chose
 * (borderless when it started windowed). On the thread that pumps the
 * window's events, which created it. The swapchain follows the resize event. */
static void window_key(const SDL_Event *ev)
{
    int to;

    if (ev->type != SDL_KEYDOWN || ev->key.repeat || !V.window)
        return;
    if (!(ev->key.keysym.scancode == SDL_SCANCODE_F11
          || (ev->key.keysym.scancode == SDL_SCANCODE_RETURN && (ev->key.keysym.mod & KMOD_ALT))))
        return;
    to = s_win_mode == WIN_WINDOWED ? s_win_full : WIN_WINDOWED;
    if (SDL_SetWindowFullscreen(V.window, window_flags(to)) != 0) {
        fprintf(stderr, "  [VK] %s: %s\n", window_mode_name(to), SDL_GetError());
        return;
    }
    s_win_mode = to;
    SDL_ShowCursor(to == WIN_WINDOWED ? SDL_ENABLE : SDL_DISABLE);
    V.swap_dirty = 1;
    fprintf(stderr, "  [VK] window: %s\n", window_mode_name(to));
}

static void scale_key(const SDL_Event *ev)
{
    if (ev->type == SDL_KEYDOWN && !ev->key.repeat
            && ev->key.keysym.scancode == SDL_SCANCODE_F10)
        scale_cycle();
}

/* ---- the window on the process main thread ------------------------------ */

extern void xbox_InputSetMainPump(int on);    /* src/input/ssx3_pad.c */

/* The highest refresh rate of the connected displays: the presenter's
 * default frame cap (more frames than a display shows only take GPU time
 * from the game). The fastest display, not the window's: under Wayland SDL
 * cannot tell which display a window is on. */
static void read_display_hz(void)
{
    SDL_DisplayMode m;
    int d, n = SDL_GetNumVideoDisplays(), hz = 0;

    for (d = 0; d < n; d++)
        if (SDL_GetCurrentDisplayMode(d, &m) == 0 && m.refresh_rate > hz)
            hz = m.refresh_rate;
    if (hz > 0 && hz != V.display_hz) {
        V.display_hz = hz;
        fprintf(stderr, "  [VK] fastest display refresh %d Hz\n", hz);
    }
}

static int (*s_guest_main)(void);
static int s_guest_rc;
static Uint32 s_guest_done_event = (Uint32)-1;

static void *guest_thread(void *unused)
{
    SDL_Event ev;

    (void)unused;
    s_guest_rc = s_guest_main();
    memset(&ev, 0, sizeof ev);
    ev.type = s_guest_done_event;
    SDL_PushEvent(&ev);
    return NULL;
}

/* The guest's thread, with xbox_HostWindowMain's stack rule: at least the
 * main thread's and generously more, since lifted code recurses on it. */
static int start_guest_thread(void)
{
    struct rlimit rl;
    size_t stack = 64u << 20;

    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;
    return vkb_thread_start(guest_thread, NULL, stack);
}

int ssx3_vk_host_main(int (*guest_main)(void))
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "  [VK] SDL init: %s; running without a window thread\n",
                SDL_GetError());
        return guest_main();
    }
    V.window = window_create("SSX 3 (Recomp, Vulkan) | loading");
    s_guest_done_event = SDL_RegisterEvents(1);
    if (!V.window || s_guest_done_event == (Uint32)-1) {
        fprintf(stderr, "  [VK] window: %s; running without a window thread\n",
                SDL_GetError());
        if (V.window) {
            SDL_DestroyWindow(V.window);
            V.window = NULL;
        }
        return guest_main();
    }
    xbox_InputSetMainPump(1);
    xbox_InputSdlInit();
    read_display_hz();
    V.host_main = 1;
    s_guest_main = guest_main;
    if (!start_guest_thread()) {
        fprintf(stderr, "  [VK] cannot start the guest thread\n");
        V.host_main = 0;
        xbox_InputSetMainPump(0);
        return guest_main();
    }
    fprintf(stderr, "  [VK] window open; guest runs on its own thread\n");

    /* Pump from the start, through the whole load, so the desktop never sees
     * an unresponsive window; refresh the FPS title while frames come. */
    for (;;) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 16)) {
            do {
                if (ev.type == s_guest_done_event)
                    return s_guest_rc;
                if (ev.type == SDL_QUIT) {
                    fprintf(stderr, "  [VK] window closed\n");
                    fflush(stderr);
                    _Exit(0);
                }
                if (ev.type == SDL_WINDOWEVENT
                        && (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED
                            || ev.window.event == SDL_WINDOWEVENT_RESIZED))
                    V.swap_dirty = 1;
                if (ev.type == SDL_DISPLAYEVENT)
                    read_display_hz();
                scale_key(&ev);
                window_key(&ev);
                xbox_InputSdlEvent(&ev);
            } while (SDL_PollEvent(&ev));
        }
        if (V.flips)
            update_title();
    }
}
