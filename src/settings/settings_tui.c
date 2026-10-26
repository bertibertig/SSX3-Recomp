/*
 * The settings screen in the terminal (`ssx3_recomp --settings`, or
 * `scripts/run.sh --settings` / `run.ps1 --settings`): categories on the
 * left, the selected category's options on the right, a description of the
 * highlighted option below. Layout and keys as Amped2-Recomp's and
 * Splatoon-Recomp's settings screens; the look is SSX 3's and matches the game
 * data installer (scripts/install_game.sh): the orange-to-yellow slanted logo,
 * the three-peak ridge, ice blue and snow white on a night-blue slope, orange
 * slanted bars for the selection, the boost meter as a gauge.
 *
 * Plain ANSI escapes and a raw terminal (termios / the Windows console in VT
 * mode), no curses, so the release binary needs nothing extra.
 *
 * Keys: Up/Down (k/j, w) move, Left/Right (h/l, a/d) change a value or switch
 * panes, Tab switches panes, Enter saves and starts, S saves, Esc/Q leaves
 * (asks when something is unsaved).
 */
#include "settings.h"

#if defined(SSX3_HAVE_VULKAN)
#include "render/vk_backend.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

/* ---- terminal --------------------------------------------------------- */

enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_TAB, K_ENTER, K_ESC, K_SAVE, K_QUIT,
       K_OTHER };

#ifdef _WIN32
static DWORD s_old_in, s_old_out;
static UINT s_old_cp;
/* The console's own buffers: a GUI-subsystem process has no standard
 * handles, attached console or not. */
static HANDLE s_hin = INVALID_HANDLE_VALUE, s_hout = INVALID_HANDLE_VALUE;

static void term_raw(void)
{
    HANDLE in = s_hin, out = s_hout;
    GetConsoleMode(in, &s_old_in);
    GetConsoleMode(out, &s_old_out);
    SetConsoleMode(out, s_old_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    s_old_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
}

static void term_restore(void)
{
    SetConsoleMode(s_hin, s_old_in);
    SetConsoleMode(s_hout, s_old_out);
    SetConsoleOutputCP(s_old_cp);
}

static void term_size(int *w, int *h)
{
    CONSOLE_SCREEN_BUFFER_INFO ci;
    *w = 80; *h = 24;
    if (GetConsoleScreenBufferInfo(s_hout, &ci)) {
        *w = ci.srWindow.Right - ci.srWindow.Left + 1;
        *h = ci.srWindow.Bottom - ci.srWindow.Top + 1;
    }
}

static int term_interactive(void)
{
    DWORD m;
    /* The exe is a GUI-subsystem program: it has no console of its own.
     * Borrow the one it was started from (run.ps1 waits for it), else open
     * one, and point the C streams at it. */
    if (!GetConsoleWindow() && !AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole())
        return 0;
    s_hin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    s_hout = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    /* The game's log after the screen goes to the same console. */
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    return GetConsoleMode(s_hin, &m) && GetConsoleMode(s_hout, &m);
}

static int read_key(void)
{
    int c = _getch();
    if (c == 0 || c == 0xE0) {
        switch (_getch()) {
        case 72: return K_UP;
        case 80: return K_DOWN;
        case 75: return K_LEFT;
        case 77: return K_RIGHT;
        default: return K_OTHER;
        }
    }
    switch (c) {
    case 27:              return K_ESC;
    case '\t':            return K_TAB;
    case '\r': case '\n': return K_ENTER;
    case 'k': case 'w': case 'K': case 'W': return K_UP;
    case 'j': case 'J':   return K_DOWN;
    case 'h': case 'a': case 'H': case 'A': return K_LEFT;
    case 'l': case 'd': case 'L': case 'D': return K_RIGHT;
    case 's': case 'S':   return K_SAVE;
    case 'q': case 'Q':   return K_QUIT;
    default:              return K_OTHER;
    }
}
#else
static struct termios s_old;

static void term_raw(void)
{
    struct termios t;
    tcgetattr(0, &s_old);
    t = s_old;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
}

static void term_restore(void)
{
    tcsetattr(0, TCSANOW, &s_old);
}

static void term_size(int *w, int *h)
{
    struct winsize ws;
    *w = 80; *h = 24;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    }
}

/* Not isatty(): the generated code exports a guest CRT function of that name
 * (Ghidra-named), and the executable's own symbol wins. */
static int term_interactive(void)
{
    struct termios t;
    return tcgetattr(0, &t) == 0 && tcgetattr(1, &t) == 0;
}

