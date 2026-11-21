# Custom Tournament

Local Play > Tournament > Custom Tournament: a tournament the player sets up, played like the game's (stages in a
row, lives carried, the globe between stages, bosses arriving at the end of a stage), without unlocking any of the
game's endings or drivers.

## The setup (kept in the profile, `[custom_tournament]`)

| Row | What |
|---|---|
| Stages | 6 or 8 |
| Stage N: Arena | any arena unlocked (the 8, then the secret ones), or Random (rolled when the tournament starts) |
| Stage N: Enemies | how many computer cars (capped by the arena's room), or Random (3 to 7) |
| Stage N: Bosses | none, 1 or 2; then Boss 1 / Boss 2: any driver (a boss or a regular one): they arrive once the stage's enemies are out ("BOSS BATTLE") |
| CPU Drivers | the roster: which drivers the enemies are drawn from at random (never a player's car): the 12, then Sweet Tooth (on at first), Minion and Dark Tooth (off at first) once unlocked; the bosses as ordinary enemies: Minion, and Dark Tooth in his first phase (no second stage: `portCarOptions` bit 10, decomp request 42; once a stage, not beside his boss fight, where his textures fit). In place of "Bosses as regular contestants" (2026-11-20): an older profile's switch becomes the two bosses' rows, Sweet Tooth on |
| Difficulty | as the tournament's; the lives too: 3 for the run (3 players 2, 4 players 1), or at every stage with Vanilla Plus "lives per level" |

The roster offers only unlocked drivers. A Versus Tournament server (Network Play) has its own setup on the same page,
kept in the profile's `[versus_tournament]` section and starting from the original 8 levels (2026-11-20); the
Custom Tournament's is `[custom_tournament]`. No saving: that stays the
tournament's. No scoreboard (held or on the results), as the tournament and Survival: only Challenge Matches and
online have it.

The defaults are the game's tournament: its 8 arenas in order, Minion after Amazonia, Dark Tooth last.

## How it runs (ui/options.c, vplus/fixes.c)

- Each stage is a Challenge Match (mode 3, Last Car Standing) on the stage's arena: `battle_match()` returns the
  stage's setup while a custom tournament runs (`battle_match_override`). The game's own tournament mode is not used:
  its level number is also the arena, and the game ties Minion, Dark Tooth, the next-level text, the Easy stop and the
  ending to it.
- The cars are chosen once (car select at the start); each next stage goes straight to the opponents' screen
  (`ui_AiSelect`: drawn from the roster), the globe and the line-up.
- **Lives:** each player's are carried from stage to stage (`ui_tour_lives`, read by `vp_lives_match_start`); the
  computer cars have one each. A player out in co-op comes back at the next stage with one life.
- **Between stages:** the globe with the next arena's name, then the line-up of the opponents' cards (not the players',
  as the tournament's).
- **Bosses:** loaded as the last opponents and parked out of play (`vp_boss_wave`, Survival's `ai_out_of_play`).
  When the others are out the game's "won" is held back, and 2 seconds later the bosses come in ("BOSS BATTLE"); the
  stage is won once they are out too. Dark Tooth's fight on any arena as Survival's (`portCarOptions`
  bit 6, texture banks); each boss only while the match has room for it.
- **2-4 players:** together against the computer cars, as the tournament's co-op: no teams or team colors; the
  port's 2-4 player rules count the players as one side (`ui_tour_on` in `vp_duo_frame`).
- **Between stages:** no results screen (the user's, 2026-11-13: a tournament is surviving to the next stage). A won
  stage goes straight on (the game's own end text, then the next stage's opponents); the match still counts for Stats
  (matches, wins, kills, deaths, damage). A lost stage: GAME OVER with RESTART (the setup's lives again) / QUIT. The
  last: TOURNAMENT WON! (or the final battle's winner) with FINISH, then back to the page.

## Online: custom co-op tournament

Network Play's Host Server, Co-op Tournament, Game settings: **Tournament: CUSTOM** (beside NEW and LOAD), then
**Stages** opens this page (as Local Play's: arenas, enemies, bosses, drivers, difficulty). The user's rules
(2026-11-12):

- **Lives:** as the tournament's: 2 players 3 each, 3 players 2 each, 4 players 1 each (a player out comes back at
  the next stage with one); the final battle after the last stage, one life each.
- **No joining** once it has started (as the online co-op tournament), and **no saves**.
- The host rolls the random arenas and enemy counts and sends them (`NetSettings.vt`, `vt_arena`, `vt_enemies`,
  `tour_custom`); each stage starts from the host's state (`coop_custom_begin`, `netplay_each_battle`).
- Between stages: no results; a won stage straight on, on every machine. A lost one: the host's RESTART / QUIT, the
  others "WAITING FOR <host>" with QUIT (leave the server), as the online co-op tournament (`net_tour_choose`).
- After the final battle (or EXIT / QUIT): everyone back in the server's lobby.

## To come

- Mod arenas and drivers (docs/MODDING.md); tournament packs (fixed stages, endings) and online versus tournaments
  on the same run.

Tests: `tour` (6 stages won, Minion's wave on stage 4), `tour_dt` (Dark Tooth as stage 1's boss).
