/* Host overlay: draw list, font, input queue (see overlay.h). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif
#if SSX3_HAVE_VULKAN
#include <SDL.h>                        /* the window's events (the Vulkan back end's window is SDL's) */
#endif

#include "overlay.h"

#define MAX_VERTS   (6 * 6000)
#define QUEUE       64
#define TEXT_CAP    256
#define PAD_REPEAT_DELAY 380u   /* ms before a held direction repeats ... */
#define PAD_REPEAT_EVERY 110u   /* ... and then how often */
#define STICK_ON    20000       /* left stick as a d-pad */
#define ANALOG_ON   0x40        /* an analog button counts as pressed above this */

#if defined(_WIN32)
static CRITICAL_SECTION s_cs;
static volatile LONG    s_cs_init;
#else
static pthread_mutex_t  s_mutex;
static pthread_once_t   s_once = PTHREAD_ONCE_INIT;
static void mutex_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);   /* a screen may call ovl_capture */
    pthread_mutex_init(&s_mutex, &a);
}
#endif
static ovl_draw_fn  s_screen;
static void       (*s_hotkey)(void);
static volatile int s_inject;           /* polls left that the game's pad gets an A press (ovl_inject_a) */
static volatile int s_capture, s_text_mode, s_text_started;
static volatile int s_fresh;            /* capture just began: buttons already held don't count */

static ovl_vertex   s_verts[MAX_VERTS];
static int          s_nverts;
static float        s_scale, s_canvas_w;     /* window pixels per canvas unit; canvas width */
static int          s_last_n, s_last_w, s_last_h;   /* the last built list and its window size */
static uint32_t     s_built_at;
static ovl_vertex  *s_dump_verts;                   /* a copy for frame dumps (another thread) */
static int          s_dump_n, s_dump_w, s_dump_h, s_dumping;

static int          s_queue[QUEUE], s_qhead, s_qtail;
static char         s_text[TEXT_CAP];
static int          s_ntext;

/* The game's front-end font (data/fonts/fefont.ffn, read from the game files at run time): an "FNTF"
 * file, u16 glyph count at +0x0A, glyph records at [+0x14], the atlas at [+0x1C]: u16 width +4, height
 * +6, 4-bit coverage from +0x10 (high nibble = left pixel). A glyph record (12 bytes): u16 code, u8 w,
 * h, u16 x, y (in the atlas), u8 advance, s8 x offset, u8 y offset (from the top of the line), pad.
 * Below the glyphs the atlas gets a solid cell (rectangles) and a disc (button prompts). */
#define FONT_PATH   "game_files/data/fonts/fefont.ffn"
#define FONT_LINE   27.0f                 /* font pixels per line (cap top 3, descenders to 27) */
#define EXTRA_H     40                    /* atlas rows added below the font */
#define DISC        36                    /* disc diameter, texels */

typedef struct { uint16_t x, y; uint8_t w, h, adv; int8_t xo; uint8_t yo; } glyph;

static uint8_t     *s_font;
static int          s_font_w, s_font_h, s_font_state;   /* state: 0 not loaded, 1 ok, -1 missing */
static glyph        s_glyph[256];
static float        s_solid_u, s_solid_v, s_disc_u0, s_disc_v0, s_disc_u1, s_disc_v1;

static void lock(void)
{
#if defined(_WIN32)
    if (InterlockedCompareExchange(&s_cs_init, 1, 0) == 0) {
        InitializeCriticalSection(&s_cs);
        s_cs_init = 2;
    }
    while (s_cs_init != 2)
        Sleep(0);
    EnterCriticalSection(&s_cs);
#else
    pthread_once(&s_once, mutex_init);
    pthread_mutex_lock(&s_mutex);
#endif
}

static void unlock(void)
{
#if defined(_WIN32)
    LeaveCriticalSection(&s_cs);
#else
    pthread_mutex_unlock(&s_mutex);
#endif
}

uint32_t ovl_time_ms(void)
{
#if defined(_WIN32)
    return GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
#endif
}

void ovl_set_screen(ovl_draw_fn draw)
{
    lock();
    s_screen = draw;
    unlock();
}

void ovl_capture(int on)
{
    lock();
    if (on && !s_capture)
        s_fresh = 1;
    s_capture = on;
    s_qhead = s_qtail = 0;
    s_ntext = 0;
    unlock();
}

int ovl_captured(void)
{
    return s_capture;
}