/* A byte, or -1 if none arrives within ms. */
static int read_byte(int ms)
{
    struct pollfd p = { 0, POLLIN, 0 };
    unsigned char c;
    if (poll(&p, 1, ms) <= 0 || read(0, &c, 1) != 1)
        return -1;
    return c;
}

static int read_key(void)
{
    int c = read_byte(-1);
    if (c == 27) {
        int c2 = read_byte(30), c3;
        if (c2 < 0)
            return K_ESC;                       /* a lone Esc */
        if (c2 != '[' && c2 != 'O')
            return K_OTHER;
        c3 = read_byte(30);
        switch (c3) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        default:
            while (c3 >= 0 && !(c3 >= 0x40 && c3 <= 0x7E))   /* rest of the sequence */
                c3 = read_byte(30);
            return K_OTHER;
        }
    }
    switch (c) {
    case '\t':            return K_TAB;
    case '\r': case '\n': return K_ENTER;
    case 'k': case 'w': case 'K': case 'W': return K_UP;
    case 'j': case 'J':   return K_DOWN;
    case 'h': case 'a': case 'H': case 'A': return K_LEFT;
    case 'l': case 'd': case 'L': case 'D': return K_RIGHT;
    case 's': case 'S':   return K_SAVE;
    case 'q': case 'Q':   return K_QUIT;
    case -1:              return K_QUIT;    /* stdin closed */
    default:              return K_OTHER;
    }
}
#endif

/* ---- output ------------------------------------------------------------ */

static char   s_out[1 << 17];
static size_t s_outn;
static int    s_x0;                         /* left margin: the screen is centred */

static void out(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(s_out + s_outn, sizeof s_out - s_outn, fmt, ap);
    va_end(ap);
    if (n > 0)
        s_outn += (size_t)n < sizeof s_out - s_outn ? (size_t)n : sizeof s_out - s_outn - 1;
}

static void flush_out(void)
{
#ifdef _WIN32
    /* As UTF-16 straight to the console: independent of the code page, and
     * one write per frame, so the VT parser never sees half a sequence. */
    static wchar_t w[1 << 17];
    HANDLE h = s_hout;
    DWORD done;
    int n = MultiByteToWideChar(CP_UTF8, 0, s_out, (int)s_outn, w, (int)(sizeof w / sizeof w[0]));
    if (n > 0 && WriteConsoleW(h, w, (DWORD)n, &done, NULL)) {
        s_outn = 0;
        return;
    }
#endif
    fwrite(s_out, 1, s_outn, stdout);
    fflush(stdout);
    s_outn = 0;
}

static void at(int row, int col) { out("\x1b[%d;%dH", row, col + s_x0); }

/* s, cut or padded to exactly w columns (every code point one column: Latin
 * text, box drawing, arrows). */
static void field(const char *s, int w)
{
    int n = 0;
    const char *p = s;
    while (*p && n < w) {
        const char *q = p + 1;
        while (((unsigned char)*q & 0xC0) == 0x80)
            q++;
        out("%.*s", (int)(q - p), p);
        p = q;
        n++;
    }
    for (; n < w; n++)
        out(" ");
}

static int cols(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80)
            n++;
    return n;
}

/* ---- the look ---------------------------------------------------------- */

/* Colours as RGB: 24-bit where the terminal says so (and on Windows 10+,
 * whose console has it in VT mode), else the nearest of the 256. */
static int s_truecolor;

static void colour(int bg, int r, int g, int b)
{
    if (s_truecolor)
        out("\x1b[%d;2;%d;%d;%dm", bg ? 48 : 38, r, g, b);
    else
        out("\x1b[%d;5;%dm", bg ? 48 : 38,
            16 + 36 * ((r * 5 + 127) / 255) + 6 * ((g * 5 + 127) / 255) + (b * 5 + 127) / 255);
}

typedef struct { int r, g, b; } Rgb;
static const Rgb k_night  = { 14, 24, 44 };    /* the slope at night: the page */
static const Rgb k_orange = { 255, 122, 0 };   /* SSX orange */
static const Rgb k_yellow = { 255, 204, 0 };   /* sun yellow */
static const Rgb k_ice    = { 120, 210, 255 }; /* ice blue */
static const Rgb k_deep   = { 70, 120, 170 };  /* mountain shade */
static const Rgb k_snow   = { 245, 250, 255 }; /* snow white */
static const Rgb k_dim    = { 130, 140, 160 };
static const Rgb k_ink    = { 20, 20, 30 };    /* text on orange */
static const Rgb k_green  = { 120, 230, 120 };
static const Rgb k_red    = { 255, 80, 60 };
static const Rgb k_xblue  = { 70, 140, 255 };

