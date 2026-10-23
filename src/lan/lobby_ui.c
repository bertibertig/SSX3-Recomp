/*
 * LAN multiplayer – the lobby screen (phase 2), drawn by the host overlay (src/ui/overlay.c) over the
 * front end in SSX 3's own style: the game's front-end font, the translucent steel-blue panels of its
 * popups, the burnt-orange selection bar with the slanted end, "• Title" headings, dashed rules and the
 * coloured button prompts.
 *
 * Opened from Single Event -> LAN Game (menu.c) or with F5 / pad Back + Y while a session is open.
 * Screens:
 *   menu      host a game, the lobbies found on the network (live), join by address, name, rider or
 *             spectator, back
 *   input     a line of text (keyboard): address, name, chat
 *   connect   joining; refusals and kicks show here
 *   lobby     players (rider, status, score, ping), chat, ready, start (host), rules (host), quick chat,
 *             kick (host), leave
 *   kick      host: whom to remove
 * The host's "Start event" (everyone ready) moves the session to the event select: every screen closes and
 * each player's front end continues into Single Event (an injected A while the cursor is on it). The host
 * picks the event, the others' picks are replaced by it when their game loads it (lan_game_entering). A
 * banner says what happens meanwhile. Back in the menus after an event, the lobby opens by itself with the
 * results and the session scoreboard.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "lan_game.h"
#include "lobby_ui.h"
#include "menu.h"
#include "../ui/overlay.h"

extern ptrdiff_t g_xbox_mem_offset;
extern int ssx3_in_game(void);          /* recomp_manual.c */

#define M8(va) (*(volatile uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* character names, filled by the game at boot (0x4DC00: 0x345A28 + character * 0x88, ASCII) */
#define CHAR_NAMES 0x00345A28u
#define CHAR_STRIDE 0x88u

/* SSX 3's front-end colours (sampled from its menus, 0xRRGGBBAA) */
#define C_PANEL   0x2C6C9CE0u           /* popup panel, translucent steel blue */
#define C_STRIP   0x1D4D78E8u           /* darker strip (column heads, banner) */
#define C_SELF    0x4A8CBCC0u           /* our own row */
#define C_BAR     0x9A3E01FFu           /* selection bar */
#define C_TEXT    0xFFFFFFFFu
#define C_DIM     0xC8DCEBFFu
#define C_GREY    0x8FAAC0FFu
#define C_DASH    0x47667AFFu
#define C_ORANGE  0xEE7A1EFFu
#define C_GOOD    0x9BE38FFFu
#define C_BAD     0xFF9C8CFFu
#define C_BTN_A   0x3EA94AFFu
#define C_BTN_B   0xB8231AFFu
#define C_BTN_X   0x2F6BD0FFu
#define C_BTN_Y   0xE0AE1CFFu

#define C_FRAME   0xE36F1CFFu           /* the orange of the big "3" frame */
#define C_INK     0x1A2A36FFu           /* dark text on the light background (button prompts) */

#define T_TITLE   42.0f                 /* text sizes: units per line on the 720-unit canvas */
#define T_ITEM    32.0f
#define T_SMALL   26.0f
#define T_TINY    22.0f

/* every screen: 4:3 area of 960 x 720 units (the game's 640 x 480 times 1.5), centred */
#define PANEL_Y   128.0f
#define PANEL_B   584.0f

enum { S_HIDDEN, S_MENU, S_INPUT, S_CONNECT, S_LOBBY, S_KICK };
enum { IN_ADDRESS, IN_NAME, IN_CHAT };

static int          s_inited, s_screen = S_HIDDEN, s_back = S_MENU, s_input_kind;
static volatile int s_want_open;
static int          s_cursor;
static char         s_name[LAN_NAME_MAX + 1];   /* shown: the typed name, else the save's */
static int          s_name_typed;
static char         s_edit[LAN_CHAT_MAX + 1];
static int          s_spectate;
static int          s_quick;
static lan_browser *s_browser;
static uint32_t     s_next_query;
static lan_found    s_found[8];
static int          s_nfound;
static lan_ui_view  s_v;
static uint8_t      s_prev_state = 0xFF, s_results_round = 0xFF;
static uint32_t     s_banner_until;
static char         s_banner[96];
static float        X0;                 /* left edge of the 960-unit wide 4:3 area */

