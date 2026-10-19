#!/usr/bin/env python3
"""Drives a launcher (run.sh / run.ps1) in a pseudo terminal plus a virtual Xbox 360 pad -- used by
scripts/test_release.sh. No focus stealing: the pad is a uinput device, the game needs
SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 (set by the caller) to read it while unfocused.

  drive.py --log out.log --seconds 85 [--keys '\\r' '\\r' '\\r'] [--pad linux|windows|none] -- <command> [args]

--keys     sent to the terminal one by one (Python escapes, e.g. '\\x1b[B' = down arrow), 1.5 s apart, after a
           start-up delay -- answers the start-up menu. The pad schedule's clock starts after the last key.
--pad      button schedule: 'linux' (Vulkan, fast) or 'windows' (CPU renderer, slow): Start to skip the intro,
           Start on the title, then A repeatedly through the menus into a race.
Answers the terminal's cursor-position queries (ESC[6n) like a real terminal -- PowerShell waits for them.
Exit code: the command's exit status (killed at --seconds counts as 0 = still running, which is a pass).
"""
import argparse, fcntl, os, pty, select, signal, struct, sys, termios, time

PAD_PLANS = {
    "linux":   [(13.5, "START"), (21.5, "START")] + [(23.5 + 1.5 * i, "A") for i in range(24)],
    "windows": [(40, "START"), (60, "START")] + [(70 + 4.0 * i, "A") for i in range(16)],
    "none":    [],
}


def make_pad():
    from evdev import AbsInfo, UInput, ecodes as e
    cap = {
        e.EV_KEY: [e.BTN_A, e.BTN_B, e.BTN_X, e.BTN_Y, e.BTN_START, e.BTN_SELECT, e.BTN_TL, e.BTN_TR,
                   e.BTN_MODE, e.BTN_THUMBL, e.BTN_THUMBR],
        e.EV_ABS: [(a, AbsInfo(0, -32768, 32767, 16, 128, 0)) for a in (e.ABS_X, e.ABS_Y, e.ABS_RX, e.ABS_RY)]
                + [(a, AbsInfo(0, 0, 255, 0, 0, 0)) for a in (e.ABS_Z, e.ABS_RZ)]
                + [(a, AbsInfo(0, -1, 1, 0, 0, 0)) for a in (e.ABS_HAT0X, e.ABS_HAT0Y)],
    }
    ui = UInput(cap, name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E, version=0x110,
                bustype=e.BUS_USB)
    buttons = {"A": e.BTN_A, "START": e.BTN_START}

    def press(name):
        ui.write(e.EV_KEY, buttons[name], 1); ui.syn(); time.sleep(0.2)
        ui.write(e.EV_KEY, buttons[name], 0); ui.syn()
    return ui, press


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--seconds", type=float, required=True)
    ap.add_argument("--keys", nargs="*", default=[])
    ap.add_argument("--key-delay", type=float, default=4.0, help="seconds before the first key")
    ap.add_argument("--pad", choices=PAD_PLANS, default="none")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
    if not cmd:
        ap.error("no command")

    plan = list(PAD_PLANS[a.pad])
    press = make_pad()[1] if plan else None
    keys = [k.encode().decode("unicode_escape").encode() for k in a.keys]

    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(cmd[0], cmd)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))

    out = open(a.log, "wb")
    t0 = time.time()
    next_key = t0 + a.key_delay
    pad_t0 = t0                              # pad schedule clock: restarts when the last menu key is sent
    status = None
    while time.time() - t0 < a.seconds:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                data = os.read(fd, 65536)
            except OSError:
                data = b""
            if not data:
                break
            out.write(data); out.flush()
            for _ in range(data.count(b"\x1b[6n")):
                os.write(fd, b"\x1b[30;1R")
        now = time.time()
        if keys and now >= next_key:
            os.write(fd, keys.pop(0)); next_key = now + 1.5
            pad_t0 = now
        while plan and not keys and now - pad_t0 >= plan[0][0]:
            press(plan.pop(0)[1])
        done, st = os.waitpid(pid, os.WNOHANG)
        if done:
            status = st
            break

    if status is None:                       # still running at the time limit: stop it, count as a pass
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGKILL):
            try:
                os.killpg(os.getpgid(pid), sig)
            except (ProcessLookupError, PermissionError):
                pass
            time.sleep(3)
            done, st = os.waitpid(pid, os.WNOHANG)
            if done:
                break
        rc = 0
    else:
        rc = os.waitstatus_to_exitcode(status)
    try:                                     # drain what is left
        while True:
            data = os.read(fd, 65536)
            if not data:
                break
            out.write(data)
    except OSError:
        pass
    out.write(f"\n[drive] exit {rc} after {time.time() - t0:.0f} s\n".encode())
    out.close()
    sys.exit(rc if rc >= 0 else 128 - rc)


if __name__ == "__main__":
    main()
