# Arbeitsprotokoll SSX3-Recomp

## Spiel-Version
| Feld | Wert |
|---|---|
| Region / Version | PAL/EU (Build-Pfad `d:\ssx\code\xbox_eu\obj\releasedvd\ssx.exe`, Original-Region-Flag 0x4 = PAL), Build 2003-09-19 |
| default.xbe SHA-1 | `409141774025fce02ab4743d1e669d35bb1d190e` (gepatcht, s. u.); Original `default.xbe_orig`: `ba876cc11ad38650eac9831b1c7f8c4dbf7c5ba9` |
| Titel-ID | 0x4541003C ("EA" 0x3C) |
| Basisadresse | 0x00010000, Image-Größe 3 864 448 |
| Entry Point | 0x00181AD2 (in `src/main.c` eingetragen); Kernel-Thunk 0x00281C40; TLS-Index 0x003A1C00 |
| .text-Größe / Funktionen | 2 201 308 B / 17 017 Funktionen (16 791 .text, 162 D3D, 39 DSOUND, 13 XPP, 10 XGRPH, 2 D3DX) |
| XDK-Version | 5558 (XAPILIB, D3D8, D3DX8, XGRAPHC, XBOXKRNL, LIBC, DSOUND – alle 1.0.5558) |
| Kernel-Imports | 159 |

## Sektionen (xbe_parser)
| Name | VA | VSize | Flags |
|---|---|---|---|
| .text | 0x00011000 | 2 201 308 | X |
| D3D | 0x0022A6E0 | 143 872 | W X |
| D3DX | 0x0024D8E0 | 12 920 | W X |
| XGRPH | 0x00250B60 | 10 356 | W X |
| DSOUND | 0x002533E0 | 156 572 | W X |
| XPP | 0x00279780 | 33 960 | W X |
| .rdata | 0x00281C40 | 497 840 | RO |
| .data | 0x002FB500 | 748 024 (raw 300 136 → BSS-Anteil) | W |
| DOLBY | 0x003B1F00 | 29 056 | |
| $$XTIMAGE / ABRTFONT / .XTLID … | ab 0x003B9080 | | Ressourcen |

## XBE-Varianten in game_files/
Alle drei 3 428 352 Bytes, Unterschiede gegen `default.xbe_orig`:
- `default.xbe`: Zertifikat Media-Flags 0x2 → 0xC00001FF, Region 0x4 → 0x7 (region-/media-free) **plus 1 Codebyte**
  an Datei-Offset 0x1740F3 = VA 0x001840F3: `test eax,eax; jge +5` (0x7D) → `jmp +5` (0xEB) direkt nach einem Call
  in der XAPI-Prozess-Initialisierung (nach `HalRegisterShutdownNotification`): Ergebnis von `sub_00183EBB` < 0
  führte ursprünglich in einen Fehlerpfad (0x001840F5 → 0x0018411B), jetzt immer weiter nach 0x001840FA. Typischer HDD/Softmod-Patch.
- `default_ffp.xbe`: wie `default.xbe` + 12 Bytes in der D3D-Sektion (VA ≈ 0x0022B5BB / 0x0022CFE2), vermutlich
  Flicker-Filter/Progressive-Patch – für den Port irrelevant, da D3D ersetzt wird.
- Pipeline läuft auf `default.xbe`. Der Codepatch ist für den Recomp harmlos/vermutlich sogar hilfreich; falls
  Verhalten abweicht, gegen `default.xbe_orig` gegenprüfen.

## Checkliste
- [x] `scripts/setup.sh` ausgeführt (venv, capstone 5.0.7) – Linux-Deps noch offen
- [x] ISO entpackt, XBE vorhanden
- [x] `parse` ausgeführt, Werte oben + in `src/main.c` eingetragen
- [x] disasm / func_id / **abi_analysis** / recomp erfolgreich
- [x] `scripts/doctor.sh` gelesen
- [x] Erster Build kompiliert (Linux, GCC 16.2, 1:38 min mit 8 Jobs, `build/ssx3_recomp` 20 MB)
- [ ] Erster Start: bis EA-Logo / Titelbildschirm / Menü / Rennen
- [ ] Renderer (NV2A → D3D11 bzw. OpenGL) prüfen
- [ ] Vergleich mit OpenTricky-Änderungen an xboxrecomp

