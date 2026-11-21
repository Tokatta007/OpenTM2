# Vanilla Plus changes

Every change we make to how the original game plays or sounds is listed here and
registered in [src/vplus/vplus.c](../src/vplus/vplus.c), so each one can be switched
on and off.

- **Presets:** *Vanilla Plus* (every change on, the default) and *Original* (every change
  off: the retail game).
- **Online:** always Vanilla Plus, so every player runs the same rules.
- **Options › Gameplay** (planned): the changes listed by category with ON/OFF toggles and
  the preset row at the top.
- **Until then:** `tm2pc.ini` next to the executable. A windowed run creates it with every
  change listed. `preset = original` turns all of them off, and a line per change
  overrides the preset.
- **Command line:** `--preset original|vanilla_plus` overrides the file; `--settings FILE`
  reads another file. Headless runs (tests) ignore `tm2pc.ini` unless `--settings` is given.

Categories, in the order the Options page lists them: **Menus**, **Bug fixes**, **Global Balancing** (rules for every car), **Character Balancing** (one option per car; the user's playtest, 2026-10-17). Descriptions stay short and plain: what changes, in one line.

## Adding a change

1. Add an id to `VpChangeId` in `vplus.h` and its entry (ini key, name, category,
   description) to `vp_changes` in `vplus.c`.
2. Implement it behind `vp_on(VP_x)`, checked every time it runs. Game-function hooks go
   in `src/vplus/fixes.c` (wired through `src/overrides.json`, usually as `wrap:`, so the
   original code still runs).
3. Add a row below. Note what retail does, and how it was checked.

## Changes

| Key | Name | Category | What changes | Original behavior | Hook |
|---|---|---|---|---|---|
| `pause_quit_boom` | Pause menu boom | Bug fixes | Resume and Quit in the in-game pause menu play the explosion together with a copy one octave lower at half its volume (same sample, envelope and pan), so it has the bass of the menu explosion without any music. | The menus' explosion (program 0x1D) is the same sound; it sounds full because a menu's music loop (program 0x1B) is playing under it, or starts with it. The pause menu's Resume and Quit play the explosion alone and sound thin. Checked in retail DuckStation: only the explosion after an in-game Quit. | wrap of `80160428` (sound commit) called from the pause menu's confirm (`801298D4`) (either choice); at the next VBlank `vp_vblank` copies the voice playing sample 0x09320 to the highest silent voice with half the pitch and keys it on. |
| `axel_eyes` | Axel's eyes | Bug fixes | Axel's eyes: a white outer pixel and a dark brown inner one each, instead of solid red (each eye is 2 pixels of his texture, mirrored). | Solid red, 2 pixels an eye. | `vp_disc_sector` (called by `hle_CdRead` for each sector read): the 2 pixels at (2, 6) of the first TIM of `TMS/AX.TMS`, set to the palette's colors nearest dark brown and white as the file is read. Above 1X, the eye is drawn sharp (white, a round dark brown iris, the user's pick): `vp_vram_upload` (every image put in VRAM, `gpu.c`) knows Axel's image and CLUT by their pixels wherever his car slot puts them, and the 8x5 texels around the eye are drawn from a 128x80 picture instead (PSY-Z patch 10, `Psyz_VideoSetTextureHD`); another upload over the image ends it. |
| `la_quake_finale` | Full quake finale | Bug fixes | Every Los Angeles quake ends with its 20 frames of explosions and the flash. | The end time is kept in a signed byte, so a quake whose debris lanes finish after frame 107 retires at once, without its finale (decomp `RETAIL_BUGS.md` 2). | wrap of `800D5AF0` (laEventsUpdate): the end time kept as s16 by the port, the event kept alive (+1) until it. Not seen running yet (needs a remote bomb on the quake zones). |
| `secret_cars_last` | Secret cars last | Menus | Car select's carousel goes Roadkill, Twister, Axel, Mr. Slam, Shadow, Hammerhead, Outlaw 2, Warthog, Mr. Grimm, Grasshopper, Thumper, Spectre, then Sweet Tooth and Minion (when unlocked), then Roadkill again. Locked cars are still skipped, and the screen still opens on the same car. | The carousel steps through the car-select indices 0-13 in order, so Sweet Tooth (12) and Minion (13) sit between Shadow and Hammerhead. | wrap of `8012305C` (TM2FourWay::run): a step on a 14-entry choice follows the new order, asking the choice's accept callback (`8012CC10`) as the original does. wrap of `80122078` (choice set) called from car select (`80131C8C`): the start value, kept as "car − 1", becomes the car before it in the new order, since the screen opens with a +1 step. |
| `carsel_portraits` | Car select portrait bar | Menus | Along the bottom of car select, every driver's portrait (larger) in its order, the carousel's framed in copper, a locked secret driver a "?" card; and the car raised nearer the middle of the screen, so big cars (Minion) stay clear of the name and the portraits. (The car select itself is always the port's HD one, see MENUS_PLAN.md.) | The bottom of car select is empty; the car where the game puts it. | drawn into the port's car select overlay (`carsel_compose`); the car raised by wraps of `80123E1C` (place) and `80123AC8` (move to): the carousel's (object 9005) height, their third coordinate, +10. |
| `carsel_multi` | New multiplayer car select | Menus | Three values. **Split** (the Vanilla Plus one): each player's own car select (the car select picture, their portrait, SELECT CAR / CAR INFO, a turning car, their own CAR INFO page), side by side over a strip of every driver's portrait with the P1 / P2 tags; 3-4 players in quarters (to come). **Shared**: one car select picture, a car for each player in their half. **Original**: each player in turn. Controls for both: Left/Right slide, R1 random, X lock in, O unlock or back; split adds Up/Down for CAR INFO. | Each player picks in turn on the (1-player) car select. | `ui_CarSelect` runs the game's screen with: wrap of `80122124` (the scene draw) drawing the carousel once per player (its car byte, place, heading); wrap of `80155E80` (viewProc) moving each projection centre into the player's view; wraps of `801372C4` and the titles' draw hiding the game's text; `rt_pad_filter` keeping the game's pads idle, then giving it X once per player to record the cars (decomp `CAR_SELECT_CAR.md`). Split: its whole layout is the background, composed at 1280x960 and put in the renderer's upscaled VRAM (`Psyz_VideoLoadImageHD`, wrap of `8012201C`); the cars draw over it. |
| `carsel_stats` | Car select stat bars | Menus | Car select shows the car's armor, special weapon and speed under SELECT CAR / CAR INFO as bars of five segments, colored red, orange, yellow, green, bright green, filled by the game's own values (decomp `CAR_STATS.md`): fractions fill part of a segment. Full bar: armor 175 HP (Minion's 210 reaches a sixth segment), special 47 damage per use (Thumper; a starting point: special is hard to rate), speed 141 (Twister). | Only CAR INFO rates the cars, with one to five balls drawn in its pictures (`BIOS/xxINFO.TIM`), kept as they are. | drawn in the port's car select overlay (`draw_car_stats` in `carsel_compose`). The overlay is off while CAR INFO is up (wrap of `801291CC`, the wait, called from car select). |
| `duplicate_cars` | Allow duplicate cars | Menus | **Off and grayed out for now (2026-10-07):** with two cars of one type one of them was invisible, took no pickups and drove through walls; the per-type tables need keying by car (decomp `SURVIVAL.md` 2). Players may pick the same car (car select, 2-4 players). Computer opponents still avoid the players' cars. | A car another player has chosen is skipped. | wrap of `8012CC10` (car accept): the players' count (settings +4) is 0 during the check, except when the opponent draw (`8012CD08`) asks; the split car select's `cs_taken`. Checked: on, both players on Roadkill; off, player 2 passed on to Spectre. |
| `carsel_sounds` | Car select special sounds | Menus | Highlighting a driver in car select plays their special weapon's sound, as Twisted Metal 1 did (1 player's carousel and the 2-player live select); it stops when car select ends. | No sound. | `vp_carsel_sound` (fixes.c): sound by car type as `weaponFireSpecial` plays it (Hammerhead the crush 37, Thumper the flame 47, Mr. Slam the arm 57, Minion 13); the one bank (UACORE.VAB) is loaded from boot. Checked (trace): each step plays the new driver's. |
| `no_round_restarts` | No round restart on death | Balancing | A player losing a life now respawns, instead of resetting the match: that car alone comes back, at the start point farthest from the other cars, with 2 seconds of shield. Tournament and Challenge Match; the last life ends the round as the game's (GAME OVER, the results). | A death restarts the battle for everyone (Challenge Match with lives: the round starts over, the tally kept). | wrap of `800D181C` (rtScheduleEvent): a death's event 1 (3/4 in Deathmatch) not scheduled while lives remain; the car alone brought back after the same delay (decomp COOP_LIVES.md 2: the per-car part of `carRestart`, `humanCarPlaceAtStart` with every other start entry marked taken, the object shown, +0xAA shield). |
| `lives_per_level` | Tournament lives per level | Balancing | 3 lives at the start of every level (2 more after the first: the HUD's "Lives" counts those, so it shows 2), as in later Twisted Metal games; co-op too, in the game's shared pool for now (each player's own, and playing on alone: decomp request 10). | 3 lives for the whole tournament (co-op 6 shared); the HUD shows 2 at the start. | wrap of `801334C0` (every level's start: car select, password or load, and between levels): settings +20 (and +21) set. Checked: a new tournament's first battle showed "Lives: 1" with 2 (so 3 shows 2). |
| `passwords_off` | Remove password menu | Menus | The port's Tournament page has no Password row, and the pause menu shows none. Off: a Password row (1 player): six symbols, Up/Down each, decoded as the game's (`8012CE7C`), starting that level as a load does. The port's menus either way. | The 1 Player menu has Tournament, Challenge Match and Password; the pause menu shows the tournament's password. | wrap of `8012E908` (1 Player menu): the port's pages, then the menu's own settings for the mode (`docs/MENUS_PLAN.md`); `ui_PauseMenu` leaves the password out. |
| `photo_rewind` | Photo mode rewind | Menus | Off by default (the player turns it on). In a battle, photo mode can go back up to 30 seconds: a timeline over its legend, L2/R2 move the mark back and forward (a quarter second a press; held, faster the harder the trigger is pressed) and X goes there: the battle is shown at that moment, the camera where it was (2026-10-28: letting go jumped at once, so a tap of L2 rewound by accident); X plays it on from there at the game's speed, heard, seen from the photo camera, up to the present (X stops it, O goes back); O goes back to the present and the pause menu. Costs about 150 MB of memory (16 copies of the game's 8 MB RAM). | Photo mode shows only the paused moment. | `vplus/killcam.c`: a snapshot of the machine (as the kill cam's) every 2 seconds of the gameplay clock, the last 16, and every frame's buttons (4096 frames). A moment asked for ends the pause with the photo held on the screen (`photo_hold`); at the next SetFrameStart the present is kept, the snapshot before the moment loaded and the frames replayed with their buttons, paced off (`rt_fast_forward`), effects muted; there photo mode opens again (`photo_rewind_session`). Playing: `killcam_camera` puts photo mode's camera on view 1. Back: the present loaded and the pause opened again (`ui_pause_again`). Test: scenario `rewind` (`TM2PC_REWIND_TEST`). |

### The balancing rules (decomp requests 40-42)

Each rule is a Vanilla Plus option (Global Balancing for the rules every car shares, then Character Balancing, one per
car; always on online) and one or more bits of the decomp's `portBalance`
word, written at each match's load (`vp_balance_word`, four.c; the decomp's `BALANCE.md`). Zero bits: retail.

| Key | Name | Bits | What changes |
|---|---|---|---|
| `bal_missiles` | Missile damage swap | 0 | fire missile 10, homing 7 |
| `bal_shield` | No wasted shield | 1 | a shield cast while one is up is refused, no cost |
| `bal_mines` | Permanent mines | 2 | mines stay; 5 per driver (a 6th removes the oldest) |
| `bal_freeze` | Freeze diminishing returns | 17 | an ice freeze within 1 s of the last (or during it) lasts half the last one (floor 5 frames) |
| `bal_ricochet` | Safe ricochet launch | 18 | a ricochet bomb cannot hit its owner before it has cleared the car |
| `bal_guns` | No chance machine guns | 20 | every connecting bullet does half damage (no 50 % coin flip; the same on average) |
| `bal_hammerhead` | Hammerhead balancing | 3, 11, 24 | rams x1.2, crush 15 + 5 on top, recharge 35 s; a borrowed special stays the lender's; brakes 2 and slide speed 60 (were 1 and 25: request 77) |
| `bal_axel` | Axel balancing | 4 | no crush, rams x1.1 |
| `bal_minion` | Minion balancing | 5 | no crush, rams x1.1, recharge 30 s |
| `bal_twister` | Twister balancing | 6, 12, 15 | no weapon at all during the tornado (refused, nothing spent), no ram damage then, recharge 30 s, the last hit throws every caught car |
| `bal_grasshopper` | Grasshopper balancing | 7, 13, 19 | leap 18, touching counts, no ram damage while leaping, a clean landing; her energy jump free |
| `bal_outlaw2` | Outlaw 2 balancing | 16, 8 | a taser shot that connected to no one may connect later in its time (one hit of 10 and the lift); recharge 45 s |
| `bal_grimm` | Mr. Grimm balancing | 21 | recharge 60 s |
| `bal_thumper` | Thumper balancing | 22 | recharge 75 s |
| `bal_shadow` | Shadow balancing | 10 | the slope splash bug fixed: 28 at most |

Each car's recharge has its own bit since decomp request 45 (PLAYTEST_ANSWERS.md; `BALANCE.md` 14.1: Outlaw 2 8, Mr. Grimm 21, Thumper
22). Bit 18 also lets a ricochet pass through its owner's car until it has left his box: no bounce, red blink or push
(14.2). The older keys `bal_recharges` and `bal_taser` are no longer read.
The final kill cam (`final_kill_cam`, Global Balancing) takes `off`, `1`, `2` or `3` shots (default 3; an older `on`
reads as 3).