static const char *const k_quick[] = { "GG!", "Ready when you are.", "One more?", "Wait for me!", "Nice run!" };
#define NQUICK ((int)(sizeof k_quick / sizeof k_quick[0]))

/* ---- drawing helpers ----------------------------------------------------- */

static void dashes(float x, float y, float w)
{
    float d;
    for (d = 0; d + 9 <= w; d += 15)
        ovl_rect(x + d, y, 9, 3, C_DASH);
}

/* the light, icy background of the game's menus (hides the menu under the screen) and its orange frame */
static void backdrop(void)
{
    float w = ovl_canvas_w(), y;
    int i;
    for (i = 0; i < 12; i++) {                      /* top #E6F0F7 .. bottom #9FC4DE */
        float t = (float)i / 11.0f;
        uint32_t r = (uint32_t)(0xE6 + (0x9F - 0xE6) * t), g = (uint32_t)(0xF0 + (0xC4 - 0xF0) * t),
                 b = (uint32_t)(0xF7 + (0xDE - 0xF7) * t);
        y = OVL_CANVAS_H * (float)i / 12.0f;
        ovl_rect(0, y, w, OVL_CANVAS_H / 12.0f + 1, r << 24 | g << 16 | b << 8 | 0xF2u);
    }
    ovl_rect(X0 + 6, 120, 16, 560, C_FRAME);        /* the frame's left side and foot */
    ovl_rect(X0 + 6, 668, 700, 16, C_FRAME);
}

/* "• Title" on an orange tab, like the top of the game's "3" */
static void title(const char *text)
{
    float q[8] = { X0 + 6, 34, X0 + 480, 34, X0 + 446, 100, X0 + 6, 100 };
    backdrop();
    ovl_poly4(q, C_FRAME);
    ovl_rect(X0 + 44, 64, 7, 7, C_TEXT);             /* the "•" before every SSX 3 heading */
    ovl_text(X0 + 58, 44, T_TITLE, C_TEXT, text, 380);
    ovl_rect(X0 + 490, 82, 440, 2, 0xFFFFFFC0u);    /* the thin line out to the logo */
    dashes(X0 + 490, 112, 440);
}

static void panel(float x, float y, float w, float h)
{
    ovl_rect(x, y, w, h, C_PANEL);
}

/* the selection bar: a slanted right end, as in the game's menus and popups */
static void bar(float x, float y, float w, float h)
{
    float q[8] = { x, y, x + w, y, x + w - h * 0.6f, y + h, x, y + h };
    ovl_poly4(q, C_BAR);
}

static void button(float x, float y, uint32_t color, const char *letter, const char *label)
{
    char l[2] = { letter[0], 0 };
    ovl_disc(x + 12, y + 13, 12, 0x00000050u);
    ovl_disc(x + 11, y + 12, 11, color);
    ovl_text(x + 11 - ovl_text_width(T_TINY - 4, l) / 2, y + 2, T_TINY - 4, C_TEXT, l, 0);
    ovl_text(x + 28, y, T_SMALL, C_INK, label, 0);
}

/* the prompts at the bottom right, stacked, dark text on the light background like the game's */
static void legend(const char *a, const char *b, const char *x, const char *y)
{
    float cx = X0 + 652, cy = 596;
    if (a) { button(cx, cy, C_BTN_A, "A", a); cy += 26; }
    if (b) { button(cx, cy, C_BTN_B, "B", b); cy += 26; }
    if (x) { button(cx, cy, C_BTN_X, "X", x); cy += 26; }
    if (y) button(cx, cy, C_BTN_Y, "Y", y);
}

/* what the selected row does: white on a panel strip at the bottom left, as in the game */
static void description(const char *text)
{
    panel(X0 + 30, 594, 600, 62);
    ovl_text(X0 + 46, 611, T_TINY, C_TEXT, text, 572);
}

static void text_right(float right, float y, float size, uint32_t c, const char *s)
{
    ovl_text(right - ovl_text_width(size, s), y, size, c, s, 0);
}

