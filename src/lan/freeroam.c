/*
 * LAN multiplayer – riders in free roam (Conquer the Mountain; phase 0, spike 4).
 *
 * In Conquer the Mountain (race setup 0x367F90 mode byte = 4) the rider manager holds only the player: 1 slot,
 * no AI (0x367F84 = 0). The game spawns AI riders at run time for the events on the mountain (in-game state
 * 0xD54D0): when the manager is ready (0x11410) it clears old AI (0x11630, plus 0x11620 if more than one),
 * creates 0x367F84 ComputerRiders in the free slots (0x13EA0 -> 0x13C60; their characters come from the
 * per-slot table 0x367FDC + slot·0x20, byte +8), pumps loading (0x11360) until every slot is loaded (0x113A0),
 * then initialises them (0x13930) and switches AI updates on (mgr+0x15 = 1). All thiscall, ecx = manager.
 *
 * lan_freeroam_tick() runs that sequence from the manager update (0x11E90) on the game's own thread.
 * Test: SSX3_FREEROAM_SPAWN=<1..5> spawns that many riders once per level load; with SSX3_PUPPET_TEST=1 the
 * one in slot 1 rides next to the player (src/lan/puppet.c).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "freeroam.h"

extern ptrdiff_t g_xbox_mem_offset;
int ssx3_in_game(void);                                 /* recomp_manual.c */

#if defined(_MSC_VER)
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;
#else
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
#endif

extern void sub_00011410(void);     /* every AI slot ready -> al */
extern void sub_00011630(void);     /* clear AI */
extern void sub_00013EA0(void);     /* create AI up to 0x367F84 */
extern void sub_00011360(void);     /* loading step */
extern void sub_000113A0(void);     /* every slot loaded -> al */
extern void sub_00013930(void);     /* initialise the new riders */

#define M32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define M8(va)  (*(volatile uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

#define AI_WANTED    0x00367F84u
#define RACE_SETUP   0x00367F90u    /* byte 0 = mode (4 = Conquer the Mountain), 1 = players, 2 = event */
#define SLOT_TABLE   0x00367FDCu    /* + slot·0x20: +0 owner, +4 flags, +8 character */
#define SLOT_REMAP   0x00346400u

/* Call a guest thiscall function with ecx = `self` and no stack arguments; returns eax. */
static uint32_t guest_call(void (*fn)(void), uint32_t self)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ret;

    g_ecx = self;
    g_esp -= 4;
    M32(g_esp) = 0;                 /* return address: the callee pops it */
    fn();
    ret = g_eax;
    g_eax = eax; g_ecx = ecx; g_edx = edx;
    return ret;
}

static int s_want = -1;             /* SSX3_FREEROAM_SPAWN */
static uint32_t s_mgr;              /* manager the current spawn belongs to */
static int s_step;                  /* 0 wait, 1 loading, 2 done */

void lan_freeroam_tick(uint32_t mgr)
{
    uint32_t slot;

    if (s_want < 0) {
        const char *e = getenv("SSX3_FREEROAM_SPAWN");
        s_want = e ? atoi(e) : 0;
        if (s_want > 5)
            s_want = 5;             /* the manager has 6 rider slots */
        if (s_want > 0)
            fprintf(stderr, "  [FREEROAM] test: %d riders in Conquer the Mountain\n", s_want);
    }
    if (s_want <= 0 || !ssx3_in_game() || M8(RACE_SETUP) != 4u)
        return;
    if (mgr != s_mgr || M32(mgr + 0x78) < 1u) {         /* new level (or the player not yet there) */
        s_mgr = mgr;
        s_step = 0;
    }
    switch (s_step) {
    case 0:
        if (M32(mgr + 0x7C) < 1u || M32(mgr + 0x80) != 0u || M32(mgr + 0x78) != 1u)
            return;                 /* only the player: free roam, no event running */
        for (slot = 1; slot <= (uint32_t)s_want; slot++)  /* a different character in every slot */
            M8(SLOT_TABLE + M32(SLOT_REMAP + 4 * slot) * 0x20 + 8) = (uint8_t)((slot * 2 + 1) % 10);
        M32(AI_WANTED) = (uint32_t)s_want;
        if (!(guest_call(sub_00011410, mgr) & 0xFF))
            return;
        guest_call(sub_00011630, mgr);
        guest_call(sub_00013EA0, mgr);
        fprintf(stderr, "  [FREEROAM] created %u AI riders, slots %u, loading\n", M32(mgr + 0x80), M32(mgr + 0x78));
        s_step = 1;
        return;
    case 1:
        if (!(guest_call(sub_000113A0, mgr) & 0xFF)) {
            guest_call(sub_00011360, mgr);
            return;
        }
        guest_call(sub_00013930, mgr);
        M8(mgr + 0x15) = 1;
        fprintf(stderr, "  [FREEROAM] riders loaded and active\n");
        s_step = 2;
        return;
    default:
        /* The game clears the AI-update flag again in free roam (seen a few seconds after the spawn);
         * without it the manager update skips our riders entirely. */
        if (M32(mgr + 0x80) > 0u && !M8(mgr + 0x15)) {
            static int told;
            if (!told++)
                fprintf(stderr, "  [FREEROAM] AI updates were switched off again, switching them back on\n");
            M8(mgr + 0x15) = 1;
        }
        return;
    }
}
