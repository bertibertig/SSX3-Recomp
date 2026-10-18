/*
 * ssx3_pad.c -- host game controllers as the Xbox gamepad (Controller S).
 *
 * Replaces xboxrecomp's src/input/xinput_device.c in our build (CMakeLists.txt
 * swaps the source; the submodule stays untouched) and implements the same
 * API (xinput_xbox.h). The rest of the chain is the runtime's: the emulated
 * USB pad (src/usb/usb_gamepad.c) asks xbox_InputGetState() for every report
 * and the game's XPP reads it over the emulated OHCI controller.
 *
 * Why our own copy:
 *   - Nothing ever called xbox_InputInit(), so the SDL backend never opened a
 *     controller: only the keyboard worked.
 *   - Controllers were opened once, at init: no hotplug, and a pad switched on
 *     after the boot (a wireless 8BitDo) was never seen.
 *   - Black/White sat on LB/RB, the opposite of the runtime's own README and
 *     of xemu (LB = White, RB = Black).
 *
 * Two stages:
 *   1. read the host pad into HostPad, the modern XInput layout every host
 *      controller can be described in (SDL GameController on POSIX, XInput
 *      on Windows);
 *   2. map HostPad onto the Xbox pad through s_bind, one row per Xbox control.
 *
 * Any controller SDL knows as a "game controller" works: XInput pads
 * (Xbox 360 / One / Series, 8BitDo in X-input mode, most clones) through the
 * kernel's xpad driver or SDL's HIDAPI drivers, and every other pad in SDL's
 * built-in mapping database. config/gamecontrollerdb.txt is loaded at init
 * when present (format: https://github.com/mdqinc/SDL_GameControllerDB), and
 * SDL_GAMECONTROLLERCONFIG still works as SDL documents it.
 *
 * TODO(input): PlayStation controllers (DualShock 4, DualSense). SDL already
 *   maps them positionally (Cross = A, Circle = B, Square = X, Triangle = Y),
 *   so they work through this file today; still open: Share/Options/touchpad
 *   click as extra binds, the light bar, and showing PlayStation button
 *   glyphs (the game draws Xbox prompts; would need the HUD/font textures).
 *   Check whether the HIDAPI PS4/PS5 drivers need
 *   SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE / _PS5_RUMBLE for rumble.
 * TODO(input): Nintendo Switch controllers (Pro Controller, Joy-Con pair,
 *   8BitDo in Switch mode). SDL2 reports Nintendo pads by *label* by default
 *   (SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS=1: the button labelled A, on
 *   the east side, is SDL "A"), so confirm/back sit on the opposite side to
 *   an Xbox pad. Decide positional vs. label mapping (probably positional,
 *   as an option), handle single Joy-Cons, and test the 8BitDo modes
 *   (X-input / D-input / Switch) one by one. ssx3_pad_type_name() already
 *   tells the families apart, so per-type defaults can hang off it.
 * TODO(input): Rebinding. s_bind is a table on purpose: load it from a config
 *   file (e.g. config/input.ini: "white = lb", "black = rb", "lt = a") and
 *   later from an in-game/launcher menu; per-port tables for local
 *   multiplayer; stick deadzone/sensitivity and trigger thresholds as
 *   options. The game has its own key map files (config\input*.map on the
 *   disc) -- check whether rebinding in-game is possible before building one.
 * Rumble: the emulated USB pad passes the game's output reports here
 *   (usb_gamepad_output, runtime patch); left motor = low frequency.
 */

#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stage 1: the host pad, in the modern layout ------------------------ */

enum {
    HB_A, HB_B, HB_X, HB_Y,                 /* south, east, west, north */
    HB_LB, HB_RB, HB_BACK, HB_START, HB_GUIDE,
    HB_LS, HB_RS,                           /* stick clicks */
    HB_DUP, HB_DDOWN, HB_DLEFT, HB_DRIGHT,
    HB_COUNT
};
enum { HA_LX, HA_LY, HA_RX, HA_RY, HA_LT, HA_RT, HA_COUNT };

typedef struct HostPad {
    uint8_t btn[HB_COUNT];      /* 0 / 1 */
    int16_t axis[HA_COUNT];     /* sticks -32768..32767 with Y up positive,
                                   triggers 0..32767 */
} HostPad;

/* ---- stage 2: the bindings --------------------------------------------- */