static void fgc(Rgb c) { colour(0, c.r, c.g, c.b); }
static void bgc(Rgb c) { colour(1, c.r, c.g, c.b); }
/* Back to plain text on the page. */
static void base(void) { out("\x1b[0m"); bgc(k_night); fgc(k_snow); }

static const char *const k_logo[] = {
    "       ▄▄█████▄   ▄▄█████▄  ▀███▄   ▄███▀      ▄█████▄▄",
    "      ███▀▀  ▀▀  ███▀▀  ▀▀    ▀███▄███▀        ▀▀  ▄██▀",
    "      ▀▀█████▄   ▀▀█████▄       ▄███▄            ▀▀██▄",
    "    ▄▄    ▄██▀ ▄▄    ▄██▀     ▄███▀███▄      ▄▄    ▄██▀",
    "    ▀██████▀▀  ▀██████▀▀    ▄███▀   ▀███▄    ▀██████▀▀",
};
#define LOGO_ROWS 5
#define LOGO_COLS 57
static const Rgb k_logo_col[LOGO_ROWS] = {
    { 255, 84, 0 }, { 255, 112, 0 }, { 255, 140, 0 }, { 255, 170, 0 }, { 255, 200, 20 } };

/* The three peaks, two rows high; peak `lit` (1..3) is orange. As
 * install_game.sh's ridge_row. */
