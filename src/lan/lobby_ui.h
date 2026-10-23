/* LAN multiplayer – the lobby screen, drawn by the host overlay (see lobby_ui.c). */
#ifndef SSX3_LAN_LOBBY_UI_H
#define SSX3_LAN_LOBBY_UI_H

void lobby_ui_init(void);               /* once the front end runs (main menu hook); idempotent */
void lobby_ui_open(void);               /* show it: Single Event -> LAN Game, or the hotkey */

#endif
