/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <string.h>   /* memcpy */

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

#if defined(_MSC_VER)
extern __declspec(thread) uint32_t g_eax;   /* thread-local like the other registers (RECOMP_TLS) */
#else
extern __thread uint32_t g_eax;
#endif
extern ptrdiff_t g_xbox_mem_offset;

/* ── Game clock for frame interpolation ─────────────────────── */

/*
 * SSX 3 advances its world in fixed steps: the app object (pointer at
 * 0x003A930C, 0x00369550) counts them at +0x20, one per 1/60 s timer tick
 * (sub_001AD880 increments it before each state update), and draws after the
 * updates that were due. When the title falls behind, one frame holds two
 * steps of motion. The Vulkan presenter (src/render/vk_backend.c) spaces
 * frames by this count rather than by arrival time, so motion stays even.
 *
 * The count is taken when the renderer presents: sub_001F3020 (renderer
 * vtable 0x002F7E50 +0x50, flush and D3D Present) is only reached through
 * indirect calls, so it can be wrapped here. Read by the NV2A thread at the
 * flip that this present queued; a present the title makes elsewhere (loading
 * screens) leaves it unchanged, and the presenter falls back to arrival time.
 */
extern void sub_001F3020(void);

volatile uint32_t g_ssx3_present_steps;

static void present_with_clock(void)
{
    uint32_t app = *(volatile uint32_t *)((uintptr_t)0x003A930Cu + g_xbox_mem_offset);

    if (app)
        g_ssx3_present_steps = *(volatile uint32_t *)((uintptr_t)(app + 0x20) + g_xbox_mem_offset);
    sub_001F3020();
}

/* ── Widescreen ─────────────────────────────────────────────── */

/*
 * SSX 3 renders 4:3 only. The main camera's projection is D3DX
 * PerspectiveFovLH (0x0024EA13, stdcall: out, fovy, aspect, zn, zf), called
 * only by the renderer's camera setup sub_001EAB40 (return address 0x001EACA5)
 * with aspect = 640/480. Multiplying that aspect by g_ssx3_wide makes the game
 * render -- and cull -- a wider view squeezed into its 640x480 target; the
 * Vulkan back end stretches screen-sized targets by the same factor and
 * narrows 2D vertices (HUD) back to 4:3. Finding from koenhendriks/SSX3-recomp
 * (CLAUDE.md, "WIDESCREEN"); see docs/notes.md.
 *
 * Only in game (races, free-roam: the app object's current state has vtable
 * 0x00299D7C). Frontend art, loading screens and movies are made for 4:3 and
 * stay 4:3; the back end shows them pillarboxed.
 */
/* Guest registers are thread-local (recomp_types.h, RECOMP_TLS). */
#if defined(_MSC_VER)
extern __declspec(thread) uint32_t g_esp;
#else
extern __thread uint32_t g_esp;
#endif

/* Requested target shape / (4/3): 1 = off. Set by the Vulkan back end from
 * SSX3_ASPECT before the guest starts; other renderers leave it at 1. */
volatile float g_ssx3_wide = 1.0f;

#define SSX3_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))

int ssx3_in_game(void)
{
    uint32_t app = SSX3_MEM32(0x003A930Cu), cur;

    if (app < 0x10000u)
        return 0;
    cur = SSX3_MEM32(app + 4);
    return cur >= 0x10000u && SSX3_MEM32(cur) == 0x00299D7Cu;
}

/* Entry hook (the recompiler calls it at the top of the generated
 * sub_0024EA13; scripts/run_pipeline.sh passes --exclude-manual). */
void sub_0024EA13_enter(void)
{
    float wide = g_ssx3_wide, aspect;
    uint32_t bits;

    if (wide <= 1.001f || SSX3_MEM32(g_esp) != 0x001EACA5u || !ssx3_in_game())
        return;
    bits = SSX3_MEM32(g_esp + 12);
    memcpy(&aspect, &bits, 4);
    aspect *= wide;
    memcpy(&bits, &aspect, 4);
    SSX3_MEM32(g_esp + 12) = bits;
}

/* ── CRT math through the x87 transcendental dispatcher ─────── */

