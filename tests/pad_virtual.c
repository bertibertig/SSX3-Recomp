/*
 * tests/pad_virtual.c -- src/input/ssx3_pad.c against a virtual Xbox 360 pad.
 *
 * No hardware needed: SDL's virtual joystick stands in for the controller.
 * Checks hotplug (a pad attached after init gets the lowest free port,
 * detaching frees it)
 * and the default map (face buttons, White = LB, Black = RB, triggers,
 * sticks with Y up) and rumble (speeds reach the pad, repeats are not
 * re-sent). Build and run: scripts/test_pad.sh
 */
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include "xinput_xbox.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* What the virtual pad's motors were last told. */
static int n_rumble;
static Uint16 rumble_low, rumble_high;

/* SDL2 wants 0 from a virtual pad's rumble callback on success, but
 * sdl2-compat (SDL2 2.32.50 and up, on top of SDL3) passes the value on as
 * SDL3's bool, where success is non-zero. Only virtual pads go through this. */
static int rumble_ok;

static int SDLCALL on_rumble(void *ud, Uint16 low, Uint16 high)
{
    (void)ud;
    n_rumble++;
    rumble_low = low;
    rumble_high = high;
    return rumble_ok;
}

/* The port the virtual pad got: a real pad plugged in keeps the one it has. */
static DWORD port;

static XBOX_INPUT_STATE poll_after_scan(void)
{
    XBOX_INPUT_STATE s;
    SDL_Delay(300);                     /* past the rescan rate limit */
    memset(&s, 0, sizeof s);
    xbox_InputGetState(port, &s);
    return s;
}

int main(void)
{
    SDL_VirtualJoystickDesc d;
    SDL_Joystick *js;
    XBOX_INPUT_STATE s;
    int idx, before[XBOX_MAX_CONTROLLERS];

    {
        SDL_version v;
        SDL_GetVersion(&v);
        rumble_ok = v.major == 2 && (v.minor > 32 || (v.minor == 32 && v.patch >= 50));
    }
    xbox_InputInit();
    for (port = 0; port < XBOX_MAX_CONTROLLERS; port++)
        before[port] = xbox_InputIsConnected(port);
    port = 0;
    while (port < XBOX_MAX_CONTROLLERS && before[port])
        port++;
    if (port == XBOX_MAX_CONTROLLERS) {
        fprintf(stderr, "pad_virtual: all four ports taken by real pads, skipped\n");
        return 0;
    }
    CHECK(xbox_InputGetState(port, &s) != 0 || port == 0);  /* port 0: keyboard */

    memset(&d, 0, sizeof d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.vendor_id = 0x045E;
    d.product_id = 0x028E;
    d.name = "Virtual Xbox 360 pad";
    d.Rumble = on_rumble;
    idx = SDL_JoystickAttachVirtualEx(&d);
    CHECK(idx >= 0);
    js = SDL_JoystickOpen(idx);
    CHECK(js != NULL);

    s = poll_after_scan();
    CHECK(xbox_InputIsConnected(port));
    CHECK(s.Gamepad.wButtons == 0);

    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_A, 1);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_Y, 1);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 1);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_START, 1);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_LEFTY, -32768);  /* up */
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_RIGHTX, 20000);
    s = poll_after_scan();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 0);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] == 255);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 255);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] == 0);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 0);
    CHECK(s.Gamepad.wButtons == (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_LEFT));
    CHECK(s.Gamepad.sThumbLY == 32767);
    CHECK(s.Gamepad.sThumbRX == 20000);

    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1);
    s = poll_after_scan();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 0);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] == 255);

    {   /* the packet number moves on every read: the game looks for edges */
        XBOX_INPUT_STATE a, b;
        xbox_InputGetState(port, &a);
        xbox_InputGetState(port, &b);
        CHECK(a.dwPacketNumber != b.dwPacketNumber);
    }

    {   /* rumble: passed on, an unchanged speed is not re-sent */
        XBOX_VIBRATION v = { 40000, 1000 };
        int n0;
        CHECK(xbox_InputSetState(port, &v) == 0);
        CHECK(rumble_low == 40000 && rumble_high == 1000);
        n0 = n_rumble;
        CHECK(xbox_InputSetState(port, &v) == 0);
        CHECK(n_rumble == n0);
        v.wLeftMotorSpeed = 0;
        v.wRightMotorSpeed = 0;
        CHECK(xbox_InputSetState(port, &v) == 0);
        CHECK(n_rumble == n0 + 1 && rumble_low == 0 && rumble_high == 0);
    }

    SDL_JoystickClose(js);
    SDL_JoystickDetachVirtual(idx);
    poll_after_scan();
    CHECK(!xbox_InputIsConnected(port));
    {
        XBOX_VIBRATION v = { 1, 1 };
        CHECK(xbox_InputSetState(port, &v) != 0);
    }

    printf(fails ? "pad_virtual: %d failure(s)\n" : "pad_virtual: all checks passed\n", fails);
    return fails != 0;
}