/* Where one Xbox control reads from. */
enum { SRC_NONE, SRC_BUTTON, SRC_AXIS };
typedef struct PadSource {
    uint8_t kind;               /* SRC_* */
    uint8_t index;              /* HB_* or HA_* */
} PadSource;

/* Xbox controls, in report order: 8 digital bits, 8 analog buttons, 4 axes. */
enum {
    XC_DUP, XC_DDOWN, XC_DLEFT, XC_DRIGHT, XC_START, XC_BACK, XC_LTHUMB, XC_RTHUMB,
    XC_A, XC_B, XC_X, XC_Y, XC_BLACK, XC_WHITE, XC_LT, XC_RT,
    XC_LX, XC_LY, XC_RX, XC_RY,
    XC_COUNT
};

#define B(i) { SRC_BUTTON, (i) }
#define A(i) { SRC_AXIS,   (i) }

/* The default map: same position on the pad. The Controller S has no
 * shoulder buttons; White and Black sit beside the face buttons and take the
 * bumpers, White left and Black right as on xemu. */
static PadSource s_bind[XC_COUNT] = {
    [XC_DUP]    = B(HB_DUP),    [XC_DDOWN]  = B(HB_DDOWN),
    [XC_DLEFT]  = B(HB_DLEFT),  [XC_DRIGHT] = B(HB_DRIGHT),
    [XC_START]  = B(HB_START),  [XC_BACK]   = B(HB_BACK),
    [XC_LTHUMB] = B(HB_LS),     [XC_RTHUMB] = B(HB_RS),
    [XC_A]      = B(HB_A),      [XC_B]      = B(HB_B),
    [XC_X]      = B(HB_X),      [XC_Y]      = B(HB_Y),
    [XC_BLACK]  = B(HB_RB),     [XC_WHITE]  = B(HB_LB),
    [XC_LT]     = A(HA_LT),     [XC_RT]     = A(HA_RT),
    [XC_LX]     = A(HA_LX),     [XC_LY]     = A(HA_LY),
    [XC_RX]     = A(HA_RX),     [XC_RY]     = A(HA_RY),
};

#undef B
#undef A

/* A source as a pressure, 0..255: buttons are fully pressed or not, triggers
 * scale down (SDL's 0..32767 >> 7). A stick as a button reads its positive
 * half -- only reachable through a custom table. */
static int source_pressure(const HostPad *h, PadSource s)
{
    int v;
    switch (s.kind) {
    case SRC_BUTTON:
        return h->btn[s.index] ? 255 : 0;
    case SRC_AXIS:
        v = h->axis[s.index] >> 7;
        return v < 0 ? 0 : v > 255 ? 255 : v;
    }
    return 0;
}

/* A source as a stick axis: a button reads full deflection. */
static SHORT source_axis(const HostPad *h, PadSource s)
{
    switch (s.kind) {
    case SRC_BUTTON:
        return h->btn[s.index] ? 32767 : 0;
    case SRC_AXIS:
        return h->axis[s.index];
    }
    return 0;
}

static void map_host_pad(const HostPad *h, XBOX_INPUT_STATE *out, DWORD packet)
{
    /* A digital Xbox bit is held at the pressure the game itself treats as
     * pressed, so a trigger bound to Start behaves like one. */
    static const WORD dig_bit[8] = {
        XBOX_GAMEPAD_DPAD_UP, XBOX_GAMEPAD_DPAD_DOWN, XBOX_GAMEPAD_DPAD_LEFT,
        XBOX_GAMEPAD_DPAD_RIGHT, XBOX_GAMEPAD_START, XBOX_GAMEPAD_BACK,
        XBOX_GAMEPAD_LEFT_THUMB, XBOX_GAMEPAD_RIGHT_THUMB,
    };
    static const int analog_index[8] = {
        XBOX_BUTTON_A, XBOX_BUTTON_B, XBOX_BUTTON_X, XBOX_BUTTON_Y,
        XBOX_BUTTON_BLACK, XBOX_BUTTON_WHITE,
        XBOX_BUTTON_LTRIGGER, XBOX_BUTTON_RTRIGGER,
    };
    XBOX_GAMEPAD *g = &out->Gamepad;
    int i;

    memset(out, 0, sizeof *out);
    out->dwPacketNumber = packet;
    for (i = 0; i < 8; i++)
        if (source_pressure(h, s_bind[XC_DUP + i]) > XBOX_ANALOG_BUTTON_THRESHOLD)
            g->wButtons |= dig_bit[i];
    for (i = 0; i < 8; i++)
        g->bAnalogButtons[analog_index[i]] =
            (BYTE)source_pressure(h, s_bind[XC_A + i]);
    g->sThumbLX = source_axis(h, s_bind[XC_LX]);
    g->sThumbLY = source_axis(h, s_bind[XC_LY]);
    g->sThumbRX = source_axis(h, s_bind[XC_RX]);
    g->sThumbRY = source_axis(h, s_bind[XC_RY]);
}

