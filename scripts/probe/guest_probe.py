#!/usr/bin/env python3
# Guest-memory probe for reverse engineering (Linux): starts scripts/run.sh as a child process (so
# /proc/<pid>/mem is readable and writable under Yama ptrace_scope=1), creates virtual Xbox 360 pads over
# uinput and takes commands from a FIFO. Output goes to <dir>/h.out, the game's console to <dir>/run.log.
#
#   python3 scripts/probe/guest_probe.py <dir>      # env: HPADS=<n> virtual pads (default 2); game env as usual,
#                                                   # e.g. RECOMP_USB_PADS=2 SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
#   echo "pad 1 START" > <dir>/h.fifo
#
# Commands, one per line:
#   pad <n> A|B|X|Y|START|BACK|LB|RB|UP|DOWN|LEFT|RIGHT [hold_s]
#   pad <n> LX|LY|RX|RY|LT|RT <value> <hold_s>
#   sleep <s>
#   r32 <addr> [count] / rf <addr> [count]      guest dwords / floats -> h.out
#   dump <addr> <len> <file>                    raw guest bytes
#   w32 <addr> <hex> / wf <addr> <f..>          write
#   lockf <addr> <f..>                          rewrite floats every 2 ms
#   follow <dst> <src> dx dy dz                 every 2 ms: vec3 at dst = vec3 at src + d
#   unlock                                      drop all locks
#   quit                                        stop the game and exit
import os, sys, time, struct, subprocess, threading
from evdev import UInput, AbsInfo, ecodes as e
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
S = os.path.abspath(sys.argv[1]); os.makedirs(S, exist_ok=True); fifo = S + "/h.fifo"; outp = S + "/h.out"
cap = {e.EV_KEY: [e.BTN_A, e.BTN_B, e.BTN_X, e.BTN_Y, e.BTN_START, e.BTN_SELECT, e.BTN_TL, e.BTN_TR, e.BTN_MODE, e.BTN_THUMBL, e.BTN_THUMBR],
       e.EV_ABS: [(a, AbsInfo(0, -32768, 32767, 16, 128, 0)) for a in (e.ABS_X, e.ABS_Y, e.ABS_RX, e.ABS_RY)] +
                 [(a, AbsInfo(0, 0, 255, 0, 0, 0)) for a in (e.ABS_Z, e.ABS_RZ)] + [(a, AbsInfo(0, -1, 1, 0, 0, 0)) for a in (e.ABS_HAT0X, e.ABS_HAT0Y)]}
npads = int(os.environ.get("HPADS", "2"))
pads = [UInput(cap, name="Microsoft X-Box 360 pad", vendor=0x045e, product=0x028e, version=0x110, bustype=e.BUS_USB, phys=f"vpad{i}") for i in range(npads)]
keys = dict(A=e.BTN_A, B=e.BTN_B, X=e.BTN_X, Y=e.BTN_Y, START=e.BTN_START, BACK=e.BTN_SELECT, LB=e.BTN_TL, RB=e.BTN_TR)
hats = dict(UP=(e.ABS_HAT0Y, -1), DOWN=(e.ABS_HAT0Y, 1), LEFT=(e.ABS_HAT0X, -1), RIGHT=(e.ABS_HAT0X, 1))
axes = dict(LX=e.ABS_X, LY=e.ABS_Y, RX=e.ABS_RX, RY=e.ABS_RY, LT=e.ABS_Z, RT=e.ABS_RZ)
time.sleep(1)
game = subprocess.Popen(["scripts/run.sh"], cwd=ROOT, stdout=open(S + "/run.log", "wb"), stderr=subprocess.STDOUT)
exe = os.path.join(ROOT, "build", "ssx3_recomp")
def base():
    for l in open(f"/proc/{game.pid}/maps"):
        if l.rstrip().endswith(exe) and l.split()[2] == "00000000":
            return int(l.split("-")[0], 16)
