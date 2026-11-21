# Hardening before mods and online play

An audit of the port's own C (2026-10-15, read-only) for what breaks with many mods or online. Items keep their
audit numbers; file:line as of that day. Status: open unless marked.

## 1. Crash or memory corruption (fix first)

| # | Where | Problem |
|---|---|---|
| 1 | `ui/options.c` `add()` | `rows[64]` with no bound: the game-type list (6 headers + 10 built-in + 24 own + 72 guests' types) can pass 64 today; a Mods page will |
| 2 | `ui/options.c` opponents row | `number_values` has 9 entries, shown up to 15-16 with a 16-car build: reads past the array |
| 3 | `profile/feat_hooks.c` `match_begin` | `mcars/mslot/mhome[16]` filled with up to 4 humans + every AI record, no bound |
| 4 | `vplus/fixes.c` `watch_step` | `cand[17]` gets up to 3 players + 16 computer cars |
| 5 | `profile/profile.c` (3 places) | a profile read into a fixed 64 KB buffer and written back: anything past it is lost; `strtok` drops blank lines |
| 6 | `ui/options.c` | `boss_room[13]`, `tournament_arenas[13]`, `arena_values[12]` indexed by level with no check |
| 7 | `profile/feats.h` | special kills one bit per car (14) in a 64-bit word: safe only while cars stop at 13 |
| 8 | `vplus/fixes.c`, `vplus/interp.c` | car slots masked `& 15`: slot 16+ touches another car's object |

## 2. Bugs found on the way

| # | Where | Problem |
|---|---|---|
| 27 | `fixes.c` `car_kills_team` | `player_team[human & 1]`: players 3-4 scored on players 1-2's teams |
| 28 | `fixes.c` `vp_AiCarDamageUpdate` | returns for computer car 9+: in 12-16 car matches cars 10-16 get no lives or respawns |
| 27b | `ui/options.c` `team_paint_begin` | car select's team colors use one VRAM row per player for players 1-2 only (`40 + (p & 1)`): players 3-4 share them; needs two more free rows (check VRAM 768, 42-43) |

## 3. Online: state outside the game's memory (rollback, kill cam, desync)

The kill cam already snapshots guest RAM and a registry of the port's own state (`vp_keep_register`,
`feats_keep_register`). Missing from it:

| # | State | Why it matters |
|---|---|---|
| 9 | the match tally (`mcars` and friends, `feat_hooks.c`) | decides Deathmatch, Gun Game, Juggernaut, Hunted, KOTH winners and Gun Game weapons; a kill-cam replay hands out the post-kill level's weapon today |
| 10 | `blow[]`, `tags[]`, `lives_before[]`, `cur_attack`, `splash_attack`, lightning caster/until | which attack gets a kill (Gun Game advancement); lightning timed by `M.vblanks` not the game clock |
| 11 | `igniter[]` | damage tally |
| 12 | LA quake finale `end[3]` | changes the game's event state |
| 13 | Juggernaut ghost `shim_slot`, `shim_seen` | hides cars and radar blips |
| 14 | `wv[4]`, `watch_hide`, `spectate_prev`, `showdown_on`, the HUD hotkey edges | drive writes into the game |
| 15 | `flipped[4]`, `four_n/four_car/four_left`, `jc_opp` | set at setup; fine for the kill cam, not for rollback across a hotkey |

## 4. Online: nondeterminism

| # | Where | Problem |
|---|---|---|
| 16 | `killcam.c` | the port's random stream seeded from the PC each battle: pickups, roulette, Survival waves, Juggernaut/Hunted picks, hills draw from it; online: one shared seed |
| 17 | `four.c` `four_radars` | 3-4 players: calls the game's own `rand()` per radar per car (depends on local views) |
| 18 | AI update throttle | retail skips "unseen" AI updates: depends on the local cameras (split mode, widescreen frustum, draw distance, mirror); online: off (`vp_ai_vs_ai`-style) |
| 19 | `four.c` `streaksDraw`, `weaponArcsDraw` | mines/flames tick and lightning damage happen per drawn view: depends on how many views a machine draws |
| 20 | HUD, radar, camera code | local settings written into game memory mid-battle (radar/list bytes, no-target, messages, showdown music, L1/R1 swap); scratch areas to exclude from state hashes or make render-only |
| 21 | `vp_on` rule switches | gameplay rules from local tm2pc.ini (spawn spacing, LA quake, Santa, round restarts, lives, co-op spectate, duplicates, balance); kill cam / photo rewind hold the round: online forces one ruleset |
| 22 | game types and controls | the match's game type looked up by name per machine; jump combo, easy energy, analog steer applied locally: send post-mapping inputs |
| 23 | kill cam shots | `SDL_rand` and `M.vblanks` choose cameras written into game memory |
| 24 | `ui/options.c` | random arena (never secret ones) and random cars from the PC's random numbers: the host decides |
| 25 | `fixes.c` | doubles in nearest-car, spawn and KOTH distance tests: use 64-bit integers |
| 26 | `ui/options.c` | lives/round decisions depend on `text_ready()` (font loaded) |

## 5. Limits for mods

| # | Limit |
|---|---|
| 29 | `PROFILE_CHARACTERS 14`: endings, saves' car check, the portrait grid, Juggernaut car |
| 30 | `BATTLE_DRIVERS 14` and `drivers` one bit per car (masked to 14 bits); `vp_CarAccept` `car < 14` |
| 31 | 15 opponent slots, 12 opponent cars; `team_of_ai` `% 15` (computer car 16 gets car 1's team) |
| 32 | 8 + 3 arenas: hill spots, `room[9]`/`types[9]`, saves' level 1-8, `four.c` levels 1-8 |
| 33 | `EXTRA_CARS 1` portrait slots |
| 34 | car type = `type >> 4`, 16 types: kill tallies, special sounds, names, Gun Game masks, car order, team paint |
| 35 | loops capped at 9/11/13 computer cars in several places (respawn, aim, cinema, spawn) |
| 36 | `HILLS_MAX 3` (5-8 wanted), pickups 32/50, Juggernaut 32, marks 64, start points 15 |
| 37 | 24 game types per profile; `builtin[16]` has no check |
| 38 | `PROFILE_MAX 32`, 5 saves, Custom Music 256 files |
| 39 | settings lines 256/1024 characters: a longer line splits into a bogus key |

## 6. Minor, for later

- **40** One local player assumed in places (spectating, `my_team`, `dm_frame` win/lose for player 1), arrays of 4.
- **41** Direct controller reads in gameplay (`M.pad_buttons`, `input_player_bits`, SDL reads): online replaces them
  with networked inputs (`hle.c` is the choke point for players 1-2).
- **42-43** Per-frame linear scans and `getenv` calls (retargeting, hills, symbol lookups); fine at 16 cars, worth
  caching.
- **44** File writes during a match (HUD hotkey saves the profile).
- **45** Snapshots are whole 16 MB copies (about 450 MB held): rollback needs smaller ones and a complete registry.
- **46** Test hooks (`TM2PC_TEST_*`, `TM2PC_FEAT_DEBUG`) change gameplay: disabled online.
- **47** Guest game types fixed at 3 owners; players 3-4 get player 1's pad settings.

## Plan

1. Section 1 and 2 now (crash risks and the two bugs).
2. Section 5 with the mod loader (lists by ID instead of fixed counts).
3. Sections 3-4 and 6 as the start of online work: one shared seed and ruleset, render-only HUD writes, the state
   registry completed, networked inputs.