Bits 9 and 14 are reserved (Sweet Tooth's spiral and Grasshopper's leap stay retail's). The decomp's
`portVisual` word (request 42, CUSTOM_TOURNAMENT_ANSWERS.md, `vp_visual_word`): bit 0 always set, no option since 2026-10-17 (the taser and the
lightning sorted by depth, no longer drawn over everything), bit 2 always set (each view draws its own car first, so a
full mesh queue or packet buffer never drops it: request 45 (PLAYTEST_ANSWERS.md)), and bit 1, the Bug fixes option `taser_low` **Taser Visual Fix** (it starts
at Outlaw 2's gun height, less jitter at the start). Tests: `TM2PC_TEST_PRE42=1` runs with the rules before requests
41-42 (balance `& 0x3DFF`, no visual bits, the 12 random opponents), to compare with older baselines.

## Planned

- **Santa in Antarctica** (Restored): the Santa pedestrian is in the 1997 PC version only (the
  manual shows him; a long-running urban legend for the PS1). Bring his sprites from the PC's
  level pack (`LEVELDB/GLACIER.TPC`, see `extracted/images-pc/LEVELS/GLACIER`) into the PS1
  Antarctica, with what places and animates a pedestrian (to find: the PS1's pedestrian
  tables, and how the PC version adds him). A portrait unlock is tied to killing him
  (`PORTRAIT_UNLOCKS.md`, Other - 3).
