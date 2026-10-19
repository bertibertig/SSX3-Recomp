/*
 * LAN multiplayer – race flow (phase 0, spike 3).
 *
 * The rider manager ([[[0x369534]+0x74]+0x0C]) runs the race state machine: mgr+4 = state (1 GameInit,
 * 2 Freeride, 3 PreRace, 4 Countdown, 5 Race, 6 EndRace, 7 Shutdown; set by 0x155A0 -> 0x15310), each with a
 * handler object (enter / update / leave) at mgr+0xA8 + 4·(state−1). Countdown: enter 0x15610 sets the
 * frame counter mgr+0x1C = 180, update 0x15620 counts it down and switches to Race at 0, which opens the
 * start gate (0xD4850, "StartgateOpen"). Race update 0x15650 counts the race clock mgr+0x10 (frames) and ends
 * the race (0x12AA0) once every human's rider+0x4D0 is >= 0: −1 until the rider finishes, then it counts up
 * by 1/60 per frame (0x24C30). rider+0x4F8 is a different value (track distance, negative).
 *
 * Finish: when a rider crosses the line, 0x23E90 (rider part) starts its finish clock (+0x4D0 = 0), sets
 * its finish time rider+0x4D8 = race clock mgr+0x10 + rider+0x4DC (a per-rider time offset) and calls the
 * GameModeMan dispatcher 0xCD550 (thiscall, slot), which calls the active mode's handler (GameModeMan
 * 0x369480, index +4 into the handler table 0x3693CC, vtable entry 7). Single race = handler 1 (vtable
 * 0x29A01C, 0xCEF90): takes rider+0x4D8 (360000 if DNF, rider+0x4E0) per run and slot; other modes take it
 * from the rider's stats block ([rider+0x760]+0xF4+0x9C, e.g. 0xCFF50). Results: 0x369408[slot] finish
 * frames, 0x369458[slot] place (0-based, 0xCE070 ranks once every human is in; riders still on the course
 * get an estimated time, 0x22EF0). lan_race_on_finish() runs before the dispatcher, so a time set there
 * (later: the host's) is what the results list and the ranking use.
 * Test: SSX3_PUPPET_FINISH=<frames> sets the puppet's (slot 1) finish time.
 *
 * Start hold: lan_race_hold_countdown() keeps the countdown at its first frame (only when it came from
 * PreRace, seen by lan_race_state_change(); not the ride behind the results screen, which comes from EndRace)
 * until the start signal, so
 * every PC counts the same 180 frames from the same moment. Signal for tests (later: the host's start
 * message): SSX3_RACE_HOLD=<seconds> releases after that many seconds, SSX3_RACE_GO_FILE=<path> when the
 * file exists. SSX3_RACE_TRACE=1 logs the state changes.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

#include "race.h"
#include "puppet.h"

extern ptrdiff_t g_xbox_mem_offset;

#define M32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

static int    s_init;
static double s_hold_s = -1.0;      /* < 0: no time-based hold */
static const char *s_go_file;
static double s_held_since = -1.0;  /* host time the current hold began, < 0: not holding */
static uint32_t s_countdown_from;   /* race state the current countdown came from */

static double now_s(void)
{
    struct timespec ts;

    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void init(void)
{
    const char *e;

    s_init = 1;
    if ((e = getenv("SSX3_RACE_HOLD")) != NULL)
        s_hold_s = atof(e);
    s_go_file = getenv("SSX3_RACE_GO_FILE");
    if (s_hold_s >= 0.0 || s_go_file)
        fprintf(stderr, "  [RACE] start hold: %s%.1f s%s%s\n", s_hold_s >= 0.0 ? "" : "no timer ",
                s_hold_s >= 0.0 ? s_hold_s : 0.0, s_go_file ? ", go file " : "", s_go_file ? s_go_file : "");
}

static int go_signal(double held)
{
    struct stat st;

    if (s_go_file && stat(s_go_file, &st) == 0)
        return 1;
    return s_hold_s >= 0.0 && held >= s_hold_s;
}

void lan_race_state_change(uint32_t from, uint32_t to)
{
    static const char *names[] = { "-", "GameInit", "Freeride", "PreRace", "Countdown", "Race", "EndRace",
                                   "Shutdown" };

    if (to == 4u)
        s_countdown_from = from;
    else            /* the ride behind the results screen enters Countdown without passing through here */
        s_countdown_from = 0;
    if (getenv("SSX3_RACE_TRACE"))
        fprintf(stderr, "  [RACE] state %s -> %s\n", from < 8u ? names[from] : "?", to < 8u ? names[to] : "?");
}

int lan_race_hold_countdown(uint32_t counter)
{
    double held;

    if (!s_init)
        init();
    if (s_hold_s < 0.0 && !s_go_file)
        return 0;
    if (counter < 180u)                         /* already counting: never stop half-way */
        return 0;
    if (s_countdown_from != 3u) {               /* only a race start (PreRace -> Countdown), not the ride
                                                   behind the results screen (EndRace -> Countdown) */
        if (s_held_since < 0.0 && counter == 180u)
            fprintf(stderr, "  [RACE] countdown after state %u: not held\n", s_countdown_from);
        return 0;
    }
    if (s_held_since < 0.0) {
        s_held_since = now_s();
        fprintf(stderr, "  [RACE] countdown held\n");
    }
    held = now_s() - s_held_since;
    if (!go_signal(held))
        return 1;
    fprintf(stderr, "  [RACE] start signal after %.2f s, countdown runs\n", held);
    s_held_since = -1.0;
    return 0;
}

static uint32_t rider_in_slot(uint32_t slot)
{
    uint32_t game = M32(0x00369534u), world, mgr;

    if (game < 0x10000u || (world = M32(game + 0x74)) < 0x10000u || (mgr = M32(world + 0x0C)) < 0x10000u
        || slot >= 6u || slot >= M32(mgr + 0x78))
        return 0;
    return M32(mgr + 0x28 + 4 * slot);
}

/* The finish time in the rider's stats block (where e.g. 0xCFF50 reads it), 0 if there is none */
static uint32_t stats_time_va(uint32_t rider)
{
    uint32_t stats = M32(rider + 0x760);

    return stats >= 0x10000u ? stats + 0xF4 + 0x9C : 0;
}

void lan_race_on_finish(uint32_t slot)
{
    static long forced = -2;
    uint32_t rider = rider_in_slot(slot), st;

    if (!rider)
        return;
    if (forced == -2) {
        const char *e = getenv("SSX3_PUPPET_FINISH");
        forced = e ? atol(e) : -1;
    }
    st = stats_time_va(rider);
    if (forced >= 0 && lan_puppet_is(rider)) {
        fprintf(stderr, "  [RACE] slot %u finish %u -> %ld frames (set from outside)\n", slot, M32(rider + 0x4D8),
                forced);
        M32(rider + 0x4D8) = (uint32_t)forced;
        if (st)
            M32(st) = (uint32_t)forced;
        return;
    }
    fprintf(stderr, "  [RACE] slot %u finish %u frames (%.2f s), stats block %d%s\n", slot, M32(rider + 0x4D8),
            M32(rider + 0x4D8) / 60.0, st ? (int)M32(st) : -1, M32(rider + 0x4E0) & 0xFF ? ", DNF" : "");
}