## Offene Fragen
- Ähnlichkeit SSX3-Engine ↔ SSX Tricky: welche Teile (Archiv-/Modellformate, Boot, Renderer) lassen sich übernehmen?
- Linux-Build (OpenGL) vs. Windows-Build (D3D11): welcher Weg zuerst?

## Log
_(Datum – was getan, was gefunden, nächster Schritt)_

- 2026-10-17 – Repo angelegt, xboxrecomp v0.14.0 als Submodul, Skripte, Referenz-Repos geholt.
- 2026-10-17 – parse: Werte oben eingetragen. `run_pipeline.sh` schrieb `ssx3_analysis.json`, disasm erwartet aber
  `<xbe-stem>_analysis.json` neben der XBE → Skript auf `default_analysis.json` umgestellt. TODO-Liste: `docs/TODO.md`.
- 2026-10-17 – Pipeline komplett (Logs in `logs/`, gitignored):
  - disasm 570 s: 17 017 Funktionen, 131 021 Xrefs (37 558 call, 558 kernel_call). Top-Kernel: NtClose, RtlLeaveCriticalSection.
  - func_id: 9 161 GAME (8 588 vtable), 13 CRT, **0 RenderWare** (SSX3 nutzt eigene EA-Engine), 11 214 unbekannt.
  - abi_analysis: 17 017 Einträge, 2 666 thiscall.
  - recomp (`--split 250`): 17 037/17 037, 0 Fehler, 1 374 816 Zeilen, 69 Dateien, 233 unaufgelöste Call-Ziele, 40 unimpl. Insns.
    Kein setjmp/longjmp gefunden; SEH-Helper `__SEH_prolog` 0x00213F68 / `__SEH_epilog` 0x00213FA3.
  - doctor: einziger Punkt "40 unimplemented instructions" (siehe TODO 6c).
  - Ghidra-Namen (4.6): `scripts/ghidra.sh` ersetzt das Windows-only `run_ghidra.sh` (analyzeHeadless statt .bat, venv-python statt `py -3`,
    kein cygpath). Ghidra 12.1.2 (`/opt/ghidra`) bringt Jython nur als Extension-Zip mit → Skript entpackt es einmalig nach
    `~/.config/ghidra/ghidra_12.1.2_DEV/Extensions/Jython` (die xboxrecomp-Skripte sind `@runtime Jython`). Heap 8G.
    Lauf 2026-10-18: ~5 min, Ghidra 9 827 Funktionen / 78 789 Symbole; `merge`: 388 Namen (143 FidDb/CRT, 245 String-Labels),
    143 treffen Recompiler-Funktionsstarts. Noch nicht mit `--apply` übernommen.
    Triage ohne GUI: `scripts/ghidra.sh classify <VA…>` (read-only, ~30 s), z. B. 0x0024B0D8 = DATA, 0x001EA662 liegt in FUN_001EA590.
  - `_common.sh`: `PYTHONUNBUFFERED=1`, sonst bleiben umgeleitete Logs minutenlang leer.
  - Nächster Schritt: Phase 2 – `main.c`/CMake für Linux (SDL2/GL) anpassen, erster Build.
- 2026-10-17 – **Host plattformübergreifend** (`src/main.c`, `CMakeLists.txt`): gemeinsames `boot_and_run()`;
  Windows: WinMain + VEH + dbghelp (unverändert); POSIX: `main()` + sigaction(SIGSEGV/SIGBUS, SA_ONSTACK) →
  `xbox_PosixMmioFault`, sonst Crash-Report mit dladdr + backtrace; `xbox_HostWindowMain(boot_and_run)` (Fenster mit
  `RECOMP_FB_WINDOW`). Template-Bug: `recomp_dispatch_init` war nicht deklariert (MSVC warnt nur, GCC ≥14 Fehler).
  `_GNU_SOURCE` nötig für dladdr/REG_RIP. CMake: `-fno-strict-aliasing` (GCC/Clang), ENABLE_EXPORTS, MinGW `-Wa,-mbig-obj`.
  Windows-Build ist **ungetestet** (kein Windows hier).
