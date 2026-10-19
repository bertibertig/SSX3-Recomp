/* LAN multiplayer – race flow (see race.c). */
#ifndef SSX3_LAN_RACE_H
#define SSX3_LAN_RACE_H

#include <stdint.h>

/* the race state machine (mgr+4) changes from `from` to `to` (0x155A0) */
void lan_race_state_change(uint32_t from, uint32_t to);

/* 1: keep the countdown frame counter (mgr+0x1C, value `counter`) where it is this frame */
int lan_race_hold_countdown(uint32_t counter);

/* a rider reached the finish (before the GameModeMan dispatcher 0xCD550 hands it to the mode): log it, set a puppet's time */
void lan_race_on_finish(uint32_t slot);

#endif
