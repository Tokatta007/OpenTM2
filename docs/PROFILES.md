# Profiles and unlocks

The original game keeps no progress: tournament progress lives only in passwords
(the retail executable has no memory-card code), and the secret characters, entered
as codes at car select, last until the console is switched off. The PC version adds
profiles that remember progress for good.

## The profile

- A profile is chosen on the profile screen at startup (docs/OPTIONS.md, Profiles), which
  also makes, renames and deletes them; `tm2pc.ini` remembers the one signed in last
  (`[profile] name = ...`). The name will also be the online screen name.
- Each profile is `profiles/<name>.ini` next to the executable (`src/profile/profile.c`):
  its picture, player 1's controls and its progress.

```ini
[profile]
name = Player
picture = Axel 1        ; a driver's portrait: 1, 2 (car select) or 3 (info screen art)

[controls]              ; player 1's: layout, song keys, each action's button and key
layout = standard
next_song = none
next_song_key = PageDown
accelerate = default
accelerate_key = W
...

[endings]
; earned by winning the tournament on Medium or Hard
Hammerhead = no
Axel = yes
...
```

- Machine settings stay in `tm2pc.ini`: which controller is whose, the controller type,
  video, audio, music, Vanilla Plus, and player 2's controls (until players 2-4 get
  profiles). A profile made by an older build, or named in `tm2pc.ini` without a file,
  takes the controls `tm2pc.ini` had (its old `p1_` keys).

- Profiles are active when a settings file is in use (normal play, or `--settings` in
  tests); headless test runs never touch them.

## Saves: updates, backups, moving, editing

- **Updates** replace the program and `ui/`, never `profiles/`; a file from an older build is
  converted when it loads (as the controls moving out of `tm2pc.ini`).
- **Backups:** each save first keeps the file before it in `profiles/backup/<name>.1.ini`
  (the newest) to `.5.ini`. Renaming a profile renames its backups; deleting it deletes them.
- **Another computer:** copy `profiles/` (or one `<name>.ini`) into the other install.
  Nothing is tied to the machine.
- **Editing:** the controls are the player's to edit. The progress (`[endings]`) carries
  `check`, a keyed hash (SipHash-2-4) of it with a key in the program: progress edited by hand
  no longer matches, and is taken from the newest backup that does, or dropped (the
  controls are kept). A determined player can still find the key in the program; this
  stops editing the file. Online records (wins, kills) are to come from the server.

## Rules (decided by the user, 2026-10-04)

| Reward | How it is earned |
|---|---|
| A character's ending | Win the single-player tournament with that character on Medium or Hard. |
| Sweet Tooth | Any one ending. |
| Minion | 7 different drivers' endings (the user, 2026-10-14; was all 13 others). |
| Dark Tooth | Every driver's ending, all 14 (Sweet Tooth and Minion included): playable Dark Tooth (decomp request 36, `portCarOptions` bit 8). |
| Secret levels | One each at 2, 3 and 4 endings. |
| Movie portraits | An ending's with that ending (hidden, a black "?" card, until then); the intro's: goal to come (grayed). A locked portrait is never in use: a profile with one gets Roadkill's Portrait 1. |
| Profile portraits | Each driver's first portrait while the driver can be played; the second and third (the info screen's art) with the driver's ending, for now (the second's goal is open: a kill with the driver's special in a tournament?). Sweet Tooth and Minion are hidden ("???", black) until unlocked. |

The original codes for Sweet Tooth and Minion keep working, for the session only.

**Co-op:** the original plays no endings after a co-op tournament, at any difficulty: after the last level,
`screenScrollText` (80136BB0) goes to the winner movies only in single-player tournament mode (1);
in co-op (2) it shows `SHELLDB2\COWIN.TIM` ("there can only be ONE winner of the ultimate prize. Try
again in single player mode"). Profiles follow that. Proposed (user, 2026-10-04): after Dark Tooth, a
sudden-death duel between the co-op players (one life each); the winner's ending plays and counts, so
co-op also progresses, one ending per run.

## How a win is detected

`80136D98` is the game's winner-movie routine, run after a won tournament. For each
human player it plays the ending of their car from the table at `801648C4` (12-byte
entries: movie name, last frame, flag; indexed by car-select index) and then
`YOUWON.STR` (not for Grasshopper). It reads the settings struct `8016475C`: +0 game
mode (1 tournament), +4 human players, +8 + i each player's car-select index,
+36 difficulty (0 Easy, 1 Medium, 2 Hard). A wrap of it (`profile_hooks.c`) records
the endings before the movies play.

Car-select index, character and ending movie: 0 Hammerhead MONSTERT, 1 Outlaw 2
COPCAR, 2 Warthog HUMVEE, 3 Mr. Grimm MRGRIM, 4 Grasshopper DUNE, 5 Thumper IMPALLA,
6 Spectre VETTE, 7 Roadkill ROADKILL, 8 Twister INDY, 9 Axel AXEL, 10 Mr. Slam FRONTL,
11 Shadow HEARSE, 12 Sweet Tooth SWEET, 13 Minion MINION. Dark Tooth (14) has none.

## Applying the unlocks

Car select offers Sweet Tooth (index 12) while `D_80180D04` is set and Minion (13)
while `D_80180D05` is (the decomp's `docs/UNLOCKS.md`); only their codes set them, and
they last until power-off. `profile_apply_unlocks` (`profile_hooks.c`, called at every
frame start) keeps them set while the profile has earned them. It only ever sets them, so
the codes still work and the tournament opponent picker's save, force and restore of
`D_80180D04` is unaffected.

## Still to do

- **Secret levels:** the original has no unlock state for them; they are reached only by a
  code at level select, only in 2-player Deathmatch. Unlocked levels go into the level
  select of any custom match, 1 Player with bots included (whether computer cars can drive
  them is asked of the decomp).
- **The video player menu** (intro, credits and every ending; character portraits in
  rows, locked ones grayed out), under Options > Movies.
- **Portrait unlocks:** decided per portrait in [PORTRAIT_UNLOCKS.md](PORTRAIT_UNLOCKS.md) (counters show progress; online counts; an online Versus Tournament win on the "TM2 Original" ruleset earns the winner's ending, with a message pointing to Movies). The direction (2026-10-05): easy and plentiful.
  Tournament wins count as *unique* wins (a driver not won with before), so replaying a
  driver earns nothing new beyond that run's own feats. Feats to track (decomp request 7:
  kill credit, weapons, pedestrians, air time, falls, speed): a kill with each driver's
  special; the first online win with each driver; online wins and kills in total; fun ones
  tied to fitting stills (top speed in Twister for her hair-blown shots, falling off the
  map for Hammerhead's fall, pedestrian kills, air time). The same kill credit feeds an
  online kill feed (who killed whom, with what).
- **More portraits:** more stills from the movies (the picks so far are in
  `tools/art/movie_portraits.json`; all stills to choose from: `extracted/movie-stills`);
  and the unlock goals.

Sweet Tooth and Minion follow the profile: when another profile signs in, the game's
flags become that profile's (a code entered at car select still unlocks them until the
next profile signs in).
