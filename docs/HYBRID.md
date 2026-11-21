# Hybrid: the port built from the decomp's C

Decided 2026-10-09 (after studying OpenPete, CTR Turbocharged and OpenCE): the port keeps the
recompiler as its base, and moves to the decomp's C where the game must change. The decomp's C is
compiled for the PS1 with the decomp's own toolchain (`references/TM2-Decomp`, `build.py`), and that
executable is recompiled as the retail one is. So:

- the C is the source of truth where we change the game;
- the game state stays one block of guest RAM (the kill cam's snapshots, rollback later);
- the integer behavior stays the PS1's (determinism across PCs);
- every hook of the port (`src/overrides.json`, the addresses in `src/vplus`, `src/ui`) works as before
  while the layout is the retail one.

## The builds

| Build | Executable | Inventory |
|---|---|---|
| `build/port-dev` | retail `SCUS_94306R.EXE` | `docs/recomp_inventory.json` |
| `build/port-hybrid` | the decomp's `out/TM2_SOURCE_E0.EXE` (all from C, retail layout), copied to `build/src-exe/` | its own, `build/src-exe/e0meta/recomp_inventory.json` |
| `build/port-ab` | retail, with chosen functions also from the source build (`TM2_ALT_EXE`, `TM2_ALT_FUNCS`) | retail |
| `build/port-variant` | the decomp's `out/TM2_SOURCE_PORT.EXE` (from C, room for 4 humans, a new layout), with `TM2_ADDRMAP` (its `addrmap.tsv`) and `TM2_PORT_JSON` (its port sections) | its own |
| `build/port-hooks` (**the hybrid**, tests) | the variant with the port's overlay (`decomp/`), built by `tools/decomp/wc.py` into `build/decomp-wc/out/`; copied to `build/port-dev/tm2pc-hooks.exe` for the comparisons | its own |
| `build/port` (**the play build, the hybrid since 2026-10-11**) | the same variant: `build/port/tm2pc.exe` | its own |
| `build/port-plain` | retail, as `build/port` was before: copied as `build/port/tm2pc-plain.exe` (kept as the reference while the comparisons run) | retail |

CMake settings: `TM2_EXE`, `TM2_INVENTORY`, `TM2_ALT_EXE`, `TM2_ALT_FUNCS` (hex addresses, comma
separated, or `@file`), `TM2_ADDRMAP`, `TM2_PORT_JSON`.

A build at a new layout boots from its own image (embedded, `gen/exe_image.c`), not the disc's, and
loads its port sections (data copied, BSS zeroed). The port names the game by retail addresses:
`GA()` (`src/runtime/addrmap.c`) gives where one is now, `gr()` the other way. The recompiler
flattens the map: the "port" rows (arrays the variant enlarged and moved out) lie inside the rows
of the data around them, and those are split around them.

## The checks

1. **Whole game, frame by frame** (`src/main.c`, at each frame's start):
   - `TM2PC_STATE_LOG=file`: a hash of the cars' records and the random state per frame.
   - `TM2PC_STATE_FULL=1`: the hash covers the whole 8 MB, except what differs between two runs of
     the same build anyway (the sound's instance pool and libsnd voices, libspu's transfer state, the
     stack's unused bottom) and the one function the source build compiles differently.
   - `TM2PC_STATE_BLOCKS=1`: also a hash per 64 KB, to see which parts differ.
   - `TM2PC_STATE_DUMP=N`: the 8 MB at frame N, to `<log>.ram`.
2. **Each function, each call** (`src/runtime/abcheck.c`, `TM2PC_AB=1` in a `port-ab` build): the
   source version runs in a sandbox (its hardware writes held back), the state is put back, the retail
   version runs for real, and the two results are compared (RAM, scratchpad, the registers a caller
   reads, the hardware writes). A difference is printed with the call and the first differing address.
   `TM2PC_AB_BREAK=N` spoils every Nth source result: the check must report it (a negative control).
3. **Against the console** (later): an emulator harness (not in this repository) pairs the retail game with a candidate,
   frame by frame.

## The pilot (2026-10-09)

- `TM2_SOURCE_E0.EXE`: 1947 of its 1948 functions compile to retail's bytes; the one that does not
  (`SpuVmSetVol` 8015BD20) is not called in the test.
- A 2-player battle (Los Angeles, 9 computer cars, scripted), the retail port three times and the
  hybrid once, 1040 frames: the cars' and random state equal on every frame in all four; of the 128
  blocks of 64 KB, the hybrid differs from retail only in the 5 where the retail runs differ from
  each other too (sound, stack).
- The A/B check on `worldGroundHeightAt` (800EF340): over 1000 calls no difference; with
  `TM2PC_AB_BREAK=500`, calls 500 and 1000 reported.

## The port variant (2026-10-10)

- `TM2_SOURCE_PORT.EXE` runs the port: the same scripted 2-player battle, 1044 frames, the cars' and
  random state equal to the retail port's on every frame (hashed by retail address, pointers mapped
  back), and frame 3600 pixel-identical.
- The retail build is unchanged by the address layer (`GA()` is the identity there): 1040 frames, no
  64 KB block differing beyond the run-to-run noise.
- 4 players (a scripted co-op tournament, 2 profiles and 2 guests): the simulation matches the retail
  port for 1898 frames (all four human cars, the computer cars, `rand()`), then drifts. The cause is
  the view-mode table: the variant keeps 9 rows, so the port's quarter rows (9-12) are off in it
  (`RB_VIEW_ROWS`) and only view 1 is drawn (full screen). Drawing other things changes how often
  the animations draw `rand()` (`animStepWeightedRandom` 80151CB8), and the game's random sequence
  drifts from there. The port cannot move the table itself in the variant (as it does in retail):
  16 of the 27 instructions that address it are in functions the variant rewrote (`viewCreate`,
  `viewProc`, two accessors). The fix is in the decomp (a request to the decomp).
- Still to do after it: the split-screen mirrors (row 13), and the port's own 4-player rebasing
  (`four.c`) reconciled with the variant's native arrays.

## The port variant with 16 view-mode rows (2026-10-10)

The decomp's variant now has a 16-row `viewModeTable` (request 26). With it, the 4-player test
(a co-op tournament, 2 profiles and 2 guests, 3306 frames) is equal to the retail port's on every
frame. `RB_VIEW_ROWS` comes from the build (`rc_view_mode_rows`, the table's size in its json).

## Our changes in C (2026-10-10)

The port's own changes to the game's C: `decomp/` (the overlay) and `tools/decomp/wc.py`, which
copies the decomp to `build/decomp-wc`, applies the overlay and builds the variant from the decomp's
newest finished build (only the units the overlay changes are recompiled; the decomp's own proof
against E0 runs on the result). `build/port-hooks` is the port built from it.

- **The settings bridge.** The game's C reads the port's settings from `portConfig`, a 64-byte block
  in the variant's port storage (`include/semantic/port_hooks.h` in the overlay). The recompiler
  writes its address (`rc_port_config`, from the json); `src/vplus/port_config.c` fills it at each
  frame's start. In other builds the address is 0 and the host's wraps do the work.
- **First change:** AI vs AI's update rule, moved from the host's wrap of `aiCarShouldUpdate` into
  the function's C. A 2-player team match on the retail port, on `port-hooks`, and on `port-hooks`
  with `TM2PC_NO_PORT_CONFIG=1` (the wrap instead): equal on every frame (~1250). The negative
  control (neither) differs from frame 231.

## Checks for the port's own code

- `python tools/test/check_addresses.py`: every game address in the port's C goes through `GA()`, or is
  marked `retail-address` (and a marked `case` label of a return address is not in a function the
  hybrid rewrites). Run it before committing host code.
- `TM2PC_LIVE_SHOT=N`: every N seconds of real time the screen as presented (menus and movies too) to
  `<shot dir>/live.bmp` and `live-prev.bmp`, overwritten; for watching a run while the PC is locked.

## The hybrid against the play build (2026-10-10)

`python tools/test/compare_builds.py` runs these scenarios on `build/port-dev/tm2pc.exe` and
`tm2pc-hooks.exe` (`--a`, `--b` for others, `--only` for some, `--list`) and compares them; about
50 minutes for all, the tournament 26 of them.

`build/port-hooks` (the decomp's variant, its newest build, with our overlay) against the retail-based
port, scripted, the state compared on every frame and a screenshot byte for byte:

| Scenario | Frames | State | Shot |
|---|---:|---|---|
| 1 player, Challenge Match, firing, out of lives (spectating) | ~1190 | equal | equal |
| 2 players, TM:Black and TM 2012 HUDs, rear-view mirrors | ~1050 | equal | equal |
| 2 players, blue energy, turbo bar, 3 lives | ~1250 | equal | equal |
| 2 players, teams (AI vs AI) | ~1850 | equal | equal |
| 2 players, cars destroyed (lives, spectating, kill cam) | ~1450 | equal | equal |
| 2 players, kill feed | ~1050 | equal | equal |
| 4 players, co-op tournament | ~3310 | equal | equal (the play build's radar arrows fixed, below) |
| 2 players, a whole co-op tournament: the 8 levels (Dark Tooth's three fights), each won, then the co-op duel | ~10500 | equal | equal |

- **The radar arrows of views 3-4.** The retail port moves the HUD arrays for 4 players, but not the
  eight radar-arrow arrays (`hudRadarArrow*Prims`, `*Sprites`), so HUDs 3-4 read past their two
  entries into the next array (HUD 3's left arrow is HUD 1's right one). The variant has them per
  HUD: the hybrid drew the right arrow. Fixed in the play build too: the eight arrays are moved
  (`tools/recomp/rebase_arrays.tsv`) and HUDs 3-4 get the parsed arrows (`vp_HudRadarArrowsLoad`,
  vplus/four.c); its 4-player screenshot now equals the hybrid's.
- **The tournament run:** `TM2PC_TEST_WIN_EVERY=600` (every computer car destroyed 600 VBlanks into
  each round), `TM2PC_TMENU_SCRIPT=-,-,-,-,x` (LEVEL COMPLETE: continue) and X presses on pad 1 every
  60 VBlanks (the globe screens). With player 2 destroyed in the duel (`TM2PC_TEST_KILL=2@45000`), equal
  through the duel's result (~10850 frames); the ending movie after it is not compared.
- **Settings that change within a frame.** AI vs AI starts when player 1 is out (spectating); the
  host's wraps see it at once, so `portConfig` is refreshed there too, not only at a frame's start
  (`port_config_sync`). Without it the 1-player run differed from that frame on.

## Solved: 1 player quit at the arena load (2026-10-11)

A 1-player Challenge Match in the hybrid ended in `exit(-1)` (`assetInstallImage`: no "PSXD"), after car
select showed cars 148/20. Our C names a wrapped function's original by its retail address
(`F_80122124_orig`, `sceneObjDraw`), but `tools/recomp/recompile.py` named the originals by their new
address and only added retail aliases where the name was free. In the variant `sceneObjSetChoiceState`
(retail 80122078, also wrapped) sits at 80122124, so `ui_SceneDraw` called it with (object, frame) every
frame: the carousel's item became the frame record's low byte. The originals are now named by their
retail address in any layout (no aliases). `compare_builds.py` now fails a run that exits early, has
fewer frames or only one shot (the 1-player scenes had passed with one side stopped).

## Solved: 3 players drifted (2026-10-10)

The 3-player co-op run drifted from frame 2798. Two layers:
- The variant's `worldUpdate` had an 8-byte larger stack frame, so a stack address left in an effect
  sprite differed (`FX_SPRITE_STACK_POINTER.md` in the decomp: never read, harmless). The decomp's
  request 29 moved its N-human radar loop to `worldRadarPlotHumans` (`.port_text`): the frame is
  retail's again.
- The cause: players 3-4's radar blips. The variant plots them itself, with the game's rule (a
  blip of a car 400 or more higher or lower shows on 1 frame in 5: `rand() % 5`), and the play build
  plots them from the host (`four_radars`, always, no `rand()`). A height gap at frame 2798 drew one
  more random number in the hybrid, and everything after drifted. With `TM2_PORT_HOOKS` the variant
  keeps retail's radar code (the overlay disables its two N-human blocks); the host plots players
  3-4's as in the play build. The radar moves into C with the rest of the view side.

`compare_builds.py --only three,four,two`: all equal.

**Rule (2026-10-10, the user's):** what moves into C moves for every player count, in one path; no
"players 1-2 the game's way, 3-4 ours". Example found by unifying: views 3-4 had skipped
`streaksDraw`, so their tracers and arcs were missing in both builds.

## The 4-player view side in C (2026-10-10: done but the display, which stays the host's)

`vplus/four.c` draws views 3-4 from the host today. In the overlay each piece would move into the
game function it extends, under `TM2_PORT_HOOKS`, with `portConfig` carrying what the host decides
(the number of humans, and with 3 players whether player 1 has the top or the left half):

| Host piece (four.c) | Into (decomp C) | Notes |
|---|---|---|
| `quarter_rows`: view-mode rows 9-12 from row 1 | **done**: the table's initial data | `rtDoubleBufferInit` is in a plain `.text` section, which the variant cannot change |
| `vp_DoubleBufferInit`: frame records and order tables of views 3-4, the packet buffers ×4 | **done**: `portViewBuffersInit` (`src/port/port_views.c`), called at the end of `shellInitMachine` | the storage stays at the host's high-RAM addresses (`port_hooks.h`, `PORT_*`) |
| `four_views_create`: the views with rows 9-12 (or 2/5 for player 1 with 3 players) | `sessionBindCars` | the variant binds humans 3-4 already |
| `vp_ViewSetFollowObj`: the sky copies of views 3-4 | **done**: `viewSetFollowObj` | `portConfig.humans`, refreshed by `four_setup` |
| `vp_RenderScene`: views 3-4 drawn after view 2 | **done**: `portViewsDraw` draws views 2..N, the game's own view-2 code replaced (one path for 2-4 players) | views 3-4 now draw their streaks too; `streaksDraw`'s two updates stay once per view 1-2, as the game's |
| `four_present`: views 3-4 and HUDs 3-4 on the screen | **done**: `portViewsPresent` shows views 2..N, each with its HUD (the game's HUD function, the host's HUD styles through its wrap), in place of the game's view-2 display | the players' mirrors: the host's, after the last view's HUD (`four_hud_done`) |
| `vp_CarCameraUpdate`: the chase offsets by view shape | **done**: `viewCarCameraUpdate` (the guard for views not made, the split mode set to the view's shape around it) | `portConfig.left`; `watch_other` stays on the host |
| `four_radars`: players 3-4's radar blips | **done**: `portRadarPlot` (`src/port/port_radar.c`), from `rtMainLoop` where the host plotted | every radar by the game's rule (other heights: 1 frame in 5, `rand()`), as the play build now; `TM2PC_TEST_SOLID_RADAR` for exact comparisons |
| pads 3-4, `watch_other` (out of lives), the lobby | stay on the host | input and the port's rules |

Each step is checked with `compare_builds.py --only four` (and a 3-player run): the overlay build
against the play build, equal frame for frame and in the screenshot. The shared code (helpers used by
several functions) needs the decomp's `.port_text` (a request to the decomp); until then, macros.
Rough effort: 2-4 days.

## Next

1. **More of our changes in C**: the wraps that change the game's rules, one at a time, each checked
   as the first (the host's version against the C's, and a negative control).
2. **The 4-player view side in C**: the variant leaves views 3-4 to the port (`four.c` does them
   from the host now); moved into the game's C under `TM2_PORT_HOOKS`, they are checked against it.

## The hybrid as the play build; far LOD (2026-10-11)

- `build/port` now builds the hybrid (its CMake points at `build/decomp-wc/out/TM2_SOURCE_PORT.*`);
  the retail-based build stays as `build/port-plain` (`tm2pc-plain.exe`) and `build/port-dev` (tests).
  After a decomp update: `python tools/decomp/wc.py sync`, `build`, then build `port`, `port-hooks`.
- `wc.py sync` failed on the decomp's request 30 (its own new rows next to ours in
  `port_variant_arrays.tsv` / `.c`): merged three ways against the decomp commit the overlay was made
  on (cefa430), line endings normalized for the merge and put back (the decomp's files are CRLF),
  then `wc.py diff` to write the overlay again.
- The variant is built with `TM2_PORT_FAR_LOD` (decomp `FAR_LOD.md`): `portFarLod` (the recompiler
  gives its address as `rc_port_far_lod`, 0 in other builds) is set by `port_config_sync` at Draw
  distance Far and Farthest: past a model's last level-of-detail band it keeps drawing that band, so
  the arena, pickups and cars stay out to the far distance. Still to do (decomp request): the
  280-mesh queue and the packet buffer, which the cars are dropped from first when full.
- Drawing more changes the simulation: the animations draw `rand()` as they are drawn
  (`animStepWeightedRandom`), so the same battle with another draw distance plays out differently.
  Online play needs one draw distance for everyone (or that rand taken out of the drawing). **Done the same
  day:** the animators draw from a stream of their own (RENDER_PLAN.md 5, "Exact replays").

## Calls matched by address inside changed functions (2026-10-12)

The port names some of the game's calls by their return address (lines marked `retail-address`: the kill feed's
weapon, splash and special recognition in `profile/feat_hooks.c`). `gr()` maps a return address back to retail
by range; inside a function the port's overlay changes, the calls move and an exact match no longer fires. The
Dark Tooth change to `weaponProjectileUpdate` (Survival) did that: every projectile kill in the hybrid's feed read
"DESTROYED" (plain vs hybrid: the dm_suicide shot differed). `feat_WeaponHit` now matches that function by its
range and finds the projectile's record by its fields when s1 does not hold it. `tools/test/changed_sites.py`
lists any other match inside a changed function (none now); run it after building the variant.
