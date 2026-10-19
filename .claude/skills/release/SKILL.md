---
name: release
description: Build, test and publish a new SSX3-Recomp release on GitHub - Linux (Docker) and Windows (MinGW) packages, local tests on Linux and under Wine, a changelog since the last release (features, bug fixes, known issues), tag, upload, mark as latest. Use when the user asks for a new release, a new version, or to ship/publish a build.
argument-hint: "[version, e.g. v0.2.0] [--yes to publish without the final review]"
---

# Release SSX3-Recomp

Makes a release the way v0.1.0 was made. Arguments: `$ARGUMENTS` (optional version, optional `--yes`).

Hard rules, every time:
- **Never upload game data**: no XBE, ISO, `game_files/` content or generated code (`src/recomp/gen/`) – not in the
  repo, not as a release asset. Packages hold only our binaries, launchers, READMEs and licenses
  (`scripts/package_release.sh` builds exactly that; check the listing anyway).
- Everything in English (CLAUDE.md). Commits end with the attribution lines from the system reminder.
- Players start the game with the launcher scripts (`run.sh` with its TUI menu, `run.ps1` / `run.bat`), so the
  release notes and tests use those, never the bare binary.
- Publishing is outward-facing: unless `--yes` was passed, show the file list and the release notes and wait for
  the user's go-ahead before pushing, tagging or uploading (step 7).

Keep a task list (one item per step below) and record the outcome in `docs/notes.md` at the end.

## 1. Preconditions

```bash
git status --short; git branch --show-current; git fetch -q origin --tags; git status -sb | head -1
gh auth status
git describe --tags --abbrev=0          # last release tag
```

- Branch `main`, clean tree, not behind `origin/main`. Uncommitted work: ask the user whether it belongs in the
  release (commit it) or not (stop).
- `game_files/default.xbe` and `src/recomp/gen/recomp_funcs.h` exist. If the pipeline inputs changed since the last
  release (`config/`, `patches/`, the xboxrecomp submodule, `src/recomp_manual.c` stubs that need new seeds), run
  `scripts/run_pipeline.sh` first (~15 min, background).
- Tools: docker (or podman), `x86_64-w64-mingw32-gcc`, `zip`, `pwsh`, `wine`, python3 with `evdev` + `pillow`,
  writable `/dev/uinput`. If one is missing, say which and stop.

## 2. Version

Given as an argument → use it (with the leading `v`). Otherwise propose one from the changes since the last tag
(semantic versioning; while below 1.0: new features → minor `v0.X.0`, only fixes → patch `v0.x.Y`) and use it,
mentioning it in the review (step 7). The tag must not exist yet (`git ls-remote --tags origin`).

## 3. Changelog and release notes

Collect what changed since the last release:

```bash
LAST=$(git describe --tags --abbrev=0)
git log --reverse --format='%h %s%n%b' "$LAST"..HEAD
git diff --stat "$LAST"..HEAD
```

Also read the `docs/notes.md` entries dated after the last release and `docs/TODO.md` (closed items = done,
open items = known issues). Sort every user-visible change into:

- **New features** – things a player can now do or see (renderer features, options, controllers, platforms)
- **Improvements** – performance, compatibility, launcher/menu, packaging
- **Bug fixes** – crashes, graphics/audio/input bugs that are gone (say what the player noticed, not the address)
- **Known issues** – still open; carry over the previous release's list, drop what is fixed, add what is new
  (including anything the tests in step 5 show)

Leave out internal-only changes (refactors, notes, tooling) unless they matter to players or people building it;
those can go in a short "For developers" section (build system, Docker, scripts).

Write `docs/release-notes/<version>.md`, based on the previous file in `docs/release-notes/`, in this order:
1. One or two sentences: what this release is (highlight the biggest change).
2. The **no game files included** warning (own PAL/EU disc, title ID 4541003C) – always.
3. **Getting started** (download, `game_files/`, launchers, Linux requirements) – copy from the previous notes,
   update if something changed.
4. **What's new**: New features / Improvements / Bug fixes.
5. **Platform status** – keep a Windows section while Windows is not fully playable: what works, what was tested
   where (so far: only on Linux with Wine and Proton, never real Windows – change this only when someone tested it).
