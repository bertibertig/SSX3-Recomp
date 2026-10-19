/* LAN multiplayer – riders in free roam (see freeroam.c). */
#ifndef SSX3_LAN_FREEROAM_H
#define SSX3_LAN_FREEROAM_H

#include <stdint.h>

/* once per rider-manager update (0x11E90), before the riders are updated */
void lan_freeroam_tick(uint32_t mgr);

#endif
