# LAN multiplayer – plan

A feature the original game never had: online-style multiplayer over LAN (and direct IP) with a lobby, up to
**6 players per race** at first (the engine's rider limit) and **12 in free roam** (Conquer the Mountain) from the
start; 12-player races and Freestyle later. Status: **planning** (2026-10-19). Findings go to `docs/notes.md`.

## What the game gives us (verified 2026-10-19, see notes.md)

- Split screen (*Multi Play*) works in the port: 2 humans + 4 AI = 6 riders, Race or Freestyle events only,
  no Conquer the Mountain. Player character select already has **X = Load game** (import a leveled rider from a
  save) and the *Bragging Rights* handicap screen.
- A race always has **6 riders** (1 human + 5 AI, or 2 + 4). Freestyle: you compete against 5 others, but not on
  the slope at the same time (scores are compared). Conquer the Mountain (free roam) is single player only, with
  no other riders on the mountain.
- No network code on Xbox: the XBE links no XNet/XOnline library. The PS2 version had online play (`dirtysock`,
  `beintnetwork`, `inputsync`), but those files are unmatched stubs in the decomp, so there is nothing to reuse.
  **All netcode is ours, on the host side (`src/net/`), not in guest code.**

## Architecture

**Each PC runs its own full game with one local player.** Other players appear as *puppet riders*: guest rider
objects whose state is written from the network instead of from a pad or the AI.

- **State sync, not lockstep.** The recompiled game is not bit-deterministic across machines (floats, streaming
  threads, frame timing, different renderers/OSes), so lockstep input sync (what the PS2's `inputsync` probably
  did) is out. Every client owns its own rider and sends snapshots (position, orientation, velocity,
  animation/trick state, board, boost/adrenaline, track progress) at 30 Hz; receivers render remote riders about
  100 ms in the past (interpolation) and extrapolate briefly when packets go missing.
- **The host owns the session:** lobby, rules, clock sync, synchronized countdown, AI riders (the host simulates
  them and broadcasts them like players), finish order and times, the scoreboard. Clients own only their own
  rider. ("Host" = the player who opened the lobby; no dedicated server needed. A headless server can come later.)
- **Transport:** UDP with [ENet](https://github.com/lsalzman/enet) (MIT, one small C library, Linux/Windows/macOS):
  reliable channel for lobby/events/chat, unreliable for snapshots. LAN discovery by UDP broadcast; direct IP by
  address and port. Same protocol for Linux, Windows and later macOS (cross-play), versioned handshake (refuses
  mismatched builds or XBEs).
- **Guest hooks** (in `src/recomp_manual.c` / overrides, never in `gen/`): read the local rider's state each tick,
  overwrite puppet riders' state, start the race on the host's signal, report finish events, add the menu entry.
- **UI:** a mix of the game's own screens and our own overlay drawn by the Vulkan back end (`src/render/`):
  - The game's menus get a **LAN** entry (Multi Play becomes *Split Screen* / *LAN*; Multi Play must then also
    show with a single pad). Character select reuses the game's *Select Character* / *Setup Character* screens,
    including **Load game** to import leveled riders.
  - The lobby, chat, scoreboard, name entry (on-screen keyboard for pads, plain typing on a keyboard) and name tags
    are overlay UI in the same style, which saves rebuilding the game's frontend screen system for each new screen.
    Moving more of it into native game screens is possible later.
- **Name tags:** project each remote rider's head position with the game's current view-projection (from the
  camera object or the vertex-program constants the renderer already sees) and draw the name in the overlay.

## Risks to settle first (phase 0 spikes)

Status (2026-10-19): spike 1 done (6 slots hard-wired, addresses in notes.md), spike 2 in progress (on-rails
flag rider+0xA84 + pose at rider+0x180 found; body not yet following), spikes 3–7 open.


Each one is a short reverse-engineering spike with a yes/no result recorded in notes.md, before building on it:

1. **Rider manager:** where the 6 race slots live (arrays, counts, HUD "x/6", start grid), how AI and human
   riders differ (Tricky: `Player` vs `OtherRider`), and how many human slots the code allows. Sources: PS2
   decomp names (`ai/rider*.cpp`, `riderhandler`, `player.cpp`), Tricky RE notes, koen's addresses.
2. **Puppet hook:** can an AI rider's controller update be swapped for "write state from the network" without
   breaking animation, board trails, shadows, sound and position ranking?
3. **Race flow:** start/countdown, finish detection, results screen: how to start in sync and feed the results
   screen with network finish times.
4. **Free roam spawning:** can extra rider objects be created on the open mountain (no other riders exist
   there)? How much guest memory 11 more rider models/animations need (Xbox: 64 MB) and whether we can give the
   guest more.
5. **Frontend:** how menus and screens are built (koen's screen IDs: 35 Load game, 13 Rider Details, …) so a new
   menu entry can open our lobby and come back.
6. **Save import:** what *Load game* reads (rider stats, level, gear, outfits) so the lobby can send it on.
7. **Performance:** 12 riders each with their own shadow maps on the CPU-bound vertex interpreter. GPU vertex
   programs (koen's `src/vk/` has them) are probably needed before 12-rider free roam is playable.

## Phases

1. **Net core (no game needed):** `src/net/` with ENet, protocol and versioning, LAN discovery, direct IP,
   clock sync, the lobby state machine, plus a headless test client so 11 fake players can join from one machine.
2. **Lobby:** LAN entry in the Multi Play menu; host/join/browse lobbies; player profile with name, character,
   outfit/board and **import of a leveled rider** from the save; **ready check**, host **kick**, **text chat**
   (keyboard: free text; pad: quick messages); host rules (see below).
3. **Race, up to 6 players:** remote players take the AI slots; **AI fill-in** for empty slots; synchronized
   countdown; puppet riders; finish order and times from the host; the game's results screen fed with them;
   **session scoreboard** (points over several races, shown in the lobby between races).
4. **Name tags:** names over riders' heads, **options:** per-player colours, fade by distance, on/off,
   arrows for riders off screen.
5. **Free roam (Conquer the Mountain), 12 players from the start:** spawn puppet riders on the mountain; send
   rate by distance (nearby riders 30 Hz, far ones less often); **late join and reconnect**; chat in game.
6. **Collisions toggle:** ghost riders (default, no contact) or bumping/knockdowns. The rider that gets hit
   decides locally (it owns its state) and sends a knockdown event; the other side only plays it back.
7. **Spectators:** join a running race or free-roam session as a viewer and cycle the camera between riders.
8. **Direct IP / internet hardening:** packet loss, jitter, larger latency, timeouts (tested with `tc netem`);
   behind NAT this needs port forwarding or a VPN (no relay server planned).
9. **Freestyle events:** first 6 players, then 12 (sequential runs like the original, scores compared through
   the host).
10. **12-player races:** raise the engine's 6-rider limit (slot arrays, start grid, HUD position, results list,
    memory). Probably the hardest part; it depends on what spikes 1 and 4 find.

### Host rules (lobby settings)

- Mode: Race event / Conquer the Mountain (later: Freestyle event), peak and event.
- **Leveled riders allowed** or **everyone at level 1** (equal stats).
- **Bragging Rights** per player (the game's own handicaps: head start, full boost, fast recovery, no ubertricks,
  leaky adrenaline, level 1 character).
- **AI fill-in** on/off (race).
- **Collisions:** ghost / bumping.
- Max players, lobby name, spectators allowed.

## Testing

- Several game instances on one machine over loopback (needs per-instance save/system dirs, cf. koen's
  `RECOMP_SYSTEM_DIR` fix for parallel boots), driven by virtual uinput pads (one per instance) with
  `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`, at 1x/60 fps.
- The headless fake clients from phase 1 for lobbies with 6/12 players without 12 game instances.
- Cross-play: Linux host + Windows client under Wine/Proton (once races run there), latency/loss with `tc netem`.

## How hard is it?

Hard: this is new game code plus reverse engineering, not a port fix. Rough order of effort:

- Moderate: net core and lobby (phases 1–2): ordinary networking code we fully control and can test without the
  game.
- Hard: race with puppet riders (3): depends on hooking the AI riders cleanly; the 6-slot race already exists.
- Hard: free roam for 12 (5): spawning riders the game never placed there, memory, performance.
- Very hard: 12-player races (10): changing engine limits in recompiled code.

The phase 0 spikes decide most of the uncertainty. If spike 2 or 4 fails, the fallback is drawing remote riders
ourselves in the renderer (a copied rider mesh outside the game's object system), which loses animation detail.
