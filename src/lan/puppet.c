/*
 * LAN multiplayer – puppet riders (phase 0, spike 2).
 *
 * A puppet is a race rider whose pose comes from outside (later: the network) instead of from its own
 * brain. The rider manager ([[[0x369534]+0x74]+0x0C], vtable 0x28216C) updates all riders in fixed
 * stages (vtable[2] 0x11E90). For a puppet, recomp_manual.c skips the brain stages (0x1F3F0 input,
 * 0x1F420 control-mode update, 0x1F450 post-update) and calls lan_puppet_pose() at the top of 0x24D50,
 * which then applies the pose at rider+0x180 to the model and board (0x246C0); animation (0x1F140)
 * keeps running. Afterwards lan_puppet_after_pose() rebuilds the rider's culling box (rider+0x460 min,
 * +0x470 max; 0x20F10, normally called by the brain's mode updates, which do not run for a puppet and
 * not in every mode): the manager's render (vtable[3] 0x12360) draws a rider only if 0x22760 finds that
 * box in the view (result in rider+0xAC4), so a stale box hid the puppet in free roam. Do not use the
 * rider's own "no pose" flag (byte +0xA84): it also stops animation and
 * posing, so only the board follows. Addresses: docs/notes.md (2026-10-19, LAN phase 0).
 *
 * Test mode (SSX3_PUPPET_TEST=1): the first AI rider (slot 1) rides beside the player, copying the
 * player's pose shifted sideways and ahead along the direction of travel (SSX3_PUPPET_OFFSET=side,ahead,up).
 * SSX3_PUPPET_TEST=2: every AI slot is a puppet, fanned out around the player (slot n: alternately left and
 * right, 150 units further out and 80 further ahead per step) – for measuring the cost of visible riders.
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "puppet.h"
#include "lan_game.h"

extern ptrdiff_t g_xbox_mem_offset;

#if defined(_MSC_VER)
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;
#else
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
#endif

extern void sub_00020F10(void);                         /* rider culling box from its pose, thiscall(pos|0) */
int ssx3_in_game(void);                                 /* recomp_manual.c */

#define M32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define MF(va)  (*(volatile float *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* Rider part fields */
#define RIDER_POSE   0x180      /* position x, y, z, 1 (z up), then orientation quaternion x, y, z, w */

static int s_mode = -1;         /* SSX3_PUPPET_TEST: 0 off, 1 beside the player */
static int s_keep_brain;        /* SSX3_PUPPET_KEEP_BRAIN=1: run the brain stages anyway (diagnostics) */

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

int lan_puppet_skip_brain(uint32_t rider)
{
    if (lan_game_active())      /* LAN: the brain runs on the remote player's input (lan_game_input) */
        return 0;
    return lan_puppet_is(rider) && !s_keep_brain;
}

int lan_puppet_is(uint32_t rider)
{
    uint32_t mgr;

    if (lan_game_active())                              /* a LAN session: the remote players' riders */
        return ssx3_in_game() && lan_game_puppet_member(rider) >= 0;

    if (s_mode < 0) {
        const char *e = getenv("SSX3_PUPPET_TEST");
        s_mode = e ? atoi(e) : 0;
        s_keep_brain = getenv("SSX3_PUPPET_KEEP_BRAIN") != NULL;
        if (s_mode > 0)
            fprintf(stderr, "  [PUPPET] test mode %d: AI slot 1 follows the player\n", s_mode);
    }
    if (s_mode <= 0 || !ssx3_in_game())
        return 0;
    mgr = rider_manager();
    if (mgr < 0x10000u || M32(mgr + 0x7C) < 1 || M32(mgr + 0x78) < 2)  /* a human in slot 0, a rider in 1 */
        return 0;
    if (rider == 0)
        return 0;
    if (s_mode == 2) {                                  /* every rider but the player's */
        uint32_t n = M32(mgr + 0x78), i;
        for (i = 1; i < n && i < 6; i++)
            if (M32(mgr + 0x28 + 4 * i) == rider)
                return 1;
        return 0;
    }
    return M32(mgr + 0x28 + 4) == rider;
}

void lan_puppet_pose(uint32_t rider)
{
    static float last[3], dir[2] = { 1.0f, 0.0f };
    float side, ahead;
    uint32_t slot = 1;
    static int have_last, have_off;
    static float off[3] = { 220.0f, 300.0f, 20.0f };
    uint32_t mgr = rider_manager(), src;
    float p[3], vx, vy, len;
    int i;

    if (lan_game_active()) {
        lan_game_pose(rider);
        return;
    }

    src = M32(mgr + 0x28);                              /* the player's rider part (slot 0) */
    if (src < 0x10000u)
        return;
    for (i = 0; i < 3; i++)
        p[i] = MF(src + RIDER_POSE + 4 * i);
    while (slot < 6 && M32(mgr + 0x28 + 4 * slot) != rider)
        slot++;
    if (slot > 1 && have_last && p[0] == last[0] && p[1] == last[1])
        goto place;                                     /* direction already updated this frame */
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

    /* default: 220 units (~3 m) to the side, 300 ahead, 20 up so it does not sink into the slope;
     * SSX3_PUPPET_OFFSET=<side>,<ahead>,<up> */
    if (!have_off) {
        const char *e = getenv("SSX3_PUPPET_OFFSET");
        have_off = 1;
        if (e)
            sscanf(e, "%f,%f,%f", &off[0], &off[1], &off[2]);
    }
place:
    side = off[0];
    ahead = off[1];
    if (s_mode == 2) {                                  /* fan: 1 right, 2 left, 3 further right … */
        side = (slot & 1 ? 1.0f : -1.0f) * 150.0f * (float)((slot + 1) / 2);
        ahead = 80.0f * (float)slot;
    }
    MF(rider + RIDER_POSE + 0) = p[0] + dir[0] * ahead - dir[1] * side;
    MF(rider + RIDER_POSE + 4) = p[1] + dir[1] * ahead + dir[0] * side;
    MF(rider + RIDER_POSE + 8) = p[2] + off[2];
    MF(rider + RIDER_POSE + 12) = 1.0f;
    for (i = 0; i < 4; i++)                             /* same orientation as the player */
        M32(rider + RIDER_POSE + 16 + 4 * i) = M32(src + RIDER_POSE + 16 + 4 * i);
}

void lan_puppet_after_pose(uint32_t rider)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;

    g_ecx = rider;
    g_esp -= 8;
    M32(g_esp + 4) = 0;                                 /* pos = NULL: use the pose at rider+0x180 */
    M32(g_esp) = 0;                                     /* return address (the callee pops it and the arg) */
    sub_00020F10();
    g_eax = eax; g_ecx = ecx; g_edx = edx;
}
