/*
 * Vulkan back end: the native Win32 window (Windows 10 and 11). Included by
 * vk_backend.c, not compiled on its own -- see "the window" there. The POSIX
 * sibling is vk_window_sdl.c; this one provides the same hooks without SDL,
 * so the Windows build needs no extra DLL: the surface is VK_KHR_win32_surface
 * and vulkan-1.dll is loaded at run time by volk.
 *
 * Threads: with ssx3_vk_host_main the window lives on the process main
 * thread, which only pumps messages; the guest runs on its own thread and the
 * presenter (frame interpolation) presents from a third. Without it the NV2A
 * thread opens the window and pumps it at every flip (win_open_lazy).
 *
 * Keys go into the runtime's key table (xbox_InputKeySet, Windows virtual-key
 * codes, as the runtime's GDI window does); F10 cycles the internal
 * resolution, F11 / Alt+Enter toggle full screen. Monitors are numbered in
 * EnumDisplayMonitors order and named by their EDID name where Windows gives
 * one (QueryDisplayConfig), else by GDI device name (\\.\DISPLAY1).
 *
 * SSX3_WINDOW=fullscreen is a topmost borderless window over the whole
 * monitor at the desktop's resolution: the frame is scaled either way, and
 * Windows 10/11 give such a window the display directly (independent flip),
 * so there is no separate exclusive mode to switch into.
 */
#include "keyboard.h"         /* xboxrecomp/src/input: xbox_InputKeySet */

#define VKB_WIN_CLASS     L"SSX3RecompVulkan"
#define WM_VKB_GUEST_DONE (WM_APP + 1)

static VkbWindow window_create(const char *title);
static void read_display_hz(void);

static double        s_t0;                 /* win_ticks_ms epoch */
static volatile LONG s_guest_done;
static int           s_guest_rc;
static int         (*s_guest_main)(void);
static RECT          s_win_rect;           /* windowed placement, for F11 back */
static DWORD         s_win_style;

/* ---- monitors -------------------------------------------------------------- */

#define VKB_MAX_MONITORS 16

typedef struct {
    HMONITOR handle;
    RECT     rect;                         /* desktop coordinates */
    RECT     work;                         /* without the task bar */
    int      primary;
    int      hz;                           /* current refresh, 0 unknown */
    WCHAR    device[CCHDEVICENAME];        /* \\.\DISPLAYn */
    char     name[128];                    /* friendly name, UTF-8 */
} WinMonitor;

static WinMonitor s_mon[VKB_MAX_MONITORS];
static int        s_nmon;

static BOOL CALLBACK monitor_add(HMONITOR h, HDC dc, LPRECT r, LPARAM unused)
{
    MONITORINFOEXW mi;
    DEVMODEW dm;
    WinMonitor *m;

    (void)dc; (void)r; (void)unused;
    if (s_nmon >= VKB_MAX_MONITORS)
        return FALSE;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(h, (MONITORINFO *)&mi))
        return TRUE;
    m = &s_mon[s_nmon++];
    memset(m, 0, sizeof *m);
    m->handle = h;
    m->rect = mi.rcMonitor;
    m->work = mi.rcWork;
    m->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    memcpy(m->device, mi.szDevice, sizeof m->device);
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    /* 0 and 1 mean "the hardware's default": unknown. */
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
            && dm.dmDisplayFrequency > 1)
        m->hz = (int)dm.dmDisplayFrequency;
    WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, m->name, sizeof m->name, NULL, NULL);
    return TRUE;
}

/* The monitors' EDID names, matched to the GDI devices through the active
 * display paths. Leaves the device name where Windows (or Wine) has none. */
static void monitor_names(void)
{
    UINT32 np = 0, nm = 0, i;
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;

    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np)
        return;
    paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof *paths);
    modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof *modes);
    if (paths && modes
            && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL)
               == ERROR_SUCCESS) {
        for (i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt;
            int k;

            memset(&src, 0, sizeof src);
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof src;
            src.header.adapterId = paths[i].sourceInfo.adapterId;
            src.header.id = paths[i].sourceInfo.id;
            memset(&tgt, 0, sizeof tgt);
            tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tgt.header.size = sizeof tgt;
            tgt.header.adapterId = paths[i].targetInfo.adapterId;
            tgt.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS
                    || DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS
                    || !tgt.monitorFriendlyDeviceName[0])
                continue;
            for (k = 0; k < s_nmon; k++)
                if (!wcscmp(s_mon[k].device, src.viewGdiDeviceName))
                    WideCharToMultiByte(CP_UTF8, 0, tgt.monitorFriendlyDeviceName, -1,
                                        s_mon[k].name, sizeof s_mon[k].name, NULL, NULL);
        }
    }
    free(paths);
    free(modes);
}