- 2026-10-17 – **Erster Start** (`logs/09_run1.log`): Boot läuft durch Memory-Layout (64 MB @ Offset, Heap 48 MB, Stack
  8 MB @ 0x00780000), 159/159 Kernel-Thunks, XAPI-Init (DVD-Auth, TDATA/UDATA/4541003c, TitleMeta.xbx, Partition-Reads),
  `t:\$u\contentmeta.xbx` fehlt (ERROR_PATH_NOT_FOUND – unkritisch?).
  - 5 unaufgelöste ICALLs: 0x0027AC53 (XPP), 0x0025AF68/73, 0x0025EA0D/18 (DSOUND) = C++-Static-Initializer
    (`mov [global], imm; ret`), per `_initterm` aufgerufen. + 3 Thread-Kontexte (0x0017ACE0, 0x00181A5E, 0x0021C1B5).
    → `tools.seed_from_log` → `config/seed_functions.json` (8 Seeds, alle per Disassembly geprüft).
    `run_pipeline.sh` gibt die Seeds jetzt automatisch an disasm weiter.
  - Crash in nativem `SetEvent`/`pthread_mutex_lock`: Titel baut seine KEVENTs selbst (Header direkt geschrieben, z. B.
    0x00F7F0D8 auf dem Hauptstack, Thread 0x0021C1B5 setzt es). Ohne Shadow geht die Gast-VA als HANDLE an Win32 –
    unter Windows schlägt SetEvent still fehl, unter POSIX (`win32_compat.c`) wird dereferenziert → SIGSEGV.
    **Fix: `RECOMP_TITLE_KEVENTS=1`** (Runtime-Opt-in). (Upstream-Kandidat: win32_compat sollte ungültige Handles ablehnen.)
- 2026-10-17 – **Zweiter Start** mit `RECOMP_TITLE_KEVENTS=1` (`logs/11_run2_kevents.log`, noch ohne Seeds): weiter bis
  4 Threads/3 NtCreateEvents, dann Crash im **XDK-D3D-Pushbuffer-Code** `sub_00234B30` (D3D-Sektion, schreibt NV2A-Methoden
  0x4A310/0x41D70/0x41D90 – Fence/Kickoff). Put-Pointer aus `*(*(0x0024B0D8))` = 0xF6808009 (unaligned, außerhalb jeder
  Apertur). 0x0024B0D8 liegt im BSS-Teil der D3D-Sektion → wird bei der D3D-Device-Init gesetzt. Aufrufer: Spielcode
  0x001EA662 → 0x0022E8CF → 0x00234F00 → 0x00234C0F → 0x00234B30.
  SSX3 linkt die XDK-D3D statisch → richtiger Weg ist der **Pushbuffer-Executor** (`RECOMP_PB_SCAN`/`RECOMP_PB_EXEC`,
  `xboxrecomp/docs/technical/pushbuffer-executor.md`), nicht die D3D8-HLE-Schicht. Nächster Schritt: herausfinden, wer
  0x0024B0D8 bzw. den Pushbuffer-Put initialisiert (Ghidra/REA-Xrefs), und mit `RECOMP_PB_SCAN=1` erneut starten.
