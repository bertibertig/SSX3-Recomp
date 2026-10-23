/*
 * LAN multiplayer – front end (phase 0, spike 5).
 *
 * Main menu = cFEStateMainMenu (class name 0x289BE4). Items are named by string hashes (0x1AAEE0, an ELF-style
 * hash) of "1conquer", "2quick play", "3multi play", "4bonus material", "5online"; 0x9CB40 maps them to the
 * next front-end state: 0x0B Conquer, 9 Single Event, 0x16 Multi Play, 5 Bonus, 0x31 Online. Selection
 * handler 0x9CC70 (thiscall, item, event; event 5 = select): for Multi Play it sets the players (0x505A0(2))
 * and the "leaving" flag this+0x48, and only *then* checks this+0x48 == 0 to open cFEPopupSelectMultiplayerMode
 * (0x4C bytes, ctor 0x9C390, layout "86multiplayermode" – present in data/ui/fe.lui), so on Xbox that popup
 * (the PS2's online/offline choice) is dead code. 0x9CAB0 hides Multi Play while fewer than 2 pads are in
 * (this+0x55 < 2).
 *
 * Tests: SSX3_MP_POPUP=1 opens the popup from Multi Play the way the dead code would; SSX3_MP_ALWAYS=1 shows
 * Multi Play with one pad (a LAN game needs only one). SSX3_MENU_TRACE=1 logs every selection call.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu.h"
#include "lan_game.h"
#include "lobby_ui.h"

extern ptrdiff_t g_xbox_mem_offset;

#if defined(_MSC_VER)
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;
#else
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
#endif

extern void sub_001A9BD0(void);     /* allocator, cdecl (size, name, flags, align) */
extern void sub_0009C390(void);     /* cFEPopupSelectMultiplayerMode ctor, thiscall (fe, menu), ret 8 */
extern void sub_00203E80(void);     /* push a screen onto the front end's stack, thiscall (screen), ret 4 */

#define M32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define M8(va)  (*(volatile uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

#define ITEM_SINGLE_EVENT 9u
#define ITEM_MULTI_PLAY 0x16u
#define EVENT_FOCUS     1u
#define EVENT_UNFOCUS   2u
#define EVENT_SELECT    5u
#define LAN_CHOICE      0x00368444u      /* byte: the popup's entry != 0 (0x9C8F0) */

/* Call a guest function: ecx = self (thiscall) or 0, `n` stack arguments (first argument first), `pops`:
 * 1 if the callee removes its arguments (stdcall / thiscall), 0 for cdecl. Returns eax. */
static uint32_t guest_call(void (*fn)(void), uint32_t self, int n, const uint32_t *args, int pops)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ret;
    int i;

    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        M32(g_esp) = args[i];
    }
    g_esp -= 4;
    M32(g_esp) = 0;                 /* return address: the callee's ret pops it */
    g_ecx = self;
    fn();
    if (!pops)
        g_esp += 4u * (uint32_t)n;
    ret = g_eax;
    g_eax = eax; g_ecx = ecx; g_edx = edx;
    return ret;
}

static int env_flag(const char *name, int *cache)
{
    if (*cache < 0)
        *cache = getenv(name) != NULL;
    return *cache;
}

static uint32_t s_focus;                /* front-end state of the main menu's focused item, 0 = none */
static int      s_popup_single;         /* the popup is open from Single Event */

int lan_menu_single_focused(void)
{
    return s_focus == ITEM_SINGLE_EVENT;
}

/* Opens cFEPopupSelectMultiplayerMode for `item`, the way 0x9CC70 does for Multi Play when its leaving
 * flag this+0x48 is still 0 (0x9CD07 ..). */
static int open_popup(uint32_t menu, uint32_t item)
{
    uint32_t args[4], obj;
    M32(menu + 0x50) = (uint32_t)(int32_t)(int8_t)M8(menu + 0x14);
    args[0] = 0x4C; args[1] = 0x0028F11Cu; args[2] = 0; args[3] = 0;   /* "cFEPopupSelectMultiplayerMode" */
    obj = guest_call(sub_001A9BD0, 0, 4, args, 0);
    if (!obj)
        return 0;
    args[0] = M32(menu + 0x10); args[1] = menu;
    obj = guest_call(sub_0009C390, obj, 2, args, 1);
    M8(obj + 0x15) = (uint8_t)(1u << (M32(menu + 0x50) & 31));
    args[0] = obj;
    guest_call(sub_00203E80, M32(menu + 0x10) + 0x18, 1, args, 1);
    M32(menu + 0x4C) = item;
    return 1;
}

