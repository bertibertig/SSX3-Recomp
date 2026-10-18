# SSX3-Recomp – Anweisungen für Claude Code

Ziel: **SSX 3 (Original-Xbox, 2003)** mit [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
statisch nach C recompilieren und als nativen PC-Port (x86-64) lauffähig machen.
Kein Emulator: der Spielcode wird einmal in C übersetzt, Kernel/GPU/Audio/Input ersetzt die xboxrecomp-Runtime.

## Warum die Xbox-Version
Xbox-Code ist bereits x86 (keine MIPS/PowerPC-Übersetzung nötig). Für den Vorgänger **SSX Tricky (Xbox)** gibt es
fertige, funktionierende xboxrecomp-Ports – das ist unsere Vorlage (siehe `reference/`).
PS2 (PS2Recomp: GS/VU1/DMA noch Stubs) und GameCube (DolRecomp: nur CPU) sind deutlich weiter weg.

## Repo-Layout
```
xboxrecomp/        Git-Submodul (sp00nznet/xboxrecomp): Python-Pipeline (tools/) + Runtime-Libs (src/). NICHT hier editieren,
                   Änderungen gehören in src/ (eigener Code) oder werden bewusst als Patch dokumentiert.
src/main.c         Host-Einstieg (aus templates/new-game). TODO: Konstanten aus xbe_parser eintragen (siehe unten)
src/recomp_manual.c  Handgeschriebene Overrides / Stubs für Funktionen, die nicht korrekt lifted werden
src/recomp/gen/    GENERIERTER Code (gitignored, aus eigener XBE) – nie committen, nie von Hand editieren
config/            Eigene Konfig, z. B. seed_functions.json (Einsprungpunkte, die die statische Analyse nicht sieht)
game_files/        Entpackte eigene Disc (gitignored): default.xbe + Daten
reference/         Lokale Lese-Kopien (gitignored): SSX-Tricky-PC, OpenTricky, ssx3-ps2-decomp
scripts/           setup.sh, extract_iso.sh, run_pipeline.sh, doctor.sh, build.sh, fetch_references.sh, ghidra.sh (Ghidra-Namen/Triage, Linux)
docs/notes.md      Laufendes Arbeitsprotokoll (Adressen, Funde, Entscheidungen) – hier ständig nachtragen
```

## Workflow (Reihenfolge einhalten)
1. `scripts/setup.sh` – venv + capstone; Linux-Build-Deps: `bash xboxrecomp/tools/linux/install_deps.sh` (sudo)
2. `scripts/extract_iso.sh <SSX3-Xbox.iso>` → `game_files/default.xbe`
3. `scripts/run_pipeline.sh parse` – XBE-Header: **Entry Point, Basisadresse, Sektionen, Titel-ID, Kernel-Imports in docs/notes.md notieren**
   und in `src/main.c` (`YOUR_GAME_ENTRY_POINT`, `YOUR_GAME_XBE_PATH`, `YOUR_GAME_DIR`) eintragen.
4. `scripts/run_pipeline.sh` (alles) – disasm → func_id → **abi_analysis (nie überspringen!)** → recomp (`--split 250`, nicht 1000: RAM/OOM).
5. `scripts/doctor.sh` – zeigt, was noch ungelöst ist (unresolved ICALLs, Stubs, ABI-Lücken).
6. `scripts/build.sh` → erster Start **wird abstürzen**. Das ist normal: stderr lesen, ICALL-Trace auswerten,
   Overrides in `src/recomp_manual.c`, Seeds in `config/`, dann wiederholen.
Die Anleitung dazu: `xboxrecomp/docs/GETTING_STARTED.md`, Debugging: `xboxrecomp/docs/pipeline/06-debugging.md`,
`xboxrecomp/docs/technical/indirect-calls.md`, `lessons-learned.md`.

## Plattform-Hinweis (wichtig)
- Das Template (`src/main.c`, `CMakeLists.txt`) ist **Windows-first** (WinMain, VEH, D3D11, dbghelp).
- Die Pipeline (Schritte 2–5) läuft überall (Python + capstone).
- Linux/macOS: die Runtime baut mit SDL2 + OpenGL 3.3 (`d3d8_gl.c`), laut Doku **weniger vollständig** als D3D11.
  Für Linux muss `main.c`/CMake angepasst werden – prüfe `xboxrecomp/README.md`, `src/` (win32_compat), und wie
  `reference/SSX-Tricky-PC/port/` bzw. NFSU2-Linux es lösen, bevor du etwas erfindest.
- Wenn die Linux-OpenGL-Route hakt: unter Windows (MSYS2 UCRT64 oder VS2022) bauen, wie `reference/*/docs/building.md`.

## Referenzen (zuerst lesen, bevor du etwas neu erfindest)
- `reference/SSX-Tricky-PC/` (MatiasRiveraC) und `reference/OpenTricky/` (GiZcesi): xboxrecomp-Ports von **SSX Tricky (Xbox)**.
  Enthalten NV2A→D3D11-Renderer, Register-Combiner/Vertex-Programs als Shader, Kernel/Timer/Audio-Anpassungen,
  Launcher, Fix-up-Pässe (`port/tools/regen.sh`), `port/src/seeds.json` und **viele RE-Notizen** in `docs/notes/`
  (Archivformate, XBD/XSH/FFN, Render-System, Boot-Sequenz, Kamera, Scoring …). SSX3 ist ein Nachfolger derselben Firma/Engine-Linie –
  Ähnlichkeiten sind plausibel, aber **jeweils verifizieren**, nicht annehmen (SSX3 wurde stark neu konzipiert).
- Diese Ports vendorn xboxrecomp **mit eigenen Änderungen**. Vor dem Übernehmen vergleichen mit dem Upstream-Stand hier
  (`xboxrecomp/`, v0.14.0, Stand 2026-10-16): vieles könnte inzwischen upstream sein.
- `reference/ssx3-ps2-decomp/`: Matching-Decomp von **SSX3 PS2 (NTSC-U, SLUS_207.72)** – nützlich für Funktions-/Struktur-Namen
  und Spiellogik per Querverweis (Strings, Konstanten), nicht als direkt übertragbarer Code.

## Regeln
- **Keine Spieldaten, keine XBE, keine ISO, keinen generierten Code committen** (alles in .gitignore). Nur eigene Quellen und Tools.
- Generierten Code in `src/recomp/gen/` nie manuell ändern – Fixes als Override (`recomp_manual.c`), Seed oder Pipeline-Patch.
- Änderungen an `xboxrecomp/` (Submodul) nur nach Rücksprache; lieber Patch/Override im eigenen Code oder Upstream-PR.
- Kleine, überprüfbare Schritte: nach jeder Änderung neu generieren/bauen/starten, Ergebnis in `docs/notes.md` festhalten.
- Wenn Zahlen/Adressen unklar sind: aus `xbe_parser`-Ausgabe oder Disassembly belegen, nichts raten.
- Der Speicherbedarf beim Kompilieren ist hoch: `--split 250`, `JOBS=` bei wenig RAM reduzieren.