/* SDL_StartTextInput belongs on the window thread: the flag is picked up there (ovl_sdl_event). */
void ovl_text_mode(int on)
{
    s_text_mode = on;
}

static void push_button(int b)
{
    static int trace = -1;
    int next = (s_qtail + 1) % QUEUE;
    if (trace < 0)
        trace = getenv("SSX3_OVL_TRACE") != NULL;
    if (trace)
        fprintf(stderr, "  [OVL] button %d\n", b);
    if (next != s_qhead) {
        s_queue[s_qtail] = b;
        s_qtail = next;
    }
}

int ovl_next_button(void)
{
    int b = 0;
    lock();
    if (s_qhead != s_qtail) {
        b = s_queue[s_qhead];
        s_qhead = (s_qhead + 1) % QUEUE;
    }
    unlock();
    return b;
}

int ovl_take_text(char *buf, int cap)
{
    int n;
    lock();
    n = s_ntext < cap - 1 ? s_ntext : cap - 1;
    memcpy(buf, s_text, (size_t)n);
    buf[n] = 0;
    s_ntext = 0;
    unlock();
    return n;
}

void ovl_set_hotkey(void (*fn)(void))
{
    s_hotkey = fn;
}

void ovl_inject_a(void)
{
    s_inject = 12;                      /* held for a few reports, so the game's edge detection sees it */
}

/* ---- pad --------------------------------------------------------------- */

int ovl_pad_feed(uint32_t port, uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly)
{
    /* index: 0..3 d-pad up/down/left/right, 4 start, 5 back, 6.. A B X Y black white */
    static const int map[12] = { OVL_UP, OVL_DOWN, OVL_LEFT, OVL_RIGHT, OVL_START, OVL_BACK,
                                 OVL_A, OVL_B, OVL_X, OVL_Y, OVL_BLACK, OVL_WHITE };
    static uint8_t held[12];
    static uint32_t next_repeat[4];
    uint8_t now[12];
    uint32_t t;
    int i;

    static int combo;
    if (port != 0)
        return 0;
    if (!s_capture) {
        /* Back + Y: the hotkey (the lobby, while a session is open) */
        int now_combo = (buttons & 0x20u) && analog[3] > ANALOG_ON;
        if (now_combo && !combo && s_hotkey)
            s_hotkey();
        combo = now_combo;
        if (s_inject > 0) {
            s_inject--;
            return s_inject > 4 ? 2 : 1;    /* A held, then a few idle reports (the release) */
        }
        return 0;
    }
    for (i = 0; i < 6; i++)
        now[i] = (buttons >> i) & 1u;
    for (i = 0; i < 6; i++)
        now[6 + i] = analog[i] > ANALOG_ON;
    now[0] |= ly > STICK_ON;
    now[1] |= ly < -STICK_ON;
    now[2] |= lx < -STICK_ON;
    now[3] |= lx > STICK_ON;
    t = ovl_time_ms();
    lock();
    if (s_fresh) {                          /* the press that opened the screen is not one for it */
        s_fresh = 0;
        memcpy(held, now, sizeof held);
    }
    for (i = 0; i < 12; i++) {
        if (now[i] && !held[i]) {
            push_button(map[i]);
            if (i < 4)
                next_repeat[i] = t + PAD_REPEAT_DELAY;
        } else if (now[i] && i < 4 && (int32_t)(t - next_repeat[i]) >= 0) {
            push_button(map[i]);
            next_repeat[i] = t + PAD_REPEAT_EVERY;
        }
        held[i] = now[i];
    }
    unlock();
    return 1;
}

/* ---- keyboard ---------------------------------------------------------- */

