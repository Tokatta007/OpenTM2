# Portrait unlocks

The user's list (`portrait_unlocks - V2.xlsx`, 2026-10-05). Each portrait's unlock, keyed to its
picture: `Source` is the movie still's id in `tools/art/movie_portraits.json` (or where a
driver's portrait comes from). Names as the game shows them (`tools/art/portrait_sheet.py`).

Rules:

- **Tournament wins are unique:** an ending (and what comes with it) is earned once per driver;
  winning again with the same driver earns nothing new (the feats during the run still count).
- **Counters** (a number in the unlock, the `Count` column) show their progress on a locked
  portrait, on a line under its description: "3 / 10".
- **Online counts too:** kills, deaths, feats in online matches count as offline. Winning the
  online *Versus Tournament* (the "TM2 Original" ruleset only) earns the winner's ending: it
  plays if the player chooses, otherwise a message says it is unlocked and is in Movies.
- Events needed (kills by weapon, ram kills, deaths, falls, freezes, pedestrians, pickups,
  turbo, top speed, the Eiffel Tower): a request to the decomp.

**Made (decomp request 7, `GAME_EVENTS.md`):** the feats are counted per profile (player 1's
signed-in profile, player 2's own; guests count nothing) in the profile file's `[feats]` section,
with a check as the progress (`src/profile/feats.c`). The events come from wraps of the game's
damage (`800E5628`), weapon hit (`800DC65C`), destroyed (`8011BE10`), hit test (freezes,
`800D6FD8`), pickup (`8010AFC8`), pedestrian (`80124384`), splash (`8010B5C4`), lightning
(`80107B00`), Eiffel Tower (`800D6710`) and launch (`801052BC`, `80105A7C`: rear fire, specials)
functions, the call site told by the return address (`src/profile/feat_hooks.c`); top speed and the
turbo are read each frame. They are written at the end of a match, on pause and on exit. Portrait
2 also counts as earned with the driver's ending (earned before feats were counted). Not yet:
Santa (not in the PS1 data: the PC's to be restored), pedestrians hit by a special (only run-overs
and weapon hits count), a Custom Battle (a Challenge Match counts for now), the kill feed.
Tests: `TM2PC_FEAT_DEBUG=1` traces the damage calls, `=2` keeps the computer cars at 1 HP.

| Portrait | Unlock | Source | Count |
|---|---|---|---|
| Roadkill - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Roadkill - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Roadkill - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Roadkill - Ending 1 | Get a remote bomb kill | ROADKILL_02_marcus |  |
| Roadkill - Ending 2 | Roadkill's ending (win the tournament on Medium or Hard) | ROADKILL_08 |  |
| Roadkill - Ending 3 | Roadkill's ending (win the tournament on Medium or Hard) | ROADKILL_09_nurses |  |
| Roadkill - Ending 4 | Roadkill's ending (win the tournament on Medium or Hard) | ROADKILL_10 |  |
| Roadkill - Ending 5 | Roadkill's ending (win the tournament on Medium or Hard) | ROADKILL_16_c1 |  |
| Twister - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Twister - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Twister - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Twister - Ending 1 | Pick up 10 turbo powerups | INDY_03 | 10 |
| Twister - Ending 2 | Pick up 88 turbo powerups | INDY_05_twister | 88 |
| Twister - Ending 3 | Reach top possible speed in-game | INDY_09_c1 |  |
| Twister - Ending 4 | Twister's ending (win the tournament on Medium or Hard) | INDY_22_c1 |  |
| Twister - Ending 5 | Twister's ending (win the tournament on Medium or Hard) | INDY_23_c2 |  |
| Twister - Ending 6 | Twister's ending (win the tournament on Medium or Hard) | INDY_27 |  |
| Axel - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Axel - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Axel - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Axel - Ending 1 | Axel's ending (win the tournament on Medium or Hard) | AXEL_09 |  |
| Axel - Ending 2 | Axel's ending (win the tournament on Medium or Hard) | AXEL_14 |  |
| Axel - Ending 3 | Axel's ending (win the tournament on Medium or Hard) | AXEL_16 |  |
| Mr. Slam - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Mr. Slam - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Mr. Slam - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Mr. Slam - Ending 1 | Mr. Slam's ending (win the tournament on Medium or Hard) | FRONTL_10 |  |
| Mr. Slam - Ending 2 | Die once in any match | FRONTL_14 |  |
| Mr. Slam - Ending 3 | Mr. Slam's ending (win the tournament on Medium or Hard) | FRONTL_21_c2 |  |
| Mr. Slam - Ending 4 | Die 10 times | FRONTL_26 | 10 |
| Mr. Slam - Ending 5 | Die 50 times | FRONTL_28 | 50 |
| Mr. Slam - Ending 6 | Mr. Slam's ending (win the tournament on Medium or Hard) | FRONTL_29 |  |
| Mr. Slam - Ending 7 | Fall off any map | FRONTL_29_look |  |
| Mr. Slam - Ending 8 | Fall off any map 5 times | FRONTL_30_fall | 5 |
| Shadow - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Shadow - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Shadow - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Shadow - Ending 1 | Shadow's ending (win the tournament on Medium or Hard) | HEARSE_07_shadow |  |
| Shadow - Ending 2 | Shadow's ending (win the tournament on Medium or Hard) | HEARSE_03 |  |
| Hammerhead - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Hammerhead - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Hammerhead - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Hammerhead - Ending 1 | Get 1 Ram kill | MONSTERT_02_afro |  |
| Hammerhead - Ending 2 | Get 10 Ram kills | MONSTERT_02_cap | 10 |
| Hammerhead - Ending 3 | Hammerhead's ending (win the tournament on Medium or Hard) | MONSTERT_03 |  |
| Hammerhead - Ending 4 | Hammerhead's ending (win the tournament on Medium or Hard) | MONSTERT_06 |  |
| Hammerhead - Ending 5 | Hammerhead's ending (win the tournament on Medium or Hard) | MONSTERT_07 |  |
| Hammerhead - Ending 6 | Hammerhead's ending (win the tournament on Medium or Hard) | MONSTERT_16_afro_fall2 |  |
| Hammerhead - Ending 7 | Hammerhead's ending (win the tournament on Medium or Hard) | MONSTERT_16_cap_fall2 |  |
| Hammerhead - Ending 8 | Hammerhead's ending (win the tournament on Medium or Hard) | MONSTERT_16_afro_splat |  |
| Hammerhead - Ending 9 | Hammerhead's ending (win the tournament on Medium or Hard) | MONSTERT_16_cap_splat |  |
| Outlaw 2 - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Outlaw 2 - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Outlaw 2 - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Outlaw 2 - Ending 1 | Get 10 kills with a fire missile | COPCAR_06_last | 10 |
| Outlaw 2 - Ending 2 | Kill yourself with your own mine or weapon | COPCAR_08 |  |
| Outlaw 2 - Ending 3 | Outlaw 2's ending (win the tournament on Medium or Hard) | COPCAR_11_both |  |
| Outlaw 2 - Ending 4 | Outlaw 2's ending (win the tournament on Medium or Hard) | COPCAR_12 |  |
| Warthog - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Warthog - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Warthog - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Warthog - Ending 1 | Get 105 kills total | HUMVEE_01_warthog | 105 |
| Warthog - Ending 2 | Warthog's ending (win the tournament on Medium or Hard) | HUMVEE_05_big |  |
| Warthog - Ending 3 | Warthog's ending (win the tournament on Medium or Hard) | HUMVEE_05_small |  |
| Mr. Grimm - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Mr. Grimm - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Mr. Grimm - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Mr. Grimm - Ending 1 | Get 10 kills with a power missile | MRGRIM_03 | 10 |
| Mr. Grimm - Ending 2 | Mr. Grimm's ending (win the tournament on Medium or Hard) | MRGRIM_04 |  |
| Mr. Grimm - Ending 3 | Mr. Grimm's ending (win the tournament on Medium or Hard) | MRGRIM_24_clean |  |
| Grasshopper - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Grasshopper - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Grasshopper - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Grasshopper - Ending 1 | Get a kill with a mine | DUNE_04 |  |
| Grasshopper - Ending 2 | Grasshopper's ending (win the tournament on Medium or Hard) | DUNE_07 |  |
| Grasshopper - Ending 3 | Freeze 15 enemies | DUNE_08 | 15 |
| Grasshopper - Ending 4 | Grasshopper's ending (win the tournament on Medium or Hard) | DUNE_17_c2 |  |
| Grasshopper - Ending 5 | Grasshopper's ending (win the tournament on Medium or Hard) | DUNE_21_last |  |
| Grasshopper - Ending 6 | Get frozen by enemies 15 times | DUNE_22z | 15 |
| Grasshopper - Ending 7 | Grasshopper's ending (win the tournament on Medium or Hard) | DUNE_23_hug |  |
| Thumper - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Thumper - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Thumper - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Thumper - Ending 1 | Thumper's ending (win the tournament on Medium or Hard) | IMPALLA_04 |  |
| Thumper - Ending 2 | Thumper's ending (win the tournament on Medium or Hard) | IMPALLA_08_c4 |  |
| Spectre - Portrait 1 | Free | car select (CARSEL.TPC) |  |
| Spectre - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Spectre - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Spectre - Ending 1 | Spectre's ending (win the tournament on Medium or Hard) | VETTE_01_c3 |  |
| Spectre - Ending 2 | Get 10 kills with a homing missile | VETTE_04_spectre | 10 |
| Spectre - Ending 3 | Spectre's ending (win the tournament on Medium or Hard) | VETTE_09_c2 |  |
| Spectre - Ending 4 | Spectre's ending (win the tournament on Medium or Hard) | VETTE_12_c2 |  |
| Sweet Tooth - Portrait 1 | Free once Sweet Tooth is unlocked | car select (CARSEL.TPC) |  |
| Sweet Tooth - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Sweet Tooth - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Sweet Tooth - Ending 1 | Win tournament with character on Medium/Hard | SWEET_01_c4 |  |
| Sweet Tooth - Ending 2 | Win tournament with character on Medium/Hard | SWEET_04_bug_b |  |
| Sweet Tooth - Ending 3 | Win tournament with character on Medium/Hard | SWEET_05_bug |  |
| Minion - Portrait 1 | Free once Minion is unlocked | car select (CARSEL.TPC) |  |
| Minion - Portrait 2 | Get 10 kills with Driver's Special | car select (CARSEL.TPC) | 10 |
| Minion - Portrait 3 | Win tournament with character on Medium/Hard | info screen (BIOS) |  |
| Minion - Ending 1 | Win tournament with character on Medium/Hard | MINION_02_c1 |  |
| Minion - Ending 2 | Win tournament with character on Medium/Hard | MINION_04_c4 |  |
| Calypso - Intro 1 | Start a tournament | INTRO_06a |  |
| Calypso - Intro 2 | Play and finish a custom battle match | INTRO_21_last |  |
| Calypso - Intro 3 | Lose all your lives in a tournament | INTRO_22 |  |
| Calypso - Intro 4 | Win a tournament | INTRO_25_eye |  |
| Calypso - Ending 1 | Get a kill with the Lightning weapon | AXEL_03_calypso |  |
| Calypso - Ending 2 | Beat tournament with 7 unique characters | AXEL_23 | 7 |
| Calypso - Ending 3 | Outlaw 2's ending (win the tournament on Medium or Hard) | COPCAR_01 |  |
| Calypso - Ending 4 | Outlaw 2's ending (win the tournament on Medium or Hard) | COPCAR_09_c1 |  |
| Calypso - Ending 5 | Grasshopper's ending (win the tournament on Medium or Hard) | DUNE_09 |  |
| Calypso - Ending 6 | Grasshopper's ending (win the tournament on Medium or Hard) | DUNE_22_first |  |
| Calypso - Ending 7 | Mr. Slam's ending (win the tournament on Medium or Hard) | FRONTL_11_clean |  |
| Calypso - Ending 8 | Shadow's ending (win the tournament on Medium or Hard) | HEARSE_08_calypso |  |
| Calypso - Ending 9 | Shadow's ending (win the tournament on Medium or Hard) | HEARSE_14_calypso |  |
| Calypso - Ending 10 | Shadow's ending (win the tournament on Medium or Hard) | HEARSE_26_c1 |  |
| Calypso - Ending 11 | Shadow's ending (win the tournament on Medium or Hard) | HEARSE_33_calypso |  |
| Calypso - Ending 12 | Warthog's ending (win the tournament on Medium or Hard) | HUMVEE_01_calypso |  |
| Calypso - Ending 13 | Thumper's ending (win the tournament on Medium or Hard) | IMPALLA_02 |  |
| Calypso - Ending 14 | Hammerhead's ending (win the tournament on Medium or Hard) | MONSTERT_12 |  |
| Calypso - Ending 15 | Hammerhead's ending (win the tournament on Medium or Hard) | MONSTERT_17_c1 |  |
| Calypso - Ending 16 | Roadkill's ending (win the tournament on Medium or Hard) | ROADKILL_02_calypso |  |
| Calypso - Ending 17 | Spectre's ending (win the tournament on Medium or Hard) | VETTE_04_calypso |  |
| Other - 1 | Kill a pedestrian | INTRO_15_left |  |
| Other - 2 | Kill 10 pedestrians | INTRO_15_right | 10 |
| Other - 3 | Kill a Santa pedestrian | HEARSE_34_kid |  |
| Other - 4 | Kill an enemy with Napalm | INDY_14_chef |  |
| Other - 5 | Die while on fire | INDY_14_family |  |
| Other - 6 | Get 50 ram kills | INDY_29 | 50 |
| Other - 7 | Get a kill with all weapons | INDY_30_kid_b |  |
| Other - 8 | Kill an enemy with a ricochet bomb | INDY_18_pirate_c2 |  |
| Other - 9 | Kill 50 enemies with your machine guns | MRGRIM_15_gunman | 50 |
| Other - 10 | Get 500 Kills | MRGRIM_18_c2 | 500 |
| Other - 11 | Destroy the Eiffel Tower | MRGRIM_19_c1 |  |
| Other - 12 | Pick up 100 health packs | ROADKILL_09_doctor | 100 |
| Other - 13 | Make it past Round 5 in Survival | SWEET_05_gardener |  |
