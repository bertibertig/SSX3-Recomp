# Releasing SSX3-Recomp

Project recipe for the global `/release` skill (it follows these steps where they are more specific than its own).
Releases are made the way v0.1.0 was made.

## Hard rules
- **Never upload game data**: no XBE, ISO, `game_files/` content or generated code (`src/recomp/gen/`) – not in the
  repo, not as a release asset. Packages hold only our binaries, launchers, READMEs and licenses
  (`scripts/package_release.sh` builds exactly that; check the listing anyway).
- Players start the game with the launcher scripts (`run.sh` with its TUI menu, `run.ps1` / `run.bat`), so the
  release notes and tests use those, never the bare binary.

## Preconditions
- Branch `main`, clean tree, not behind `origin/main`.
- `game_files/default.xbe` and `src/recomp/gen/recomp_funcs.h` exist. If the pipeline inputs changed since the last
  release (`config/`, `patches/`, the xboxrecomp submodule, `src/recomp_manual.c` stubs that need new seeds), run
  `scripts/run_pipeline.sh` first (~15 min, background).
- Tools: docker (or podman), `x86_64-w64-mingw32-gcc`, `zip`, `pwsh`, `wine`, python3 with `evdev` + `pillow`,
  writable `/dev/uinput`.

## Release notes
- Sources besides git: the `docs/notes.md` entries dated after the last release and `docs/TODO.md`.
- Write `docs/release-notes/<version>.md`, based on the previous file there. Always include the
  **no game files included** warning (own PAL/EU disc, title ID 4541003C) and the Getting started section
  (download, `game_files/`, launchers, Linux requirements).
- Keep a Windows section while Windows is not fully playable: what works, what was tested where (so far: only on
  Linux with Wine and Proton, never real Windows – change this only when someone tested it).
- Full changelog link: `https://github.com/bertibertig/SSX3-Recomp/compare/<last>...<version>`.
- License line: GPL-3.0-or-later, third-party licenses in `licenses/`, unofficial fan project.
- Also update `README.md` (Status table, Platforms) and `scripts/release/README.txt` (the player README in the
  packages: known issues, Windows status).

## Build

```bash
scripts/package_release.sh <version> > logs/package-<version>.log 2>&1    # background, ~5-15 min
```

Linux is built in the Ubuntu 24.04 container (glibc 2.39+), Windows with the host's MinGW. Checks:

```bash
tail -3 logs/package-<version>.log; cat dist/SHA256SUMS-<version>.txt
tar tvzf dist/SSX3-Recomp-<version>-linux-x86_64.tar.gz; unzip -l dist/SSX3-Recomp-<version>-windows-x86_64.zip
(tar tzf dist/*-<version>-*.tar.gz; unzip -Z1 dist/*-<version>-*.zip) | grep -iE '\.(xbe|iso|xiso|big|loc)$|/data/' && echo "GAME DATA FOUND - STOP"
objdump -T dist/SSX3-Recomp-<version>-linux-x86_64/ssx3_recomp | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1   # <= 2.39
x86_64-w64-mingw32-objdump -p dist/SSX3-Recomp-<version>-windows-x86_64/ssx3_recomp.exe | grep 'DLL Name'      # XINPUT1_4, no MinGW runtime DLLs
```

## Test (Linux and Wine)

```bash
scripts/test_release.sh <version> > logs/test-release-<version>.log 2>&1   # background, ~5 min
cat logs/release-test-<version>/summary.txt
```

It unpacks the packages like a player, checks the missing-game-files message, drives `run.sh` with the start-up
menu forced on and `run.ps1` (menu + the default CPU-renderer flow under Wine) with a virtual pad
(no focus stealing), and writes frame dumps. Look at `logs/release-test-<version>/linux_frames.png` and
`windows_frames.png`.

Pass criteria:
- Linux: menu summary line + saved `run.conf`; pad on port 1; frames show intro → title → menus → **a race**;
  game fps near 60 in the race; `[drive] exit 0` (still running at the time limit).
- Windows (Wine): missing-files message; `run.ps1` menu saves `run.conf`; pad on port 1; frames reach the menus.
  Compare with the last release's known Windows state (v0.1.0: crashes when a race loads, exit 139) – a new or
  earlier crash is a regression, a race that now works is a bug fix for the notes.

Do not publish a Linux build that does not reach a race.

## Work log
Add a `docs/notes.md` entry (date, version, test results) before the release commit.

## Publish

```bash
git push origin main
git tag -a <version> -m "SSX3-Recomp <version>" && git push origin refs/tags/<version>
gh release create <version> --verify-tag --title "SSX3-Recomp <version>" \
  --notes-file docs/release-notes/<version>.md --latest \
  dist/SSX3-Recomp-<version>-linux-x86_64.tar.gz dist/SSX3-Recomp-<version>-windows-x86_64.zip \
  dist/SHA256SUMS-<version>.txt
```

No re-download test needed (the uploaded files are the tested ones; `--verify-tag` and the checksums file cover
the upload).