int ovl_sdl_event(const void *sdl_event)
{
#if !SSX3_HAVE_VULKAN
    (void)sdl_event;
    return 0;
#else
    const SDL_Event *ev = (const SDL_Event *)sdl_event;
    int b = 0;

    if (s_text_mode != s_text_started) {
        s_text_started = s_text_mode;
        if (s_text_started)
            SDL_StartTextInput();
        else
            SDL_StopTextInput();
    }
    if (!s_capture) {
        if (ev->type == SDL_KEYDOWN && !ev->key.repeat && ev->key.keysym.scancode == SDL_SCANCODE_F5 && s_hotkey)
            s_hotkey();
        return 0;
    }
    if (ev->type == SDL_TEXTINPUT) {
        const char *c;
        lock();
        for (c = ev->text.text; *c; c++)
            if (*c >= 32 && *c < 127 && s_ntext < TEXT_CAP)
                s_text[s_ntext++] = *c;
        unlock();
        return 1;
    }
    if (ev->type != SDL_KEYDOWN)
        return 0;                      /* key-ups go on, so nothing sticks in the game */
    switch (ev->key.keysym.scancode) {
    case SDL_SCANCODE_UP:       b = OVL_UP; break;
    case SDL_SCANCODE_DOWN:     b = OVL_DOWN; break;
    case SDL_SCANCODE_LEFT:     b = OVL_LEFT; break;
    case SDL_SCANCODE_RIGHT:    b = OVL_RIGHT; break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: b = OVL_A; break;
    case SDL_SCANCODE_ESCAPE:   b = OVL_B; break;
    case SDL_SCANCODE_BACKSPACE: b = OVL_BACKSPACE; break;
    case SDL_SCANCODE_TAB:      b = OVL_TAB; break;
    case SDL_SCANCODE_F1:       b = OVL_X; break;
    case SDL_SCANCODE_F2:       b = OVL_Y; break;
    case SDL_SCANCODE_F3:       b = OVL_START; break;
    case SDL_SCANCODE_F4:       b = OVL_BACK; break;
    case SDL_SCANCODE_PAGEUP:   b = OVL_WHITE; break;
    case SDL_SCANCODE_PAGEDOWN: b = OVL_BLACK; break;
    case SDL_SCANCODE_F10:
    case SDL_SCANCODE_F11:
        return 0;                      /* the window's own keys */
    default:
        break;
    }
    if (b) {
        lock();
        push_button(b);
        unlock();
    }
    return 1;
#endif
}

/* ---- drawing ----------------------------------------------------------- */

float ovl_canvas_w(void)
{
    return s_canvas_w;
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 uint32_t rgba)
{
    ovl_vertex *v;
    uint32_t c = ((rgba >> 24) & 0xFFu) | ((rgba >> 8) & 0xFF00u) | ((rgba << 8) & 0xFF0000u)
               | ((rgba << 24) & 0xFF000000u);       /* 0xRRGGBBAA -> bytes R, G, B, A */
    if (s_nverts + 6 > MAX_VERTS)
        return;
    x0 *= s_scale; y0 *= s_scale; x1 *= s_scale; y1 *= s_scale;
    v = &s_verts[s_nverts];
    v[0].x = x0; v[0].y = y0; v[0].u = u0; v[0].v = v0;
    v[1].x = x1; v[1].y = y0; v[1].u = u1; v[1].v = v0;
    v[2].x = x1; v[2].y = y1; v[2].u = u1; v[2].v = v1;
    v[3] = v[0];
    v[4] = v[2];
    v[5].x = x0; v[5].y = y1; v[5].u = u0; v[5].v = v1;
    v[0].rgba = v[1].rgba = v[2].rgba = v[3].rgba = v[4].rgba = v[5].rgba = c;
    s_nverts += 6;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }

