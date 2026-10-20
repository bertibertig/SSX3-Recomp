/* LAN multiplayer – the net core in the game (see lan_game.c). Rider = guest address of a rider part. */
#ifndef SSX3_LAN_GAME_H
#define SSX3_LAN_GAME_H

#include <stdint.h>

int      lan_game_active(void);                 /* 1: a LAN session runs (SSX3_LAN_HOST / SSX3_LAN_JOIN) */
int      lan_game_hold(void);                   /* countdown hold: 1 keep holding, 0 GO, -1 no session */
int      lan_game_puppet_member(uint32_t rider); /* lobby member driving this rider, -1: none */
int      lan_game_pose(uint32_t rider);         /* 1: the rider is a remote player's (pose written if known) */
void     lan_game_tick(uint32_t mgr);           /* each rider-manager update: send our pose */
uint32_t lan_game_finish(uint32_t slot, uint32_t frames);   /* a rider finished: the time to use */
void     lan_game_before_ai(uint32_t mgr);      /* the race creates its AI riders (0x13C60): remote looks in */
uint32_t lan_game_result_name(uint32_t slot);
int      lan_game_wait_end(void);
void     lan_game_after_finish(uint32_t slot);  /* after the mode handler took a finish: re-rank a late puppet */               /* 1: our humans are through, but remote players still ride */   /* results row of a remote player: guest VA of its name, or 0 */

#endif