/* SSX3_PAD_TRACE=1: every change of a port's buttons, as the game gets them,
 * for checking a controller's mapping without the game's own screens. Sticks
 * are left out (they change on every report); triggers show as pressures. */
static void trace_state(DWORD port, const XBOX_INPUT_STATE *s)
{
    static int on = -1;
    static XBOX_GAMEPAD last[XBOX_MAX_CONTROLLERS];
    static const char *const dig_name[8] = {
        "up", "down", "left", "right", "start", "back", "lthumb", "rthumb"
    };
    static const char *const ana_name[8] = {
        "a", "b", "x", "y", "black", "white", "lt", "rt"
    };
    const XBOX_GAMEPAD *g = &s->Gamepad;
    char line[256];
    size_t n = 0;
    int i, changed = 0;

    if (on < 0)
        on = getenv("SSX3_PAD_TRACE") && *getenv("SSX3_PAD_TRACE") == '1';
    if (!on || port >= XBOX_MAX_CONTROLLERS)
        return;
    if (g->wButtons != last[port].wButtons)
        changed = 1;
    for (i = 0; i < 8; i++)
        if ((g->bAnalogButtons[i] > XBOX_ANALOG_BUTTON_THRESHOLD)
                != (last[port].bAnalogButtons[i] > XBOX_ANALOG_BUTTON_THRESHOLD))
            changed = 1;
    last[port] = *g;
    if (!changed)
        return;
    for (i = 0; i < 8 && n < sizeof line - 16; i++)
        if (g->wButtons & (1u << i))
            n += (size_t)snprintf(line + n, sizeof line - n, " %s", dig_name[i]);
    for (i = 0; i < 8 && n < sizeof line - 16; i++)
        if (g->bAnalogButtons[i] > XBOX_ANALOG_BUTTON_THRESHOLD)
            n += (size_t)snprintf(line + n, sizeof line - n, " %s(%u)",
                                  ana_name[i], g->bAnalogButtons[i]);
    line[n] = 0;
    fprintf(stderr, "  PAD: port %lu held:%s  L(%d,%d) R(%d,%d)\n",
            (unsigned long)port + 1, n ? line : " -",
            g->sThumbLX, g->sThumbLY, g->sThumbRX, g->sThumbRY);
    fflush(stderr);
}

/* Merge the keyboard on top of a pad on port 0 rather than only standing in
 * for a missing one (as upstream): a real pad keeps working, the keys only
 * add. Under Wine XInputGetState succeeds with nothing plugged in, so a
 * keyboard behind its failure would never be read. */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    if (!xbox_InputKeyboardPoll(&kb))
        return;
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is read
     * as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>

static BOOL g_controller_connected[XBOX_MAX_CONTROLLERS];

void xbox_InputInit(void)
{
    DWORD i;
    for (i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        g_controller_connected[i] = XInputGetState(i, &state) == ERROR_SUCCESS;
        if (g_controller_connected[i])
            fprintf(stderr, "  PAD: port %lu <- XInput pad %lu\n",
                    (unsigned long)i + 1, (unsigned long)i);
    }
}