static void font_load(void)
{
    FILE *f;
    long len;
    uint8_t *d = NULL;
    uint32_t gt, bm, n, i, fw, fh, x, y;

    if (s_font_state)
        return;
    s_font_state = -1;
    if ((f = fopen(FONT_PATH, "rb")) == NULL) {
        fprintf(stderr, "  [OVL] %s missing: the overlay draws no text\n", FONT_PATH);
        return;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0x20 && (d = (uint8_t *)malloc((size_t)len)) != NULL && fread(d, 1, (size_t)len, f) == (size_t)len
            && !memcmp(d, "FNTF", 4)) {
        n = rd16(d + 0x0A);
        gt = rd32(d + 0x14);
        bm = rd32(d + 0x1C);
        fw = bm + 8 <= (uint32_t)len ? rd16(d + bm + 4) : 0;
        fh = bm + 8 <= (uint32_t)len ? rd16(d + bm + 6) : 0;
        if (fw && fh && !(fw & 1) && fw >= DISC + 8 && gt + 12u * n <= (uint32_t)len
                && bm + 0x10 + fw * fh / 2 <= (uint32_t)len
                && (s_font = (uint8_t *)calloc((size_t)fw * (fh + EXTRA_H), 1)) != NULL) {
            const uint8_t *src = d + bm + 0x10;
            for (y = 0; y < fh; y++)
                for (x = 0; x < fw; x += 2) {
                    uint8_t b = *src++;
                    s_font[y * fw + x] = (uint8_t)((b >> 4) * 17);
                    s_font[y * fw + x + 1] = (uint8_t)((b & 15) * 17);
                }
            for (i = 0; i < n; i++) {
                const uint8_t *r = d + gt + 12u * i;
                uint16_t c = rd16(r);
                if (c < 256) {
                    s_glyph[c].w = r[2]; s_glyph[c].h = r[3];
                    s_glyph[c].x = rd16(r + 4); s_glyph[c].y = rd16(r + 6);
                    s_glyph[c].adv = r[8]; s_glyph[c].xo = (int8_t)r[9]; s_glyph[c].yo = r[10];
                }
            }
            /* the solid cell (4x4 at the left) and an anti-aliased disc next to it */
            for (y = fh + 2; y < fh + 6; y++)
                for (x = 2; x < 6; x++)
                    s_font[y * fw + x] = 255;
            for (y = 0; y < DISC; y++)
                for (x = 0; x < DISC; x++) {
                    float dx = (float)x + 0.5f - DISC / 2.0f, dy = (float)y + 0.5f - DISC / 2.0f;
                    float a = DISC / 2.0f - 1.0f - sqrtf(dx * dx + dy * dy);
                    a = a < 0 ? 0 : a > 1 ? 1 : a;
                    s_font[(fh + 2 + y) * fw + 8 + x] = (uint8_t)(a * 255.0f + 0.5f);
                }
            s_font_w = (int)fw;
            s_font_h = (int)(fh + EXTRA_H);
            s_solid_u = 4.0f / (float)s_font_w;
            s_solid_v = (float)(fh + 4) / (float)s_font_h;
            s_disc_u0 = 8.0f / (float)s_font_w;
            s_disc_v0 = (float)(fh + 2) / (float)s_font_h;
            s_disc_u1 = (float)(8 + DISC) / (float)s_font_w;
            s_disc_v1 = (float)(fh + 2 + DISC) / (float)s_font_h;
            s_font_state = 1;
        }
    }
    if (s_font_state != 1)
        fprintf(stderr, "  [OVL] %s: not a font the overlay understands\n", FONT_PATH);
    free(d);
    fclose(f);
}

void ovl_rect(float x, float y, float w, float h, uint32_t rgba)
{
    quad(x, y, x + w, y + h, s_solid_u, s_solid_v, s_solid_u, s_solid_v, rgba);
}

void ovl_poly4(const float xy[8], uint32_t rgba)
{
    ovl_vertex *v;
    uint32_t c = ((rgba >> 24) & 0xFFu) | ((rgba >> 8) & 0xFF00u) | ((rgba << 8) & 0xFF0000u)
               | ((rgba << 24) & 0xFF000000u);
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    int i;
    if (s_nverts + 6 > MAX_VERTS)
        return;
    v = &s_verts[s_nverts];
    for (i = 0; i < 6; i++) {
        v[i].x = xy[2 * order[i]] * s_scale;
        v[i].y = xy[2 * order[i] + 1] * s_scale;
        v[i].u = s_solid_u;
        v[i].v = s_solid_v;
        v[i].rgba = c;
    }
    s_nverts += 6;
}

void ovl_disc(float cx, float cy, float r, uint32_t rgba)
{
    quad(cx - r, cy - r, cx + r, cy + r, s_disc_u0, s_disc_v0, s_disc_u1, s_disc_v1, rgba);
}

float ovl_text_width(float size, const char *s)
{
    float k = size / FONT_LINE, w = 0;
    for (; *s; s++)
        w += s_glyph[(unsigned char)*s].adv * k;
    return w;
}

float ovl_text(float x, float y, float size, uint32_t rgba, const char *s, float max_w)
{
    float k = size / FONT_LINE, x0 = x;
    for (; *s; s++) {
        const glyph *g = &s_glyph[(unsigned char)*s];
        if (!g->adv)
            g = &s_glyph['?'];
        if (max_w > 0 && x + g->adv * k - x0 > max_w)
            break;
        if (g->w && g->h && s_font_state == 1)
            quad(x + g->xo * k, y + g->yo * k, x + (g->xo + g->w) * k, y + (g->yo + g->h) * k,
                 (float)g->x / s_font_w, (float)g->y / s_font_h,
                 (float)(g->x + g->w) / s_font_w, (float)(g->y + g->h) / s_font_h, rgba);
        x += g->adv * k;
    }
    return x - x0;
}

