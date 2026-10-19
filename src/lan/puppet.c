/*
 * LAN multiplayer – puppet riders (phase 0, spike 2).
 *
 * A puppet is a race rider whose pose comes from outside (later: the network) instead of from its own
 * brain. The rider manager ([[[0x369534]+0x74]+0x0C], vtable 0x28216C) updates all riders in fixed
 * stages (vtable[2] 0x11E90). For a puppet, recomp_manual.c skips the brain stages (0x1F3F0 input,
 * 0x1F420 control-mode update, 0x1F450 post-update) and calls lan_puppet_pose() at the top of 0x24D50,
 * which then applies the pose at rider+0x180 to the model and board (0x246C0); animation (0x1F140)
 * keeps running. Do not use the rider's own "no pose" flag (byte +0xA84): it also stops animation and
 * posing, so only the board follows. Addresses: docs/notes.md (2026-10-19, LAN phase 0).
 *
 * Test mode (SSX3_PUPPET_TEST=1): the first AI rider (slot 1) rides beside the player, copying the
 * player's pose shifted sideways and ahead along the direction of travel.
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "puppet.h"

extern ptrdiff_t g_xbox_mem_offset;
int ssx3_in_game(void);                                 /* recomp_manual.c */

#define M32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define MF(va)  (*(volatile float *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* Rider part fields */
#define RIDER_POSE   0x180      /* position x, y, z, 1 (z up), then orientation quaternion x, y, z, w */

static int s_mode = -1;         /* SSX3_PUPPET_TEST: 0 off, 1 beside the player */

static uint32_t rider_manager(void)
{
    uint32_t game = M32(0x00369534u), world;

    if (game < 0x10000u)
        return 0;
    world = M32(game + 0x74);
    if (world < 0x10000u)
        return 0;
    return M32(world + 0x0C);
}

int lan_puppet_is(uint32_t rider)
{
    uint32_t mgr;

    if (s_mode < 0) {
        const char *e = getenv("SSX3_PUPPET_TEST");
        s_mode = e ? atoi(e) : 0;
        if (s_mode > 0)
            fprintf(stderr, "  [PUPPET] test mode %d: AI slot 1 follows the player\n", s_mode);
    }
    if (s_mode <= 0 || !ssx3_in_game())
        return 0;
    mgr = rider_manager();
    if (mgr < 0x10000u || M32(mgr + 0x7C) < 1 || M32(mgr + 0x78) < 2)  /* a human in slot 0, a rider in 1 */
        return 0;
    return rider != 0 && M32(mgr + 0x28 + 4) == rider;
}

void lan_puppet_pose(uint32_t rider)
{
    static float last[3], dir[2] = { 1.0f, 0.0f };
    static int have_last;
    uint32_t mgr = rider_manager(), src;
    float p[3], vx, vy, len;
    int i;

    src = M32(mgr + 0x28);                              /* the player's rider part (slot 0) */
    if (src < 0x10000u)
        return;
    for (i = 0; i < 3; i++)
        p[i] = MF(src + RIDER_POSE + 4 * i);
    if (have_last) {                                    /* direction of travel, smoothed */
        vx = p[0] - last[0];
        vy = p[1] - last[1];
        len = sqrtf(vx * vx + vy * vy);
        if (len > 1.0f && len < 2000.0f) {
            dir[0] = 0.8f * dir[0] + 0.2f * vx / len;
            dir[1] = 0.8f * dir[1] + 0.2f * vy / len;
            len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
            dir[0] /= len;
            dir[1] /= len;
        }
    }
    last[0] = p[0]; last[1] = p[1]; last[2] = p[2];
    have_last = 1;

    /* 220 units (~3 m) to the side, 300 ahead, 20 up so it does not sink into the slope */
    MF(rider + RIDER_POSE + 0) = p[0] + dir[0] * 300.0f - dir[1] * 220.0f;
    MF(rider + RIDER_POSE + 4) = p[1] + dir[1] * 300.0f + dir[0] * 220.0f;
    MF(rider + RIDER_POSE + 8) = p[2] + 20.0f;
    MF(rider + RIDER_POSE + 12) = 1.0f;
    for (i = 0; i < 4; i++)                             /* same orientation as the player */
        M32(rider + RIDER_POSE + 16 + 4 * i) = M32(src + RIDER_POSE + 16 + 4 * i);
}