6. **Known issues**.
7. **Full changelog**: `https://github.com/bertibertig/SSX3-Recomp/compare/<last>...<version>`.
8. License line (GPL-3.0-or-later, third-party licenses in `licenses/`, unofficial fan project).

Update `README.md` (Status table, Platforms) and `scripts/release/README.txt` (the player README in the packages:
known issues, Windows status) where the facts changed. Do not commit yet.

## 4. Build the packages

```bash
scripts/package_release.sh <version> > logs/package-<version>.log 2>&1    # background, ~5-15 min
```

Linux is built in the Ubuntu 24.04 container (glibc 2.39+), Windows with the host's MinGW. Then check:

```bash
tail -3 logs/package-<version>.log; cat dist/SHA256SUMS-<version>.txt
tar tvzf dist/SSX3-Recomp-<version>-linux-x86_64.tar.gz; unzip -l dist/SSX3-Recomp-<version>-windows-x86_64.zip
(tar tzf dist/*-<version>-*.tar.gz; unzip -Z1 dist/*-<version>-*.zip) | grep -iE '\.(xbe|iso|xiso|big|loc)$|/data/' && echo "GAME DATA FOUND - STOP"
objdump -T dist/SSX3-Recomp-<version>-linux-x86_64/ssx3_recomp | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1   # <= 2.39
x86_64-w64-mingw32-objdump -p dist/SSX3-Recomp-<version>-windows-x86_64/ssx3_recomp.exe | grep 'DLL Name'      # XINPUT1_4, no MinGW runtime DLLs
```

## 5. Test locally (Linux and Wine)

```bash
scripts/test_release.sh <version> > logs/test-release-<version>.log 2>&1   # background, ~5 min
cat logs/release-test-<version>/summary.txt
```

It unpacks the packages like a player, checks the missing-game-files message, drives `run.sh` with the start-up
menu forced on and `run.ps1` (menu + the default CPU-renderer flow under Wine) with a virtual pad
(no focus stealing), and writes frame dumps. **Look at** `logs/release-test-<version>/linux_frames.png` and
`windows_frames.png` with the Read tool.

Pass criteria:
- Linux: menu summary line + saved `run.conf`; pad on port 1; frames show intro → title → menus → **a race**;
  game fps near 60 in the race; `[drive] exit 0` (still running at the time limit).
- Windows (Wine): missing-files message; `run.ps1` menu saves `run.conf`; pad on port 1; frames reach the menus.
  Compare with the last release's known Windows state (v0.1.0: crashes when a race loads, exit 139) – a new or
  earlier crash is a regression, a race that now works is a bug fix for the notes.

On a regression: stop, report it with the log lines and frames, and ask whether to fix it first or release with
it listed as a known issue. Do not publish a Linux build that does not reach a race.

## 6. Commit the notes

Add a `docs/notes.md` entry (date, version, test results), then:

```bash
git add -A && git commit -m "Release <version>: release notes, README" ...   # with the attribution lines
```

## 7. Review (skip only with --yes)

Show the user, in one message: version, the asset list with sizes (`ls -la dist/*<version>*`), what each archive
contains (file list), the test summary, and the release notes text. Ask once whether to publish. Wait for the answer.

## 8. Publish

```bash
git push origin main
git tag -a <version> -m "SSX3-Recomp <version>" && git push origin refs/tags/<version>
gh release create <version> --verify-tag --title "SSX3-Recomp <version>" \
  --notes-file docs/release-notes/<version>.md --latest \
  dist/SSX3-Recomp-<version>-linux-x86_64.tar.gz dist/SSX3-Recomp-<version>-windows-x86_64.zip \
  dist/SHA256SUMS-<version>.txt
gh release list --limit 3        # the new one must say "Latest"
gh release view <version> --json isDraft,isPrerelease,assets --jq '{isDraft,isPrerelease,assets:[.assets[].name]}'
```

Not a draft, not a pre-release, marked **Latest**. No re-download test needed (the uploaded files are the tested
ones; `--verify-tag` and the checksums file cover the upload).

## 9. Report

Release URL, version, the highlights from the notes, test results (Linux / Wine), anything left open. Offer a
follow-up session for regressions or known issues found in step 5.