static const char *character_name(int c, char *buf, int cap)
{
    int i;
    if (c < 0 || c > 9)
        return "-";
    for (i = 0; i < cap - 1; i++) {
        uint8_t ch = M8(CHAR_NAMES + (uint32_t)c * CHAR_STRIDE + (uint32_t)i);
        if (ch < 32 || ch > 126)
            break;
        buf[i] = (char)ch;
    }
    buf[i] = 0;
    return i ? buf : "-";
}

static const char *event_name(int e, char *buf, int cap)
{
    switch (e) {
    case 0:  return "Snow Jam";
    case 1:  return "Metro-City";
    case 14: return "Happiness";
    default: snprintf(buf, (size_t)cap, "event %d", e); return buf;
    }
}

static void race_time(uint32_t ms, char *buf, int cap)
{
    snprintf(buf, (size_t)cap, "%u:%02u.%02u", ms / 60000u, ms / 1000u % 60u, ms % 1000u / 10u);
}

/* ---- screens: state ------------------------------------------------------ */

static void show(int screen)
{
    s_screen = screen;
    s_cursor = 0;
    ovl_text_mode(screen == S_INPUT);
    ovl_capture(screen != S_HIDDEN);
}

static void input_start(int kind, const char *initial, int back)
{
    s_input_kind = kind;
    s_back = back;
    snprintf(s_edit, sizeof s_edit, "%s", initial);
    show(S_INPUT);
}

static void browse(void)
{
    uint32_t now = ovl_time_ms();
    if (!s_browser) {
        if (lan_net_init() != 0 || (s_browser = lan_browse_open()) == NULL)
            return;
        s_next_query = now;
    }
    if ((int32_t)(now - s_next_query) >= 0) {
        lan_browse_query(s_browser);
        s_next_query = now + 1500;
    }
    s_nfound = lan_browse_poll(s_browser, s_found, 8);
}

static void browse_stop(void)
{
    if (s_browser) {
        lan_browse_close(s_browser);
        s_browser = NULL;
    }
    s_nfound = 0;
}

static void join(const char *addr, uint16_t port)
{
    browse_stop();
    if (lan_game_open(0, addr, port, s_name_typed ? s_name : NULL, s_spectate))
        show(S_CONNECT);
}

/* ---- menu ---------------------------------------------------------------- */

enum { M_HOST, M_FOUND, M_ADDRESS, M_NAME, M_SIDE, M_BACK };

static int menu_rows(int *kind, int *arg)
{
    int n = 0, i;
    kind[n] = M_HOST; arg[n++] = 0;
    for (i = 0; i < s_nfound && i < 5; i++) { kind[n] = M_FOUND; arg[n++] = i; }
    kind[n] = M_ADDRESS; arg[n++] = 0;
    kind[n] = M_NAME; arg[n++] = 0;
    kind[n] = M_SIDE; arg[n++] = 0;
    kind[n] = M_BACK; arg[n++] = 0;
    return n;
}

