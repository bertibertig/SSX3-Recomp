# TODO – SSX3-Recomp Bring-up

Reihenfolge nach `CLAUDE.md` / `xboxrecomp/docs/GETTING_STARTED.md`. Details und Funde → `docs/notes.md`.

## Phase 1 – Pipeline (statische Recompilierung)
- [x] 0. `scripts/setup.sh` – venv + capstone 5.0.7 vorhanden
- [x] 1. XBE vorhanden (`game_files/default.xbe`, bereits entpackt)
- [x] 2. `run_pipeline.sh parse` → `game_files/default_analysis.json` (Name ist Pflicht); Werte in `docs/notes.md`
- [x] 2b. Konstanten in `src/main.c` eintragen (Entry Point, XBE-Pfad, Spielordner)
- [x] 3. `run_pipeline.sh disasm` – 17 017 Funktionen (16 791 in .text), 131 021 Xrefs, ~9,5 min
- [x] 4. `run_pipeline.sh funcid` – 9 161 GAME (5 217 vtable), 13 CRT, 0 RenderWare, 11 214 unbekannt
- [x] 4.5 `run_pipeline.sh abi` – 17 017 Einträge, 2 666 thiscall
- [x] 4.6 Ghidra-Namen: `scripts/ghidra.sh` (Linux-Wrapper) + Pipeline-Schritt `names` (`merge_names.py --apply`, automatisch nach disasm) → 143 Namen
- [x] 5. `run_pipeline.sh recomp` – 17 037/17 037 übersetzt, 0 Fehler, 1,37 Mio. Zeilen C in 69 Dateien (50 MB, größte 1,7 MB), 233 unaufgelöste Call-Ziele gestubbt, ~2 min
- [x] 6. `scripts/doctor.sh` – 0 ICALL-Ziele (statisch, ohne Laufzeit-Log), 40 unimplementierte Instruktionen
- [x] 6b. Unaufgelöste Call-Ziele 231 → 8: Ursache war `--text-only` (XDK-Sektionen D3D/D3DX/XGRPH/DSOUND/XPP fehlten) → `--extra-sections`
- [ ] 6b'. Die restlichen 8 Stubs sichten
- [ ] 6c. Unimplementierte Instruktionen prüfen: aaa/arpl/insb/outsd/verw/sldt/int1 = vermutlich Daten als Code (falsche Funktionen?); wbinvd/cli/sti/hlt/sgdt/ljmp = XDK/Kernel-nah, No-op ok

## Phase 2 – Build (Linux, SDL2 + OpenGL)
- [x] Linux-Build-Deps bereits vorhanden (cmake, gcc, clang, ninja, SDL2 2.32, GL, OpenSSL 3.6, ffmpeg)
- [x] `src/main.c` / `CMakeLists.txt` plattformübergreifend: gemeinsames `boot_and_run()`; Windows = WinMain + VEH + dbghelp,
      POSIX = `main()` + sigaction (SIGSEGV/SIGBUS, `xbox_PosixMmioFault`) + `xbox_HostWindowMain`, dladdr/backtrace im Crash-Report
      (Muster aus `xboxrecomp/docs/runtime/README.md` „POSIX hosts“). CMake: Win-Libs/`/bigobj` nur Windows, MinGW `-Wa,-mbig-obj`,
      POSIX Threads/dl/m + ENABLE_EXPORTS, GCC/Clang `-fno-strict-aliasing`
- [x] Windows-Build gegengeprüft: MinGW-Cross-Build unter Linux, läuft unter Proton (MSVC weiter ungetestet)
- [x] `scripts/build.sh` – kompiliert (GCC 16.2, ~1:40 min mit `JOBS=8`, ~0,4 GB RAM pro Compiler); Start: `scripts/run.sh`

