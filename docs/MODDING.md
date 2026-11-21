# Mods: design (draft for review, 2026-10-15)

**Built so far (2026-10-25):**
- The scan and the checks of section 9 (`src/mods/mods.c`): every `mods/<kind>/<mod>/mod.ini` at start (a problem in
  tm2pc.log, the mod skipped); `tm2pc --check-mod <folder>` prints every problem of one folder.
- Skins, offline (`src/vplus/mod_cars.c`): each skin without a problem is the decomp's mod car 0x20 + m
  (`TM2_PORT_CAR_NUMBERS`, `portModCars`: its base car's stats, special, sounds and hit box); car select's carousel
  offers it right after its car (showing the base's turning car and portraits for now); a human on a skin gets its
  car.dmd / car.tms through `portCarSource`. Not yet: the skin's own car-select model and portraits, the 2-4 player
  car select, Options > Mods (every mod without a problem is on), online.
- Guides: `assets/mods/README.txt`, `assets/mods/skins/_TEMPLATE/GUIDE.txt` (complete), `assets/mods/arenas/_TEMPLATE/
  GUIDE.txt` (files and route rules; arenas are not loaded yet: what loading them needs is in `docs/ARENA_LOADING.md`).
- Tests: `TM2PC_TEST_P1_SKIN=m` (player 1 on skin m), `TM2PC_MODS_DEBUG` (the carousel's children, its steps).
- The user's decisions (2026-10-25): Select cycles a car's looks in every car select (skins out of Left/Right; mod cars
  at the end of Left/Right); a skin's own name, portraits (`portrait_1/2.bmp`) and CAR INFO (`carinfo.bmp`, 640 x 480);
  Options > Mods split by kind with On/Off (`[mods]` in tm2pc.ini by uid) and a grayed mod's reason; online: the
  server's mods downloaded on joining after a pop-up, a size limit (default 50 MB) in Online Services. (Built on branch
  mods-select-and-page: untested until the user's playtest ends.)
- Online (branch mods-select-and-page): `src/mods/mod_files.c` (each kind's allowed file names, content checks: DXSP /
  TXSP / BMP / WAV / text, size limits, FNV-1a fingerprints, the receiver's staging folder; `tm2pc --test-mod-download
  <mod> <game folder>` passes: install, a changed byte, a path in a name, a program as a model, the limit);
  `src/net/lobby_mods.c` (the host's skins that are on = the server's list, LM_LIST / ASK / FILES / PART / FAIL on the
  lobby's reliable channel, 8 KB a frame; the joiner's Server Mods page: Download or Leave; a failure leaves the server
  with the reason in the chat); a player's skin = the server list's index (LobbyPlayer.skin, LobbyStart.skin); while in
  a server only its mods are on and skins are numbered in its order (every machine the same numbers). Not yet: the
  browser's "mods" column (the LAN / master listing carries no mod list: a master protocol change), mod cars and arenas
  online. Tests: `TM2PC_NET_AUTOMODS=1` accepts the pop-up.
- Characters (2026-10-26): `mods/characters/<id>` (`[character] base = <car>`, portraits required) are mod cars like
  skins (the same table, `vplus/mod_cars.c`: `mod_car_is_character`, `mod_characters`, `mod_character`), but their
  own cars in Left / Right at the end of the portrait row (the user's: after Spectre, or after Dark Tooth with "secret
  cars last"), in the 1-player carousel (`vplus/fixes.c` `car_order`, the opening's step) and the 2-4 player car
  select (`grid_car` / `roster_n`); their own name, portraits and in-match model and textures; for now their base's
  stats, special, sounds, hit box and turning car in car select. Online they travel as skins do (the host's list, a
  player's mod as the list's index). Stats and size of their own: decomp request 70 (CAR_PROFILES.md; PER_CAR_PLAN.md E.2's
  `portCarProfile`). Guide: `assets/mods/characters/_TEMPLATE/GUIDE.txt`. Test characters:
  scratchpad `make_test_character.py` (the recolored Spectre with rotated portraits).
- Characters' sounds, computer drivers and endings (2026-10-30, the user's):
  - **Sounds:** `engine.wav` and `special.wav` (PCM, 8000 to 48000 Hz, at most 4 and 6 seconds) are read and encoded
    as the SPU's ADPCM (`src/mods/mod_wav.c`) into a bank of the port's per character, 512 KB, read at its first use
    (`src/vplus/mod_sounds.c`). PSY-Z patch 22 (`Psyz_SpuSetVoiceBank`, `Psyz_SpuSetVoicePitchScale`) lets a voice
    decode from that bank instead of SPU RAM, so the game's 512 KB are never touched and any number of characters can
    have sounds. At each key-on (`runtime/hw.c`) the decomp's tags (request 87: `portVoiceCar`, `portVoiceKind`) say
    whose engine or special the voice plays; a character's voice plays its file with the game's volume, envelope,
    reverb and pitch, the pitch scaled so the file plays at its own rate at the pitch the game starts it with. The
    engine loops; the special loops when the base's sample does. Not match state. Linked 2026-10-30 (decomp 3acd36c),
    checked in a match on a Spectre-based character: its engine from its file on its own voice, each of 4 specials
    fired from its file (two voices each, as Spectre's), other cars' sounds untouched (`TM2PC_SOUND_TRACE=1`). Tests: `tm2pc --test-mod-sound <folder> <out.wav>` (the files through the SPU: checked, each at its
    rate, the engine looping), `TM2PC_TEST_MOD_SOUND=<mod>,<kind>,<sample or *>`, `TM2PC_MOD_SOUND_DUMP`.
  - **The computer driver:** `[ai]` in mod.ini (not a separate ai.ini): hunt, attack and their hurt values, the three
    moods' times, retaliate, pass, patience: stats-block fields through `portCarProfile` (CAR_STATS.md 2; checked in a
    match: every value and the derived retaliation budget in the record). `fire_chance` goes to
    `portSpecialProfile[m]` byte 5 (request 87, linked: in a Challenge Match the character's driver had its 80, stock
    cars 35; the game's first computer car there always gets 100, any car). `TM2PC_AI_TRACE=N` with
    `TM2PC_MODS_DEBUG=1` prints each computer car's (`fire`).
  - **Endings:** a character's `portModCars[m].ending_movie` is a name of the port's (`\MODS\ENDING<m>`), which the
    wrapped `moviePlayEnding` (`ui_MoviePlayEnding`) shows as the port's page: `ending.bmp` or the portrait, "<NAME>
    WINS", `ending.txt`. A skin's is its car's movie. A win with a character earns its ending in the profile
    (`[mod_endings]`, by uid) and never its base's (before: the settings byte, the base's ending and its unlocks).
    Options > Movies has a Mods box while a character is on (the grid nine wide), its page the characters' endings,
    locked until earned. Tests:
    `TM2PC_ENDING_PAGE=1 --ui-shot 57,<k>,WxH,file.bmp` (the k-th character's page), `TM2PC_ENDING_SHOT`.

What players will be able to add, how a mod is laid out, what each kind must supply, how mods are turned on, and how
they travel online. Written to be reviewed with the user before anything is built; the open questions are at the end.

## 1. What can be modded

| Kind | What it adds | First version |
|---|---|---|
| **Character** | a new driver and car: stats, a special, art, sounds, an ending | yes |
| **Skin** | another look for an existing car: same stats, special, sounds and hit box | yes |
| **Arena** | a new map: geometry, textures, routes for computer cars, pickups, start points | yes |
| **Tournament pack** | a ready-made tournament: arenas in order, roster, bosses, text between levels, endings | yes |
| Game type, textures of existing arenas, music packs | | later |

**Mods are data only.** A mod holds pictures, models, sounds, movies and text files the game reads; never programs or
scripts. That is what lets mods download by themselves online without being a way to spread viruses (section 8). A
new kind of special or game rule needs new code in the game itself; with demand, more special templates can be added
to the game for mods to use (section 4.3).

## 2. The mods folder

Next to the game:

```
mods/
  characters/
    _TEMPLATE/            a complete example and the guide (GUIDE.txt), copied to start a new character
    <your-character>/
  skins/
    _TEMPLATE/
    <your-skin>/
  arenas/
    _TEMPLATE/
    <your-arena>/
  tournaments/
    _TEMPLATE/
    <your-pack>/
```

Each mod is one folder with a `mod.ini` (its manifest) and its files. A download that bundles several (e.g. a
Twisted Metal 1 pack: characters, arenas and a tournament) is just several folders, each in its kind's folder; the
manifests name each other (`requires`).

**`mod.ini`, every mod:**

```
[mod]
id = tm1-mr-grimm            ; the readable name the author picks: lowercase letters, digits, '-'
uid = 3f9c2a7e5b1d4c08a6e2f0b9d7c4e1a2   ; made once by the mod tool (128 random bits), never changes
name = Mr. Grimm (Twisted Metal 1)
kind = skin                  ; character, skin, arena, tournament
version = 1.0
author = ...
game_version = 1             ; the mod format it was made for (the game refuses newer ones it cannot read)
requires =                   ; other mods' ids this one needs (a tournament pack's characters and arenas)
```

`name` is shown whole in the port's own screens (car select, Options > Mods). The game's own screens print it with
the game's font into a fixed number of letters, so they get its first 16 letters, as a profile name's limit (a
character's target panel name: its first 15). Keep the start of a long name meaningful: "Mr. Grimm (TM1)" over
"Twisted Metal 1's Mr. Grimm".

**Telling mods apart, with no list of every mod:** no central registry is needed.

- `uid` is 128 random bits the mod tool writes when a mod is created (copying `_TEMPLATE` by hand leaves it empty,
  and the game asks to make one). Two mods made anywhere never share one by chance.
- `version` is the author's; the game does not trust it alone. **What a mod is** = its `uid` plus a **hash of its
  files** (computed by the game). Online, a server lists uid + hash; joining players need exactly those.
- Two mods with the same `id` but different `uid`s are different mods; the menus show the author next to the name.
  The same `uid` with different files is two versions of one mod: the game keeps the one turned on and says which.
- Profiles, stats and tournament progress keep mods by `uid`, so a renamed or updated mod keeps its progress.
- A public mod list or website can come later (for finding mods); the game never depends on it.

## 3. Turning mods on: Options > Mods

- The page lists every mod found in `mods/`, a page per kind (characters, skins, arenas, tournaments), with an on/off
  switch each, its author and version. A page shows up to 127 mods; past that its last row says more did not fit.
- A mod with a problem is listed grayed with the reason ("missing car model", "stats out of range", "needs
  tm1-suburbs"), and cannot be turned on. The game checks every file when it starts (section 9).
- **On** means it shows up everywhere its kind does:
  - characters: every car select, the opponent pickers (Challenge Match, Survival, Custom Tournament), Random;
  - skins: a choice on the car's page in car select (the original look or the skins that are on);
  - arenas: every arena list (Challenge Match, Survival, Custom Tournament), Random;
  - tournament packs: Local Play's Tournament menu (section 7).
- Two skins for the same car are fine (both choosable). Two mods with the same `id`: the newer version is used and the
  page says so.
- The choice is kept in `tm2pc.ini` (`[mods]`), for every profile on the PC.

## 4. Characters

### 4.1 What a character folder holds

| File | What | Notes |
|---|---|---|
| `mod.ini` | manifest (section 2) and `[character]`: name, base car, special template, stats, colors | section 4.2 |
| `car.dmd`, `car.tms` | the car's model and textures, the game's own formats (as on the disc) | a converter from common 3D formats is a tool to write (section 10) |
| `carselect.dmd`, `carselect.tms` | the turning car in car select (optional: the in-game model is used) | the port draws mod cars turning in car select, loading only the one shown, so any number of mods can be on |
| `portrait_1.bmp`, `portrait_2.bmp` | the two car-select portraits, 64 x 128 | as `ui/cars/<car>_<n>.bmp` (Dark Tooth's are the first) |
| `carinfo.bmp` | the CAR INFO screen, 640 x 480: a picture the modder draws (ratings and story) | required for a character (the user, 2026-10-27); a skin may keep its car's |
| `plate.bmp` | the car's picture in the line-up, the lobby and the opponents page, 128 x 64 or larger in that shape | required for a character; a skin may keep its car's |
| `bio.txt` | the Car Info text: driver, car, story | |
| `engine.wav`, `special.wav` | engine loop and the special's sound | played from a bank of the port's (PSY-Z patch 22), not the game's sound RAM |
| `ending.mpg`, `ending.bmp`, `ending.txt` | the ending: its video, picture and story | section 6 |
| `[ai]` in `mod.ini` (optional) | computer-car behavior: its moods, how often it fires | defaults from the base car |

### 4.2 Stats and the base car

**Built (2026-10-27):** the keys, ranges and units are in `assets/mods/characters/_TEMPLATE/GUIDE.txt` ("Stats"),
checked in `mods/mods.c` and handed to the decomp's `portCarProfile` (request 70, CAR_PROFILES.md). Computer cars drive characters: a
Challenge Match opponent picked by name, or Random with Options > Mods "Mod Cars in Random" (Challenge Match,
Custom Tournament, Survival; offline: online Random keeps the game's drivers; the game's Tournament never).

Every character names a **base car**, one of the game's 15. The base car gives everything the mod does not set, and
the special's behavior. `[character]` sets:

- **Stats** (the game's car stats, `CAR_STATS.md`): top speed, armor (HP), acceleration, braking, turning, mass,
  special charges and recharge time. Each within limits so a mod cannot make an unbeatable car by accident; the menu
  shows the ratings as Car Info does.
- **Size:** track width, wheelbase, ride and roof height. **This is the hit box** (the game builds it from these
  numbers, `carBuildCollisionBox`): it should match the model.
- Name, team/radar colors, whether it starts unlocked (default yes). Team colors (built 2026-10-27, skins too): `team_paint =`
  the body texture's palette entries that take the team's color, as the game's cars (`vplus/team_paint.h`); without
  it the car keeps its own colors in team games.

### 4.3 The special

A mod picks a **special template** and sets its numbers; it cannot add new behavior (that would need code). Built
2026-10-28 (decomp request 73, `SPECIAL_TEMPLATES.md` 6; `TM2_PORT_SPECIAL_PROFILE`, `portSpecialProfile[m]` filled by
`vplus/mod_cars.c` from `[character]`, checked in `mods/mods.c`):

- `special =` the base's own (default), one of the 10 drivers' specials that work on any car (Sweet Tooth, Outlaw 2,
  Warthog, Mr. Grimm, Thumper, Spectre, Roadkill, Axel, Shadow, Minion) or a pickup weapon (fire, homing, power
  missile, remote bomb, napalm, ricochet, Mega Napalm, freeze missile). Hammerhead's, Grasshopper's, Twister's,
  Mr. Slam's and Dark Tooth's specials need their own car (a character on one keeps its own). A template whose shot
  lives in its car's file (Sweet Tooth, Mr. Grimm, Spectre, Roadkill, Axel, Shadow) loads that file with the match
  (`portCarSpecialLoad`).
- Its numbers (`special_damage`, `_life`, `_speed`, `_turn`, `_range`, `_height`, `_duration`, `_freeze_damage`,
  `_freeze_time`, `_burn_time`, `_blast_damage`, `_blast_radius`, `_turn_back`) within the decomp's limits, and the
  computer drivers' chance to fire it (`ai_special_ahead`, `ai_special_behind`). The full list: the template's GUIDE.txt.
- No machine-gun burst (the game has none). More templates later, as mods ask for them.

### 4.4 Twisted Metal 1 characters

A TM1 driver missing from TM2 (Crimson Fury, Darkside, Yellow Jacket) is a character: base car the closest
TM2 car, its TM1 stats as numbers, its TM1 special as the nearest template. Its model and textures come from TM1's
files through a converter (to be checked: TM1 is the same engine family). A TM1 look for a driver who is in both
games (Mr. Grimm without his sidecar, TM1's Sweet Tooth...) is a skin (section 5).

## 5. Skins

- A skin replaces only the **look** of an existing car: the in-game model and textures, and optionally the car-select
  model, portraits, Car Info art and its name ("Mr. Grimm (TM1)").
- **Everything else stays the car's**: stats, special, sounds, and the **hit box** (built from the stats, not the
  model), so a skin never changes balance and is allowed online like any mod.
- **Size:** a skin's model may be somewhat bigger or smaller than the original (the guide will say about 15%). The hit
  box stays the original's, as retail's hit boxes are boxes anyway. A body very different in size or shape (a car
  with no sidecar for Mr. Grimm is still fine: his box is his TM2 one) is better made as a character, with its own
  size.
- The special's look can be part of the skin (its objects live in the car's model file), its behavior not.
- Chosen in car select per player (the original or any skin that is on); computer cars use the original unless the
  match says otherwise (an option for later).

## 6. Endings

A character (or a tournament pack, per character) may have an ending:

- **A movie:** `ending.mpg`, MPEG-1 video with MP2 sound in an MPEG program stream (the user's choice, 2026-10-30):
  made from any video with one ffmpeg command (the character guide's "The video"), decoded by pl_mpeg
  (`third_party/pl_mpeg`, MIT, one header; MPEG-1's patents have expired). At most 854 x 480, 2 minutes, 40 MB;
  checked before it is played (`src/mods/mod_video.c`: MPEG-2 video, other sound, too large or too long refused, the
  page shown instead). Its sound goes through the movies' XA stream at its own rate. Built 2026-10-30.
- **And a page:** `ending.bmp` (640 x 480 or 16:9; else the driver's portrait) with "<name> wins" and the text of
  `ending.txt`; after a movie only when there is an `ending.txt`. Without either file, the portrait and "<name> wins".

**Endings unlock only inside their mod:** winning a mod tournament never unlocks the original game's endings or its
characters.

**The Movies menu gets a "Mods" box** next to the drivers' portraits, shown only while a mod that is on has endings. It opens a page of the mods that have endings
(each mod's name or picture); a mod's page shows its endings as portraits, locked until earned, as the original
endings are. A tournament pack names, for each driver it lets you play (original or mod), the ending that driver
earns by winning the pack (or one ending for every driver).

## 7. Tournaments: Custom Tournament and tournament packs

Local Play > Tournament gains two entries:

- **Custom Tournament:** the player builds one: 6 to 8 arenas (each chosen or random; mod arenas included), the number
  of enemies per arena (or random), which arenas end with a boss and who it is (mod characters included), the
  difficulty and lives. Kept per profile, several saved. No endings unlocked (it shows the winner's portrait).
  Online's versus tournament will use the same settings.
- **Tournament packs** (mods, `mods/tournaments/`): everything set by the pack: arena order, the roster (who can be
  picked, mod characters included), enemies and bosses per level, text and pictures between levels, endings per
  character (section 6). A pack shows up as its own entry once it is on. Progress (levels reached, endings earned) is
  kept per profile and per pack.

`tournaments/<pack>/mod.ini`:

```
[tournament]
name = Twisted Metal 1
drivers = tm1-crimson-fury, tm1-mr-grimm, ...       ; the roster (original cars by name, mods by id)
levels = 6
[level1]
arena = tm1-suburbs
enemies = 3
boss =                                              ; or a driver
text = level1.txt                                   ; shown before the level (optional picture: level1.bmp)
...
[endings]
tm1-crimson-fury = endings/crimson_fury/            ; a movie or pictures (section 6)
```

## 8. Arenas

### 8.1 What an arena folder holds

| File | What | Notes |
|---|---|---|
| `arena.dmd`, `arena.tms` | geometry, objects and textures, the game's formats | pickups, breakables and pedestrians are objects in the DMD (ids 401-430, 500-520...) |
| `arena.grp`, `arena.rmp` | sections (floors, edges) and ramps/raised surfaces | the GRP holds the computer cars' route tables too |
| `arena.pts` | **route points for computer cars**: at most 140 points and 35 sections | without them computer cars cannot drive it; `tools/routes/` shows how ours were made |
| `arena.ini` | name, start points (up to 15 route points), sky color and lighting, fall height, music track, cars it holds, King of the Hill hills (5-8), boss room | the game sets these per arena in code today: the mod's file replaces that |
| `card.bmp` (required), `wide.bmp`, `lineup.bmp`, `radar.bmp` | its level-select card (344 x 260, 4:3, its name on it, as the game's `SC-*` cards: the browser, the lobbies), a 2:1 picture in the brick frame (Survival; made from the card if missing), the line-up's background, its radar map | a mod arena without a card is listed with the reason and cannot be turned on |
| `music.*` (optional) | its music (MP3, FLAC, OGG or WAV, as Custom Music) | each mod arena gets a row in Options > Custom Music (default: its own music, or a game track it names); the page scrolls |

**Picture templates:** `mods/arenas/_TEMPLATE/` holds `card_template.png` (a card's stone border, 344 x 260, the inside
transparent) and `wide_template.png` (the 2:1 pictures' brick frame, 512 x 256, its opening x 15..496, y 8..248
transparent), with `PICTURES.txt`: the scene goes under them, saved as `card.bmp` / `wide.bmp`. They are cut from the
player's own disc art, so they are made, not shipped: `python tools/art/ui_assets.py --out <any> --templates
<game folder>/mods/arenas/_TEMPLATE`.

**Hazards:** an arena's own events (LA's quakes, Moscow's collapse, Paris's tower) are code; a mod arena gets the
generic ones (breakables, pedestrians, teleporters if the game's warps can be set as data: to be checked).

### 8.2 A guide and a checker

The arena guide explains each file, the route rules (points kept off edges, every section reachable, jumps with a
run-up and landing point as New York's), and `tools/routes/` as working examples. A checker (section 9) runs the
route walk our generators' validators run, so a modder sees "section 12 cannot reach section 3" before playing.

## 9. Checks the game makes (and a modder's tool)

At start, for every mod:

- `mod.ini` complete, the id unique, every file it names present, sizes within limits;
- pictures the right sizes, sounds a supported format, models and textures parse;
- stats and special numbers within limits;
- arenas: route tables consistent, every point reachable, start points valid;
- tournament packs: every driver and arena they name present and on.

A failed mod is listed with the reason and stays off. The same checks as a command-line tool for modders
(`tm2pc --check-mod <folder>`).

## 10. Online

- **The host's mods:** a server lists the mods it runs (ids, versions, and a hash of every file). The server browser
  shows them.
- **Joining:** missing or different mods are downloaded into `mods/` (only what the server lists, nothing else),
  checked, and turned on for that match. Everyone in a match runs exactly the same mods and version.
- **Safe downloads:**
  - only data files of the allowed types (the formats in sections 4-8; no programs, scripts or libraries), checked
    by content, not just by name;
  - size limits per file and per mod;
  - names checked: nothing written outside its own folder in `mods/`;
  - every file checked against the hash the server listed; a file that fails is deleted;
  - downloaded mods are parsed by the same checks as local ones before use.
- **Game updates** are different (they are programs): the game checks at start for a newer version and offers it
  (as OpenCE does), downloaded only from our own signed releases, then restarts. Online play requires the newest
  version. This needs a public release channel first (the repositories are private today).

## 11. What has to be built (in order)

1. **Loading a car's files from a folder** instead of the disc (the decomp's `portCarSource`, `CAR_GROUPS.md` 7.7)
   and a mod car as a 16th+ car type on a base car (settings index 0x20 and up: `PER_CAR_PLAN.md` E). Car stats as
   data (`portCarProfile`, proposed in the decomp, not built).
2. Options > Mods, `mod.ini`, the checks (section 9).
3. Skins (the simplest: the model swap only).
4. Characters: stats, special templates, art in car select and Car Info, AI.
5. Arenas: levels loaded from a folder, `arena.ini` replacing the per-arena code (sky, lighting, start points, music,
   hills).
6. Custom Tournament (Local Play), then tournament packs and mod endings.
7. Sounds from mods; movies from a file.
8. Online: the mod list, downloads, the checks.
9. Tools: a model converter (common 3D formats, TM1's files), the mod checker, the template folders.

## 12. Open questions

- **Formats:** the DMD/TMS byte formats are not fully written down yet (the decomp has readers, not a spec);
  converters need them. A request for the decomp.
- **Sounds:** the game's sounds are one shared bank (`UACORE.VAB`); how to add a mod's samples (a port voice bank:
  the port already has 4 voices of its own, request 34).
- **Car select:** the turning car comes from one shared model file with exactly 14 cars and no free VRAM; mod cars
  need the port to draw them (as Dark Tooth's placeholder).
- **Arena code:** sky, lighting, start points, music track and events are set in code per arena; which become data.
- **Movies:** STR only (works today, needs a tool to make them) or a common format (a decoder in the game).
- **The roster:** the target is 128 mod drivers on top of the 15. The decomp keeps car numbers in signed bytes with
  mod cars from 0x20, which leaves room for about 96: a request to widen it.
- **Limits:** how many mod car types fit in one match (VRAM: 16 with texture banks), and memory for a mod arena's files.
  **Any number of mods can be on:** only what a match uses is loaded (its arena, its cars and their sounds); menus
  list mods by name and load a mod's pictures and car-select model only when shown.
- **TM1's files:** checking TM1's car and level formats against TM2's for a converter.