OFFSYM = int(next(l.split()[0] for l in subprocess.run(["nm", exe], capture_output=True, text=True).stdout.splitlines()
                  if l.endswith(" g_xbox_mem_offset")), 16)      # host-side guest base offset (PIE-relative)
mem = None; off = None; wmem = None; locks = {}
def gwrite(a, data):
    global wmem
    gread(0, 0)
    if wmem is None: wmem = open(f"/proc/{game.pid}/mem", "r+b", buffering=0)
    wmem.seek(a + off); wmem.write(data)
def locker():
    while True:
        for a, (src, delta) in list(locks.items()):
            try:
                if src is None: gwrite(a, delta)
                else:  # follow: dst = vec3 at src + delta
                    v = struct.unpack("<3f", gread(src, 12)); gwrite(a, struct.pack("<3f", *(v[i] + delta[i] for i in range(3))))
            except Exception: pass
        time.sleep(0.002)
threading.Thread(target=locker, daemon=True).start()
def gread(a, n):
    global mem, off
    if mem is None:
        mem = open(f"/proc/{game.pid}/mem", "rb", buffering=0)
        mem.seek(base() + OFFSYM); off = struct.unpack("<q", mem.read(8))[0]
    mem.seek(a + off); return mem.read(n)
if not os.path.exists(fifo): os.mkfifo(fifo)
out = open(outp, "a", buffering=1)
while True:
    with open(fifo) as f:
        for line in f:
            p = line.split()
            if not p: continue
            try:
                if p[0] == "quit": game.terminate(); sys.exit(0)
                if p[0] == "sleep": time.sleep(float(p[1])); continue
                if p[0] == "pad":
                    ui = pads[int(p[1]) - 1]; b = p[2].upper()
                    if b in keys:
                        hold = float(p[3]) if len(p) > 3 else 0.12
                        ui.write(e.EV_KEY, keys[b], 1); ui.syn(); time.sleep(hold); ui.write(e.EV_KEY, keys[b], 0); ui.syn()
                    elif b in hats:
                        hold = float(p[3]) if len(p) > 3 else 0.12
                        a, v = hats[b]; ui.write(e.EV_ABS, a, v); ui.syn(); time.sleep(hold); ui.write(e.EV_ABS, a, 0); ui.syn()
                    else:
                        ui.write(e.EV_ABS, axes[b], int(p[3])); ui.syn(); time.sleep(float(p[4])); ui.write(e.EV_ABS, axes[b], 0); ui.syn()
                    time.sleep(0.25); continue
                if p[0] == "w32": gwrite(int(p[1], 16), struct.pack("<I", int(p[2], 16))); continue
                if p[0] == "wf": gwrite(int(p[1], 16), struct.pack(f"<{len(p)-2}f", *map(float, p[2:]))); continue
                if p[0] == "lockf": locks[int(p[1], 16)] = (None, struct.pack(f"<{len(p)-2}f", *map(float, p[2:]))); continue
                if p[0] == "follow": locks[int(p[1], 16)] = (int(p[2], 16), tuple(map(float, p[3:6]))); continue
                if p[0] == "unlock": locks.clear(); continue
                a = int(p[1], 16); n = int(p[2], 0) if len(p) > 2 and p[0] != "dump" else 1
                if p[0] == "r32":
                    out.write(f"{a:08X}: " + " ".join(f"{v:08X}" for v in struct.unpack(f"<{n}I", gread(a, 4*n))) + "\n")
                elif p[0] == "rf":
                    out.write(f"{a:08X}: " + " ".join(f"{v:.3f}" for v in struct.unpack(f"<{n}f", gread(a, 4*n))) + "\n")
                elif p[0] == "dump":
                    open(p[3], "wb").write(gread(a, int(p[2], 0))); out.write(f"dumped {p[3]}\n")
            except Exception as ex:
                out.write(f"ERR {line.strip()}: {ex}\n")