static void menu_screen(void)
{
    int kind[16], arg[16], n, b, i;
    float y;
    char line[96];
    static const char *const desc[] = {
        "Open a lobby on this PC. Players on your network can join it.",
        "Join this lobby.",
        "Type the host's address (a name or an IP, \"host:port\" for another port).",
        "Your name in the lobby. Press A to type a new one.",
        "Spectators watch a player's event instead of riding in it.",
        "Back to the main menu.",
    };

    browse();
    n = menu_rows(kind, arg);
    while ((b = ovl_next_button()) != 0) {
        if (b == OVL_UP)   s_cursor = (s_cursor + n - 1) % n;
        if (b == OVL_DOWN) s_cursor = (s_cursor + 1) % n;
        if (s_cursor >= n) s_cursor = n - 1;
        if ((b == OVL_LEFT || b == OVL_RIGHT) && kind[s_cursor] == M_SIDE)
            s_spectate = !s_spectate;
        if (b == OVL_B) {
            browse_stop();
            show(S_HIDDEN);
            return;
        }
        if (b != OVL_A)
            continue;
        switch (kind[s_cursor]) {
        case M_HOST:
            browse_stop();
            if (lan_game_open(1, "", 0, s_name_typed ? s_name : NULL, 0))
                show(S_LOBBY);
            return;
        case M_FOUND:
            join(s_found[arg[s_cursor]].address, s_found[arg[s_cursor]].port);
            return;
        case M_ADDRESS: input_start(IN_ADDRESS, "", S_MENU); return;
        case M_NAME:    input_start(IN_NAME, s_name, S_MENU); return;
        case M_SIDE:    s_spectate = !s_spectate; break;
        case M_BACK:    browse_stop(); show(S_HIDDEN); return;
        }
    }

    title("LAN Game");
    panel(X0 + 30, PANEL_Y, 900, PANEL_B - PANEL_Y);
    y = PANEL_Y + 16;
    for (i = 0; i < n; i++) {
        uint32_t c = C_TEXT;
        if (kind[i] == M_ADDRESS && !s_nfound) {         /* the search, where found lobbies would be */
            ovl_text(X0 + 70, y + 4, T_SMALL, C_GREY, "Looking for games on your network...", 0);
            y += 40;
        }
        if (i == s_cursor)
            bar(X0 + 30, y - 3, 660, 40);
        switch (kind[i]) {
        case M_HOST:
            ovl_text(X0 + 70, y, T_ITEM, c, "Host a game", 0);
            break;
        case M_FOUND: {
            const lan_found *f = &s_found[arg[i]];
            snprintf(line, sizeof line, "Join %s", f->lobby_name);
            ovl_text(X0 + 70, y, T_ITEM, c, line, 560);
            snprintf(line, sizeof line, "%u/%u%s", f->players, f->max_players,
                     f->state == LAN_STATE_LOBBY ? "" : f->state == LAN_STATE_SELECT ? " picking" : " racing");
            text_right(X0 + 900, y + 6, T_SMALL, i == s_cursor ? C_TEXT : C_DIM, line);
            break;
        }
        case M_ADDRESS:
            ovl_text(X0 + 70, y, T_ITEM, c, "Join by address", 0);
            break;
        case M_NAME:
            ovl_text(X0 + 70, y, T_ITEM, c, "Name", 0);
            ovl_text(X0 + 330, y, T_ITEM, i == s_cursor ? C_TEXT : C_DIM, s_name, 0);
            break;
        case M_SIDE:
            ovl_text(X0 + 70, y, T_ITEM, c, "Join as", 0);
            ovl_text(X0 + 330, y, T_ITEM, i == s_cursor ? C_TEXT : C_DIM, s_spectate ? "< Spectator >" : "< Rider >", 0);
            break;
        case M_BACK:
            ovl_text(X0 + 70, y, T_ITEM, c, "Back", 0);
            break;
        }
        y += 40;
    }
    description(desc[kind[s_cursor < n ? s_cursor : 0]]);
    legend("Select", "Back", NULL, NULL);
}

/* ---- text input ---------------------------------------------------------- */

static void input_screen(void)
{
    char typed[64];
    int b, n, cap = s_input_kind == IN_NAME ? LAN_NAME_MAX : s_input_kind == IN_ADDRESS ? 63 : LAN_CHAT_MAX;
    static const char *const heads[] = { "Join by address", "Your name", "Chat" };
    static const char *const prompts[] = {
        "Type the host's address and press Enter.",
        "Type your name and press Enter.",
        "Type a message and press Enter.",
    };

    n = ovl_take_text(typed, sizeof typed);
    if (n && (int)strlen(s_edit) + n <= cap)
        strncat(s_edit, typed, (size_t)cap - strlen(s_edit));
    while ((b = ovl_next_button()) != 0) {
        if (b == OVL_BACKSPACE && s_edit[0])
            s_edit[strlen(s_edit) - 1] = 0;
        if (b == OVL_B) {
            show(s_back);
            return;
        }
        if (b != OVL_A)
            continue;
        if (s_input_kind == IN_ADDRESS && s_edit[0]) {
            char addr[64], *colon;
            uint16_t port = 0;
            snprintf(addr, sizeof addr, "%s", s_edit);
            if ((colon = strrchr(addr, ':')) != NULL) {
                *colon = 0;
                port = (uint16_t)atoi(colon + 1);
            }
            join(addr, port);
            return;
        }
        if (s_input_kind == IN_NAME && s_edit[0]) {
            snprintf(s_name, sizeof s_name, "%s", s_edit);
            s_name_typed = 1;
        }
        if (s_input_kind == IN_CHAT)
            lan_game_ui_chat(s_edit);
        show(s_back);
        return;
    }

    if (s_back == S_LOBBY)
        title(s_v.lobby.rules.lobby_name);
    else
        title("LAN Game");
    panel(X0 + 30, 220, 900, 200);
    ovl_text(X0 + 70, 240, T_ITEM, C_TEXT, heads[s_input_kind], 0);
    ovl_rect(X0 + 60, 298, 840, 52, 0x123A5CE0u);
    {
        float w = ovl_text(X0 + 84, 308, T_ITEM, C_TEXT, s_edit, 790);
        if ((ovl_time_ms() / 450u) & 1u)
            ovl_rect(X0 + 86 + w, 312, 4, 32, C_ORANGE);
    }
    ovl_text(X0 + 70, 368, T_SMALL, C_DIM, prompts[s_input_kind], 0);
    legend("OK (Enter)", "Cancel (Esc)", NULL, NULL);
}

