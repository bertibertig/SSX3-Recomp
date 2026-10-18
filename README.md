# SSX3-Recomp

Experimentelle **statische Recompilation von SSX 3 (Original-Xbox)** zu einem nativen x86-64-PC-Programm,
auf Basis von [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (als Git-Submodul unter `xboxrecomp/`).

> Dieses Repo enthält **keinen Spielcode und keine Spieldaten**. Du brauchst dein eigenes, legal erworbenes
> SSX-3-Xbox-Disc-Image. Der C-Code wird lokal aus deiner `default.xbe` erzeugt und nie committet.

Status: Setup vorbereitet, Pipeline noch nicht ausgeführt. Siehe `docs/notes.md`.

## Schnellstart

```bash
git clone --recurse-submodules <dieses-repo>     # bzw. lokal: git submodule update --init --recursive
cd SSX3-Recomp

scripts/setup.sh                                  # Python-venv + capstone, Tool-Check
bash xboxrecomp/tools/linux/install_deps.sh       # nur Linux: SDL2, libepoxy, OpenSSL, ffmpeg (sudo)

scripts/extract_iso.sh "/pfad/zu/SSX 3 (Xbox).iso"   # -> game_files/default.xbe
scripts/run_pipeline.sh parse                     # Entry Point etc. -> src/main.c + docs/notes.md
scripts/run_pipeline.sh                           # disasm, func_id, abi, recomp -> src/recomp/gen/
scripts/doctor.sh                                 # Bring-up-Bericht
scripts/build.sh                                  # CMake-Build
```

Der erste Start stürzt erwartungsgemäß ab – ab da beginnt die iterative Arbeit
(siehe `xboxrecomp/docs/GETTING_STARTED.md`, Schritt 7–10).

## Mit Claude Code arbeiten

Im Repo-Wurzelverzeichnis `claude` starten: `CLAUDE.md` erklärt Ziel, Layout, Workflow und Regeln.
Gute erste Aufträge:

1. „Führe Setup und Pipeline-Schritt `parse` aus und trage die XBE-Werte in `src/main.c` und `docs/notes.md` ein.“
2. „Vergleiche `reference/OpenTricky/xboxrecomp` mit `xboxrecomp/` und liste, welche Port-Änderungen upstream noch fehlen.“
3. „Führe die komplette Pipeline aus, dann `scripts/doctor.sh`, und fasse die größten Lücken zusammen.“

## VS Code

Ordner öffnen (`code .`) und die empfohlenen Erweiterungen installieren. Unter *Terminal → Run Task* gibt es die
Pipeline-Schritte als Tasks. Der generierte Code in `src/recomp/gen/` ist von Suche und File-Watcher ausgenommen (sehr groß).

## Referenzen

- xboxrecomp: https://github.com/sp00nznet/xboxrecomp
- SSX Tricky PC (Xbox-Recomp des Vorgängers): https://github.com/MatiasRiveraC/SSX-Tricky-PC
- OpenTricky: https://github.com/GiZcesi/OpenTricky
- SSX3 PS2 Decomp: https://github.com/ssxdecomp/ssx3 · GameCube: https://github.com/ssxdecomp/ssx3_gc

Lizenz der eigenen Dateien: noch festzulegen (xboxrecomp: MIT; wenn Code aus OpenTricky übernommen wird: GPL-3.0-only beachten).
