# Frame interpolation (above 60 fps)

Goal: menus and races shown at 120 fps and more (the display's refresh, e.g. 270 Hz) on the Vulkan back end.
Default on; `SSX3_INTERP=0` restores the plain path (the game's own frames, presented on the NV2A thread).

## Why interpolation, not a faster game
SSX 3 simulates in fixed steps. The app object (pointer at 0x003A930C → 0x00369550) sets its logic rate to 60
(`sub_001ADAC0`: app+0x14 = 60, app+0x18 = 1/60) and starts the frame timer with 60 Hz (`sub_001AE480`, vtable
0x002F5300: period = 1000/hz ms, self-correcting one-shot XAPI timer; callback `sub_001AE030` → `sub_001AD5F0`).
`sub_001AD880` runs one state update per due tick (app+0x20 counts them) and then renders. The dt at app+0x18 is
barely read; the constant 1/60 (0x3C888889) is hard-coded in ~50 code sites and many data tables. A 120 Hz tick would
run the game at double speed, and fixing every constant is not realistic. So the game keeps simulating at 60 and the
renderer generates the frames in between.

## How it works (`src/render/vk_backend.c`)
- **Recording.** Every clear and draw is executed as before and also recorded (vertices, combiner block, views,
  samplers, render state, push constants) into a frame packet. Six packets in a ring; the NV2A thread fills one per
  game frame.
- **Flip.** Only the commands that drew the shown surface are kept. Render targets they sample (shadow maps etc.) are
  copied into the packet (the next frame redraws them while the presenter still reads this one). V.cmd is submitted
  without presenting and the packet goes to the presenter thread.
- **Pairing** (presenter, once per game frame). Each draw is paired with the same object in the frame before: same
  identity key (textures by guest address, blend/depth/alpha/stencil enables, combiner program) and vertex count,
  preferring identical texture coordinates (static geometry keeps its UVs while it moves), then the nearest. A partner
  further than 1/4 of the screen away is rejected (cuts, respawns). ~90 % of race draws pair.
- **Borrowed motion.** An unpaired 3D draw (mostly terrain chunks whose near-plane clipping changed the vertex count)
  would stand still and open cracks against moving neighbours. Its vertices take the motion of a paired vertex at the
  exact same screen position (shared edges), else of the nearest paired vertex at a similar depth (32×24 grid of
  sampled motions). Unpaired screen-space draws (HUD) stay put.
- **Replay.** Each shown frame re-renders the packet into the presenter's own colour/depth image with x, y, z and rhw
  blended `prev + (cur - prev) * t`, then blits it to the swapchain. Colours, UVs and textures come from the newer frame.
- **Timing.** `src/recomp_manual.c` wraps the renderer's present (`sub_001F3020`, renderer vtable +0x50, only reached
  indirectly) and records app+0x20, the world-step count. The blend into a frame lasts its step delta × the measured
  step length (≈16.67 ms; two steps when the game fell behind), so motion stays even although frames arrive 12–30 ms
  apart. Presents from other call sites (loading screens) fall back to the smoothed arrival interval. Each blend starts
  where the previous one ends, at least a cushion (2× the arrival jitter, 2–16 ms) after the frame arrived, and drifts
  back towards that minimum. Shown frames therefore trail the game by about one game frame plus the cushion.
- **Pacing.** Default cap = the highest refresh rate among the connected displays (Wayland cannot say which display
  the window is on); `SSX3_FPS_LIMIT=<n>` sets it, `0` uncaps (this costs game fps: the replays fill the shared GPU
  queue), `SSX3_VSYNC=1` uses FIFO. Mailbox otherwise.
- **Lifetimes.** Textures used in the last 8 flips are not evicted (the presenter may still sample them). Packets are
  reused only after the presenter's submissions that read them have retired (serials + fences, both threads poll).

## Measured (RTX 2080 Ti, 270 Hz display, Snow Jam race, 2026-10-19)
| Config | shown fps | game fps |
|---|---|---|
| `SSX3_INTERP=0` | 60 | 54–60 (pre-race ~44) |
| `SSX3_FPS_LIMIT=120` | 119.6–120.7, menus and race | 59–61 |
| default (cap 270) | 266–271 | 37–60 (pre-race dip as without interpolation) |
| `SSX3_FPS_LIMIT=0` | 600–1100 | 42–60 |

The log line `[VKI] holds/5s` counts blends that ran out before the next game frame arrived (visible stutter): 0 in
races after the game-clock timing. `[VKI] blended draws/5s` shows the pairing rate.

## Known limits / next steps
- Latency: +1 game frame + cushion (~24 ms) compared with `SSX3_INTERP=0`.
- Only positions are blended; animated UVs and colours step at 60 Hz.
- A camera cut whose meshes stay within 1/4 of the screen blends for one frame.
- Videos are fixed-rate; their texture is updated in place, so a video frame can appear a few ms early. Harmless.
- The game's own rate is still bound by the NV2A thread (CPU vertex programs): pre-race screen ~40 fps. Interpolation
  hides uneven arrival, but not missing motion. GPU vertex programs remain the lever for a steady 60.
- Windows: same presenter; its frame-cap sleep is a high-resolution waitable timer (`vk_platform.h`), since
  `Sleep` rounds to the 15.6 ms timer tick. Tested under Proton (120 fps cap and 270 Hz).
- Debug: `SSX3_VKI_DUMP=<prefix>` writes frames at blend 0, 1/2 and 1 (stalls the presenter while writing).