/* The screen's logic and drawing run at most every BUILD_MS: the renderer may present far more often
 * (frame interpolation), and the list stays valid in between. Only the renderer's presenting thread calls
 * this, so the list needs no lock; the screen callback runs unlocked too (it takes the LAN lock, and the
 * pad thread must never wait behind it: the game reads its pad through that thread). */
#define BUILD_MS 16u

int ovl_build(int w, int h, const ovl_vertex **verts)
{
    uint32_t now = ovl_time_ms();
    if (!s_screen || w <= 0 || h <= 0)
        return 0;
    font_load();
    if (s_font_state != 1)
        return 0;
    *verts = s_verts;
    if (w == s_last_w && h == s_last_h && now - s_built_at < BUILD_MS)
        return s_last_n;
    s_built_at = now;
    s_scale = (float)h / OVL_CANVAS_H;
    s_canvas_w = (float)w / s_scale;
    s_nverts = 0;
    s_screen();
    s_last_n = s_nverts;
    s_last_w = w;
    s_last_h = h;
    if (s_dumping) {
        lock();
        if (!s_dump_verts)
            s_dump_verts = (ovl_vertex *)malloc(sizeof s_verts);
        if (s_dump_verts)
            memcpy(s_dump_verts, s_verts, sizeof(ovl_vertex) * (size_t)s_last_n);
        s_dump_n = s_dump_verts ? s_last_n : 0;
        s_dump_w = w;
        s_dump_h = h;
        unlock();
    }
    return s_last_n;
}

/* Frame dumps (SSX3_VK_DUMP) read back the game's surface, which never holds the overlay: this draws the
 * last overlay over such a picture in software (B, G, R, A bytes, w x h), stretched from the window. */
void ovl_composite_bgra(uint8_t *px, int w, int h)
{
    int t, n;
    float sx, sy;
    s_dumping = 1;                          /* from now on each build keeps a copy for us */
    lock();
    n = s_dump_n;
    if (!n || s_font_state != 1 || !s_dump_w || !s_dump_h) {
        unlock();
        return;
    }
    sx = (float)w / (float)s_dump_w;
    sy = (float)h / (float)s_dump_h;
    for (t = 0; t + 2 < n; t += 3) {
        const ovl_vertex *a = &s_dump_verts[t], *b = &s_dump_verts[t + 1], *c = &s_dump_verts[t + 2];
        float ax = a->x * sx, ay = a->y * sy, bx = b->x * sx, by = b->y * sy, cx = c->x * sx, cy = c->y * sy;
        float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        int x0, x1, y0, y1, x, y;
        uint32_t col = a->rgba;
        if (area == 0.0f)
            continue;
        x0 = (int)floorf(fminf(ax, fminf(bx, cx))); x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, cx)));
        y0 = (int)floorf(fminf(ay, fminf(by, cy))); y1 = (int)ceilf(fmaxf(ay, fmaxf(by, cy)));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++) {
                float px_ = (float)x + 0.5f, py_ = (float)y + 0.5f;
                float w0 = ((bx - px_) * (cy - py_) - (by - py_) * (cx - px_)) / area;
                float w1 = ((cx - px_) * (ay - py_) - (cy - py_) * (ax - px_)) / area;
                float w2 = 1.0f - w0 - w1, u, v, al;
                int tu, tv;
                uint8_t *d;
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                u = w0 * a->u + w1 * b->u + w2 * c->u;
                v = w0 * a->v + w1 * b->v + w2 * c->v;
                tu = (int)(u * (float)s_font_w);
                tv = (int)(v * (float)s_font_h);
                if (tu < 0 || tv < 0 || tu >= s_font_w || tv >= s_font_h)
                    continue;
                al = (float)((col >> 24) & 0xFFu) / 255.0f * (float)s_font[tv * s_font_w + tu] / 255.0f;
                d = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
                d[2] = (uint8_t)((float)(col & 0xFFu) * al + (float)d[2] * (1.0f - al));
                d[1] = (uint8_t)((float)((col >> 8) & 0xFFu) * al + (float)d[1] * (1.0f - al));
                d[0] = (uint8_t)((float)((col >> 16) & 0xFFu) * al + (float)d[0] * (1.0f - al));
            }
    }
    unlock();
}

int ovl_font_alpha(int *w, int *h, const uint8_t **alpha)
{
    font_load();
    if (s_font_state != 1)
        return 0;
    *w = s_font_w;
    *h = s_font_h;
    *alpha = s_font;
    return 1;
}