/* ---- connecting ---------------------------------------------------------- */

static void connect_screen(void)
{
    int b;
    if (s_v.joined) {
        show(S_LOBBY);
        return;
    }
    while ((b = ovl_next_button()) != 0)
        if (b == OVL_B || b == OVL_A) {
            lan_game_close();
            show(S_MENU);
            return;
        }
    title("LAN Game");
    panel(X0 + 30, 220, 900, 140);
    ovl_text(X0 + 70, 250, T_ITEM, C_TEXT, s_v.status[0] ? s_v.status : "Connecting...", 820);
    legend(NULL, "Cancel", NULL, NULL);
}

/* ---- lobby --------------------------------------------------------------- */

enum { L_READY, L_START, L_RULES, L_CHAT, L_QUICK, L_KICK, L_LEAVE };

static int lobby_rows(int *kind)
{
    int n = 0;
    if (!s_v.spectator)
        kind[n++] = L_READY;
    if (s_v.host) {
        kind[n++] = L_START;
        kind[n++] = L_RULES;
    }
    kind[n++] = L_CHAT;
    kind[n++] = L_QUICK;
    if (s_v.host)
        kind[n++] = L_KICK;
    kind[n++] = L_LEAVE;
    return n;
}

/* the players who still have to get ready before the host can start; returns how many */
static int not_ready(char *names, int cap)
{
    int i, n = 0;
    names[0] = 0;
    for (i = 0; i < LAN_MAX_MEMBERS; i++) {
        const lan_member *m = &s_v.lobby.members[i];
        if (!m->used || !m->connected || m->profile.spectator || m->ready)
            continue;
        if (n++)
            strncat(names, ", ", (size_t)cap - strlen(names) - 1);
        strncat(names, m->profile.name, (size_t)cap - strlen(names) - 1);
    }
    return n;
}

static const char *member_status(const lan_member *m, char *buf, int cap)
{
    const lan_lobby *l = &s_v.lobby;
    if (!m->connected)
        return "Reconnecting";
    if (m->profile.spectator)
        return "Watching";
    switch (l->state) {
    case LAN_STATE_LOBBY:     return m->ready ? "Ready" : "Not ready";
    case LAN_STATE_SELECT:    return m->ready ? "At the start" : m->loading ? "Loading" : "Picking";
    case LAN_STATE_COUNTDOWN: return "Starting";
    case LAN_STATE_RUNNING:
        if (!m->finished)
            return "Racing";
        race_time(m->finish_ms, buf, cap);
        return buf;
    case LAN_STATE_RESULTS: {
        char t[24];
        if (!m->place)
            return "-";
        race_time(m->finish_ms, t, sizeof t);
        snprintf(buf, (size_t)cap, "%u. %s", m->place, t);
        return buf;
    }
    }
    return "";
}