static void monitors_read(void)
{
    s_nmon = 0;
    EnumDisplayMonitors(NULL, NULL, monitor_add, 0);
    monitor_names();
}

static int display_primary(void)
{
    int i;

    for (i = 0; i < s_nmon; i++)
        if (s_mon[i].primary)
            return i;
    return 0;
}

/* SSX3_MONITOR: primary (default), a display number or part of a name. */
static int display_pick(void)
{
    const char *e = getenv("SSX3_MONITOR");
    char *end;
    long v;
    int i;

    if (s_nmon <= 1)
        return 0;
    if (e && *e && _stricmp(e, "primary")) {
        v = strtol(e, &end, 10);
        if (!*end) {
            if (v >= 0 && v < s_nmon)
                return (int)v;
        } else {
            size_t len = strlen(e);
            for (i = 0; i < s_nmon; i++) {
                const char *name = s_mon[i].name;
                size_t k;
                for (k = 0; name[k]; k++)
                    if (!_strnicmp(name + k, e, len))
                        return i;
            }
        }
        fprintf(stderr, "  [VK] SSX3_MONITOR=%s: no such monitor, using the primary one\n", e);
    }
    return display_primary();
}

/* Real pixels on high-DPI desktops: without this Windows would scale the
 * window up from a lower resolution (blurry) and report scaled monitor sizes.
 * Per-monitor v2 is Windows 10 1703+; older systems get system-wide awareness. */
static void dpi_aware(void)
{
    typedef BOOL (WINAPI *SetCtx)(HANDLE);
    HMODULE user = GetModuleHandleW(L"user32.dll");
    SetCtx set = user ? (SetCtx)(void *)GetProcAddress(user, "SetProcessDpiAwarenessContext") : NULL;

    /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4 */
    if (!set || !set((HANDLE)(LONG_PTR)-4))
        SetProcessDPIAware();
}

int ssx3_vk_displays(Ssx3Display *out, int max)
{
    int i, primary, n;

    dpi_aware();
    monitors_read();
    primary = display_primary();
    n = s_nmon < max ? s_nmon : max;
    for (i = 0; i < n; i++) {
        snprintf(out[i].name, sizeof out[i].name, "%s", s_mon[i].name);
        out[i].w = (int)(s_mon[i].rect.right - s_mon[i].rect.left);
        out[i].h = (int)(s_mon[i].rect.bottom - s_mon[i].rect.top);
        out[i].hz = s_mon[i].hz;
        out[i].primary = i == primary;
    }
    return n;
}

/* `ssx3_recomp --list-displays`: one line per monitor,
 * "<number>\t<name>\t<W>x<H>\t<Hz>\t<primary 0/1>". */
int ssx3_vk_list_displays(void)
{
    Ssx3Display d[VKB_MAX_MONITORS];
    int n = ssx3_vk_displays(d, VKB_MAX_MONITORS), i;

    for (i = 0; i < n; i++)
        printf("%d\t%s\t%dx%d\t%d\t%d\n", i, d[i].name, d[i].w, d[i].h, d[i].hz, d[i].primary);
    fflush(stdout);
    return 0;
}

/* SSX3_ASPECT=auto: the shape of that monitor. */
static void wide_auto(int display)
{
    if (!R.wide_auto)
        return;
    if (display < 0 || display >= s_nmon) {
        wide_from_desktop(0, 0);
        return;
    }
    wide_from_desktop(s_mon[display].rect.right - s_mon[display].rect.left,
                      s_mon[display].rect.bottom - s_mon[display].rect.top);
}

/* The highest refresh rate of the connected displays: the presenter's
 * default frame cap (more frames than a display shows only take GPU time
 * from the game) -- the same rule as on POSIX. */
static void read_display_hz(void)
{
    int i, hz = 0;

    monitors_read();
    for (i = 0; i < s_nmon; i++)
        if (s_mon[i].hz > hz)
            hz = s_mon[i].hz;
    if (hz > 0 && hz != V.display_hz) {
        V.display_hz = hz;
        fprintf(stderr, "  [VK] fastest display refresh %d Hz\n", hz);
    }
}

/* ---- window modes ---------------------------------------------------------- */

#define VKB_STYLE_WINDOWED (WS_OVERLAPPEDWINDOW | WS_VISIBLE)
#define VKB_STYLE_FULL     (WS_POPUP | WS_VISIBLE)

static const WinMonitor *window_monitor(HWND w)
{
    HMONITOR h = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    int i;

    for (i = 0; i < s_nmon; i++)
        if (s_mon[i].handle == h)
            return &s_mon[i];
    return s_nmon ? &s_mon[0] : NULL;
}