## Phase 3 – Erster Start / Debugging-Schleife
- [x] Erster Start: 5 ICALL-Ziele + 3 Thread-Kontexte → 8 Seeds (`config/seed_functions.json`), neu generiert → keine ICALL-Fehler mehr
- [x] Crash in `SetEvent` (vom Titel gebaute KEVENTs) → `RECOMP_TITLE_KEVENTS=1` (in `scripts/run.sh` Standard)
- [x] Crash im D3D-Pushbuffer-Code: Device-Pointer NULL, weil `Direct3D_CreateDevice` scheiterte (abgeschnittene D3D-Funktionen) → XDK-Sektionen
- [x] Hänger `D3D_BlockOnFence` → `xbox_Nv2aMirrorFence(0x0024B0D8, 0x2C, 0x30)` in `src/main.c`
- [x] Hänger `BlockUntilVerticalBlank` → `RECOMP_VBLANK=1` (Standard in `scripts/run.sh`)
- [x] 3 neue ICALL-Ziele + 1 Thread-Kontext geseedet (12 Seeds) → NULL-Schleife weg, Boot lädt ~30 Asset-Dateien
- [x] DirectSound/APU: `RECOMP_AC97_READY=1` (Standard). Windows/Proton lief sofort; Linux brauchte xboxrecomp-Patch 0001
      (x86-64-MMIO-Decoder für ucontext: APU/AC'97/OHCI) + case-insensitive Pfade → beide Plattformen gleich weit
- [x] Windows-Build per MinGW-Cross-Compile (`scripts/build_win.sh`) + Proton-Start (`scripts/run_win.sh`)
- [x] DSP-Handshake: läuft automatisch (Runtime „DSP doorbell ack: auto“)
- [x] Grafik, erstes Bild: Speicherlayout vor dem SDL/GL-Fenster mappen (`boot_memory()`), deaktivierte Vertex-Attribute
      im Executor (Runtime-Patch) → EA BIG, THX, Intro-Video sichtbar (CPU-Rasterizer + `RECOMP_FB_WINDOW`)
- [x] Boot-Logos überspringbar gemacht: `SSX3_SKIP_LOGOS=1` (Standard), `SSX3_SKIP_INTRO=1`
- [x] USB-Controller: `xbox_OhciInit()` aufrufen; XPP-Callbacks 0x00281A82, 0x0028101D (Laufzeit) + 4 statisch aus den
      XPP-Zeigertabellen geseedet (19 Seeds)
- [x] 19 Seeds → Tastendruck funktioniert: Intro überspringen, Titelbild, Hauptmenü, Charakterauswahl erreicht
- [ ] **Morgen zuerst:** Linux-Performance (1,5 fps vs. 7–13 fps unter Proton) – Details/Hypothesen in notes.md
      (letzter Eintrag): usleep-Poll in `WaitForMultipleObjectsEx`, Zeitbasis/Timer, Kickoff-Busy-Bit
- [ ] Danach: ein Rennen starten (Charakter wählen → Strecke), Ingame-Grafik prüfen
- [ ] Fence: Mirror durch `nv2a_pb_set_semaphore_target` ersetzen, sobald Desyncs/Grafikfehler auftreten
- [ ] Performance: CPU-Rasterizer ist langsam → später Render-Back-End (`nv2a_backend.h`, OpenGL/D3D11)
- [ ] `t:\$u\contentmeta.xbx` fehlt beim Boot – prüfen, ob relevant
- [ ] xboxrecomp-Patch 0001 als Upstream-PR anbieten (sp00nznet/xboxrecomp)
- [ ] Meilensteine: EA-Logo → Titelbildschirm → Menü → Rennen
- [ ] Renderer NV2A → OpenGL/D3D11 prüfen; OpenTricky-/SSX-Tricky-PC-Änderungen an xboxrecomp vergleichen

## Offen
- Welche XBE ist die Basis? `default.xbe` = Original + Region/Media-Patch + 1-Byte-Codepatch (siehe notes.md).