static void lobby_screen(void)
{
    int kind[8], n, b, i, others;
    float y;
    char line[160], buf[48], waiting[96];
    const lan_lobby *l = &s_v.lobby;

    if (!s_v.open) {
        show(S_MENU);
        return;
    }
    if (!s_v.joined) {                                  /* the host went away: connect shows why */
        show(S_CONNECT);
        return;
    }
    n = lobby_rows(kind);
    if (s_cursor >= n)
        s_cursor = n - 1;
    others = not_ready(waiting, sizeof waiting);
    while ((b = ovl_next_button()) != 0) {
        if (b == OVL_UP)   s_cursor = (s_cursor + n - 1) % n;
        if (b == OVL_DOWN) s_cursor = (s_cursor + 1) % n;
        if (kind[s_cursor] == L_QUICK && (b == OVL_LEFT || b == OVL_RIGHT))
            s_quick = (s_quick + (b == OVL_LEFT ? NQUICK - 1 : 1)) % NQUICK;
        if (kind[s_cursor] == L_RULES && (b == OVL_LEFT || b == OVL_RIGHT || b == OVL_A)) {
            lan_rules r = l->rules;
            r.leveled = !r.leveled;
            lan_game_ui_rules(&r);
            continue;
        }
        if (b == OVL_Y) {                               /* quick chat from anywhere on the screen */
            lan_game_ui_chat(k_quick[s_quick]);
            continue;
        }
        if (b == OVL_B) {                               /* close; the session stays (F5 / Back + Y) */
            show(S_HIDDEN);
            return;
        }
        if (b != OVL_A)
            continue;
        switch (kind[s_cursor]) {
        case L_READY:
            lan_game_ui_ready(!l->members[s_v.local_id].ready);
            break;
        case L_START:
            if (lan_game_ui_start() == 2)
                s_cursor = 0;
            break;
        case L_CHAT:  input_start(IN_CHAT, "", S_LOBBY); return;
        case L_QUICK: lan_game_ui_chat(k_quick[s_quick]); break;
        case L_KICK:  show(S_KICK); return;
        case L_LEAVE:
            lan_game_close();
            show(S_MENU);
            return;
        }
    }

    title(l->rules.lobby_name);

    /* players */
    panel(X0 + 30, PANEL_Y, 600, 284);
    ovl_rect(X0 + 30, PANEL_Y, 600, 28, C_STRIP);
    ovl_text(X0 + 50, PANEL_Y + 3, T_TINY, C_DIM, "Player", 0);
    ovl_text(X0 + 222, PANEL_Y + 3, T_TINY, C_DIM, "Rider", 0);
    ovl_text(X0 + 332, PANEL_Y + 3, T_TINY, C_DIM, "Status", 0);
    text_right(X0 + 560, PANEL_Y + 3, T_TINY, C_DIM, "Pts");
    text_right(X0 + 620, PANEL_Y + 3, T_TINY, C_DIM, "Ping");
    y = PANEL_Y + 34;
    for (i = 0; i < LAN_MAX_MEMBERS && y < PANEL_Y + 270; i++) {
        const lan_member *m = &l->members[i];
        const char *st;
        uint32_t c;
        if (!m->used)
            continue;
        if (i == s_v.local_id)
            ovl_rect(X0 + 30, y - 3, 600, 29, C_SELF);
        if (i == 0)
            ovl_disc(X0 + 40, y + 11, 5, C_ORANGE);     /* the host */
        ovl_text(X0 + 50, y, T_TINY, m->connected ? C_TEXT : C_GREY, m->profile.name, 165);
        ovl_text(X0 + 222, y, T_TINY, C_DIM,
                 m->profile.spectator ? "-" : character_name(m->profile.character, buf, sizeof buf), 105);
        st = member_status(m, line, sizeof line);
        c = !strcmp(st, "Ready") || !strcmp(st, "At the start") ? C_GOOD
          : !strcmp(st, "Not ready") || !strcmp(st, "Reconnecting") ? C_BAD : C_TEXT;
        ovl_text(X0 + 332, y, T_TINY, c, st, 180);
        snprintf(buf, sizeof buf, "%u", m->score);
        text_right(X0 + 560, y, T_TINY, C_TEXT, buf);
        snprintf(buf, sizeof buf, "%u", m->ping_ms);
        text_right(X0 + 620, y, T_TINY, C_DIM, i == 0 ? "-" : buf);
        y += 29;
    }

    /* chat */
    panel(X0 + 30, PANEL_Y + 292, 600, PANEL_B - PANEL_Y - 292);
    for (i = 0; i < s_v.nchat && i < 6; i++) {
        const char *ln = s_v.chat[s_v.nchat - 1 - i];
        ovl_text(X0 + 46, PANEL_B - 32 - 25 * (float)i, T_TINY, strchr(ln, ':') ? C_TEXT : C_DIM, ln, 570);
    }

    /* actions */
    panel(X0 + 640, PANEL_Y, 290, PANEL_B - PANEL_Y);
    ovl_rect(X0 + 640, PANEL_Y, 290, 28, C_STRIP);
    ovl_text(X0 + 656, PANEL_Y + 3, T_TINY, C_DIM, l->rules.mode == LAN_MODE_FREEROAM ? "Conquer the Mountain" : "Race", 0);
    y = PANEL_Y + 40;
    for (i = 0; i < n; i++) {
        uint32_t c = C_TEXT;
        const char *label = "";
        if (i == s_cursor)
            bar(X0 + 640, y - 3, 290, 38);
        switch (kind[i]) {
        case L_READY: label = l->members[s_v.local_id].ready ? "Ready" : "Get ready"; break;
        case L_START:
            label = l->state == LAN_STATE_RESULTS ? "New round" : "Start event";
            if (l->state == LAN_STATE_LOBBY && others)
                c = C_GREY;
            break;
        case L_RULES: label = l->rules.leveled ? "< Leveled riders >" : "< Level 1 riders >"; break;
        case L_CHAT:  label = "Chat"; break;
        case L_QUICK: snprintf(line, sizeof line, "< %s >", k_quick[s_quick]); label = line; break;
        case L_KICK:  label = "Remove a player"; break;
        case L_LEAVE: label = s_v.host ? "Close lobby" : "Leave lobby"; break;
        }
        ovl_text(X0 + 660, y, T_SMALL + 2, c, label, 260);
        y += 40;
    }

    /* what the selected action does */
    switch (kind[s_cursor]) {
    case L_READY: description("Tell the host you are set for the next event."); break;
    case L_START:
        if (l->state == LAN_STATE_RESULTS)
            description("Back to the lobby for the next event. The points stay.");
        else if (others) {
            snprintf(line, sizeof line, "Waiting for: %s", waiting);
            description(line);
        } else
            description("Everyone picks a rider in Single Event; your event is the one everyone rides.");
        break;
    case L_RULES:
        description(l->rules.leveled ? "Riders bring their leveled stats from their saves."
                                     : "Everyone rides with level 1 stats (one bar everywhere).");
        break;
    case L_CHAT:  description("Type a message (keyboard)."); break;
    case L_QUICK: description("Left / right picks a message, A sends it. Y sends it from anywhere here."); break;
    case L_KICK:  description("Remove a player from the lobby. They cannot come back this session."); break;
    case L_LEAVE:
        description(s_v.host ? "Close the lobby for everyone." : "Leave the lobby.");
        break;
    }
    legend("Select", "Close", NULL, "Quick chat");
}