- 2026-10-18 – **D3D-Crash analysiert** (Ghidra-Projekt aus `scripts/ghidra.sh`, neue Abfragen `scripts/ghidra.sh xrefs|decomp`,
  gdb-Breakpoints auf die generierten `sub_*`-Symbole; `RECOMP_KERNEL_LOG_BUDGET=2000` für den vollen Kernel-Trace):
  - Im Crash ist `esi=0`: `sub_00234B30` lädt als Erstes `esi = [0x0024B0D8]` → **Device-Pointer NULL**; 0xF6808009 ist nur
    der Inhalt von Xbox-VA 0. Der einzige Schreiber von 0x0024B0D8 ist `FUN_0023F720` (= `Direct3D_CreateDevice`, statisches
    Device bei 0x0024B0E0; setzt den Pointer im Fehlerfall wieder auf 0). Einziger Aufrufer: Spielcode 0x001AE1F4 in
    `FUN_001AE160` – der Rückgabewert wird ignoriert, das Spiel zeichnet weiter.
  - Device-Layout (aus `FUN_00234680`, Pushbuffer-Init): +0x00 Put, +0x04 Limit, +0x24 Basis, +0x28 Ende, +0x2C Ref/Fence,
    +0x30 Fence-Seite, +0x44 Fence-Ring; Pushbuffer 0x80000 B (`DAT_0024D8D4`), Kickoff 0x8000 B (`DAT_0024D8D0`).
    `FUN_00184212` = XPhysicalAlloc-artig (Attr < 0 → `MmAllocateContiguousMemoryEx` über `FUN_0018915d`).
  - gdb: `sub_0023F720` → `sub_0023A840` (Device-Init) → sofort `sub_0023AD10` (Cleanup); `sub_0023A2A0` wird nie erreicht,
    und es gibt nur 2 Contiguous-Allokationen (4 KB, wieder freigegeben, und 45 MB vom Spiel) – der Pushbuffer wird nie angelegt.
  - **Ursache:** `--text-only` – die XDK-Sektionen D3D/D3DX/XGRPH/DSOUND/XPP werden nicht linear durchsucht. Der Recompiler
    kennt `sub_0023A840` nur bis 0x0023A86E; der Rest ist ein leerer Stub (`recomp_stubs_unresolved.c`: „0x0023A86E: not
    detected“) → Init liefert Müll zurück → CreateDevice schlägt fehl. Auch `0x00234620`/`0x00234680` fehlten ganz
    (keine Funktion zwischen 0x002344E0 und 0x00234720). Viele der 231 Stubs liegen bei 0x0023…/0x0025….
  - **Fix:** `run_pipeline.sh` gibt disasm `--extra-sections D3D,D3DX,XGRPH,DSOUND,XPP` (DOLBY nicht: DSP-Code).
    Gleichzeitig Schritt 4.6: neuer Pipeline-Schritt `names` (`merge_names.py --apply`) läuft nach jedem disasm automatisch.
- 2026-10-18 – **Nach dem XDK-Sektions-Fix** (`logs/16_pipeline_xdk_names.log`): 18 599 Funktionen (D3D 712, D3DX 87,
  DSOUND 789, XGRPH 49, XPP 240; vorher 162/2/39/10/13), unaufgelöste Call-Ziele 231 → **8**, 143 Ghidra-Namen übernommen.
  Kein Crash mehr: `Direct3D_CreateDevice` läuft komplett (Pushbuffer 2 MB @ 0x80002000, Framebuffer 2400+1200 KB,
  `KeConnectInterrupt` Vektor 3 → ISR 0x0023B290).
  - Hänger 1: `D3D_BlockOnFence` (0x00234BE0) wartet, bis das GPU-Fence-Wort `*(dev+0x30)` den Zähler `dev+0x2C` erreicht –
    niemand führt den Pushbuffer aus. **Fix:** `xbox_Nv2aMirrorFence(0x0024B0D8, 0x2C, 0x30)` in `src/main.c`
    („always caught up“; nur gültig ohne `RECOMP_PB_EXEC`, sonst `nv2a_pb_set_semaphore_target`).
  - Hänger 2: `D3DDevice_BlockUntilVerticalBlank` (0x0022DF20) wartet auf Event `dev+0x1DAC` (0x0024CE8C), das der
    VBlank-DPC setzt. **Fix:** `RECOMP_VBLANK=1` (Runtime-60-Hz-VBlank; ISR „claimed it“) – in `scripts/run.sh` Standard.
  - Danach: erstes Spiel-Asset geöffnet (`D:\data\ui\mapgfx.big`), >100 000 ICALLs. Dann 3 neue ICALL-Fehler: 0x00279960,
    0x0027A16F (XPP, USB-Host-Init, Pool-Tag 'USBH'), 0x002000E0 (.text, Ghidra: Funktionsstart; disasm: Alias in
    sub_00200041) + Thread-Kontext 0x001AE3B0 → Seeds (jetzt 12). Folge-Hänger: Endlosschleife über NULL-Funktionszeiger
    (193 Mio. ICALLs nach Ziel 0) – vermutlich weil die fehlenden Initialisierer nie liefen.
  - Hilfsmittel: `scripts/ghidra.sh xrefs|decomp` (eigene Skripte in `scripts/ghidra/`), gdb mit Python-Timer
    (Prozess nach N s anhalten, `thread apply all bt`), `RECOMP_KERNEL_LOG_BUDGET`, `RECOMP_WATCHDOG_SECS`.