int lan_menu_select(uint32_t menu, uint32_t item, uint32_t event)
{
    static int popup = -1, trace = -1;
    uint32_t state;

    if (item < 0x10000u)
        return 0;
    state = M32(item + 0x18);
    if (env_flag("SSX3_MENU_TRACE", &trace))
        fprintf(stderr, "  [MENU] select: menu 0x%08X item 0x%08X (state %u) event %u\n", menu, item, state, event);
    if (event == EVENT_FOCUS)
        s_focus = state;
    else if (event == EVENT_UNFOCUS && s_focus == state)
        s_focus = 0;
    if (event != EVENT_SELECT)
        return 0;

    /* Single Event: Solo or LAN Game (the popup's second entry, LAN_TEXT_*). */
    if (state == ITEM_SINGLE_EVENT) {
        if (M32(menu + 0x4C) == item) {             /* the popup made its choice and hands the selection back */
            M32(menu + 0x4C) = 0;
            s_popup_single = 0;
            if (M8(LAN_CHOICE)) {
                M8(LAN_CHOICE) = 0;
                lobby_ui_open();
                return 1;                           /* stay in the main menu under the lobby */
            }
            return 0;
        }
        if (lan_game_in_select())
            return 0;                               /* a LAN event select: straight on, as one player */
        if (!open_popup(menu, item))
            return 0;
        s_popup_single = 1;
        return 1;
    }

    /* Multi Play (test): SSX3_MP_POPUP=1 opens the popup the way the dead code would. */
    if (!env_flag("SSX3_MP_POPUP", &popup) || state != ITEM_MULTI_PLAY)
        return 0;
    if (M32(menu + 0x4C) == item) {
        M32(menu + 0x4C) = 0;
        fprintf(stderr, "  [MENU] Multi Play: popup choice made, continuing\n");
        return 0;
    }
    if (open_popup(menu, item))
        fprintf(stderr, "  [MENU] Multi Play: opened cFEPopupSelectMultiplayerMode\n");
    return 1;
}

/* Texts the Xbox locale lacks, by the string hash the front end asks 0x760E0 for (found with
 * SSX3_TEXT_TRACE=1), and one it has but that does not fit when the popup opens from Single Event. */
static const struct { uint32_t hash; int replace; const char *text; } k_texts[] = {
    { 0x08349A04u, 0, "LAN Game" },                             /* the popup's entry 1 */
    { 0x00A95544u, 0, "Race other players on your network." }, /* ... its description */
    { 0x08814BD4u, 1, "Ride against the computer." },           /* entry 0: "2 player quick competition." */
};

uint32_t lan_menu_text(uint32_t hash, int missing)
{
    static int trace = -1;
    static uint32_t guest[sizeof k_texts / sizeof k_texts[0]];
    size_t i, k, n;

    for (i = 0; i < sizeof k_texts / sizeof k_texts[0]; i++) {
        uint32_t args[4];
        if (k_texts[i].hash != hash || (k_texts[i].replace ? !s_popup_single : !missing))
            continue;
        if (!guest[i]) {        /* UTF-16 copy in guest memory, made once (heap, never freed) */
            n = strlen(k_texts[i].text);
            args[0] = (uint32_t)(n + 1) * 2u; args[1] = 0x0028F11Cu; args[2] = 0; args[3] = 0;
            if ((guest[i] = guest_call(sub_001A9BD0, 0, 4, args, 0)) == 0)
                return 0;
            for (k = 0; k <= n; k++)
                *(volatile uint16_t *)((uintptr_t)(guest[i] + 2u * k) + g_xbox_mem_offset) =
                    (uint16_t)(unsigned char)k_texts[i].text[k];
        }
        return guest[i];
    }
    if (missing && env_flag("SSX3_TEXT_TRACE", &trace))
        fprintf(stderr, "  [MENU] no text for hash 0x%08X\n", hash);
    return 0;
}

int lan_menu_multiplay_pads(uint32_t menu)
{
    static int always = -1;

    return env_flag("SSX3_MP_ALWAYS", &always) && M8(menu + 0x55) < 2u;
}