static void kick_screen(void)
{
    int ids[LAN_MAX_MEMBERS], n = 0, i, b;
    float y;
    for (i = 1; i < LAN_MAX_MEMBERS; i++)
        if (s_v.lobby.members[i].used)
            ids[n++] = i;
    if (!s_v.open || !s_v.host) {
        show(S_LOBBY);
        return;
    }
    while ((b = ovl_next_button()) != 0) {
        if (b == OVL_UP && n)   s_cursor = (s_cursor + n - 1) % n;
        if (b == OVL_DOWN && n) s_cursor = (s_cursor + 1) % n;
        if (b == OVL_B) { show(S_LOBBY); return; }
        if (b == OVL_A && n) {
            lan_game_ui_kick(ids[s_cursor < n ? s_cursor : 0]);
            show(S_LOBBY);
            return;
        }
    }
    title("Remove a player");
    panel(X0 + 30, PANEL_Y, 600, PANEL_B - PANEL_Y);
    y = PANEL_Y + 16;
    if (!n)
        ovl_text(X0 + 70, y, T_SMALL, C_GREY, "Nobody else is here.", 0);
    for (i = 0; i < n; i++) {
        if (i == s_cursor)
            bar(X0 + 30, y - 3, 600, 38);
        ovl_text(X0 + 70, y, T_ITEM - 4, C_TEXT, s_v.lobby.members[ids[i]].profile.name, 480);
        y += 42;
    }
    legend("Remove", "Back", NULL, NULL);
}