static void read_xinput(const XINPUT_GAMEPAD *x, HostPad *h)
{
    static const struct { WORD bit; int hb; } btn[] = {
        { XINPUT_GAMEPAD_A, HB_A }, { XINPUT_GAMEPAD_B, HB_B },
        { XINPUT_GAMEPAD_X, HB_X }, { XINPUT_GAMEPAD_Y, HB_Y },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, HB_LB },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, HB_RB },
        { XINPUT_GAMEPAD_BACK, HB_BACK }, { XINPUT_GAMEPAD_START, HB_START },
        { XINPUT_GAMEPAD_LEFT_THUMB, HB_LS }, { XINPUT_GAMEPAD_RIGHT_THUMB, HB_RS },
        { XINPUT_GAMEPAD_DPAD_UP, HB_DUP }, { XINPUT_GAMEPAD_DPAD_DOWN, HB_DDOWN },
        { XINPUT_GAMEPAD_DPAD_LEFT, HB_DLEFT }, { XINPUT_GAMEPAD_DPAD_RIGHT, HB_DRIGHT },
    };
    size_t i;

    memset(h, 0, sizeof *h);
    for (i = 0; i < sizeof btn / sizeof btn[0]; i++)
        h->btn[btn[i].hb] = (x->wButtons & btn[i].bit) != 0;
    /* XInput's Y axes already point up, like the Xbox's. */
    h->axis[HA_LX] = x->sThumbLX;
    h->axis[HA_LY] = x->sThumbLY;
    h->axis[HA_RX] = x->sThumbRX;
    h->axis[HA_RY] = x->sThumbRY;
    /* Triggers 0..255 -> 0..32767, so >> 7 gives the 0..255 back. */
    h->axis[HA_LT] = (int16_t)(x->bLeftTrigger * 128 + (x->bLeftTrigger >> 1));
    h->axis[HA_RT] = (int16_t)(x->bRightTrigger * 128 + (x->bRightTrigger >> 1));
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi;
    HostPad h;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (XInputGetState(dwPort, &xi) != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0) {
            /* Keyboard and mouse stand in for an absent pad, so the game is
             * playable with no controller plugged in. */
            int kb;
            memset(pState, 0, sizeof *pState);
            kb = xbox_InputKeyboardPoll(pState);
            if (mkb_merge(dwPort, pState))
                g_controller_connected[dwPort] = TRUE;
            if (kb || mkb_enabled()) {
                trace_state(dwPort, pState);
                return ERROR_SUCCESS;
            }
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    g_controller_connected[dwPort] = TRUE;
    read_xinput(&xi.Gamepad, &h);
    map_host_pad(&h, pState, xi.dwPacketNumber);
    if (dwPort == 0) {
        keyboard_merge(pState);
        /* The mouse owns the aim stick while it moves, WASD the movement. */
        mkb_merge(dwPort, pState);
    }
    trace_state(dwPort, pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION v;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    v.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    v.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(dwPort, &v);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    return dwPort < XBOX_MAX_CONTROLLERS && g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xc;
    DWORD rc;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    rc = XInputGetCapabilities(dwPort, dwFlags, &xc);
    if (rc != ERROR_SUCCESS)
        return rc;
    memset(pCaps, 0, sizeof *pCaps);
    pCaps->Type = xc.Type;
    pCaps->SubType = xc.SubType;
    pCaps->Flags = xc.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

/* One Xbox port per open controller. A port keeps its pad until that pad is
 * unplugged; a new pad takes the lowest free port. */
typedef struct PadSlot {
    SDL_GameController *gc;
    SDL_JoystickID      id;
} PadSlot;

static PadSlot g_slot[XBOX_MAX_CONTROLLERS];
static DWORD   g_packet[XBOX_MAX_CONTROLLERS];
static int     g_init;
static Uint32  g_last_scan;

/* Set while a window's main thread pumps SDL events (fb_present_sdl.c,
 * src/render/vk_backend.c). The pump already updates the controllers, and
 * SDL_GameControllerUpdate pumps events itself, which on macOS must only
 * happen on the main thread; so then this side only reads them. */
static volatile int g_main_pump;

void xbox_InputSetMainPump(int on)
{
    g_main_pump = on;
}

/* Whether any controller is open: SDL must stay up for it at shutdown. */
int xbox_InputAnyOpen(void)
{
    int i;
    for (i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_slot[i].gc)
            return 1;
    return 0;
}

static const char *ssx3_pad_type_name(SDL_GameController *gc)
{
#if SDL_VERSION_ATLEAST(2, 0, 12)
    switch (SDL_GameControllerGetType(gc)) {
    case SDL_CONTROLLER_TYPE_XBOX360:      return "Xbox 360";
    case SDL_CONTROLLER_TYPE_XBOXONE:      return "Xbox One/Series";
    case SDL_CONTROLLER_TYPE_PS3:          return "PlayStation 3";
    case SDL_CONTROLLER_TYPE_PS4:          return "PlayStation 4";
#if SDL_VERSION_ATLEAST(2, 0, 14)
    case SDL_CONTROLLER_TYPE_PS5:          return "PlayStation 5";
#endif
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
#if SDL_VERSION_ATLEAST(2, 24, 0)
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con";
#endif
    default:                               break;
    }
#else
    (void)gc;
#endif
    return "generic";
}

/* Close pads that went away, open new ones into free ports. Rate-limited:
 * SDL learns of new devices in its event pump, so a quarter second is soon
 * enough and keeps the per-report cost to a tick read. Caller holds the
 * joystick lock. */
static void rescan(int force)
{
    Uint32 now = SDL_GetTicks();
    int i, j, n;

    if (!force && now - g_last_scan < 250)
        return;
    g_last_scan = now;

    for (i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        if (g_slot[i].gc && !SDL_GameControllerGetAttached(g_slot[i].gc)) {
            fprintf(stderr, "  PAD: port %d disconnected\n", i + 1);
            fflush(stderr);
            SDL_GameControllerClose(g_slot[i].gc);
            g_slot[i].gc = NULL;
        }
    }

    n = SDL_NumJoysticks();
    for (j = 0; j < n; j++) {
        SDL_JoystickID id;
        int free_slot = -1, known = 0;

        if (!SDL_IsGameController(j))
            continue;
        id = SDL_JoystickGetDeviceInstanceID(j);
        for (i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
            if (g_slot[i].gc && g_slot[i].id == id)
                known = 1;
            else if (!g_slot[i].gc && free_slot < 0)
                free_slot = i;
        }
        if (known || free_slot < 0)
            continue;
        g_slot[free_slot].gc = SDL_GameControllerOpen(j);
        if (!g_slot[free_slot].gc) {
            fprintf(stderr, "  PAD: cannot open \"%s\": %s\n",
                    SDL_GameControllerNameForIndex(j), SDL_GetError());
            continue;
        }
        g_slot[free_slot].id = id;
        g_packet[free_slot]++;
        fprintf(stderr, "  PAD: port %d <- \"%s\" (%s, %04x:%04x)\n",
                free_slot + 1, SDL_GameControllerName(g_slot[free_slot].gc),
                ssx3_pad_type_name(g_slot[free_slot].gc),
                SDL_GameControllerGetVendor(g_slot[free_slot].gc),
                SDL_GameControllerGetProduct(g_slot[free_slot].gc));
        fflush(stderr);
    }
}

void xbox_InputInit(void)
{
    const char *db;

    if (g_init)
        return;
    g_init = 1;
    /* Keep the default SIGINT/SIGTERM: SDL would turn them into an SDL_QUIT
     * that nothing reads without a window, and Ctrl-C would stop working. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)
            && SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "  PAD: SDL game controllers unavailable: %s\n", SDL_GetError());
        return;
    }
    /* Extra mappings, for pads SDL's built-in database does not know. */
    db = getenv("SSX3_PAD_DB");
    if (!db || !*db)
        db = "config/gamecontrollerdb.txt";
    {
        FILE *f = fopen(db, "rb");
        if (f) {
            int added;
            fclose(f);
            added = SDL_GameControllerAddMappingsFromFile(db);
            fprintf(stderr, "  PAD: %d mapping(s) from %s\n", added, db);
        }
    }
    SDL_LockJoysticks();
    if (!g_main_pump)
        SDL_GameControllerUpdate();
    rescan(1);
    SDL_UnlockJoysticks();
    if (!xbox_InputAnyOpen())
        fprintf(stderr, "  PAD: no game controller yet (hotplug is on)\n");
    fflush(stderr);
}

/* SDL's Y axes point down, the Xbox's up. Clamped, so -32768 does not
 * overflow, and a centred stick stays at 0. */
static int16_t flip_y(Sint16 v)
{
    return v == -32768 ? 32767 : (int16_t)-v;
}

static void read_sdl(SDL_GameController *c, HostPad *h)
{
    static const SDL_GameControllerButton btn[HB_COUNT] = {
        [HB_A] = SDL_CONTROLLER_BUTTON_A,         [HB_B] = SDL_CONTROLLER_BUTTON_B,
        [HB_X] = SDL_CONTROLLER_BUTTON_X,         [HB_Y] = SDL_CONTROLLER_BUTTON_Y,
        [HB_LB] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        [HB_RB] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        [HB_BACK] = SDL_CONTROLLER_BUTTON_BACK,   [HB_START] = SDL_CONTROLLER_BUTTON_START,
        [HB_GUIDE] = SDL_CONTROLLER_BUTTON_GUIDE,
        [HB_LS] = SDL_CONTROLLER_BUTTON_LEFTSTICK,
        [HB_RS] = SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        [HB_DUP] = SDL_CONTROLLER_BUTTON_DPAD_UP,
        [HB_DDOWN] = SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        [HB_DLEFT] = SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        [HB_DRIGHT] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    };
    int i;

    for (i = 0; i < HB_COUNT; i++)
        h->btn[i] = SDL_GameControllerGetButton(c, btn[i]) != 0;
    h->axis[HA_LX] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    h->axis[HA_LY] = flip_y(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    h->axis[HA_RX] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    h->axis[HA_RY] = flip_y(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
    h->axis[HA_LT] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    h->axis[HA_RT] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    HostPad h;
    int have;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_init)
        xbox_InputInit();

    /* The main thread's event pump opens and closes devices under this lock
     * too; holding it keeps a pad from going away between check and read. */
    SDL_LockJoysticks();
    if (!g_main_pump)
        SDL_GameControllerUpdate();
    rescan(0);
    have = g_slot[dwPort].gc != NULL;
    if (have)
        read_sdl(g_slot[dwPort].gc, &h);
    SDL_UnlockJoysticks();

    if (!have) {
        if (dwPort == 0 && xbox_InputKeyboardPoll(pState)) {
            trace_state(dwPort, pState);
            return ERROR_SUCCESS;
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    map_host_pad(&h, pState, ++g_packet[dwPort]);
    if (dwPort == 0)
        keyboard_merge(pState);
    trace_state(dwPort, pState);
    return ERROR_SUCCESS;
}

/* SSX3_PAD_TRACE=1 also logs each rumble change. */
static void trace_rumble(DWORD port, const XBOX_VIBRATION *v, int sent)
{
    static int on = -1;
    if (on < 0)
        on = getenv("SSX3_PAD_TRACE") && *getenv("SSX3_PAD_TRACE") == '1';
    if (!on)
        return;
    fprintf(stderr, "  PAD: port %lu rumble low=%u high=%u%s\n",
            (unsigned long)port + 1, v->wLeftMotorSpeed, v->wRightMotorSpeed,
            sent ? "" : " (no pad / no rumble motor)");
    fflush(stderr);
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    /* What each port's motors were last set to, and when. */
    static XBOX_VIBRATION last[XBOX_MAX_CONTROLLERS];
    static Uint32 last_sent[XBOX_MAX_CONTROLLERS];
    static SDL_GameController *last_gc[XBOX_MAX_CONTROLLERS];
    DWORD rc = ERROR_DEVICE_NOT_CONNECTED;
    SDL_GameController *gc;
    Uint32 now;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return rc;
    SDL_LockJoysticks();
    gc = g_slot[dwPort].gc;
    now = SDL_GetTicks();
    if (gc) {
        rc = ERROR_SUCCESS;
        /* An Xbox motor runs until the title changes its speed; SDL rumble
         * needs a duration. So: the longest SDL takes (~65 s), renewed
         * well before it runs out, and nothing sent while the speeds stay
         * the same -- the title re-sends them every frame, and each send is
         * a USB/Bluetooth write on the real pad. A new pad on the port gets
         * the current speeds too. */
        if (gc != last_gc[dwPort]
                || pVibration->wLeftMotorSpeed != last[dwPort].wLeftMotorSpeed
                || pVibration->wRightMotorSpeed != last[dwPort].wRightMotorSpeed
                || ((last[dwPort].wLeftMotorSpeed || last[dwPort].wRightMotorSpeed)
                    && now - last_sent[dwPort] > 30000)) {
            int ok = SDL_GameControllerRumble(gc, pVibration->wLeftMotorSpeed,
                                              pVibration->wRightMotorSpeed,
                                              0xFFFF) == 0;
            if (pVibration->wLeftMotorSpeed != last[dwPort].wLeftMotorSpeed
                    || pVibration->wRightMotorSpeed != last[dwPort].wRightMotorSpeed)
                trace_rumble(dwPort, pVibration, ok);
            last_gc[dwPort] = gc;
            last_sent[dwPort] = now;
        }
    } else if (pVibration->wLeftMotorSpeed != last[dwPort].wLeftMotorSpeed
               || pVibration->wRightMotorSpeed != last[dwPort].wRightMotorSpeed) {
        trace_rumble(dwPort, pVibration, 0);
    }
    last[dwPort] = *pVibration;
    SDL_UnlockJoysticks();
    return rc;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    return dwPort < XBOX_MAX_CONTROLLERS && g_slot[dwPort].gc != NULL;
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps || !g_slot[dwPort].gc)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCaps, 0, sizeof *pCaps);
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
