# reference/

Local, shallow read-only copies of third-party projects (not part of this repo, via `.gitignore`).
Create/update with `scripts/fetch_references.sh`.

| Folder | Project | Use |
|---|---|---|
| SSX-Tricky-PC | MatiasRiveraC/SSX-Tricky-PC | xboxrecomp port of SSX Tricky (Xbox) incl. RE notes in docs/notes/ |
| OpenTricky | GiZcesi/OpenTricky | Continuation with improved D3D11 renderer, launcher, ultrawide |
| ssx3-ps2-decomp | ssxdecomp/ssx3 | Matching decomp of SSX3 (PS2, NTSC-U) as a name/struct reference |
| SSX3-recomp-koen | koenhendriks/SSX3-recomp | Independent xboxrecomp port of **the same game** (SSX 3 Xbox PAL, same XBE build): Windows/MSVC-first, playable, Vulkan renderer, `CLAUDE.md` = detailed bring-up log with addresses |

Mind the licenses (OpenTricky: GPL-3.0-only; SSX3-recomp-koen: GPL-2.0-or-later, its `patches/xboxrecomp-local.patch`
MIT/LGPL-2.1 like the toolkit; xboxrecomp: MIT; xemu parts: LGPL-2.1+). Facts from their notes (addresses, causes,
mechanisms) can be used freely – verify them against our XBE. Copying code or text means GPL obligations for this repo.