/*
 * sub_00213850 is exp(double), sub_00212C18 fmod(double, double): cdecl,
 * arguments on the guest stack, result in st(0). Both tail-jump into the
 * CRT's hand-written dispatchers (ctrandisp1/ctrandisp2 -> trandisp1/2, a
 * computed `jmp [ebx]` into shared tails), and the lifted path returns with
 * the caller's ebx replaced by stale stack data. Finding and addresses from
 * koenhendriks/SSX3-recomp (CLAUDE.md, "Stuck on Loading...": exp() from the
 * loading draw sub_001AD7B0 broke the state machine sub_001AD880's ebx);
 * our implementation. Our callers: sub_001FE340 (exp, 12 sites),
 * sub_001B3C80, sub_00148040/60, sub_001489F0, sub_0020FB80, sub_001AD7B0;
 * fmod: sub_00102F10, sub_00103260. Generated bodies are left out by
 * --exclude-manual (scripts/run_pipeline.sh), so regenerate after changing
 * this list.
 */
#include <math.h>

#if defined(_MSC_VER)
extern __declspec(thread) double g_fp_stack[8];
extern __declspec(thread) int g_fp_top;
#else
extern __thread double g_fp_stack[8];
extern __thread int g_fp_top;
#endif

static double guest_double(uint32_t va)
{
    double d;
    uint32_t u[2];
    u[0] = SSX3_MEM32(va);
    u[1] = SSX3_MEM32(va + 4);
    memcpy(&d, u, sizeof d);
    return d;
}

static void fp_push_result(double v)
{
    g_fp_top = (g_fp_top + 7) & 7;
    g_fp_stack[g_fp_top] = v;
}

void sub_00213850(void)
{
    fp_push_result(exp(guest_double(g_esp + 4)));
    g_esp += 4;                            /* ret (cdecl: the caller pops the args) */
}

void sub_00212C18(void)
{
    fp_push_result(fmod(guest_double(g_esp + 4), guest_double(g_esp + 12)));
    g_esp += 4;
}

/* ── LAN multiplayer: puppet riders ─────────────────────────── */

/*
 * Rider update stages of the rider manager (vtable[2] 0x11E90), all fastcall with ecx = rider part.
 * For a puppet (src/lan/puppet.c) the brain stages are skipped -- each pops its own return address,
 * so a skip is esp += 4 -- and the pose is written right before 0x24D50 applies it. Wrappers rather
 * than entry hooks: the generated bodies stay as sub_X_gen (scripts/run_pipeline.sh regenerates
 * them after this list changes).
 */
#include "lan/puppet.h"

#if defined(_MSC_VER)
extern __declspec(thread) uint32_t g_ecx;
#else
extern __thread uint32_t g_ecx;
#endif

extern void sub_0001F3F0_gen(void);
extern void sub_0001F420_gen(void);
extern void sub_0001F450_gen(void);
extern void sub_00024D50_gen(void);

/* brain input */
void sub_0001F3F0(void)
{
    if (lan_puppet_skip_brain(g_ecx)) { g_esp += 4; return; }
    sub_0001F3F0_gen();
}

/* brain control-mode update */
void sub_0001F420(void)
{
    if (lan_puppet_skip_brain(g_ecx)) { g_esp += 4; return; }
    sub_0001F420_gen();
}

/* brain post-update, model tint */
void sub_0001F450(void)
{
    if (lan_puppet_skip_brain(g_ecx)) { g_esp += 4; return; }
    sub_0001F450_gen();
}

/* pose -> model and board (0x246C0); a puppet's culling box afterwards */
void sub_00024D50(void)
{
    uint32_t rider = g_ecx;
    int puppet = lan_puppet_is(rider);

    if (puppet)
        lan_puppet_pose(rider);
    sub_00024D50_gen();
    if (puppet)
        lan_puppet_after_pose(rider);
}

/*
 * Race countdown update (handler vtable 0x2820D8 [2]), stdcall: [esp+4] = rider manager, ret 4.
 * While the start is held (src/lan/race.c) the frame counter mgr+0x1C is left alone this frame.
 */
#include "lan/race.h"

extern void sub_00015620_gen(void);

void sub_00015620(void)
{
    uint32_t mgr = SSX3_MEM32(g_esp + 4);

    if (lan_race_hold_countdown(SSX3_MEM32(mgr + 0x1C))) { g_esp += 8; return; }
    sub_00015620_gen();
}