- 2026-10-18 – **Mit 12 Seeds** (`logs/25_run8.log`): NULL-Schleife weg. Boot lädt jetzt `ui\mapgfx.big`, `audio\{music,music2,
  audio,speech}.big`, `scripts\scdat.big`, alle `char\*txx.big` + `mdlxbx.big`, `config\input*.map`, `fonts\{fe,hud}font.ffn`,
  `char\boltxbx.dat`, `be\rwrdxbx.dat`, `be\charDB.dbl`, `locale\cmnamer.loc` – dann Crash in DirectSound:
  - `IDirectSound_CreateSoundBuffer` (0x00258FFD) → 0x00258A38 → 0x00258830 → Buffer-Voice-Ctor `FUN_00260DCD` → `AddRef` auf
    `*(*(buffer+8)+0xC)` = **0xFFFFFFFF** → DirectSound-Objekt nie fertig initialisiert (`ebx=0x8007000E` ist nur die
    Konstante aus `(p ? S_OK : E_OUTOFMEMORY)`). Ursache laut `xboxrecomp/docs/pipeline/06-debugging.md` („Runs, Never Draws
    a Frame“): XDK-DirectSound prüft den AC'97-Codec, ohne `RECOMP_AC97_READY=1` scheitert die Init.
  - Mit `RECOMP_AC97_READY=1` (`logs/26_run9_ac97.log`): emulierte APU startet (SDL2-Audio), APU-Apertur 0xFE800000..0xFE830000
    wird für MMIO getrappt, Codec meldet bereit. Crash dann in `sub_00263DB4` beim **Lesen des APU-Registers 0xFE801100**:
    Der Trap feuert, aber `xbox_PosixMmioFault` hat **nur auf arm64 einen Decoder** – auf Linux x86-64 lehnt er ab → SIGSEGV.
    Der x86-64-Decoder (`xboxrecomp/src/platform/mmio_decode.h`) und `apu_hook_handle_mmio` sind auf `_WIN32` beschränkt
    (arbeiten auf `CONTEXT`). Unter Windows sollte dieser Schritt funktionieren. → Lücke in der Runtime, nicht im Spiel.
    Zusätzlich warnt die Runtime: „AC97: bus-master write trap not available on this host; a channel reset will spin“.
  - Danach zu erwarten (Kommentar in xbox_memory_layout.c): DirectSound übergibt dem Audio-DSP einen Befehlsblock und wartet,
    bis er gelöscht wird; die DSPs sind Stubs → ggf. `RECOMP_DSP_ACK=<va>`.
- 2026-10-18 – **Windows-Build unter Proton** (`scripts/build_win.sh` = MinGW-Cross-Build mit xboxrecomps
  `cmake/mingw-w64-x86_64.cmake`, `scripts/run_win.sh` = umu-run/GE-Proton, Arbeitsverzeichnis Repo-Root, eigener Prefix
  `~/.local/share/ssx3-recomp-proton`). Erst Build-Fehler: Ghidra-Namen `fpclass`/`chgsign` kollidieren mit MinGW-`math.h`
  (MSVC-„Old Names“) → Lifter-Reserved-Liste ergänzt. Mit `RECOMP_AC97_READY=1` (`logs/30_win_run1.log`): APU auf XAudio2,
  AC'97-Trap, DSP-Doorbell auto-quittiert, APU-Interrupts „claimed“, Spiel öffnet `movies\eabig.mad`, `thx.mad`,
  `intro_dj.mad` – **Ton der Intro-Videos hörbar** (Nutzer bestätigt), kein Bild (Pushbuffer wird nicht ausgeführt).
  → Diagnose bestätigt: Unter Linux fehlte nur der x86-64-MMIO-Decoder.
