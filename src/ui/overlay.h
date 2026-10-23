/*
 * Host overlay: screens we draw over the game (the LAN lobby first), independent of the renderer.
 *
 * Drawing: a screen registers a draw callback (ovl_set_screen). The renderer calls ovl_build() once per
 * presented frame; that runs the callback, which draws with ovl_rect / ovl_text on a virtual canvas
 * OVL_CANVAS_H units high and ovl_canvas_w() wide (the window's aspect), and hands back triangles in
 * window pixels: textured with the font atlas (ovl_font_alpha): the game's own front-end font
 * (data/fonts/fefont.ffn, read at run time) plus a solid cell for rectangles and a disc for button prompts.
 *
 * Input: while a screen holds the input (ovl_capture), pad port 0 (with the keyboard keys the game maps
 * onto it) and the keyboard come to the overlay and the game sees an idle pad. Button presses and typed
 * text queue up and are read in the draw callback (ovl_next_button, ovl_take_text).
 *
 * Threads: ovl_build and the screen callback run on the renderer's presenting thread (at most every
 * 16 ms), pad reads on the USB thread, SDL events on the window thread; only the input queues are locked.
 */
#ifndef SSX3_UI_OVERLAY_H
#define SSX3_UI_OVERLAY_H

#include <stdint.h>

#define OVL_CANVAS_H 720.0f

typedef struct {
    float    x, y;          /* window pixels */
    float    u, v;          /* font atlas, normalized */
    uint32_t rgba;          /* bytes R, G, B, A in memory (VK_FORMAT_R8G8B8A8_UNORM) */
} ovl_vertex;

/* buttons, as the overlay reads them (pad port 0 or the keyboard) */
enum {
    OVL_UP = 1, OVL_DOWN, OVL_LEFT, OVL_RIGHT,
    OVL_A,                  /* confirm: A / Enter */
    OVL_B,                  /* back: B / Escape */
    OVL_X, OVL_Y,           /* X / F1, Y / F2 */
    OVL_START, OVL_BACK,    /* Start / F3, Back / F4 */
    OVL_WHITE, OVL_BLACK,   /* page up / page down */
    OVL_BACKSPACE, OVL_TAB, /* keyboard only */
};

/* ---- screens -------------------------------------------------------------- */
typedef void (*ovl_draw_fn)(void);
void  ovl_set_screen(ovl_draw_fn draw);  /* NULL: nothing drawn */
void  ovl_capture(int on);               /* the screen takes the input (the game sees an idle pad) */
int   ovl_captured(void);
void  ovl_text_mode(int on);             /* typed characters go to ovl_take_text (SDL text input) */

/* ---- drawing (inside the draw callback) ---------------------------------- */
float ovl_canvas_w(void);
void  ovl_rect(float x, float y, float w, float h, uint32_t rgba);   /* rgba = 0xRRGGBBAA */
void  ovl_disc(float cx, float cy, float r, uint32_t rgba);          /* filled circle (button prompts) */
void  ovl_poly4(const float xy[8], uint32_t rgba);                   /* filled quad, corners in order */
/* Text with its top-left at x, y, `size` units per line; stops at `max_w` (0: no limit). Returns its width. */
float ovl_text(float x, float y, float size, uint32_t rgba, const char *s, float max_w);
float ovl_text_width(float size, const char *s);
uint32_t ovl_time_ms(void);              /* monotonic, for blinking and timeouts */

/* ---- input --------------------------------------------------------------- */
int   ovl_next_button(void);             /* next queued press, 0 = none */
int   ovl_take_text(char *buf, int cap); /* typed characters since the last call (printable ASCII) */

void  ovl_set_hotkey(void (*fn)(void));  /* F5 or pad Back + Y while the overlay is not capturing */
void  ovl_inject_a(void);                /* the game's pad 1 gets one A press (continue in its menu) */
/* the pad, from src/input/ssx3_pad.c: 0 = untouched, 1 = the game gets an idle pad, 2 = an idle pad with
 * A held (ovl_inject_a) */
int   ovl_pad_feed(uint32_t port, uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly);
/* an SDL_Event from the window thread: 1 = the overlay took it */
int   ovl_sdl_event(const void *sdl_event);

/* ---- renderer ------------------------------------------------------------ */
/* Runs the screen for a window of w x h pixels. Returns the vertex count (triangle list), 0 = nothing. */
int   ovl_build(int w, int h, const ovl_vertex **verts);
/* The atlas (the game's front-end font plus a solid cell and a disc): 8-bit coverage, w*h bytes. 0: no
 * font (game files missing), the overlay stays off. */
int   ovl_font_alpha(int *w, int *h, const uint8_t **alpha);
/* frame dumps: the last overlay drawn over a w x h picture (bytes B, G, R, A), stretched from the window */
void  ovl_composite_bgra(uint8_t *px, int w, int h);

#endif
