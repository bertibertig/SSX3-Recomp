/* LAN multiplayer – puppet riders (see puppet.c). Rider = guest address of a rider part. */
#ifndef SSX3_LAN_PUPPET_H
#define SSX3_LAN_PUPPET_H

#include <stdint.h>

int  lan_puppet_is(uint32_t rider);     /* 1: this rider is a puppet */
int  lan_puppet_skip_brain(uint32_t rider); /* 1: skip its brain stages (all but SSX3_PUPPET_KEEP_BRAIN) */
void lan_puppet_pose(uint32_t rider);   /* write the puppet's pose (rider+0x180) before it is applied */

#endif