- 2026-10-18 – **xboxrecomp-Patch 0001** (`patches/xboxrecomp/`, Details dort): x86-64-MMIO-Decoder für Linux-ucontext
  (APU, AC'97, OHCI), case-insensitive Pfadauflösung auf POSIX (`intro_dj.mad` vs. `Intro_DJ.mad`), MinGW-Old-Names im
  Lifter. Linux (`logs/35_linux_run_case.log`) erreicht jetzt denselben Stand wie Proton. `RECOMP_AC97_READY=1` ist in
  `run.sh`/`run_win.sh` Standard. Decoder-Test: `tests/mmio_decode_x64_linux.c`.
  Nächster großer Block: **Grafik** – Pushbuffer-Executor (`RECOMP_PB_EXEC=1`) + Fenster (`RECOMP_FB_WINDOW=1`), dabei den
  Fence-Mirror durch `nv2a_pb_set_semaphore_target` ersetzen. Videos sind EA-MAD (Software-Decoder im Spiel, landet als
  Textur im Pushbuffer).
- 2026-10-18 – **Grafik: erstes Bild.**
  - `RECOMP_FB_WINDOW` (SDL/GL) initialisierte den Grafiktreiber vor dem Speicherlayout → Mesa belegte genau die Adresse
    des Contiguous-Fensters (Gast-Offset + 0x80000000) → `failed (error 8)`, Crash in `sub_00184BEE`. Fix in `src/main.c`:
    `boot_memory()` (XBE laden + Layout) läuft vor `xbox_HostWindowMain`, der Rest (`boot_and_run`) auf dem Gast-Thread.
  - `RECOMP_PB_EXEC=1`: 264 Draws, aber alle Dreiecke „degenerate/NaN“. `RECOMP_VSH_TRACE`/`RECOMP_VSH_DUMP`: Das
    FMV/2D-Programm (`dp4` gegen v8..v15) liest seine Matrix aus **deaktivierten Vertex-Attributen**, gesetzt per
    `SET_VERTEX_DATA4F_M` (0x1A80..0x1AFC); der Executor lieferte für deaktivierte Attribute (0,0,0,1) → jede Ecke
    (640, 0, 16777215, 1). Fix im Runtime-Patch (nv2a_pb_exec.c). Danach: EA SPORTS BIG, THX und das Intro-Video
    korrekt im Fenster und in `RECOMP_FB_DUMP`-Bildern. Videoframe = Textur 0x823BCA00, 640x480, Format 0x24 linear.
  - Fence-Mirror bleibt vorerst (läuft mit dem Executor im Segment-Scan-Modus).
- 2026-10-18 – **Eingabe.** `xbox_OhciInit()` wurde nie aufgerufen (Template ruft es nicht) → kein USB-Controller →
  `RECOMP_KEYBOARD`/`RECOMP_PAD_LIVE` wirkungslos. Aufruf in `boot_and_run` ergänzt. Danach Enumeration bis
  `SET_CONFIGURATION(1)`, dann `[ICALL] Failed to resolve VA 0x00281A82` (XID-Klassentreiber-Callback in XPP, folgt auf
  `ret 8`) → Seed (jetzt 14). Tastenbelegung (xboxrecomp `src/input/keyboard.c`): Enter=Start, Backspace=Back,
  Z/X/C/V=A/B/X/Y, Pfeile=D-Pad, WASD=linker Stick, IJKL=rechter Stick, 1/3=Trigger, Q/E=Weiß/Schwarz.
- 2026-10-18 – **Boot-Videos.** `FUN_00080730`: `DAT_003083B8` = Bitmaske der Boot-Videos (Bit0 EA BIG, Bit1 THX, Bit2
  Intro; .data-Init 7), Skip-Flag `this+0xB5A94` nur fürs Intro gesetzt, Skip-Tasten 0x70/0x7A. Option in `src/main.c`:
  `SSX3_SKIP_LOGOS=1` (Standard in den Startskripten) löscht Bit0/1 vor dem Start, `SSX3_SKIP_INTRO=1` auch Bit2.
  `run.sh`/`run_win.sh` setzen jetzt standardmäßig PB_EXEC, FB_WINDOW, USB, KEYBOARD, SKIP_LOGOS.
- 2026-10-18 – **Front end erreicht.** Mit 19 Seeds (XPP-Callbacks) funktioniert die Eingabe: `RECOMP_PAD_LIVE`-Tasten
  überspringen das Intro, dann Titelbild „Press START“ → „Checking hard disk“ → Hauptmenü (Single Event / Conquer The
  Mountain / Multi Play / Previews) → Charakterauswahl mit 3D-Fahrermodellen. Alles korrekt gerendert (CPU-Rasterizer).
- 2026-10-18 – **Offen: Performance unter Linux.** Titel/Menü: Linux **~1,5 fps**, Windows-Build unter Proton **7,6–13 fps**
  (Proton-Fenster kleiner, laut Nutzer). Gleiche Runtime + gleicher Spielcode → Linux-spezifisch. Ausgeschlossen:
  USB (ohne `RECOMP_USB` gleich langsam), SDL-Fenster/vsync (ohne `RECOMP_FB_WINDOW` gleich), Rasterizer (NV2A-Thread
  in 40/40 Stichproben idle in `sched_yield`). gdb-Stichproben: **alle Threads warten** – Hauptschleife
  (`sub_001AE3C0`) abwechselnd im D3D-Kickoff-Spin (`sub_00234A10`, Busy-Bit 0x100410) und in `NtWaitForSingleObjectEx`
  (`sub_00181E10` = WaitForSingleObject), Worker `sub_0017ACE0`/`sub_0017BB30` in `NtWaitForSingleObjectEx`, XDK-Thread
  `sub_0021C1B5` in `KeWaitForMultipleObjects` (POSIX: `WaitForMultipleObjectsEx` pollt mit `usleep(1000)`), mit Fenster
  zusätzlich `D3DDevice_BlockUntilVerticalBlank` (`sub_0022DF20`). strace (`clock_nanosleep`, letzte ~15 s): 17 484×
  1 ms (der usleep-Poll), dazu Schlafzeiten 4/8/14/24/28/34/44/54 ms (aus Gast-`Sleep`/Timer). Hypothesen für morgen:
  (1) Latenz der 1-ms-Poll-Schleife in `WaitForMultipleObjectsEx` – durch echtes Condvar-Warten ersetzen;
  (2) Zeitbasis/Takt unter Linux (KeQueryPerformanceCounter/-Frequency, KeQuerySystemTime, Timer-DPCs mit `Sleep(10)` im
  Timer-Thread) → Spiel schläft zu lange; (3) Kickoff-Spin wartet auf das Busy-Bit, das der Poll-Thread nur alle 256 Worte
  löscht. Werkzeuge: `RECOMP_KERNEL_LOG_BUDGET`, gdb mit SIGSTOP-Timer (Skripte in der Session), `strace -f -tt -T`
  (nur als Startprozess, `ptrace_scope=1`).
- 2026-10-18 – **Performance, Teil 1.** Mit `perf` (`perf record -e cycles:u -g --user-callchains`, paranoid=2):
  Rasterizer-gebunden. NV2A-Thread rendert (`nv2a_rc_eval`, `sample_tex`, `read_rgb`, `raster_xf_triangle`), Hauptthread
  spinnt im D3D-Kickoff `sub_00234A10`, ein zweiter D3D-Thread in `sub_0023B490` (beide warten auf die GPU). Die
  gdb-SIGSTOP-Stichproben von gestern waren irreführend (trafen nur Wartezustände). Ursachen des Linux-Rückstands:
  (1) Rasterizer-Thread-Pool war `_WIN32`-only → POSIX-Pool mit pthreads (Runtime-Patch); (2) die fps-Anzeige nutzte
  `clock()` = CPU-Zeit → Linux-Werte ~3× zu niedrig (gestriges „1,5 vs. 13 fps“ war teils Messartefakt; real 4,4 fps).
  Titelbild real: 1 Thread 4,4 fps → 12 Threads **15,4 fps**; `-march=x86-64-v3` nur +8 % (16,7). Alle 12 Raster-Threads
  gleichmäßig ausgelastet → für 60 fps braucht es ein GPU-Render-Back-End (Empfehlung OpenGL zuerst, Vulkan später).