/* ---- banner (no input taken) --------------------------------------------- */

static void banner(const char *text)
{
    float w = ovl_text_width(T_SMALL, text) + 60;
    float q[8] = { X0 + 40, 14, X0 + 40 + w + 20, 14, X0 + 40 + w, 52, X0 + 40, 52 };
    ovl_poly4(q, C_STRIP);
    ovl_rect(X0 + 40, 14, 8, 38, C_ORANGE);
    ovl_text(X0 + 64, 19, T_SMALL, C_TEXT, text, 0);
}

static void hidden(void)
{
    const lan_lobby *l = &s_v.lobby;
    char buf[48], line[160];
    int in_game;
    if (!s_v.open)
        return;
    in_game = ssx3_in_game();
    if (s_v.wait[0]) {
        banner(s_v.wait);
        return;
    }
    if (!in_game && l->state == LAN_STATE_SELECT) {
        if (s_v.host)
            snprintf(line, sizeof line, "LAN: pick your rider and the event - everyone rides yours.");
        else if (l->rules.event != LAN_EVENT_NONE)
            snprintf(line, sizeof line, "LAN: pick your rider - the host picked %s.",
                     event_name(l->rules.event, buf, sizeof buf));
        else
            snprintf(line, sizeof line, "LAN: pick your rider - the host picks the event.");
        banner(line);
        return;
    }
    if (s_banner[0] && (int32_t)(ovl_time_ms() - s_banner_until) < 0)
        banner(s_banner);
}

/* ---- the overlay's draw callback ----------------------------------------- */

static void draw(void)
{
    float w = ovl_canvas_w();
    X0 = w > 960 ? (w - 960) / 2 : 0;
    lan_game_view(&s_v);

    if (s_want_open) {
        s_want_open = 0;
        if (s_screen == S_HIDDEN)
            show(s_v.open ? (s_v.joined ? S_LOBBY : S_CONNECT) : S_MENU);
    }
    if (!s_name_typed)
        lan_game_save_name(s_name, sizeof s_name);

    /* the host started: every screen closes and the front end goes on into Single Event */
    if (s_v.open && s_v.lobby.state != s_prev_state) {
        if (s_v.lobby.state == LAN_STATE_SELECT && s_prev_state != 0xFF) {
            if (s_screen != S_HIDDEN)
                show(S_HIDDEN);
            if (!ssx3_in_game()) {
                if (lan_menu_single_focused())
                    ovl_inject_a();
                else {
                    snprintf(s_banner, sizeof s_banner, "LAN: choose Single Event.");
                    s_banner_until = ovl_time_ms() + 8000;
                }
            }
        }
        s_prev_state = s_v.lobby.state;
    }
    if (!s_v.open)
        s_prev_state = 0xFF;
    /* back in the menus after an event: the results and the scoreboard */
    if (s_v.open && s_v.lobby.state == LAN_STATE_RESULTS && s_results_round != s_v.lobby.round
        && !ssx3_in_game() && s_screen == S_HIDDEN) {
        s_results_round = s_v.lobby.round;
        show(S_LOBBY);
    }

    switch (s_screen) {
    case S_HIDDEN:  hidden(); break;
    case S_MENU:    menu_screen(); break;
    case S_INPUT:   input_screen(); break;
    case S_CONNECT: connect_screen(); break;
    case S_LOBBY:   lobby_screen(); break;
    case S_KICK:    kick_screen(); break;
    }
}

static void hotkey(void)
{
    if (s_screen == S_HIDDEN)
        s_want_open = 1;
}

void lobby_ui_init(void)
{
    if (s_inited)
        return;
    s_inited = 1;
    ovl_set_screen(draw);
    ovl_set_hotkey(hotkey);
}

void lobby_ui_open(void)
{
    lobby_ui_init();
    s_want_open = 1;
}
