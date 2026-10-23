/* LAN multiplayer – the net core in the game (see lan_game.c). Rider = guest address of a rider part. */
#ifndef SSX3_LAN_GAME_H
#define SSX3_LAN_GAME_H

#include <stdint.h>

#include "net.h"

int      lan_game_active(void);                 /* 1: a LAN session runs (SSX3_LAN_HOST / SSX3_LAN_JOIN) */
int      lan_game_hold(void);                   /* countdown hold: 1 keep holding, 0 GO, -1 no session */
int      lan_game_puppet_member(uint32_t rider); /* lobby member driving this rider, -1: none */
int      lan_game_pose(uint32_t rider);         /* 1: the rider is a remote player's (pose written if known) */
void     lan_game_tick(uint32_t mgr);           /* each rider-manager update: send our pose */
uint32_t lan_game_finish(uint32_t slot, uint32_t frames);   /* a rider finished: the time to use */
void     lan_game_before_players(uint32_t mgr); /* the race creates its human riders (0x139C0): spectator look */
void     lan_game_before_ai(uint32_t mgr);      /* the race creates its AI riders (0x13C60): remote looks in */
uint32_t lan_game_result_name(uint32_t slot);   /* results row of a remote player: guest VA of its name, or 0 */
int      lan_game_wait_end(void);               /* 1: our humans are through, but remote players still ride */
void     lan_game_after_finish(uint32_t slot);  /* after the mode handler took a finish: re-rank a late puppet */
void     lan_game_input(uint32_t brain, uint32_t input); /* 0x30970: record ours / give a puppet its player's */
void     lan_game_entering(void);               /* 0xE1B50: we leave the menus for an event (before it loads) */

/* ---- the lobby screen (lobby_ui.c) -------------------------------------------------------------- */
#define LAN_UI_CHAT 8                                   /* chat lines kept */
#define LAN_UI_LINE (LAN_NAME_MAX + LAN_CHAT_MAX + 4)

typedef struct {
    int       open;                                     /* a session exists */
    int       joined;                                   /* ... and we have a place in it */
    int       host, local_id, spectator;
    char      status[96];                               /* connection news ("Connecting ...", "Refused ...") */
    char      wait[96];                                 /* what our game waits for while it loads, or "" */
    lan_lobby lobby;
    char      chat[LAN_UI_CHAT][LAN_UI_LINE];
    int       nchat;
    uint32_t  now;                                      /* host clock */
} lan_ui_view;

int      lan_game_open(int host, const char *addr, uint16_t port, const char *name, int spectate);
void     lan_game_close(void);
void     lan_game_view(lan_ui_view *v);                 /* a copy of the session for drawing */
void     lan_game_ui_ready(int on);
void     lan_game_ui_chat(const char *text);
void     lan_game_ui_kick(int member);
/* host: lobby -> event select (1; 0 = not everyone ready), or after the results back to the lobby (2) */
int      lan_game_ui_start(void);
void     lan_game_ui_rules(const lan_rules *r);
void     lan_game_save_name(char *out, int cap);        /* the save's profile name */
int      lan_game_in_select(void);                      /* 1: the session's players pick the event now */

#endif
