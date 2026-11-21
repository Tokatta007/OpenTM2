# Custom Battle (the new Challenge Match)

The user's lists (2026-10-06), sorted into menus. The menus are the port's own (as Options), the
battle the game's.

## Words

| Word | Means | Examples |
|---|---|---|
| **Mode** | the rules the port plays: how a match is won | Last Car Standing, Deathmatch, Juggernaut, Gun Game, Hunted (was Oddball), King of the Hill |
| **Game type** | a saved rule set for one mode: weapons, teams, hazards, respawn time... Built in (locked) or the player's own | Twisted Metal, Rockets Only, Weapon Roulette, the player's "Napalm Party" |
| **Match settings** | chosen each time, never saved in a game type | arena, opponents (how many, who, teams), lives, kills to win, time limit |

## Flow (draft 2, 2026-10-06)

1. **Challenge Match > GAME TYPES:** the game types under a header per mode; the built-in ones
   with a padlock, the player's own in light blue. The chosen one has a copper dot, and the
   page opens on it, so X goes straight on.
   Legend: X = SELECT, Square = NEW (a new game type from the one highlighted: named first,
   then the editor), Triangle = EDIT (the player's own only).
2. **CHALLENGE MATCH:** Game type (X: the list again), Arena, Opponents, the mode's own
   settings (Last Car Standing: Lives; the others: Kills to win, Time limit), Start.
3. **The editor** (titled with the game type's name): Mode, Drivers, Respawn time, Teams,
   Weapons, Energy attacks, Pickups, Arena hazards, Falling off map, Rename, Save, and for the
   player's own Delete (X held 3.5 s, as a profile's). Circle leaves without saving.

**GAME TYPES' side:** the highlighted type's mode (a symbol, as Halo's game types have, its
name and what it is) and the game type's own settings, only those that differ from the game's
(weapons Default / Only X / Custom, auto-refill when on, each energy attack disabled, no health
or turbo pickups, pickup respawn, teams, hazards or destructibles off, falling off, drivers).
The player's own types have their own description (typed, up to 48 characters).

Mode descriptions: Last Car Standing "Last car or team alive wins."; Deathmatch "First to the
kills goal wins."; Juggernaut "Only the Juggernaut can score points. Kill the Juggernaut to take
his place."; Gun Game "Each kill gives you the next weapon you must score a kill with. The first
to complete them all wins."; Hunted "Score points by staying alive as the target. Kill the
target, and become the target."; King of the Hill "Hold the zone to earn points over time. First
to the points goal wins." King of the Hill to have a moving hill and
multiple hills as settings.

Drivers: a group row (All, No bosses: Minion and Dark Tooth off, Custom) over each driver in
car-select order, then **Random opponents**: Minion in random and Dark Tooth in random (each "???" until unlocked; off
by default; game type keys `minion_random`, `dark_tooth`). The random opponents are drawn from the 12 drivers on, Sweet
Tooth once unlocked (his row on), and those two (decomp request 42: Dark Tooth in his first phase, `portCarOptions` bit
10; one a match, where his textures fit). Arena Hazards: one switch for the hazards and one for the
destructibles (their arenas in the description). Pickup respawn time in seconds (the game's
200 frames: about 7).

On the match pages, a **summary**: the **line-up** (the arena's opponent-card screen with a card
for every car: the players' with their names, the opponents' (Random "?"), up to 16 in a grid)
and the game type's rules in a few lines (the editor: the rules only). At Start the game's own
card deal plays (`8012A910`, as before a tournament level; up to 9 cards for now).

## Settings

| Group | Settings | Where |
|---|---|---|
| Arena | the 8 arenas, Random; the secret ones once they work | match |
| Opponents | how many (0 to the limit, below), each Random or a driver (car-select order), team color | match |
| Mode's settings | Lives (Last Car Standing), Kills to win, Time limit, Sudden death | match |
| Respawn time | | game type |
| Teams | off / 2 / 3 / 4 (Red, Blue, Green, Gold); friendly fire on/off; homing never locks on teammates | game type (colors: match) |
| Weapons | each pickup weapon: on/off, ammo per pickup (1-9), auto-refill time; infinite weapons; roulette (every N seconds, N of the weapon each switch, from the weapons on) | game type |
| Energy attacks | each on/off: freeze, mines, napalm, shield, invisibility, Minion's volley (always off online). The jump is always on. | game type |
| Pickups | health on/off, turbo on/off, pickup respawn time (standard / faster / slower / never) | game type |
| Arena hazards | the automatic ones: Antarctica's ice, Amazonia's lava, Hong Kong's vehicles | game type |
| Arena destructibles | the ones players set off: LA quake, Eiffel Tower, Moscow dish, New York and Holland targets | game type |

Made 2026-10-14 (decomp request 38, `TM2_PORT_HAZARDS`, decomp `ARENA_SWITCHES.md`): the game type's **Hazards** and
**Destructibles** rows (Arena page) write `portArenaOptions` at the load (bit 0 hazards off, bit 1 destructibles off;
the C latches them at each round start). Hazards: Antarctica's collapsing ice, lava burns (Amazonia, LA section 14),
the fire patches (LA, Moscow's bridges), Hong Kong's trains (removed). Destructibles: the LA bridge ("quake"), LA's
timed target, Moscow's four bridges and its dish, the Eiffel Tower (and its AI route patch), New York's event, Holland's
barns, Hong Kong's trains as targets. The ordinary breakable props stay. A tournament or Survival: as retail. Test:
`arena_off` (`TM2PC_TEST_ARENA` 8 and 6). Per-arena switches later (`portArenaItems`, `ARENA_SWITCHES.md` 8).
| Falling off map | Death (default), 10 / 25 / 50 / 75% damage (back on the map), No penalty | game type |

Vanilla Plus (planned, gray): **Weapon capacity**, more than the game's 24 pickup weapons.

## What the game's code says (decomp, 2026-10-06)

- **Car limit:** 11 cars: 2 human car records and 9 computer car records (`aiCars` 80188B5C),
  and the opponent cards stop at 9. Start points: LA 10, Moscow 10, Paris 11, Amazonia 11,
  New York 10, Antarctica 15, Holland 15, Hong Kong 14 (more cars reuse spots at random).
  Car models are packed into ~737 KB with no check, and a duplicate car loads a second model.
  **To do:** 16 cars (the records moved to larger arrays, the memory checked; online too),
  and the card deal for up to 16 (players included) before that.
- **Memory per arena** (the load area's 737,300 bytes: the arena's DMD then each car's, 25-39 KB):
  Holland (298 KB) and Antarctica (310 KB) hold 10 cars; Paris 407, Hong Kong 416, LA 422,
  Moscow 424, New York 471 and Amazonia 473 KB do not (up to ~825 KB with 10). 9 opponents
  everywhere needs the load area moved to more memory (the port's RAM is the PS1's 2 MB; a
  larger RAM, and the loader's base moved), the same work as 16 cars.
- **Pickups:** one timer for all of them (`pickupRespawnTick` 8010AED0): every 200 frames a
  random pickup spot comes back (health only half the time). Ammo per pickup is a table by
  weapon (`pickupAmmoTable` 8018429C: 2 each, remote bombs 1), capped by the 24-slot bay.
  Health 417 (full), 418 (a third), turbo 430 (+100).
- **Energy attacks:** `801046FC` by kind 100-106 (freeze, mines, napalm, shield, jump,
  invisibility, and Minion's volley, which any car can fire with its code).
- **Opponent cards:** `SCREEN\SHOWOPS<level>.TIM` background (the PC's are 640x480),
  `SCREEN\<car>PLATE.TIM` cards (128x64), a card every 11 frames with sound 3, layouts for 1-9
  (`80164618`); the runner deals them in modes 0-2 only (the port: also for these matches).
- **Falling off:** the car's health is set to -1 below `worldFallDeathZ`; a rescue is
  `humanCarPlaceAtStart` 800FD6C4 (`aiPlaceOnRouteNode` 800F6518 for computer cars).
- **Weapon icons:** the HUD's sprites (DMD object 1260, in each level's TMS): for the weapon rows.

## Survival (plan, 2026-10-07)

1-4 players together on one arena (chosen), against 15 rounds of computer cars, as far as you
can get. A third item in the 1 Player and 2 Players menus. Options: just your car and the
arena; the computer cars always fight on Hard and always target the players (Only target
humans); a difficulty later may change which enemies come, not how they fight.

- **Rounds** (the user's): round 1 one enemy, then more each round up to 12; round 5 two
  Minions; round 10 Dark Tooth with 2 Dark Tooth heads; round 13 nine enemies and 3 Minions;
  round 14 six Minions; round 15 three Dark Tooths and 9 heads.
- **Death:** a player out spectates; at the next round they come back if one player survived
  the round (all out: the run ends, the round reached is the score).
- **What the game allows today:** 9 computer cars at once (12 needs SIXTEEN_CARS.md's work);
  every car type in the match is loaded at its start (the waves use the cars loaded then:
  a round turns on more of them, each back with aiCarStart as a respawn); one car per type
  (two Minions or three Dark Tooths need the per-car registries, SIXTEEN_CARS.md 1.3); VRAM
  room for 8-14 different types by arena; Dark Tooth alone takes the room of about 7 cars and
  his boss logic is tied to Hong Kong (level 8). So: rounds 1-12 can come first with distinct
  cars (Minion on round 5 as one car); the boss and duplicate rounds after request 14.

## Cameras, spectating and the kill cam (plan, 2026-10-07)

The game's views (`viewCarCameraUpdate` 80102BE8): 1-4 and 7 chase cameras (7 follows a
computer car), 5-6 the fixed track cameras (cinematic), 8 none: the camera is not moved, so the
port can drive it (position and rotation in the human record's camera fields, as
`viewSetPosition` / `viewSetRotation`). With view 8 the port can add: an aerial shot, a wide
shot framing two cars (zooming to keep both in), a missile's point of view.

**Kill cam** (the user's idea): the last kill shown three times (the spectated view, a
cinematic angle, the weapon's point of view following the missile to the hit), then an aerial
shot, the survivors driving on a moment, then the results. The game is deterministic with its
inputs (one rand stream, fixed timestep), so a replay is possible: keep a copy of the game's
state a few seconds back (its RAM, 8 MB, and VRAM) and the pads since; to replay, restore the
copy and run the same inputs again with another camera (sounds muted or replayed). Work: the
state copy and restore (the recompiled code keeps everything in guest RAM, the runtime's own
state beside it: GPU, SPU, timers), the input log, the port cameras above.

## Scoreboard (plan, 2026-10-06)

Columns: place (results), player (profile, or CPU), car (its card), kills, deaths, then one
mode column where DAMAGE is (points in Juggernaut, Gun Game's weapon, Hunted and King of the Hill
points; LIVES in Last Car Standing with lives), and PING online. Two columns past 8 cars (up to
16). At 4:3 the narrowest columns give way (damage first); wider windows can show them all.
Teams: each team's total over its own block of rows (2 columns: one team a side), the team's
color on each car's card frame.

## Match end and results (to do)

Last Car Standing (the default) ends when one car is left, with a results screen: the winner,
then a scoreboard (kills, deaths, place) taking most of the screen. The in-game scoreboard is
the same with ping online. Ideas: multi-kill pop-ups ("double kill"...) listed again after the
match; online: the next map, a map vote, or back to the lobby (with playlists).
Made 2026-10-14: kill streaks, with the user's names (Unreal Tournament's).
- Multi-kills, each enemy kill within 4 seconds of the last: 2 DOUBLE KILL!, 3 MULTI KILL!, 4 ULTRA KILL!,
  5 MONSTER KILL!, 6 LUDICROUS KILL!, 7 and up HOLY SHIT!
- Sprees, enemy kills without dying (any death starts over; a new round too): 5 KILLING SPREE!, 10 RAMPAGE!,
  15 DOMINATING!, 20 UNSTOPPABLE!, 25 GODLIKE!, 30 MASSACRE!
A kill's messages come in turn, 2 seconds each: its multi-kill, its spree (kept through a newer kill until shown),
then YOU KILLED X (alone: 4 seconds). Vanilla Plus switches (Menus): Kill streaks, and Kill message for YOU KILLED X. It appears where YOU KILLED X does, under the same Vanilla Plus switch.
Teammate kills don't count. Test: `multi` (`TM2PC_TEST_FEED_P1`: the test's kills credited to player 1). Still to do:
listing them after the match. The game's
challenge mode plays rounds until Quit, so the port ends the match itself and counts kills for
every car.

## The modes' settings (the user's, 2026-10-07; first drafts made)

First drafts (on Deathmatch's respawns, time limit and "Points to win"): Hunted (a target
scoring a point a second, its killer the next), Juggernaut (half damage, the only one
scoring, a point a kill, its killer the next), Gun Game (the 7 pickup weapons in turn, a
kill the next). The plan for each:

- **Juggernaut:** more health %; full health on becoming it; weapon damage boost; ram damage
  boost; invisibility (the game's energy attack 105 kept on: a Predator / active camo
  shimmer); infinite ammo; forced weapons; forced car (one extra car loaded hidden: after
  duplicate cars, `SURVIVAL.md` 2); an indicator (gold or platinum paint: decomp request 19;
  a glow from the game's effects); a sound and "NEW JUGGERNAUT" when it changes. The others
  hunt only the Juggernaut but still hurt each other.
- **Suicides (made 2026-10-11, the user's rule):** Deathmatch: a car destroyed by its own weapons or
  with no killer (a fall, the arena) loses a kill (`match_destroyed`, the count may go below 0), so
  dying on purpose keeps no kill from the others. Juggernaut: the Juggernaut killing itself passes it
  to the car that did it the most damage in that life (`jugg_damage`, from `vp_mode_damage`; none:
  a car at random). The game writes a car's killer byte only when an attacker brings it down and
  never clears it, so a fall named the killer of the car's last life: the port now clears it at
  each death (`feat_Destroyed`; nothing of the game's reads it). Tests: `dm_suicide`,
  `jugg_suicide` (`TM2PC_TEST_KILL_SELF`).
- **A fall after a hit is a kill (made 2026-10-11, the user's idea):** each car keeps the last car that
  hurt it, with what and when (`last_hit`, `feat_Damage`); dying with no killer or by its own weapons
  within 6 seconds of that hit credits that car (Mr. Slam's throw off a roof, a ram over an edge), with
  that hit's weapon in the feed. Older than 6 seconds, or no hit: a suicide as above. Kept with the kill
  cam's machine (the Juggernaut passes to the killer).
- **Gun Game:** a list of the weapons and specials (and ram kills), ordered weakest to
  strongest or the reverse, each on or off (all on by default); infinite ammo; ammo per
  weapon. Specials for any car where they are projectiles or effects (Sweet Tooth, Outlaw
  2, Warthog, Mr. Grimm, Spectre, Roadkill, Thumper, Axel, Minion; Shadow to try; not
  Twister, Grasshopper, Mr. Slam). The HUD: the special's icon with the driver's name. The
  weapon pickups one look ("resupply"), refilling the current weapon.
  **Specials as levels (made 2026-10-14, decomp request 35 `TM2_PORT_SPECIAL_AS`):** the Gun Game page's
  Specials rows (game type `gun_specials`, bit type >> 4; off by default): Sweet Tooth, Outlaw 2, Warthog, Mr.
  Grimm, Thumper, Spectre, Roadkill, Axel, Shadow, Minion (the decomp refuses Hammerhead, Grasshopper, Twister,
  Mr. Slam, Dark Tooth). They come after the pickup weapons (strongest first: before them), the ram kill and the
  machine gun last. On a special level every car fires that special (`portCarSpecialAsSet` each frame it is off:
  a new level, or the slot reset by a swap or a drop-in), weapon slot 0 chosen, its count the level's ammo (or
  infinite). A kill counts on a special level unless it is a machine gun or ram kill (a borrowed special's kill is
  told by its projectile: Sweet Tooth's spiral as a napalm, Minion's as a freeze). At the load the specials whose
  cars are not in the match load as parked spare cars (`portCarSpecialLoad`, Sweet Tooth, Mr. Grimm, Spectre,
  Roadkill, Axel, Shadow), with the raised pools (bit 3) and texture banks. Test: `gun_specials` (the 10 specials
  as the only levels, infinite ammo).
- **King of the Hill, several hills and chosen spots** (made 2026-10-14): the following settings.
  - Hills: 1-3 at once, each scoring for whoever holds it alone.
  - Hill spots: CHOSEN or RANDOM. CHOSEN uses `vplus/hill_spots.c`: 8 per arena (6 in Moscow's small bowl), one per
    area of the arena. The areas are the route's sections in its PTS file, biggest first, spread at least 2500 apart.
    Each spot was checked in the game, the hill seen from 2400 away (the `tourlook` scenario, `TM2PC_TOUR` +
    `TM2PC_TOUR_LOOK`). Amazonia's lava-side spots were swapped for solid ground. A hill never takes another hill's
    spot or its own last one. RANDOM uses any route point away from the other hills.
  Teams work in King of the Hill: a held hill scores a point a second for its team (one, however many of it are on the hill), the team with the most points wins (test `koth_teams`). Radar fix: a hill's square (a TILE in the radar's
  line packets) no longer turns a car's blip into a big square on the next frame. The game sets the packet code
  only once, so each plotted blip now gets its LINE_F2 code back. Test: `koth3` (3 hills, Chosen).
- **King of the Hill (in phases):** number of hills (1 by default); moving hills and their
  time; specific hills or random; a point every 1-2 seconds held. The hill white, in the
  holding team's color when held; HUD "YOU HOLD THE HILL" / "CONTESTED". Phase 1: game
  effects at its corners and a radar mark; phase 2 (made 2026-10-13): a wireframe box over the
  hill's circle, 600 units high, drawn in each view's world (`hudstyle_world_marks` from four.c's renderScene wrap: each
  piece in the view's order table at its depth, a little nearer far away, so cars and walls in front cover it and the
  HUD is over it; `vplus/hud_styles.c`): white
  free and while contested, the holder's team color (no teams: its car's radar color). The flames and the
  smoke column are gone (`TM2PC_HILL_FLAMES=1` brings them back). Its radar mark is a 6x6 square (its blip's two lines made TILEs) in the box's color. The hill's status is the message
  line's (its bar, level with the radar), as Juggernaut's and Hunted's.
  The computer cars (made 2026-10-11): while the hill is free or their own side holds it,
  they go for it (mood 0, the route's next node picked nearest the hill: a wrap of
  `aiFollowRouteTarget` 800F7D54 gives `aiPickLinkNode` the hill as its aim point), firing at
  their targets as ever; while another side holds it they hunt the holder. Measured on Los
  Angeles (`koth` scenario, `TM2PC_HILL_DEBUG`): the cars' average distance from the hill fell
  from about 4500 to 1600-4000, with up to 4 of 9 on it (before: at most 1). The hill's place
  is random again (the port's random numbers, `vp_rand`, were never seeded: the first
  hill was always route point 0; now seeded from SDL's before each battle).
- **Hunted:** the target can attack or not (machine gun and ramming always); 1-4 targets;
  teams: a kill switches that target, or the whole hunted team switches (attack/defend).
  Made 2026-10-14 (the user's rework; "a whole team hunted is just a game of run away"):
  teams now work in Hunted and King of the Hill. The Targets count (1-4) holds with teams.
  "With teams" options:
  - ONE BY ONE: a target's killer of another team takes its place, so both teams can have targets.
  - SWAP TEAM: all the targets are on one team. A kill by another team gives that team the targets:
    the killer plus random teammates, up to the count. A target killed any other way is replaced
    from its own team.
  The computer cars guard their own team's targets: they go for the enemy nearest the target
  when one is within 2500 units, otherwise they hunt the other team's targets. Each target scores
  a point a second (the team's score is the sum). Who is a target, shown to everyone except the
  target itself:
  - a diamond over the car in its team's color (gold without teams), pulsing brighter;
  - a beam of light up from its roof, sorted in the world like the hill's box.
  The status line reads PROTECT YOUR TARGET(S), TARGETS: RED TEAM, or n TARGETS.
  Tests: `hunted_teams` (swap, 2), `hunted_one` (one by one, 3).
- **Juggernaut invisibility** (game type `jugg_invisible`, made 2026-10-14): the Juggernaut's
  body is hidden, with a glimpse at uneven times and the invisibility attack's blue sparks
  around it. A player who is the Juggernaut still sees their own car in their own view
  (`vp_shim_view` around each view's renderScene). It is also off the other players' radars except during its glimpses. Tests: `jugg_invis`, and `jugg_invis_me`
  (`TM2PC_SPECIAL_SLOT=0`: player 1 is the Juggernaut).
- **All:** spawn protection time (the shield after a respawn) as a game type setting; a kill
  feed ("X <icon> Y", Counter-Strike style; text first, then horizontal icons for every
  weapon, special, ram, falling and self); the radar: the Juggernaut red, the others green,
  hills white then the team color, bigger marks if the radar allows.
- **Car loading:** all 14 cars on every arena do not fit: video memory (each car's textures
  in the 1024x512 VRAM; Paris 8 cars, Hong Kong 9, Antarctica 10), not RAM.

## Lobby (made 2026-10-07, draft 1; the user's idea, as Halo 2/3)

Made: Start on the Challenge Match page opens the LOBBY: the players and computer cars
(tag, card, name, team; Left/Right on a row changes its team), Choose car (car select, then
back to the lobby with the card shown), Start match (straight on with the chosen cars, or
through car select), Edit settings; the line-up and game type on the right; after a match the
lobby again. The Opponents page no longer has the teams. Not yet: car select's team color
(Square), online rows (ready, ping, chat), Survival through the lobby.

The plan:

After the Challenge Match (or Survival) settings, Start goes to a **lobby**, the same locally
and online, instead of straight to car select:

- **Right:** the arena's picture, the game type's summary (the side panel of the Challenge
  Match pages) and the match settings (lives, teams, opponents and difficulty).
- **Left:** everyone in the match, one row each: the player's portrait and name (or CPU),
  their car's card (or "?" until chosen), their team color. Online later: a ready mark, ping,
  and a text chat box under the list.
- **Rows for the host:** Choose car (car select, then back to the lobby), Start match, Edit
  settings (back to the Challenge Match page). Each player's own row: Left/Right changes their
  team; the host can also change a CPU's team (Left/Right on its row) and its car.
- **Car select's team color:** Triangle switches your team there too, and the spinning car
  shows it (made: draft 7).
- The Opponents page then keeps only how many, which drivers and the difficulty; the teams move
  to the lobby.

## Made so far

- Draft 7 (2026-10-08, overnight):
  - **2 players and computer cars:** the Opponents page's Number works for 2 players (one car
    fewer than for 1). The match runs in the challenge mode with both players (as co-op
    Survival), in split screen. The port decides the round (`vp_duo_frame`): each side (a team,
    or without teams each car) is in while a car of it is alive, coming back, or (a player) has
    lives left; a player out of lives watches the computer cars (X next, O back). One side left:
    its win (the results name the player or computer car, or the team). Player 2's radar shows
    the computer cars too (the game plots them for player 2 only in the co-op tournament; co-op
    Survival gets this too). Each half's "Enemies: N" counts that player's enemies.
  - **Teams with 2 players:** each player's team from the lobby; teammates' hits do nothing
    without friendly fire; the computer cars hunt the other teams.
  - **Team colors:** every car on a team is painted in its team's color at load (decomp
    TEAM_COLORS.md way A: a recolored copy of the body CLUT's paint entries per car,
    `team_paint.h`), for 1 and 2 players. Since 2026-10-11 the recolored palette goes over the car's own
    body palette (where the game put it) instead of into a new VRAM spot: the game's `vramUploadClut` takes a
    spot whose first pixel is free and wrote the 256-wide row over the start of another car's texture
    (Thumper and Shadow came out scrambled in team games), and most arenas have no fully free row left.
  - **Car select with teams:** Triangle switches the player's team; their spinning car is in
    the team's colors (a recolored palette in CLUT row (768, 40 + player), the shown car's body
    primitives pointed at it for that player's draw, decomp TEAM_COLORS.md 5.2), and their
    portrait frames take the team's color.
  - The respawn modes with 2 players: each half shows its own KILLS (Gun Game: LEVEL); the match ends by player 1's score
    line; the target panel of a player shows the other player, not the nearest car.

- Draft 6 (2026-10-07, overnight):
  - **Teams** (Last Car Standing, 1 player): the Challenge Match page has Teams (Off, 2, 3, 4);
    the Opponents page your team and each opponent's (a dot in the team's color, Square cycles
    it); the line-up cards in team colors. The game type keeps Friendly fire (now made). In
    battle the computer cars hunt the nearest car of another team; without friendly fire a
    teammate's hit does nothing; Enemies counts the other teams; the round is won when they are
    out (also by teammates while you spectate) and lost when your team is; "RED TEAM WINS";
    scoreboard rows with a team-color bar, each team's kills and cars left under the board.
    Known: computer cars fight each other slowly (decomp request 17).
  - **Survival test** (1 player): 10 rounds on Hard, round 10 eight cars and Minion; the waiting
    cars parked out of play (decomp SURVIVAL.md 3.1); "ROUND N" printed by the game in the pause
    menu's font after the HUD, with the explosion; the Survival page shows the arena's
    level-select picture. Results: "YOU SURVIVED N ROUNDS", Retry or Quit.
  - **Co-op tournament duel** (draft): the co-op win shows the duel's text, the two players fight
    on level 8 (Deathmatch, one round, one life each), the winner's ending plays and goes to
    both profiles.
  - Infinite weapons fixed: the game's setter is dead code (never recompiled); its store is
    written directly.

- Draft 5 (2026-10-07): computer cars have lives too (decomp AI_LIVES.md: a car with a life left
  stays an enemy, comes back alone after 7 seconds at the start point farthest from the others,
  2 seconds of shield; Dark Tooth one life). Game type "Computer cars": Classic (they hunt the
  players and spare each other), Everyone equal (the nearest car of any kind; always hurt),
  Only target humans (no damage between computer cars). Spectating: X next, O back, Square
  cinematic (view 5's track cameras, a new car every 8 s); the bar along the bottom.
  Mode symbols reordered (Last Car Standing skull, Deathmatch crosshair, Hunted on-sight);
  Survival (nailed head), Co-op (champions) and Versus Tournament (trophy cup) in the sheet.

- Draft 4 (2026-10-06): the scoreboard (results and in battle) shows each car's card, the player
  (profile, or CPU), the driver, kills, deaths and damage dealt (PING to come online); the results
  go over the last frame headed by the winner's card and "<NAME> WINS", Rematch or Exit. New game
  types start from the game's rules. Turned-off health and turbo pickups give a weapon that is on.
  Specials: on/off, recharge speed (0.5x-4x), special pickups. Auto-refill, weapon roulette,
  falling off map (Death, 10-75% damage back on the map, no penalty). Lives for the players in
  Last Car Standing (1-6: a car comes back alone, decomp COOP_LIVES.md 2; computer cars: one).
  Still gray: teams, hazards and destructibles, respawn time, Dark Tooth, the other modes.

- Draft 3 (2026-10-06): the match ends with its first round (Last Car Standing: you out, or the
  last opponent out; 2 players: one of you out) on a results screen (the winner, the scoreboard:
  place, driver, kills, deaths; Rematch or back to the Challenge Match page); the game's own tally
  screens are left out. In any battle, holding a controller's touchpad or guide button (or Tab)
  shows the scoreboard. The tally: `match_*` in `feat_hooks.c` (every destroyed car, its killer's
  slot). Made in the game type: weapons on/off (a turned-off weapon's pickup gives one that is on),
  ammo per pickup (`pickupAmmoTable`), infinite weapons (the players), each energy attack,
  health and turbo pickups (off: they give nothing; they still show), pickup respawn time
  (`pickupRespawnPeriod`); `vplus/fixes.c`. 9 opponents on every arena (fewer where VRAM is short:
  Paris 7, Hong Kong 8): the car models load above 2 MB (the guest has 8 MB).

- Draft 1 (PR #105): one page with presets.
- Draft 2: the flow above, with the summary and the line-up. Working: Last Car Standing with
  the game's rules (Twisted Metal), the drivers allowed, the arena, the opponents (how many,
  which, difficulty), and the player's own game types (new, edit, rename, save, delete; kept
  in `[game_type1..24]`, the match in `[battle]`). Everything else is gray.
- `battle_start` sets the settings and returns the car select; the game's opponent select
  (`ui_AiSelect`) and arena select (`ui_LevelSelect`) are skipped for these matches.

## Survival: Dark Tooth on any arena (2026-10-12)

Survival's final boss is always Dark Tooth (the Final Boss choice is gone, 2026-10-16). Round
10's last car is him, with his whole fight on the chosen arena: his missiles, the head's swoop,
stage 2 (the head on the car, 110 HP) after his first death, and the run won at the second.

- **In C (the hybrid only):** `portConfig.boss_anywhere` (+0x04, `PCFG_BOSS_ANYWHERE`), set by
  `port_config_sync` while such a run is on. The overlay applies decomp `SURVIVAL.md` 1.5's patches:
  his boss controller on any level (`weapon_tick_init.c`), his stage 2 for the first Dark Tooth not
  parked, wherever he is loaded, with no Hong Kong clamps or round event (`scene_init.c`), Hong
  Kong's jumps, warps and markers only on level 8 (`car_restart.c`, `screen_pause_menu.c`,
  `car_object_contact.c`), wreck state 4 after his last death (no endless debris, `car_damage.c`),
  and the missile's wobble (`projectile_hit.c`). `bitmapInit` then reserves (0,0,640,240) instead
  of 256 rows: the 16 rows under the 320x240 frame buffers hold 32 more 256-color CLUTs
  (`SIXTEEN_CARS.md` 4.6 option 2). With the flag off every unit is the decomp's.
- **VRAM:** his 31 images leave room for few other car types (players' included). The line-up
  follows `SURVIVAL.md` 4.1 ("rows freed, largest first", him loaded last): Los Angeles,
  New York, Hong Kong 3 types; Amazonia, Antarctica 6; Holland 9; Moscow 10 (all 9 opponents).
  Paris has no room: Minion there. The game's "Relocatable texture not loaded" is now always
  printed to the log (`bios.c`) so a test sees a car without room.
- **The plain build** (no portConfig) keeps Minion.
- **Tested** (scripted, `TM2PC_TEST_SURV_ROUND=10` starts at round 10): Los Angeles (2 others),
  Moscow (8 others): no texture warning, his head and missiles shown, stage 2 reached, "YOU
  SURVIVED ALL 10 ROUNDS!". A full 10-round run on Los Angeles reached his stage 2 too.
- **Later:** his music (`audioSetDarkToothMusic`), several Dark Tooths and heads as enemies (per-car
  boss state, `SURVIVAL.md` 1.6), and more types beside him (request 32 step 4, VRAM banks).

## Prepared for 16 cars (2026-10-12)

While the decomp builds request 32 steps 2-3 (16 cars, the car swap), the port's side is made ready:

- **One place for the counts:** `src/vplus/cars.h` / `cars.c`. `CARS_MAX` (16) sizes every per-car array;
  `CARS_HUMANS_MAX` (4, the variant's human records); `cars_ai_max()` (the computer-car records the build has:
  9 today, the variant's `TM2_MAX_AI_CARS` after step 2); `cars_ai_count()`; `HUMAN_REC`/`AI_REC`.
  Every loop over computer cars (respawns, Survival, teams, AI targets, King of the Hill, Gun Game, kill credit,
  the kill cam's hash and dump, the in-between frames, 3-4 player radars) goes through them.
- **Players 3-4 fixed:** infinite weapons, auto-refill, roulette, Gun Game's weapons and the Juggernaut's infinite
  weapons were given to players 1-2 only (loops bounded by 2); now every player.
- **Arena room:** `cars_arena_room(level)`: the game's limits today (8-11 cars), 16 on every arena once the build
  has more computer-car records (step 2). `cars_arena_types(level)`: the different car types whose textures fit
  next to the arena's (decomp `SIXTEEN_CARS.md` 4.4, largest first): Moscow and Holland 14, Los Angeles, Amazonia
  and New York 12, Antarctica 10, Hong Kong 9, Paris 8. Past that, with duplicates in the build
  (`cars_duplicates()`, decomp step 1), a random opponent repeats a type already in the match; until then the
  picker is as before (same choices, same random draws).
- **Saved match setups** keep 15 opponents (`BATTLE_OPPONENT_SLOTS`); the menus offer what the build can play
  (`opponents_playable()`).
- **Tournament line-up:** past 9 opponents the game's card table (9 cards) is replaced by the port's (wrap of
  8012A910, `ui_CardScreen`): the arena's picture, the players' cards first (red, blue... frames), then the
  opponents, dealt one every 6 frames, held 2.5 seconds (X or Start: on). Up to 16 cards in a 4 x 4 grid; the
  Challenge Match summary draws the same (`draw_lineup_cards`). Tests: `TM2PC_TEST_LINEUP=N` (ours for any
  count, held N frames), `TM2PC_TEST_LINEUP_CARS=16` (filled to 16).
- **Split-screen car select at Uncapped:** each player's car (drawn by the port with its own place and heading)
  is now blended between its last two scene frames on the in-between frames, as the game's own objects are
  (`interp_scene_t`). Before, the cars turned in whole steps at 100+ Hz. `TM2PC_INTERP_DEBUG` logs the blends.

## Teams, the kill cam, waiting to come back, and the line-up (2026-10-12)

- **Teammates' kills:** in the team test the blue computer cars wrecked three of their own: Axel's shockwave
  and Thumper's flames (area specials) aimed at the red players hit the blue cars beside them, and friendly fire
  was on. A teammate's wreck now never counts as a kill (friendly fire or not), and new match setups start with
  friendly fire off (Teams > Friendly fire; saved profiles keep their own setting). The log has a line per kill
  (`kill: X (team a) killed Y (team b), attack n`).
- **Kill cam full screen at any frame rate:** its full-screen shots are drawn by interp.c on its own frames;
  at the Original frame rate interp.c captured nothing, so the battle's split views showed. It now captures while
  a kill cam (or rewind playback) shows, whatever the setting.
- **Wreck first, then watch:** a player who is out sees their burning car for the game's own delay (the death
  event's: 420 VBlanks, 7 s, as a tournament before its restart), then spectates (1 player) or watches the
  drivers still in (2 players). With a respawn timer (Deathmatch's, the game type's seconds) the wait is that
  delay plus the timer: after the delay the player watches, "RESPAWNING IN N" counting down on their message
  line, then comes back with their own view. (2 players: one watcher at a time; the second keeps the wreck view
  and the countdown.) Tests: 1P out at VBlank 2401, spectating from 2821; Deathmatch 2P death at 2602, back at
  3202 (7 + 3 s).
- **The line-up before a Challenge Match:** the port's, for every match (the game's card table shows the
  opponents only): every car, the players first with their profile names, team-colored frames, dealt as the game
  deals (from below, turning toward three turns, landing a little askew with the game's card sound 3), up to 16
  in a 4 x 4 grid. A tournament keeps the game's screen up to 9 opponents. `TM2PC_TEST_LINEUP_AT=F` draws the
  deal as at frame F (tests).
- Known: the HUD font's minus sign sits low ("KILLS: _1" after a Deathmatch suicide).

## Both players watching, the score line, 3-4 player tournaments, teammates (2026-10-12)

- **Both players can watch at once** (2 players): each has their own watched car, chase or cinematic, turn and
  Triangle (`wv[2]` in fixes.c). The game has one watched car (viewWatchedAiCar): four.c's wrap of
  viewCarCameraUpdate sets it to that player's while their view's camera runs (`vp_watch_view`). Each watching
  player gets their own banner in their half. Tested: both destroyed 10 frames apart in Deathmatch, both watching
  with "RESPAWNING IN N", both back.
- **Photo mode's bar:** the sticks' line is gone (self-explanatory); the buttons stay.
- **"KILLS: -1":** the HUD font's dash sits on the line; the minus is printed 3 pixels up (`hud_text_print`).
- **Kills in the lives spot:** in split screen, Deathmatch's "Kills: N" (Gun Game's "Level: N") goes where the
  game's split HUD prints a co-op tournament's "Lives:" (font 2, its green, x 11 near the view's bottom), for each
  player; the line at the top is gone. (One view: the game's own lives line already says it.)
- **The kill feed in split screen:** the top-right group is raised there (level with the game's HUD), and the
  feed's top went with it: the first line was cut off. `hudlay_feed_top` makes up for the raise. Kills shown with a
  word (rams, the machine gun, freeze, mines) drew the victim's name twice ("HAMMERHEADGRASSHOPPER FROZE
  HAMMERHEAD"): now KILLER WORD VICTIM.
- **The line-up's pace:** as the game's (11 frames a card at 60), then half a second (was 2.5). Each frame drew
  every card turned and filtered at the window's size: at 2560x1440 well under 60 frames a second. The arena's
  picture and the cards that have landed are kept in a picture of their own; each frame draws only the card in
  flight (11 cards: 2.63 s at 2560x1440). Survival shows no line-up (its cars change every round).
- **3-4 player tournaments:** `tournamentOpponentCount` is a level's cars with the humans; one player has it less
  1 computer cars, and every player count faces those (the user's, 2026-10-27: players 3-4 no longer bring a
  computer car each; the lives stay: 3 players 2 a level, 4 players 1), as the arena has room (`cars_arena_room`).
- **Lives per level** (Vanilla Plus): 3 a level; 3 players 2, 4 players 1 (each player past 2 takes a life).
- **Teammates:** a computer car whose target is "the nearest player" (-1) chased and shot the nearest player of
  any team. `humanCarNearest` (8011C8E4, its callers: the chase, aiGetTargetCar, specials) is wrapped: with teams,
  the nearest player of another team than the asking car's. The variant's aimed shots (port_ai.c `portAiAim`) skip
  teammates (portConfig +0x06 teams, +0x07 players', +0x0B computer cars' teams). In a mixed team game (computer
  cars on both teams, friendly fire on, 150 s), 43 of the 48 hits on teammates were rams (packs chasing one enemy),
  2 the machine gun, 1 freeze, 1 a special; none homing. With friendly fire off (the default) none hurt.
  `TM2PC_TEAM_DEBUG` logs each hit on a teammate with its attack.
- **The score in the lives spot, by game type** (`score_line_print`, one view centered on (50, 212), split screens
  from x 11): Deathmatch "Kills: N", Juggernaut "Kills: N" (made as the Juggernaut: its points), Gun Game "Level: N"
  (one view "of M"), King of the Hill and Hunted "Points: N". The game's lives line is blank in these modes (its
  text is too short for "Points:"); Gun Game's own one-view line is gone into this.
- **The line-up while dealing:** the landed cards were laid out for their own count (3 x 3 up to 9) while the card
  in flight used the whole line-up's (4 x 4): sizes and places jumped until the last card. Every card is laid out by
  the whole count now (`draw_lineup_cards` draws a range of them).
- **The message bar:** one view and split screen measured the same (at 1280x960: the bar rows 20..79, the letters
  28..59: 2 game units above, 5 under; the game's own numbers, a 15-unit bar with the baseline 11 under its top).
- **Message text centered** (Customize HUD > Message text: Centered, the default, or Classic): the bar stays level
  with the radar's top (both at the same row); its letters go a unit down (one view: the game's message sprites,
  hudMessageTextSprites 80181E34.., moved in the layout; split screens: the port's own print). At 1280x960 the
  letters then sit 12 px under the bar's top and 16 over its bottom (Classic: 8 and 20). `[video]
  hud_message_text = centered | classic`.

## 16 cars in the build (decomp request 32 steps 1-3, integrated 2026-10-12)

The hybrid is built with the decomp's `TM2_PORT_CAR_GROUPS`, `TM2_PORT_CARS=16` and `TM2_PORT_CAR_SWAP`
(`tools/decomp/wc.py`, `PORT_DEFINES`; `wc.py sync --rev REV` takes a decomp commit through `git archive`).
The recompiler hands the host the build's port symbols by name (`rc_port_symbols`, `port_sym()` in
`vplus/cars.c`), its computer-car records (`rc_ai_cars_max`: 16) and a HUD radar record's size
(`rc_hud_radar_bytes`: 0x430 with 15 blips, 0x370 retail).

- **Counts:** `cars_ai_max()` 16, every arena 16 cars (`cars_arena_room`), duplicates on (`cars_duplicates()`:
  the build has `portCarGroups`); a random opponent repeats a type once the arena's VRAM holds no more types.
  "Allow duplicate cars" (Vanilla Plus) is unlocked.
- **The load** (`vp_SessionLoad`): more than 11 cars sets `portCarOptions` bits 1-3 (car DMDs high, starts spread,
  raised weapon pools) before the load; 11 or fewer: 0, as retail. Opponent slots 9..15 go through the settings'
  accessors (`settingsGet/SetOpponentCar`: the decomp's `sessionPortOpponentCars`).
- **3-4 players:** cars 3-4 first among the opponents, bound with their load group (`carBind*ByType(i, type,
  group)`, through `portCarBindTypeOf/GroupOf`), so two of a type are each their own car. A tournament's opponent
  count includes players 3-4's slots, a Challenge Match's does not (`four_computer`); the tournament's computer cars
  are capped by the arena's types too (the game draws different cars).
- **Radars:** players 3-4's radar records are copied at the build's record size (a 0x370 copy into 0x430 records
  wrote past RAM: the 4-player crash with more than 11 cars); the blip colors read count and buffer from the
  record's end, and with duplicates find the blip's car by position (team colors right with one car type).
- **Line-up:** every opponent (it stopped at 10), the parked Juggernaut car left out.
- **Force Juggernaut car** (game type, Juggernaut page; off by default) and **Juggernaut car** (any driver, Minion
  among them): the car is loaded as one opponent more, the last, parked after every bind (`portCarParkAi`) and left
  out of `aiCarTotal`. Each new Juggernaut takes it over at the next battle frame (`portCarSetGroup`: the cars
  exchange places, HP full with "Full health"), the one before getting its own car back first. The slot-keyed
  state (the Juggernaut, the tally: `match_slot_moved`) follows the record to its new slot; before every bind each
  record gets its own car again (`portCarBindGroup` cleared).
- **Tests** (`compare_builds.py`): `sixteen` (2 players, 14 opponents), `sixteen_same` (16 of one car: a game type
  with one driver), `sixteen_teams` (4 players, 4 teams of 4, one car), `sixteen_tourney4` (4 players' tournament),
  `jugg_car` (the forced car taken over and handed back). In these only the hybrid plays the counts; the plain build
  keeps the game's limits.
- **What the variant moved, read from the build** (`recompile.py`): the HUD radar record (`rc_hud_radar_bytes`), the
  shield pool's size (`rc_shield_count`: the spawn shield's record after N objects; a fixed 4 left the shield on),
  the projectile records (`rc_projectile_count`, the kill feed's tags), the car-pair row (`rc_car_pair_stride`, the
  tests' state hash reads the pair tables pair by pair). Car models always go in the variant's `portCarDmdArea`
  (`portCarOptions` bit 1): the port's own place above 2 MB is the variant's port memory now. The kill feed's
  attack, where the variant moved a call, is matched by the function (`gr_function`, `changed_sites.py`).
- **Equal to the plain build at 11 cars or fewer:** one, two, teams, kills, lives, dm_suicide, koth, feed,
  jugg_suicide (state every frame and the shot). `four` is not: a 4-player tournament now plays 12 cars on
  Los Angeles (8 computer cars), more than the plain build holds.
- **Limits for now:** cars of one type share their look, specials and engine sound (decomp request 33: per-car
  CLUTs, per-key specials, sounds), so team colors on a shared type repaint every car of it; a kill cam whose
  replay spans a Juggernaut change shows the car where it was (swaps are not replayed).

## 16 MB, each car's specials, Dark Tooth per car, texture banks (decomp requests 32-4, 33-2a, 33-2e; 2026-10-13)

- **16 MB of guest RAM** (`GUEST_RAM_BYTES`, `runtime/psx_cpu.h`): the decomp's port memory is 0x80200000-0x80BFFFFF
  (its `--port-ram-end` 0x80C00000), the port's own areas moved up by 0x00600000 to 0x80C00000-0x80DFFFFF (packets,
  order tables, frame records, the moved arrays of the plain build at 0x80DE0000: `tools/recomp/rebase.py`). A draw
  list's links are 24 bits: its end is 0xFFFFFF only (bit 23 is an address now: `gpu.c`). The kill cam, photo mode
  and the in-between frames copy all 16 MB.
- **Each car its own special** (`TM2_PORT_CAR_SPECIALS`, `CAR_GROUPS.md` 10): two Twisters, Mr. Slams, Shadows,
  Axels or Grasshoppers no longer share one state. The retail bodies of the tornado, leap and Shadow strike updates
  run from the decomp's own code (`...UpdateOne`): the recompiler hands the host the build's port functions
  (`rc_port_functions`, `port_function_of()`), so the kill feed still names their kills SPECIAL.
- **Dark Tooth per car** (`TM2_PORT_DARK_TOOTH`, `CAR_GROUPS.md` 11): Survival with Dark Tooth sets `portCarOptions`
  bit 6 (his fight on any arena), in place of the overlay's `portConfig.boss_anywhere` (its hunks gave way to the
  decomp's at the merge).
- **Texture banks** (`TM2_PORT_TEXTURE_BANKS`, `TEXTURE_BANKS.md` 6, PSY-Z patch 15): every car's textures and CLUTs
  in a VRAM of its own (a layer of a texture array in the renderer, made at its first upload; a CPU copy answers
  StoreImage). A textured polygon names its bank in its tpage bits 9..13 (banks 1-31); `gpu.c` reads
  `portVramUploadBank` when a transfer starts. Bit 5 is set at the load when the match needs it: more car types
  than the arena's VRAM holds (`cars_arena_types_vram`), or a type twice; else retail's VRAM (the equality tests).
  With banks `cars_arena_types` is 16 on every arena. `TM2PC_TEX_BANKS=1` always, `=0` never. The GL renderer has
  none (the game keeps them off).
- **Team colors per car:** with banks each car's body CLUT is repainted in its own bank (`portTexBanks.body_clut`,
  `portTexLoadImage`): two of a type on two teams each show their team. Without banks the slot of a group is
  group x 32 with the decomp's `TM2_PORT_CARS` (it was x 20: the wrong slot). Players 3-4's groups are theirs
  (their team), the computer cars after them.
- **Each car's engine its own voice** (`TM2_PORT_CAR_SOUND`, `CAR_GROUPS.md` 12, decomp PR 37): `portCarOptions` bit 4
  with a type twice or more than 11 cars (else retail's calls): every car's engine and Thumper's flame its own
  instance, the far engines dropping first when the 24 voices run out (effects keep theirs). No cap (the user's).
  Dark Tooth's own hit (`aiDarkToothBossUpdate`, changed) is matched by its function for the kill feed. Test:
  `surv_boss` (Survival's round 10 on Los Angeles, bit 6).
- **Survival for 3-4 players** (2026-10-13): the Survival page is open with 3-4 players (it was "Coming later").
  Every player picks in the live car select (`survival_pick`, players 3-4 the port's as in the tournament), then the
  game's opponent select. Co-op as with 2: a player destroyed watches (their own camera, legend and trimmed HUD)
  until the next round brings them back; all out ends the run. Survival has 12 computer cars at most for every player
  count (`SURVIVAL_CARS`, the user's: 16 cars with 4 players; the rounds bring more of them each time, round 10 all;
  the plain build keeps the game's 9). With texture banks Dark Tooth has room beside any cars (`boss_room` was the VRAM's), and a run
  with him turns banks on. Tests: `surv2`, `surv4`, `surv_boss` (`surv2` is not equal to the plain build: 12 cars against its 9).

## The user's playtest (2026-10-17)

- **Defaults:** respawn time 0 (the steps say "additional" seconds after the game's own wait: the rows are 0, then
  the old steps); pickups "DEFAULT" (the game's); special recharge adds 0.25x and 3x (`SPECIAL_QUARTER`,
  `SPECIAL_TRIPLE`, `battle.h`; the recharge num/den tables in `fixes.c`).
- **Built-in game types:** Rockets Only and No Health removed (their rules are a few rows in any game type).
- **CPU logic:** the original AI is "Classic"; the rows say "CPU" for computer cars.
- **Gun Game:** no Lightning and no ram kill in the default levels (`gun_round_start` skips both; a player's own
  game type can still list them), specials on by default (`BATTLE_SPECIAL_AS`), the order strongest to weakest
  (`gun_order = 1`). Computer cars use their level's weapon: `vp_AiFireDecide` (wrap of `80103CA0`) on a level that
  is not a special clears the special fire bit, fires the pickup weapon instead and selects it (the AI's ammo is
  refilled to 1 every tick by `weaponTick`). Their respawn waits the humans' burn time (`burn_wait`) plus the
  game type's seconds.
- **Kill credit after requests 41-42:** remote bomb, special and Shadow splash, machine gun, ram and pedestrian
  kills are matched by the function containing the return address (`gr_function`), not the exact address, since the
  decomp recompiles those functions to new lengths (`tools/test/changed_sites.py` allows the old sites).
- **Second session (2026-10-17, `PLAYTEST_2026-10-17b.md`):** Weapon Roulette 20 s, 4 a switch; **Roulette weapons**
  (`roulette_add`): Replace (each switch takes the weapons away; weapon pickups give the roulette's weapon) or Add.
  Gun Game: a computer car's special refused at `weaponFireSpecial` unless its level is one (`vp_gun_blocks_special`).
  Spawns: the route nodes picked past the start points remembered for the frame (16 cars no longer share one).
  Teams and friendly fire both on the match's Teams page. The scoreboard: respawn modes by points (Gun Game's level),
  Last Car Standing with lives by lives, drawn again only when it changes, wider on wide screens.
