# The new menus (the user's plan, 2026-10-05)

Every front-end menu is to be replaced by the port's own, drawn at the window's
resolution but looking like the old ones (their clean backgrounds, `MENU_ART.md`;
the game's music and sounds). New screens draw their own sides on wide screens
(`[video] menu_sides`), so the old menus get no side fill of their own.

## The flow

1. **Title** (done): LOCAL PLAY, NETWORK PLAY, OPTIONS; O goes back to the profiles.
2. **Local Play: how many players** (1, 2, 3, 4).
   - **1:** on to the 1 Player menu.
   - **2-4:** the screen splits (2: halves; 3-4: as the game splits them, halves then
     quarters). Each player signs in, or makes a profile, picks a portrait and presses
     Ready. Then one screen again for the mode, and split again for car select.
     Split car select is a Vanilla Plus option (purists keep the original).
3. **1 Player menu:** TOURNAMENT, CHALLENGE MATCH (and PASSWORD when Vanilla Plus
   "Passwords off" is off: then the game's own menu is used).
4. **Tournament:** NEW TOURNAMENT, LOAD TOURNAMENT, CUSTOM TOURNAMENT.
5. **Challenge Match:** many new options, to be laid out so it is easy and not
   overwhelming (to design).
6. **Car select** and **level select**: new, looking like the old. Vanilla Plus option:
   a row of every driver's portrait as in a fighting game, a highlight on the chosen one
   (good for split screen); locked drivers a black "?" card, as on the portrait screen.

**2-player sign-in (made):** after 2 Players, the screen splits in halves, each read from
its player's controller (`input_player_menu_buttons`). Player 2 joins with Start on a second
controller (it becomes player 2's). Each half is laid out as the profile screen: the profiles,
(for player 2) Guest, a "?" portrait, then New Profile on the left, the chosen one's portrait on
the right. On a wide window with the sides drawn, each half of the window is that player's,
its list and portrait centered in it; otherwise each player's area is framed. New Profile's name is entered on that half, so both can name at once (the keyboard
types for player 1 only; player 2 uses their controller). X readies; O un-readies or backs
out. A profile can be one player's only. When both are ready, X or Start goes on and Square
opens Controls with a Player row (player 2's controls are saved to their profile's
`[controls]`). Controls from the title's Options has no Player row: only player 1's. Player
1's profile is signed in and player 2's layout and controller buttons come from theirs (the
keyboard stays player 1's); then the game's 2 Player menu, for now. Tests:
`TM2PC_P2_SCRIPT` drives player 2; `--ui-shot 93` (row -2: player 2 naming).

**Car select's art in the background (made):** with the upscaled VRAM (internal resolution above
1), the 1-player and shared screens' art (portraits, titles, emblem, the drivers' bar) is composed
with the PC picture into the background, and the game's own portrait (objects 9006/9007), emblems
(9008/9009) and titles (9001/9002) are not drawn: the cars, arrows and name draw over it, at any
window shape. The second portrait (shown once a driver is chosen, 9007) is put up through the
overlay for the moment before the next screen. CAR INFO uses the PC's picture. Without the
upscaled VRAM, the overlay as before.

**Sharp backgrounds (PSY-Z patch 08):** `Psyz_VideoLoadImageHD` puts a larger picture into the
renderer's upscaled VRAM where the game has just loaded its own (320x240). Car select's background
(the PC's 640x480 `CARSEL_TIM`, its legend cleaned away) goes in each frame (wrap of `8012201C`),
so the road and flames behind the cars are as sharp as the port's art.

**Profile options in the 2-player sign-in (made):** Square on a profile in either half: PORTRAIT
(the portrait grid in the half, 6 a row, its name and how a locked one is earned below), RENAME
(the letter grid in the half), DELETE (X held 3.5 s, a bar filling). While a half changes a profile
it is signed in (its unlocks and the port's calls apply), then player 1's again; one half at a
time. Preview: `--ui-shot 93,-3|-4|-5`.

**New car select (made):** always the port's (no original to fall back to).
- **1 player:** the top (clean picture, portrait, SELECT CAR / CAR INFO in the port's lettering,
  the chosen one on the copper bar, the emblem) and bottom drawn at the window's resolution
  around the game's turning car; no legend. The portrait bar along the bottom is Vanilla Plus
  `carsel_portraits`.
- **2 players:** the port's own screen, both choosing at once: each half the driver's large
  Portrait 3, name and READY; the portrait row between the halves (one above the other on
  4:3, side by side with the row at the bottom on wide windows), player 1 red and player 2
  blue with P1 / P2 tags; no duplicate cars (a locked-in driver is passed over). Then the
  cars are set as the game's car select sets them and its next screen follows. No 3D car
  here (the game's carousel shows one car).
- `--ui-shot 92,car` previews 1 player, `--ui-shot 91,car` 2 players; `--ui-shot 92,car` previews it (with
`TM2PC_SHOT_BG` a capture of the game's picture).

## The new car select (the user's design, 2026-10-05)

- On the clean car select background (`extracted/menus-clean/SHELLDB1_CARSEL_TIM.png`),
  with the game's pieces: SELECT CAR / CAR INFO (CARSEL.TPC #1, plain and on the bar), the
  driver's portrait at the top left, the player-count emblem at the top right (#24 "1",
  #25 "2"; "3" and "4" to be drawn to match). Isolated copies: `extracted/carsel-assets/`.
- The portrait row at the bottom, where the legend was (the legend moves or shrinks).
  With 1-2 players a black band is added under the picture for it (the picture is not
  squashed). With 3-4 players the row runs across the middle, the line between players
  1-2 and 3-4.
- Highlights: 1 player gold; with more, player 1 red, 2 blue, 3 green, 4 gold, each with
  a small P1-P4 tag at the corner of the portrait it is on.
- Duplicate cars: the original allows none (a chosen car is skipped). A Vanilla Plus option
  "Allow duplicate cars" (tournament), and in Custom Battle a setting for the match's creator
  (the host online). Without duplicates, a chosen driver's portrait shows as taken and the
  others cannot move onto it.
- The 3D car keeps turning in the middle, as the game's carousel draws it.

**Split orientation for 2 players:** on 4:3 one above the other; on a wide window side by
side (or an option). In the port's menus this is easy. In a battle it is the game's
split screen: changing it needs its viewports and cameras (decomp request 5, widescreen).

## To do (from the user's notes, 2026-10-05)

- **2-player car select like the 1-player one:** each half with its live turning car, the
  portrait at the top left and SELECT CAR / CAR INFO (the middle bar and P tags stay). The
  car is to be drawn once per player by the game's own carousel draw (decomp request 9), not
  recorded: a recording would lose the shadow, the R1 drive-in, and later render fixes.
- **Split-screen car select (made, the default):** each player's own car select in their view,
  the drivers' strip below (2 players; 3-4 in quarters with the strip between, to come). Tuning:
  `TM2PC_SPLIT_F` (car scale, %), `TM2PC_SPLIT_Y` (car height in the view, %).
- **Shared screen, to try:** without SELECT CAR / CAR INFO (room for 4 portraits at the top),
  smaller cars; maybe Square for one player's CAR INFO (stops the others meanwhile; needs a legend).
- **2-player car select, live (made, decomp request 9):** the game's car select with a car per
  player, drawn by the game's own carousel draw (each car's place a little further back for the
  big ones: Axel, Mr. Slam, Hammerhead, Sweet Tooth, Minion), animations as the game's (slide,
  R1 drop and rise, turning). Tuning for tests: `TM2PC_CL_Y`, `TM2PC_CL_Z`, `TM2PC_CL_DX`,
  `TM2PC_CL_DY`. To do: CAR INFO per player, 3-4 players.
- **Vanilla Plus `carsel_multi`** "New multiplayer car select" (made, on by default): off,
  each player picks in turn on the 1-player screen.
- **Movies:** the Intro with a Calypso portrait, the Credits with a Sweet Tooth one (the cover
  art: the title picture's face and burning hair; also a new Sweet Tooth portrait).
- **Updated car stats** (Vanilla Plus): new stats, and the CAR INFO pictures to match: the
  ball rows redrawn in new pictures where a stat changes (the original pictures otherwise).
- **Handling words** (CAR INFO: Sluggish, Intermediate, Tight, Ultra Tight): when the turning
  controls (and analog steering) are worked on, compare them with the turning values in the car
  data (decomp `CAR_STATS.md`: turning speed +76, steering +94/+108, slide speed +86).
- **Stat bars** (made on car select, Vanilla Plus `carsel_stats`; CAR INFO keeps its balls): five segments with dividers, filled by the car's actual value
  (fractions fill part of a segment, so close cars still differ); colored by segment: red,
  orange, yellow, green, bright green. Minion alone reaches a sixth segment (6/5), drawn past
  the bar's end. The values from the game's car data (decomp `CAR_STATS.md`).
- **Fighting-game style car select** (draft shown, to decide): the large Portrait 3 at the left,
  the turning car in the middle, name and stats at the right, the portrait row at the bottom.

## Tournament saves

- Bookmarks, apart from the profile's progress: 3-5 slots, overwritten freely.
- After a won level, a menu: CONTINUE, SAVE, QUIT. SAVE picks one of the slots.
  (Autosave once a run has been saved: maybe, if saving is not quick enough.)
- After losing: RESTART (the level again, no password) or QUIT.
- Sweet Tooth and Minion can be saved (the original's passwords cannot).
- What a save holds is what a password holds (level, car, the flags; decomp
  `TOURNAMENT_PASSWORDS.md`), set into the settings block as the password path does.
- **Made:** the slots (5, in the profile file as `[save1]`..`[save5]`: level (from its start: its
  bosses, Minion and Dark Tooth, are part of it), driver, difficulty, when; checked as the progress) and LOAD TOURNAMENT: five
  cards with the driver's Portrait 3 (the info screen's art), name, "LEVEL n / 8", the arena and the difficulty; X loads it the
  password's way (settings, then `801334C0`: opponents, globe, arena).
- **Co-op saves (made):** the five slots are player 1's, for any number of players: a save keeps
  how many played and each driver (`players = 2`, `driver2 = ...`; checked with the rest) and
  loads with as many (a 2-player save runs the 2-player sign-in, then co-op from its level,
  6 shared lives). The card shows both drivers and "2P".
- **Made (decomp request 8, `TOURNAMENT_FLOW.md`):** wraps of `screenScrollText` (80136BB0)
  and the session runner (8012BE5C). After a won level 1-7: LEVEL COMPLETE with CONTINUE,
  SAVE (1 player, signed in: the save cards, X saves over a slot; the item then reads
  "SAVED IN n") and QUIT (as the pause menu's: the title). After the last life: GAME OVER
  with RESTART (the level from its start; players and cars kept, opponents, their count and
  the lives anew: 3, co-op 6) and QUIT. Drawn as the pause menu over the last frame. Tests:
  `TM2PC_TEST_RUNNER=3` (or 1) skips the level with that result; `TM2PC_TMENU_SCRIPT` drives
  the menus; `--ui-shot 88,0|1`.

## Order of work

1. Players screen and the 1 Player and Tournament menus (New Tournament, Challenge
   Match as the game's; Load and Custom shown, not made yet); Vanilla Plus
   "Passwords off" (off: the game's 1 Player menu with its password).
2. Tournament saves, the after-level and game-over menus (decomp: the flow between
   tournament levels and after the last life).
3. Split-screen sign-in for 2 players, then 3-4 (decomp: more than 2 players).
4. Car select (with the portrait row) and level select.
5. Challenge Match's options; Custom Tournament.