static void ridge(int row, int w, int lit)
{
    static const char *const blocks[9] = { " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    static const int height[4] = { 0, 10, 13, 16 };
    int centre[4], x, r;

    centre[0] = 0;
    centre[1] = w * 18 / 100;
    centre[2] = w * 47 / 100;
    centre[3] = w * 78 / 100;
    for (r = 0; r < 2; r++) {
        int cur = -1;
        at(row + r, 1);
        base();
        for (x = 0; x < w; x++) {
            int best = 0, owner = 0, p, lvl, col;
            for (p = 1; p <= 3; p++) {
                int d = x > centre[p] ? x - centre[p] : centre[p] - x;
                int h = height[p] - (d * 5 + 2) / 4;
                if (h > best) { best = h; owner = p; }
            }
            if (best < 1)
                best = 1;
            lvl = r == 0 ? best - 8 : best;
            lvl = lvl < 0 ? 0 : lvl > 8 ? 8 : lvl;
            col = owner == lit ? 0 : r == 0 ? 1 : 2;
            if (col != cur) {
                fgc(col == 0 ? k_orange : col == 1 ? k_snow : k_deep);
                cur = col;
            }
            out("%s", blocks[lvl]);
        }
    }
    base();
}

/* An orange slanted bar: ▶ ◢ text ◤, w columns in all. */
static void bar(const char *text, int w, int dim)
{
    base();
    fgc(dim ? k_deep : k_orange);
    out("◢");
    bgc(dim ? k_deep : k_orange);
    fgc(dim ? k_snow : k_ink);
    out("\x1b[1;3m");
    field(text, w - 2);
    out("\x1b[22;23m");
    base();
    fgc(dim ? k_deep : k_orange);
    out("◤");
    base();
}

/* "▶▶ TITLE ────" in orange italics. */
static void panel(int row, int col, const char *title, int w)
{
    int n = cols(title) + 4, i;
    at(row, col);
    base();
    fgc(k_orange);
    out("\x1b[1;3m▶▶ %s\x1b[22;23m ", title);
    fgc(k_deep);
    for (i = n; i < w; i++)
        out("─");
    base();
}

/* ---- the model ---------------------------------------------------------- */

#define MAX_DISPLAYS 16

static struct {
    int  count;
    char name[MAX_DISPLAYS][96];
    int  hz[MAX_DISPLAYS];
    int  primary;
} s_disp;

static int s_have_vulkan;

typedef struct {
    const char *label;
    const char *help;
    int  (*count)(void);
    void (*text)(int i, char *buf, size_t n);
    int  (*get)(void);
    void (*set)(int i);
    const char *(*disabled)(void);   /* NULL, or why it cannot be changed now */
    int  meter;                      /* draw the value as a boost meter */
} Option;

typedef struct {
    const char   *title;
    const char   *blurb;
    const Option *opts;
    int           nopts;
} Category;

#define S g_ssx3_settings

static const char *vulkan_only(void)
{
    return S.renderer == SSX3_RENDER_CPU ? "Vulkan renderer only" : NULL;
}

/* Resolution */
static int  n_scale(void)  { return SSX3_SCALE_MAX; }
static int  g_scale(void)  { return S.resolution_scale - 1; }
static void s_scale(int i) { S.resolution_scale = i + 1; }
static void t_scale(int i, char *b, size_t n)
{
    int h = 480 * (i + 1), w = (int)(h * ssx3_aspect_value(S.aspect_ratio) + 0.5);
    if (S.aspect_ratio == SSX3_ASPECT_4_3)
        w = 640 * (i + 1);
    snprintf(b, n, "%dx  %d x %d%s", i + 1, w, h, i == 0 ? "  original" : "");
}

/* Aspect */
static int  n_aspect(void)  { return SSX3_ASPECT_COUNT; }
static int  g_aspect(void)  { return S.aspect_ratio; }
static void s_aspect(int i) { S.aspect_ratio = i; }
static void t_aspect(int i, char *b, size_t n)
{
    static const char *const extra[SSX3_ASPECT_COUNT] = {
        "original", "widescreen", "widescreen", "ultrawide", "the monitor's shape" };
    snprintf(b, n, "%s  %s", i == SSX3_ASPECT_AUTO ? "Auto" : ssx3_aspect_name(i), extra[i]);
}

/* Frame rate */
static int  n_fps(void)  { return SSX3_FPS_COUNT; }
static int  g_fps(void)  { return S.frame_rate; }
static void s_fps(int i) { S.frame_rate = i; }
static void t_fps(int i, char *b, size_t n)
{
    int d, hz = 0;
    for (d = 0; d < s_disp.count; d++)
        if (s_disp.hz[d] > hz)
            hz = s_disp.hz[d];
    switch (i) {
    case SSX3_FPS_60:       snprintf(b, n, "60 fps  original, no interpolation"); break;
    case SSX3_FPS_DISPLAY:
        if (hz)
            snprintf(b, n, "Display refresh  (%d Hz)", hz);
        else
            snprintf(b, n, "Display refresh");
        break;
    case SSX3_FPS_UNCAPPED: snprintf(b, n, "Uncapped"); break;
    default: {
        static const int v[SSX3_FPS_COUNT] = { 60, 0, 120, 144, 165, 240, 0 };
        snprintf(b, n, "%d fps", v[i]);
    } }
}

static int  n_vsync(void)  { return 2; }
static int  g_vsync(void)  { return S.vsync; }
static void s_vsync(int i) { S.vsync = i; }
static void t_vsync(int i, char *b, size_t n)
{
    snprintf(b, n, "%s", i ? "On   waits for the refresh (FIFO)" : "Off  lowest latency, no tearing (mailbox)");
}

/* Display */
static int  n_window(void)  { return SSX3_WIN_COUNT; }
static int  g_window(void)  { return S.window_mode; }
static void s_window(int i) { S.window_mode = i; }
static void t_window(int i, char *b, size_t n)
{
    static const char *const names[SSX3_WIN_COUNT] = {
        "Windowed", "Borderless fullscreen", "Fullscreen" };
    snprintf(b, n, "%s", names[i]);
}

/* Monitor: 0 = the main one, then each display. */
static int  n_monitor(void) { return 1 + s_disp.count; }
static int  g_monitor(void)
{
    char *end;
    long v;
    int i;
    if (!strcmp(S.monitor, "primary"))
        return 0;
    v = strtol(S.monitor, &end, 10);
    if (!*end && v >= 0 && v < s_disp.count)
        return (int)v + 1;
    for (i = 0; i < s_disp.count; i++)
        if (strstr(s_disp.name[i], S.monitor))
            return i + 1;
    return 0;
}
static void s_monitor(int i)
{
    if (i == 0)
        snprintf(S.monitor, sizeof S.monitor, "primary");
    else
        snprintf(S.monitor, sizeof S.monitor, "%d", i - 1);
}
static void t_monitor(int i, char *b, size_t n)
{
    if (i == 0)
        snprintf(b, n, "Main monitor%s%s", s_disp.count ? "  " : "",
                 s_disp.count ? s_disp.name[s_disp.primary] : "");
    else
        snprintf(b, n, "%d: %s", i, s_disp.name[i - 1]);
}

static int  n_renderer(void)  { return SSX3_RENDER_COUNT; }
static int  g_renderer(void)  { return S.renderer; }
static void s_renderer(int i) { S.renderer = i; }
static void t_renderer(int i, char *b, size_t n)
{
    if (i == SSX3_RENDER_VULKAN)
        snprintf(b, n, "Vulkan  GPU%s", s_have_vulkan ? "" : "  (no driver found: falls back)");
    else
        snprintf(b, n, "Software  CPU reference, 640x480");
}

/* Language: auto, then the languages the disc has. */
static int s_langs[SSX3_LANG_COUNT], s_nlangs;
static int  n_lang(void)  { return s_nlangs; }
static int  g_lang(void)
{
    int i;
    for (i = 0; i < s_nlangs; i++)
        if (s_langs[i] == S.language)
            return i;
    return 0;
}
static void s_lang(int i) { S.language = s_langs[i]; }
static void t_lang(int i, char *b, size_t n)
{
    int l = s_langs[i];
    if (l == SSX3_LANG_AUTO)
        snprintf(b, n, "Automatic  system: %s", ssx3_language_name(ssx3_language_resolve(SSX3_LANG_AUTO)));
    else
        snprintf(b, n, "%s", ssx3_language_name(l));
}

/* Game */
static int  n_bool(void) { return 2; }
static void t_onoff(int i, char *b, size_t n) { snprintf(b, n, "%s", i ? "On" : "Off"); }
static int  g_logos(void)  { return S.skip_logos; }
static void s_logos(int i) { S.skip_logos = i; }
static int  g_intro(void)  { return S.skip_intro; }
static void s_intro(int i) { S.skip_intro = i; }
static int  g_kbd(void)    { return S.keyboard; }
static void s_kbd(int i)   { S.keyboard = i; }

static const Option k_res_opts[] = {
    { "Internal resolution",
      "The resolution the slopes are drawn at before they are scaled to the window. 1x is "
      "the Xbox's 640x480; every step multiplies both sides. Higher values sharpen edges, "
      "trees and the far side of the mountain and cost GPU time (5x is 25 times the pixels "
      "of 1x). F10 cycles it while you ride.",
      n_scale, t_scale, g_scale, s_scale, vulkan_only, 1 },
};
static const Option k_aspect_opts[] = {
    { "Aspect ratio",
      "Races get wider: more of the mountain at the sides, the height of the view stays as "
      "on the Xbox. The HUD keeps its 4:3 shape in the middle; menus, loading screens and "
      "movies stay 4:3 with bars at the sides. Auto takes the monitor's shape.",
      n_aspect, t_aspect, g_aspect, s_aspect, vulkan_only, 0 },
};
static const Option k_fps_opts[] = {
    { "Frame rate",
      "The game runs at 60 steps per second like the Xbox. Above 60, extra frames are blended "
      "between the game's own (frame interpolation): smoother motion, about one frame more "
      "latency. Display refresh follows the fastest monitor. 60 shows only the game's frames.",
      n_fps, t_fps, g_fps, s_fps, vulkan_only, 0 },
    { "V-Sync",
      "Off presents each frame as soon as it is ready, without tearing where the driver has "
      "mailbox mode. On waits for the monitor's refresh: even pacing, more latency.",
      n_vsync, t_vsync, g_vsync, s_vsync, vulkan_only, 0 },
};
static const Option k_display_opts[] = {
    { "Window mode",
      "Windowed: a resizable window. Borderless fullscreen: a window covering the monitor, "
      "quick to Alt+Tab away from. Fullscreen: full screen at the desktop's resolution. "
      "Alt+Enter or F11 switch between windowed and full screen while you ride.",
      n_window, t_window, g_window, s_window, vulkan_only, 0 },
    { "Monitor",
      "The monitor the game opens on. Main monitor follows the desktop's primary display.",
      n_monitor, t_monitor, g_monitor, s_monitor, vulkan_only, 0 },
    { "Renderer",
      "Vulkan draws on the GPU and has every option on this screen. Software is the CPU "
      "reference renderer: 640x480, 4:3, 60 fps, a window - slow, kept for comparisons.",
      n_renderer, t_renderer, g_renderer, s_renderer, NULL, 0 },
};
static const Option k_lang_opts[] = {
    { "Language",
      "The language of the game's menus and texts, from the ones on your disc (the PAL disc "
      "has English, French, German and Spanish). Automatic uses your system's display "
      "language when the disc has it, else English.",
      n_lang, t_lang, g_lang, s_lang, NULL, 0 },
};
static const Option k_game_opts[] = {
    { "Skip boot logos",
      "Skips the EA BIG and THX logos at start-up.",
      n_bool, t_onoff, g_logos, s_logos, NULL, 0 },
    { "Skip intro movie",
      "Skips the intro movie and goes straight to the title screen. Off: it plays, any button "
      "skips it.",
      n_bool, t_onoff, g_intro, s_intro, NULL, 0 },
    { "Keyboard controls",
      "The keyboard as controller 1: arrows steer, Z = A, X = B, Enter = Start (README, "
      "Controllers). Turn it off when a pad should be controller 1 on its own.",
      n_bool, t_onoff, g_kbd, s_kbd, NULL, 0 },
};

#undef S

#define NOPTS(a) ((int)(sizeof a / sizeof a[0]))
static const Category k_cats[] = {
    { "Resolution Scaling", "Ride above the Xbox's 640x480.",            k_res_opts,     NOPTS(k_res_opts) },
    { "Aspect Ratio",       "Widescreen and ultrawide slopes.",           k_aspect_opts,  NOPTS(k_aspect_opts) },
    { "Framerate",          "Beyond the Xbox's 60 frames per second.",    k_fps_opts,     NOPTS(k_fps_opts) },
    { "Display",            "Window mode, monitor and renderer.",         k_display_opts, NOPTS(k_display_opts) },
    { "Language",           "The game's text.",                           k_lang_opts,    NOPTS(k_lang_opts) },
    { "Game",               "Start-up and controls.",                     k_game_opts,    NOPTS(k_game_opts) },
};
#define NCATS NOPTS(k_cats)

enum { ACT_START, ACT_SAVE, ACT_QUIT, NACTS };
static const char *const k_acts[NACTS] = { "Save & drop in", "Save", "Quit" };

static void query(void)
{
    int i;
#if defined(SSX3_HAVE_VULKAN)
    Ssx3Display d[MAX_DISPLAYS];
    s_disp.count = ssx3_vk_displays(d, MAX_DISPLAYS);
    for (i = 0; i < s_disp.count; i++) {
        snprintf(s_disp.name[i], sizeof s_disp.name[i], "%.20s  %dx%d %dHz",
                 d[i].name, d[i].w, d[i].h, d[i].hz);
        s_disp.hz[i] = d[i].hz;
        if (d[i].primary)
            s_disp.primary = i;
    }
    s_have_vulkan = ssx3_vk_available();
#endif
    s_nlangs = 0;
    s_langs[s_nlangs++] = SSX3_LANG_AUTO;
    for (i = SSX3_LANG_EN; i < SSX3_LANG_COUNT; i++)
        if (ssx3_language_on_disc(i) || i == g_ssx3_settings.language)
            s_langs[s_nlangs++] = i;
    if (s_nlangs == 1)                                /* no game data here: list them all */
        for (i = SSX3_LANG_EN; i < SSX3_LANG_COUNT; i++)
            s_langs[s_nlangs++] = i;
}

/* ---- drawing ----------------------------------------------------------- */

static int wrap(const char *text, int w, char lines[][256], int max)
{
    int n = 0;
    while (*text && n < max) {
        int len, cut;
        while (*text == ' ')
            text++;
        len = (int)strlen(text);
        if (cols(text) <= w) {
            snprintf(lines[n++], 256, "%s", text);
            break;
        }
        for (cut = w; cut > 0 && text[cut] != ' '; cut--)
            ;
        if (cut == 0)
            cut = w < len ? w : len;
        snprintf(lines[n++], 256, "%.*s", cut, text);
        text += cut;
    }
    return n;
}

typedef struct {
    int  cat;        /* 0..NCATS-1 categories, NCATS.. actions */
    int  opt;
    int  pane;       /* 0 left, 1 right */
    int  dirty;
    char status[200];
} Ui;

/* An Xbox face button: a coloured key cap, then what it does. */
static void button(const char *key, Rgb col, const char *what)
{
    base();
    bgc(col);
    fgc(k_ink);
    out("\x1b[1m %s \x1b[22m", key);
    base();
    fgc(k_snow);
    out(" %s   ", what);
}

static void draw(const Ui *u)
{
    int TW, H, W, lw = 26, rw, i, row, top, fy, big;
    char buf[256];
    const Category *c = u->cat < NCATS ? &k_cats[u->cat] : NULL;

    term_size(&TW, &H);
    s_x0 = 0;
    out("\x1b[0m\x1b[H\x1b[2J");
    if (TW < 64 || H < 22) {
        out("Please make the terminal at least 64x22 (now %dx%d).", TW, H);
        flush_out();
        return;
    }
    /* The page: the whole terminal night blue, the screen centred on it. */
    for (row = 1; row <= H; row++) {
        out("\x1b[%d;1H", row);
        base();
        out("%*s", TW, "");
    }
    W = TW > 104 ? 104 : TW;
    s_x0 = (TW - W) / 2;
    rw = W - lw - 3;

    /* Header: the logo and the ridge when there is room, else one line. The
     * lit peak walks along the categories: 1, 2, 3, 1, ... */
    big = H >= 36 && W >= LOGO_COLS + 4;
    top = 1;
    if (big) {
        for (i = 0; i < LOGO_ROWS; i++) {
            at(top + 1 + i, (W - LOGO_COLS) / 2);
            base();
            fgc(k_logo_col[i]);
            out("\x1b[1;3m%s\x1b[22;23m", k_logo[i]);
        }
        top += LOGO_ROWS + 2;
    } else {
        at(top, 3);
        base();
        fgc(k_orange);
        out("\x1b[1;3mSSX 3\x1b[22;23m");
        top += 1;
    }
    at(top, (W - 33) / 2);
    base(); fgc(k_ice); out("R E C O M P");
    fgc(k_dim); out("   ·   ");
    fgc(k_snow); out("\x1b[1mSETTINGS\x1b[22m");
    top += 1;
    if (big) {
        ridge(top + 1, W, u->cat < NCATS ? u->cat % 3 + 1 : 0);
        top += 3;
    } else {
        top += 1;
    }
    fy = top;

    /* Left: the categories as the trail down the mountain, then the actions. */
    panel(fy, 2, "CATEGORIES", lw);
    row = fy + 2;
    for (i = 0; i < NCATS + NACTS; i++) {
        int sel = i == u->cat;
        const char *name = i < NCATS ? k_cats[i].title : k_acts[i - NCATS];
        if (i == NCATS)
            row++;                                   /* gap before the actions */
        at(row++, 2);
        if (sel && u->pane == 0) {
            snprintf(buf, sizeof buf, "▶ %s", name);
            bar(buf, lw, 0);
        } else {
            base();
            if (i < NCATS) {
                fgc(sel ? k_orange : k_deep);
                out(sel ? " ◆" : " ◇");
                fgc(sel ? k_orange : k_snow);
            } else {
                fgc(sel ? k_orange : k_dim);
                out("  ");
            }
            if (sel) out("\x1b[1m");
            snprintf(buf, sizeof buf, " %s", name);
            field(buf, lw - 2);
            out("\x1b[22m");
        }
    }

    /* Right: the category's options. */
    row = fy;
    if (c) {
        panel(row++, lw + 4, c->title, rw);
        at(row++, lw + 4); base(); fgc(k_dim); field(c->blurb, rw - 2);
        row++;
        for (i = 0; i < c->nopts; i++) {
            const Option *o = &c->opts[i];
            const char *why = o->disabled ? o->disabled() : NULL;
            int sel = u->pane == 1 && i == u->opt, v = o->get(), n = o->count(), lab = 21;
            int meter = o->meter ? n * 2 + 1 : 0;
            char val[160];
            o->text(v, val, sizeof val);
            snprintf(buf, sizeof buf, "%s %s %s", v > 0 ? "◀" : " ", val, v < n - 1 ? "▶" : " ");
            at(row, lw + 3);
            if (sel) {
                char line[256];
                snprintf(line, sizeof line, " %-*s%s", lab - 1, o->label, buf);
                bar(line, rw - meter, why != NULL);
            } else {
                base();
                fgc(why ? k_dim : k_snow);
                snprintf(val, sizeof val, "  %s", o->label);
                field(val, lab);
                fgc(why ? k_dim : k_yellow);
                out("\x1b[1m");
                field(buf, rw - lab - meter);
                out("\x1b[22m");
            }
            if (meter) {                            /* the boost meter */
                int p;
                base();
                out(" ");
                for (p = 0; p < n; p++) {
                    fgc(why ? k_dim : p <= v ? (p == n - 1 ? k_yellow : k_orange) : k_deep);
                    out("%s", p <= v ? "▰ " : "▱ ");
                }
            }
            row++;
            if (why) {
                at(row++, lw + 3 + lab);
                base(); fgc(k_dim); out("(%s)", why);
            }
            row++;
        }
        if (u->pane == 1 || c->nopts == 1) {
            char lines[12][256];
            int n = wrap(c->opts[u->pane == 1 ? u->opt : 0].help, rw - 5, lines, 12), k;
            row++;
            for (k = 0; k < n && row < H - 3; k++) {
                at(row++, lw + 4);
                base(); fgc(k_ice); out("▌ ");
                fgc(k_snow); out("%s", lines[k]);
            }
        }
    } else {
        int act = u->cat - NCATS;
        const char *what = act == ACT_START ? "Saves the settings and drops you in: the game starts with them."
                         : act == ACT_SAVE ? "Saves the settings and stays here."
                         : "Leaves without starting the game.";
        panel(row++, lw + 4, k_acts[act], rw);
        row++;
        at(row++, lw + 4); base(); fgc(k_snow); field(what, rw - 2);
        row++;
        at(row++, lw + 4); base(); fgc(k_dim); out("File  "); fgc(k_ice);
        field(ssx3_settings_path(), rw - 8);
    }

    /* Bottom: the keys as Xbox buttons, or a message. */
    at(H - 1, 2);
    base(); fgc(k_deep);
    for (i = 2; i < W; i++)
        out("─");
    at(H, 3);
    if (u->status[0]) {
        base(); fgc(k_yellow); out("\x1b[1m");
        field(u->status, W - 4);
    } else {
        base(); fgc(k_dim);
        if (W >= 100)
            out("↑↓ move  ←→ change  Tab pane   ");
        button("A", k_green, "Enter: save & drop in");
        button("X", k_xblue, "S: save");
        button("B", k_red, "Esc: back");
        if (u->dirty) {
            base(); fgc(k_orange); out("\x1b[1m● unsaved");
        }
    }
    out("\x1b[0m");
    flush_out();
}

/* ---- main loop --------------------------------------------------------- */

static int option_enabled(const Option *o) { return !o->disabled || !o->disabled(); }

static void change(Ui *u, int dir)
{
    const Option *o;
    int v;

    if (u->cat >= NCATS)
        return;
    o = &k_cats[u->cat].opts[u->opt];
    if (!option_enabled(o))
        return;
    v = o->get() + dir;
    if (v < 0 || v >= o->count())
        return;
    o->set(v);
    u->dirty = 1;
}

static int save(Ui *u)
{
    if (ssx3_settings_save() != 0) {
        snprintf(u->status, sizeof u->status, "Could not write %s", ssx3_settings_path());
        return 0;
    }
    u->dirty = 0;
    snprintf(u->status, sizeof u->status, "Saved to %s", ssx3_settings_path());
    return 1;
}

int ssx3_settings_tui(void)
{
    Ui u;
    int result = -1, confirm_quit = 0;
    const char *ct = getenv("COLORTERM");

    if (!term_interactive()) {
        fprintf(stderr, "--settings needs a terminal (settings file: %s)\n", ssx3_settings_path());
        return 0;
    }
    memset(&u, 0, sizeof u);
    query();
    s_truecolor = (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit")));
#ifdef _WIN32
    s_truecolor = 1;                              /* Windows 10+ console, VT mode */
#endif

    term_raw();
    out("\x1b[?1049h\x1b[?25l");                  /* alternate screen, hide cursor */
    while (result < 0) {
        int k;
        draw(&u);
        k = read_key();
        if (k != K_OTHER && k != K_NONE)
            u.status[0] = 0;
        if (confirm_quit) {
            confirm_quit = 0;
            if (k == K_QUIT || k == K_ESC || k == K_ENTER)
                result = 0;
            else if (k == K_SAVE && save(&u))
                result = 0;
            continue;
        }
        switch (k) {
        case K_UP:
            if (u.pane == 0) {
                if (u.cat > 0) { u.cat--; u.opt = 0; }
            } else if (u.opt > 0) {
                u.opt--;
            }
            break;
        case K_DOWN:
            if (u.pane == 0) {
                if (u.cat < NCATS + NACTS - 1) { u.cat++; u.opt = 0; }
            } else if (u.opt < k_cats[u.cat].nopts - 1) {
                u.opt++;
            }
            break;
        case K_LEFT:
            if (u.pane == 1) {
                const Option *o = &k_cats[u.cat].opts[u.opt];
                if (o->get() == 0 || !option_enabled(o))
                    u.pane = 0;                   /* at the left end: back to the list */
                else
                    change(&u, -1);
            }
            break;
        case K_RIGHT:
            if (u.pane == 0) {
                if (u.cat < NCATS) u.pane = 1;
            } else {
                change(&u, +1);
            }
            break;
        case K_TAB:
            if (u.cat < NCATS)
                u.pane ^= 1;
            break;
        case K_ENTER:
            if (u.cat < NCATS) {
                if (u.pane == 0) { u.pane = 1; break; }
                if (save(&u)) result = 1;         /* Enter in the options: save & start */
            } else if (u.cat == NCATS + ACT_START) {
                if (save(&u)) result = 1;
            } else if (u.cat == NCATS + ACT_SAVE) {
                save(&u);
            } else if (u.dirty) {
                snprintf(u.status, sizeof u.status,
                         "Unsaved changes - Q/Esc/Enter: quit without saving, S: save and quit, other key: stay");
                confirm_quit = 1;
            } else {
                result = 0;
            }
            break;
        case K_SAVE:
            save(&u);
            break;
        case K_ESC:
            if (u.pane == 1) { u.pane = 0; break; }
            /* fall through */
        case K_QUIT:
            if (u.dirty) {
                snprintf(u.status, sizeof u.status,
                         "Unsaved changes - Q/Esc/Enter: quit without saving, S: save and quit, other key: stay");
                confirm_quit = 1;
            } else {
                result = 0;
            }
            break;
        default:
            break;
        }
    }
    out("\x1b[0m\x1b[?25h\x1b[?1049l");           /* back to the normal screen */
    flush_out();
    term_restore();
    return result == 1;
}
