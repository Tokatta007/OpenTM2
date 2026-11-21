# Loading mod arenas: what is missing

The game checks arena mods (`src/mods/mods.c`) but doesn't load them. This list, from a survey of the decomp on
2026-11-12, covers what loading one needs. Every game function is already C in the decomp, so nothing has to be
decompiled first. The work is port wraps (`src/overrides.json`, `wrap:`) or decomp `TM2_PORT_*` variant requests.
`D` is `references/TM2-Decomp/src/semantic/`.

## Status (branch `arena-loading`, 2026-11-14)

Done (`src/mods/mod_arena.c`):

- Items 1-3 below for play: level 13 is the mod arena. Its DMD and TMS come from the mod's folder through the game's
  own loads, `arenaLoadHolland` (`arenaSetup`'s default) is wrapped to load its routes, start points, sections,
  surfaces and sky from `arena.ini` and the mod's files, and `arena.ini`'s `warp1..3` set the teleporters.
- Item 5 in part: `gameGetLevel` answers 7 (Holland) for level 13, so the AI formulas, the music, the fall heights and
  every per-level rule take Holland's. Left from Holland: the hidden health spot's place (`car_tick.c:220`) and the
  AI's teleporter exits (`aiWarpSetExitNode`): both need data, a decomp request.
- `--check-mod` checks an arena's file sizes and `arena.ini` (points, starts, sections, surfaces).
- Tests: `TM2PC_TEST_MOD_ARENA=<id>` plays a Challenge Match on that mod. `tools/mods/clone_arena.py` makes a mod
  from the player's own disc's Holland. The copy plays as Holland: computer cars drive and fight on it.
  Its pickups are the same 25 as Holland's, slot by slot (`TM2PC_PICKUP_DEBUG`). (A health pickup at the start
  point stayed in the copy's run: on Holland a computer car takes it 93 steps in; the copy has 7 computer cars, not 9.)

Since (2026-11-13):

- The menus: mod arenas that are on (Options > Mods) come after the secret arenas in every arena list (Challenge
  Match, Survival, Custom Tournament stages, playlists), by name, with their card.bmp; Random picks among them too.
  A match's arena value for one is 100 + its place in that list; online the list follows the server's mod list
  (every machine the same), and joiners download the server's mods before joining, as mod cars.
- Music: `roundStartMusic` had no mode for level 13 (no music at all): it gets Holland's, and the port then plays
  the mod's own music file, the Custom Music page's "Mod arenas" row, or another arena's track picked at random.
- `arena.ini`'s `fall_limit` and `fall_death`.

Not done: the health spot and the AI's teleporter exits (decomp request 114), King of the Hill hills, a line-up
picture. A custom tournament saves a mod arena stage by its place in the list (it moves if mods are turned on or
off).
An unknown arena gets the smallest car room (7 computer cars), as its textures' room is unknown.

## How a level loads today

- The level number is in settings +0x18:
  - 1-8 are the arenas;
  - 9 has no files;
  - 10-12 are Swamp, Cyburbia and Roof Tops.
- `sessionLoadArenaAndCars` 8012BAFC (`D/shell_main.c`):
  1. loads `TMS\<stem>.TMS`, then the cars' TMS;
  2. loads `DMD\<stem>.DMD` to 0x80019C40;
  3. walks its objects with `worldLoadObject`.
- `arenaSetup` 8011995C (`D/arena_setup.c`): a level switch to `arenaLoad<Name>`. **Its default is Holland**, so an
  unknown number loads Holland. Each loader:
  - sets the sky color and the short chase range;
  - loads `TERRAIN\X.PTS` (routes), `X.GRP` (6036 bytes) and `X.RMP` (6300 bytes);
  - holds the section and surface counts in code, not in the files.

## Needed, in order

1. **A level number for mod arenas** (one shared number, for example 13, with the mod chosen by the port).
   - Wrap `arenaSetup`'s switch for that number.
   - Wrap `levelFileStem` 8012ABD0 and `levelDisplayName` 8012ACB8, which say "UNKNOWN LEVEL" today.
   - The port's own per-level arrays then need one more entry:
     - options.c `tournament_arenas[13]`, `arena_values`, `boss_room[13]` and the card tables;
     - battle.h `BATTLE_ARENAS`;
     - uibuild.c `ARENA_CARDS[12]`;
     - hill_spots.c `spots[9]`.
2. **The files from the mod folder.** `vp_FileLoad` (fixes.c, wraps `fileioLoadFileIntoRam`) sends that level's
   `DMD\`, `TMS\` and `TERRAIN\` names to `mods/arenas/<id>/`. `vp_LevelDmdFileName` and `TmsFileName` already
   rewrite the names.
3. **A loader for the mod number:**
   - the sky color, the PTS node count and the start list from `arena.ini`;
   - the GRP section count and RMP surface count from the files.

   `src/vplus/secret_routes.c` already writes routes and GRP tables straight into guest RAM for Cyburbia and Roof
   Tops: the same model.
4. **Per-level code switches given data** (wraps):
   - `worldFallLimitZ` 8011EC0C and `worldFallDeathZ` 8011ECC4: the fall heights;
   - `roundStartMusic` 80128DB4: a new number gives an out-of-range mode;
   - `worldWarpsSetup` 80115310: the teleporters' exits, 3 at most;
   - the hidden health spot (`D/car_tick.c:220`);
   - `tournamentOpponentCount` (already wrapped by `vp_TournamentOpponentCount`);
   - the King of the Hill spots.
5. **Formulas that grow with the level number:**
   - `weaponFireInterval = (80 - 4*level) >> diff` (`D/round_reset.c:74`) goes wrong at 20 or more;
   - AI strength `3*level` (`D/ai_route_nodes.c:45`);
   - AI waits `(8-level)*60` (`D/ai_steps.c:526,1451`).

   A mod arena should pass a stand-in level (for example 5, as secret_routes.c does for two AI checks) or a
   difficulty from arena.ini.
6. **Level rules the AI keys on levels 3/5/6/8** (`D/ai_route_helpers.c:249`, `D/ai_steps.c:747-771`): decide which
   ones a mod arena gets (arena.ini flags).
7. **The DMD:**
   - Its pointers are absolute for 0x80019C40, and the arena and the cars share 0x80019C40-0x800CDC54 (737,300 bytes).
     Either mods are built for that base (the guide says so now), or a relocation pass is needed (a decomp request).
     Moving the cars' DMDs to `portCarDmdArea` frees room for bigger arenas.
   - The DMD must carry the shared objects (effects 650-691, icons 1260, radar arrows 1290-1293, car and weapon parts):
     `--check-mod` should check for them.
8. **The TMS:** its images go to their own VRAM places. It must hold Roadkill's and Dark Tooth's key images and the
   shared shadow and turbo images (decomp `TEXTURE_BANKS.md`). `--check-mod` should check the places are free for
   the cars.
9. **The menus:**
   - the level select (`ui_LevelSelect`, options.c), Challenge Match, Survival and Custom Tournament lists, and
     Random;
   - the online listing (the mod's id in the room, the download).

## Not possible from a mod

These are code, not data, so a mod arena gets none of them:

- the level events: LA's quakes, Moscow's collapse and dish, the Eiffel tower, New York's and Holland's targets,
  Hong Kong's trains and Antarctica's ice;
- the remote-bomb and lightning triggers tied to them;
- Dark Tooth's level-8 rules.

The full census is in decomp `docs/ARENA_SWITCHES.md` §2.

## Decomp requests this would need

None are needed to start: wraps of the addresses above work in the recompiled build. Two would help later:

- DMD relocation for another load address;
- a data-driven `arenaSetup` default.
