# Releases and the updater

## The updater (`src/runtime/update.c`)

At start (normal play only; test runs with `--settings` skip it), the game asks in the background whether a newer
version is out: a GitHub Releases "latest" answer (`[update] url` in tm2pc.ini, default
`UPDATE_URL_DEFAULT` in `src/settings.h`; `[update] check = off` turns it off). It compares the release's tag with
its own version (`TM2PC_VERSION` in CMakeLists.txt: v1.2.10 is newer than v1.2.9) and takes the release's first
asset whose name ends in `.zip`.

**Before the game** (the user's: the update all before the game, 2026-10-21): the check waits up to 4 s at start,
before the game's window; a newer one asks "New update vX is available (you have vY). Do you want to update OpenTM2
before starting?" (Yes / No). Yes: a small window with the download's bar (MB, percent, seconds left); then the new
version starts with the same command line and goes into the game. Closing the window stops the download (the old
files stay) and the game starts; a failure says so and the game starts as it is. A slow answer (over 4 s): the game
starts, and the Options row below offers it.

When one is newer, the Options menu also shows **Update to vX** first. Choosing it downloads the zip in the background
("Downloading vX: N%"), then puts each file in place: the old one renamed `NAME.old` (Windows lets a running program be
renamed), the new one where it was; anything failing puts the old ones back ("Update failed: try again"). Then
**Restart for vX** starts the new executable and ends this one; the next start removes the `.old` files
(`update/old.txt` lists them).

A player's own files are never written, whatever the zip holds: `tm2pc.ini`, `tm2pc.log`, `profiles/`, `music/`,
`mods/`, `screenshots/`; nor anything outside the game's folder.

Test: `python tools/test/update_test.py` (a release served from 127.0.0.1 to a scratch copy in build/update-test: found,
downloaded, swapped while running, the player's files untouched, the old executable removed at the next start).
Test switches: `TM2PC_UPDATE_ANSWER` (yes / no: the question answered), `TM2PC_UPDATE_NO_RESTART`, `TM2PC_UPDATE_SHOT`
(the window as a BMP), `TM2PC_UPDATE_URL` (where to ask; also turns the check on in test runs), `TM2PC_UPDATE_WAIT` (the
answer before the game goes on), `TM2PC_UPDATE_AUTO` (download and swap at once).

## Making a release

1. Raise `TM2PC_VERSION` in CMakeLists.txt (for example v0.9.0 to v0.9.1) and build `build/port`.
2. `python tools/release/pack_release.py` writes `build/release/OpenTM2-<version>-Windows-x64.zip`: `tm2pc.exe`,
   `fonts/`, `art/` and `recomplauncher.json` in a folder named for the version.
3. On GitHub, a new release with the tag exactly the version (`v0.9.1`) and the zip attached.

## Where the releases live

This repository's GitHub releases (public): `UPDATE_URL_DEFAULT` asks for its "latest". A release carries only
`tm2pc.exe`, `fonts/` and `art/` (the mode symbols, CC BY 3.0); the game makes its menu pictures from the player's own
discs at its first start (docs/FIRST_RUN_SETUP.md).

## RecompLauncher

[RecompLauncher](https://recomplauncher.com) installs OpenTM2 from these releases. `recomplauncher.json` (the repository's
root, packed beside `tm2pc.exe`) tells it: the PS1 disc's `.cue` (checked by `SCUS_943.06`), the optional PC version, the
setup step (`tm2pc.exe --prepare --game=... --pc=...`, done when `ui/setup.txt` is there; `prepare.version` follows
`UI_BUILD_VERSION`, so raise both together), the start (`--game=...`) and the saves (`profiles/`, `tm2pc.ini`). Started by
it (`RECOMPLAUNCHER` set), the game never offers its own update. The file's schema:
https://recomplauncher.com/schema/recomplauncher.json.