/* Race state change (rider manager), thiscall: ecx = manager, [esp+4] = new state, ret 4. */
extern void sub_000155A0_gen(void);

void sub_000155A0(void)
{
    uint32_t from = SSX3_MEM32(g_ecx + 4), to = SSX3_MEM32(g_esp + 4);

    if (from != to)
        lan_race_state_change(from, to);
    sub_000155A0_gen();
}

/* "Every human finished" (fastcall: ecx = rider manager -> al = 1). The race update 0x15650 asks it each frame
 * before it ends the race (return address 0x15663); the mode handlers ask it to finalize the results. LAN: the
 * race update hears "not yet" while remote players are still on the course; the mode handlers keep the real
 * answer (they finalize only when a human finishes, so a later "yes" from a puppet's finish would never come). */
extern void sub_00012AA0_gen(void);
int lan_game_wait_end(void);

void sub_00012AA0(void)
{
    int race_update = SSX3_MEM32(g_esp) == 0x00015663u;

    sub_00012AA0_gen();
    if (race_update && (g_eax & 0xFF) == 1 && lan_game_wait_end())
        g_eax &= ~0xFFu;
}

/* Rider manager update (vtable 0x28216C [2]), thiscall: ecx = manager, ret. Free-roam spawning first. */
#include "lan/freeroam.h"
#include "lan/lan_game.h"

extern void sub_00011E90_gen(void);

void sub_00011E90(void)
{
    lan_game_tick(g_ecx);
    lan_freeroam_tick(g_ecx);
    sub_00011E90_gen();
}

/* Main menu selection (cFEStateMainMenu), thiscall: ecx = menu, [esp+4] = item, [esp+8] = event, ret 8. */
#include "lan/menu.h"

extern void sub_0009CC70_gen(void);

void sub_0009CC70(void)
{
    if (lan_menu_select(g_ecx, SSX3_MEM32(g_esp + 4), SSX3_MEM32(g_esp + 8))) { g_esp += 12; return; }
    sub_0009CC70_gen();
}

/* Main menu: show or hide Multi Play by the pad count at menu+0x55 (fastcall, ret). */
extern void sub_0009CAB0_gen(void);

void sub_0009CAB0(void)
{
    uint32_t menu = g_ecx;
    uint8_t pads = *(volatile uint8_t *)((uintptr_t)(menu + 0x55) + g_xbox_mem_offset);

    lan_game_active();          /* LAN: open the session in the main menu, so profiles are in before a race loads */

    if (!lan_menu_multiplay_pads(menu)) {
        sub_0009CAB0_gen();
        return;
    }
    *(volatile uint8_t *)((uintptr_t)(menu + 0x55) + g_xbox_mem_offset) = 2;
    sub_0009CAB0_gen();
    *(volatile uint8_t *)((uintptr_t)(menu + 0x55) + g_xbox_mem_offset) = pads;
}

/* GameModeMan "rider reached the finish" dispatcher, thiscall: ecx = GameModeMan, [esp+4] = slot, ret 4. */
extern void sub_000CD550_gen(void);

void sub_000CD550(void)
{
    uint32_t slot = SSX3_MEM32(g_esp + 4);

    lan_race_on_finish(slot);
    sub_000CD550_gen();
    lan_game_after_finish(slot);
}

/* ComputerRider creation for the race (thiscall: ecx = rider manager, ret): takes each AI slot's character
 * from the race slot table. LAN: the remote players' characters and names go into their slots first. */
extern void sub_00013C60_gen(void);

void sub_00013C60(void)
{
    lan_game_before_ai(g_ecx);
    sub_00013C60_gen();
}

/* Character name (stdcall: [esp+4] character, [esp+8] variant, ret 8 -> eax = name). The results screen
 * (0xA4620) asks it for every AI row; its caller keeps the row's slot at [esp+0x18], which is [esp+0x24] here.
 * LAN: a remote player's row shows the player's name. */
extern void sub_0004DC00_gen(void);

void sub_0004DC00(void)
{
    uint32_t name = 0;

    if (SSX3_MEM32(g_esp) == 0x000A49FCu)
        name = lan_game_result_name(SSX3_MEM32(g_esp + 0x24));
    sub_0004DC00_gen();
    if (name)
        g_eax = name;
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x001F3020u)
        return present_with_clock;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}
