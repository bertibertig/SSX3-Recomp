/* LAN multiplayer – front end (see menu.c). */
#ifndef SSX3_LAN_MENU_H
#define SSX3_LAN_MENU_H

#include <stdint.h>

/* main menu selection (0x9CC70): 1 if handled here (the original must not run) */
int lan_menu_select(uint32_t menu, uint32_t item, uint32_t event);

/* 1: pretend two pads are in while 0x9CAB0 decides whether Multi Play is shown */
int lan_menu_multiplay_pads(uint32_t menu);

#endif