/* Cover the monitor the window is on (borderless / "fullscreen"). */
static void window_go_full(HWND w, int mode)
{
    const WinMonitor *m;

    monitors_read();
    m = window_monitor(w);
    if (!m)
        return;
    SetWindowLongPtrW(w, GWL_STYLE, VKB_STYLE_FULL);
    SetWindowPos(w, mode == WIN_FULLSCREEN ? HWND_TOPMOST : HWND_TOP,
                 m->rect.left, m->rect.top, m->rect.right - m->rect.left,
                 m->rect.bottom - m->rect.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static void window_go_windowed(HWND w)
{
    SetWindowLongPtrW(w, GWL_STYLE, s_win_style);
    SetWindowPos(w, HWND_NOTOPMOST, s_win_rect.left, s_win_rect.top,
                 s_win_rect.right - s_win_rect.left, s_win_rect.bottom - s_win_rect.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* Alt+Enter / F11: windowed <-> the full-screen mode SSX3_WINDOW chose
 * (borderless when it started windowed). */
static void window_toggle(HWND w)
{
    int to = s_win_mode == WIN_WINDOWED ? s_win_full : WIN_WINDOWED;

    if (to == WIN_WINDOWED) {
        window_go_windowed(w);
    } else {
        GetWindowRect(w, &s_win_rect);
        window_go_full(w, to);
    }
    s_win_mode = to;
    V.swap_dirty = 1;
    fprintf(stderr, "  [VK] window: %s\n", window_mode_name(to));
}

/* ---- messages -------------------------------------------------------------- */

static LRESULT CALLBACK window_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:
        fprintf(stderr, "  [VK] window closed\n");
        fflush(stderr);
        _Exit(0);

    case WM_SIZE:
        V.swap_dirty = 1;
        return 0;

    case WM_DISPLAYCHANGE:
        read_display_hz();
        return 0;

    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;

    case WM_SETCURSOR:
        /* No cursor over the picture in full screen. */
        if (s_win_mode != WIN_WINDOWED && LOWORD(l) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int first = !(l & 0x40000000);     /* not an auto-repeat */
        if (first && (w == VK_F11 || (w == VK_RETURN && (l & 0x20000000)))) {
            window_toggle(h);
            return 0;
        }
        if (first && w == VK_F10) {
            scale_cycle();
            return 0;                      /* F10 alone would open the window menu */
        }
        xbox_InputKeySet((int)w, 1);
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcW(h, m, w, l) : 0;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        return m == WM_SYSKEYUP ? DefWindowProcW(h, m, w, l) : 0;

    case WM_SYSCHAR:
        return 0;                          /* no "ding" for Alt+Enter */

    case WM_SYSCOMMAND:
        /* Alt alone would enter the menu loop, which stops this thread's
         * message pump until the next key. */
        if ((w & 0xFFF0) == SC_KEYMENU)
            return 0;
        break;

    case WM_VKB_GUEST_DONE:
        InterlockedExchange(&s_guest_done, 1);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void pump_events(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* The game window, in the mode SSX3_WINDOW asks for, on the monitor
 * SSX3_MONITOR names. Windowed: client area window_size(), centred on the
 * monitor and shrunk to fit it. */
static VkbWindow window_create(const char *title)
{
    WNDCLASSEXW wc;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WCHAR wtitle[256];
    const WinMonitor *mon;
    RECT r;
    HWND w;
    int d, ww, wh, x, y;

    dpi_aware();
    monitors_read();
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = VKB_WIN_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fprintf(stderr, "  [VK] RegisterClassEx failed (error %lu)\n", GetLastError());
        return NULL;
    }

    window_mode_init();
    d = display_pick();
    wide_init();
    wide_auto(d);
    window_size(&ww, &wh);
    mon = s_nmon ? &s_mon[d] : NULL;

    s_win_style = VKB_STYLE_WINDOWED;
    r.left = r.top = 0;
    r.right = ww;
    r.bottom = wh;
    AdjustWindowRectEx(&r, s_win_style, FALSE, 0);
    ww = r.right - r.left;
    wh = r.bottom - r.top;
    if (mon) {
        /* Shrunk only to the whole monitor: Wine has reported work areas a
         * third of the screen high (panels of other monitors), which would
         * make a tiny window. Centred in the work area when it fits there. */
        const RECT *in = &mon->work;
        int mw = mon->rect.right - mon->rect.left, mh = mon->rect.bottom - mon->rect.top;
        if (ww > mw || wh > mh) {
            /* Too big for the screen: the largest window of the same shape. */
            double k = (double)mw / ww < (double)mh / wh ? (double)mw / ww : (double)mh / wh;
            ww = (int)(ww * k);
            wh = (int)(wh * k);
        }
        if (ww > in->right - in->left || wh > in->bottom - in->top)
            in = &mon->rect;
        x = in->left + (in->right - in->left - ww) / 2;
        y = in->top + (in->bottom - in->top - wh) / 2;
    } else {
        x = y = CW_USEDEFAULT;
    }
    s_win_rect.left = x;
    s_win_rect.top = y;
    s_win_rect.right = x + ww;
    s_win_rect.bottom = y + wh;

    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
    w = CreateWindowExW(0, VKB_WIN_CLASS, wtitle, s_win_style, x, y, ww, wh,
                        NULL, NULL, inst, NULL);
    if (!w) {
        fprintf(stderr, "  [VK] CreateWindowEx failed (error %lu)\n", GetLastError());
        return NULL;
    }
    if (s_win_mode != WIN_WINDOWED)
        window_go_full(w, s_win_mode);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    GetClientRect(w, &r);
    fprintf(stderr, "  [VK] window: %s %ldx%ld on display %d (%s)\n", window_mode_name(s_win_mode),
            (long)r.right, (long)r.bottom, d, mon ? mon->name : "?");
    return w;
}

/* ---- hooks for vk_backend.c ---------------------------------------------- */

static int win_instance_extensions(const char **exts, uint32_t *n)
{
    if (*n < 2)
        return 0;
    exts[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    exts[1] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    *n = 2;
    return 1;
}

static int win_create_surface(void)
{
    VkWin32SurfaceCreateInfoKHR ci;
    VkResult r;

    if (!vkCreateWin32SurfaceKHR) {
        fprintf(stderr, "  [VK] the Vulkan driver has no VK_KHR_win32_surface\n");
        return 0;
    }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hinstance = GetModuleHandleW(NULL);
    ci.hwnd = V.window;
    r = vkCreateWin32SurfaceKHR(V.instance, &ci, NULL, &V.wsi);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "  [VK] vkCreateWin32SurfaceKHR: %d\n", (int)r);
        return 0;
    }
    return 1;
}

static void win_drawable_size(int *w, int *h)
{
    RECT r;

    if (V.window && GetClientRect(V.window, &r)) {
        *w = r.right - r.left;
        *h = r.bottom - r.top;
    } else {
        *w = *h = 0;
    }
}

static int win_display_index(void)
{
    const WinMonitor *m = V.window ? window_monitor(V.window) : NULL;
    return m ? (int)(m - s_mon) : -1;
}

static uint32_t win_ticks_ms(void)
{
    if (!s_t0)
        s_t0 = vkb_now();
    return (uint32_t)((vkb_now() - s_t0) * 1000.0);
}

static void win_set_title(const char *title)
{
    WCHAR w[256];

    MultiByteToWideChar(CP_UTF8, 0, title, -1, w, 256);
    SetWindowTextW(V.window, w);
}

static int win_open_lazy(void)
{
    if (!s_t0)
        s_t0 = vkb_now();
    V.window = window_create("SSX 3 (Recomp, Vulkan)");
    return V.window != NULL;
}

/* ---- the window on the process main thread ------------------------------ */

static void *guest_thread(void *unused)
{
    (void)unused;
    s_guest_rc = s_guest_main();
    /* To the window, not the thread: a thread message is lost while the
     * window is being moved or resized (a modal loop). */
    PostMessageW(V.window, WM_VKB_GUEST_DONE, 0, 0);
    return NULL;
}

/* The guest's thread, with the POSIX host's stack rule: at least the main
 * thread's (the executable's stack reserve) and generously more, since lifted
 * code recurses on it. */
static int start_guest_thread(void)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)dos + dos->e_lfanew);
    size_t stack = 64u << 20;

    if ((size_t)nt->OptionalHeader.SizeOfStackReserve > stack)
        stack = (size_t)nt->OptionalHeader.SizeOfStackReserve;
    return vkb_thread_start(guest_thread, NULL, stack);
}

int ssx3_vk_host_main(int (*guest_main)(void))
{
    s_t0 = vkb_now();
    V.window = window_create("SSX 3 (Recomp, Vulkan) | loading");
    if (!V.window) {
        fprintf(stderr, "  [VK] no window; running without a window thread\n");
        return guest_main();
    }
    read_display_hz();
    V.host_main = 1;
    s_guest_main = guest_main;
    if (!start_guest_thread()) {
        fprintf(stderr, "  [VK] cannot start the guest thread\n");
        V.host_main = 0;
        return guest_main();
    }
    fprintf(stderr, "  [VK] window open; guest runs on its own thread\n");

    /* Pump from the start, through the whole load, so Windows never marks
     * the window as not responding; refresh the FPS title while frames come. */
    for (;;) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 16, QS_ALLINPUT);
        pump_events();
        if (s_guest_done)
            return s_guest_rc;
        if (V.flips)
            update_title();
    }
}
