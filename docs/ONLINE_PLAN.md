# Online play: the plan

The user's list (2026-10-17) and how each part is built. The model was agreed on 2026-10-02: a deterministic simulation
(the game's own fixed 20 steps a second, one ruleset, shared random seeds) driven by every player's inputs, with input
delay and **rollback**; a full state transfer only to join late, recover from a desync, or move the host.

## 1. The shape

- **Listen servers.** A player hosts (Host Server); the others join it. The host relays: every client sends its inputs
  to the host, the host sends everyone's inputs to everyone (a star, not a mesh: 16 players would be 240 links in a
  mesh, 15 here, and only the host needs to be reachable). The host also owns the lobby, the playlist, votes and kicks.
- **One simulation everywhere.** Every machine runs the whole match from the same inputs. Nothing about the match is
  sent but inputs (a few bytes per player per step), plus a state hash now and then.
- **Input slots.** A match has up to 16 slots; each machine fills 1-4 of them (its local split-screen players). A slot's
  input comes from a local pad, the network, or the AI (a computer car), set per slot when the match starts (the
  guardrail agreed on 2026-10-02).
- **A master server** (our own small service) for what a listen server can't do alone: the online server list,
  getting through home routers (NAT hole punching, and a relay when that fails), friends and presence, and the global
  stats. LAN play needs none of it (servers found by broadcast).

### Dedicated servers and master servers

- **Both kinds of server.** A player's game hosts (Host Server), or a **dedicated server**: the game started with no
  window and no players (`--headless`, a `dedicated` setting file), always on, running a playlist. To the players it
  is the same host; it just has no local slots. Project Reclaimer (Halo 3) works this way: player hosting and dedicated
  servers (Windows or Linux/Docker, several servers per machine, each its own process, a `dedicated.toml`).
- **Smoother?** Not by itself: with rollback every machine runs the match and only inputs (and voice) cross the network,
  so the host's machine does little (relaying about 1-2 KB/s of inputs per player, plus voice). What a dedicated
  server gives: it never leaves (no host migration), it sits in a data center with a good connection (often a better
  route to everyone than a home host), nobody has a host advantage, and it can run the playlists and moderation 24/7.
  A home host with a weak upload and 16 talking players is where a dedicated server helps most (voice is the biggest
  stream: 16 kbit/s per talker).
- **The master server** is needed for **online play over the internet**, not only stats: the server browser's list,
  introducing a player to a server behind a home router (hole punching), friends and presence, and the stats. It
  carries no match traffic (the players talk to the server directly), so it is small and cheap. LAN play and joining
  by address (Direct Connect) work without it. Project Reclaimer: two default masters and fallback ones learned from
  the others; servers report to all of them. We do the same (more than one master; the address list in the game, and
  updated by the masters).
- **Hosting:** the master needs a machine with a public address that answers UDP (hole punching): a small VPS (Hetzner,
  DigitalOcean, Vultr: about $4-6 a month), which can also run a few dedicated servers. **Cloudflare** suits the
  website and the stats API (Workers and the D1 database, free to start) and the domain, but its Workers do not take
  raw UDP, so the master's hole punching stays on the VPS. Neither project says who hosts its masters.
- **Voice:** OpenCE uses Opus, 20 ms frames, relayed by the host without decoding and 8 talkers at most; Project
  Reclaimer compressed voice at about 16 kbit/s per talker, global/team/proximity channels, sent through the server.
  Ours: the same (built: `src/net/voice.c`).

**Master server built** (2026-10-20, `tools/master/master.c`, protocol `src/net/master.h`): one UDP port (27522), Windows
or Linux, nothing on disk. A hosting machine registers its listing every 4 s from its game socket (so the master sees
the public address its router gave it), the other machines of an internet server send a keep-alive, a machine leaving
says BYE; the Internet browser asks for the list (one ask a second per address). Hole punching: a machine that wants to
reach another (a joining machine, the peers after a host migration) asks the master, which sends both an INTRO with
the other's public address; both send a few probes, so each router lets the other's packets in. The master
only introduces machines it has heard from. The game uses the masters of `[network] masters` (the DigitalOcean
droplet first, the NAS second; `TM2PC_MASTER=host[:port]` overrides). Tests: `tools/test/master_test.py` (listed,
joined through it, the host migrating: the new host listed, the old one gone; two masters).

**Relay** (2026-10-20): a joining machine that hears nothing from the host for 3 s (introductions too: a router hole
punching cannot pass, symmetric NAT, some mobile networks) sends through the first master instead: RELAY (the other's
address, then the packet), passed on as RELAYED (the sender's address first); the host answers the same way. The master
relays only between machines it has heard from, at most 1 MB/s a sender, and logs its relayed traffic every 10 s. A
relayed machine's reliable data (the host's state, a few MB packed) is paced to 400 KB/s; a match through the relay
uses about 95 KB/s, the state's transfer about 370 KB/s for some seconds. Test: `TM2PC_NET_FORCE_RELAY=1` on a joining
machine. Found 2026-10-21 with 3-4 players through it: the relay's share (600 KB/s) is split between the relayed
machines (each its own 400 KB/s went over the master's cap with two states at once); a machine waiting for the host's
state gives the host 20 s, not 2, before it counts it gone (nothing comes before the host takes it in); and a battle
waiting for a machine whose state is still coming through the relay goes on alone only when nothing at all has come
for 10 s (or after a minute).

## 2. Rollback netcode

- The game steps 20 times a second (the port draws 60 FPS and more between the steps). At 20 Hz a step is 50 ms, so a
  100 ms round trip is about 2 steps: rollbacks are short (a few steps), and the inputs per packet tiny.
- **Each step:** read the local players' inputs (after the port's own mapping: jump combo, easy energy attacks, analog
  steering, `HARDENING.md` 22), send them, and advance with the inputs known; a missing remote input is predicted (the
  player's last one). When a remote input arrives and differs from the prediction: load the state of that step, step
  forward again to now with the real inputs (resimulation, nothing drawn), and go on.
- **Input delay** of 1-2 steps (set by the ping, adjustable) keeps most rollbacks away; the rollback window is about
  8 steps (400 ms) at most: a player further behind is waited for (the game stalls briefly) rather than rolled back
  further.
- **Time sync:** each machine keeps its step count within a step of the host's (a machine ahead runs a hair slower).
- **State:** guest RAM (the game's 2 MB plus the port variant's sections), the scratchpad, and the port's own state
  registered with the kill cam's keep list (`HARDENING.md` 3 lists what is still missing). A snapshot per step for the
  window (8 x a few MB, copied at 20 Hz: cheap); later only the pages a step changed.
- **What must be fixed first** (`HARDENING.md` 3-4): one shared seed for the port's random numbers; nothing that
  depends on what this machine draws (the AI's "unseen" throttle, damage per drawn view, kill-cam camera picks);
  render-only HUD writes; integer distance tests; the ruleset and game type sent by the host, not looked up locally.
  Decomp request 44 asks where the game itself breaks those rules.
- **Built so far (2026-10-22, `src/net/rb_state.c`; tests only: `TM2PC_NET_ROLLBACK=1`).** With the decomp's
  `STEP_SPLIT` and its re-run pieces (`portRollbackOptions` 0x17D, decomp ONLINE_ROLLBACK.md 12) the main loop's
  iteration runs the whole step, then draws; its last call (`pauseMenu`, after the frame went to the GPU) is between two
  steps. There each machine keeps the step's state (the `snapshot yes` spans of the build's `rollback_regions.tsv`, the
  models' node fields, the BIOS and port random seeds, the port's kept blocks, every human's input), and on a late
  input that differs puts the state of that step back and runs the steps since with `portRunStep` (no drawing, no
  sound, the palettes uploaded once after). Two lessons: the state is taken after the frame went to the GPU (taken at
  the pacing wait, a loaded state's order tables went to the GPU with other packets), and the clock's interrupt
  accumulator (`timerTicsAccum`) stays live across a batch (put back, the main loop's pacing ended its next wait at
  once and the machines' clocks parted). A machine runs at most `TM2PC_NET_WINDOW` steps (4) ahead of the inputs it
  has; one step an iteration of the game's loop (the session's `one_step`).
  - **The re-run check** (`TM2PC_TEST_RERUN=K,EVERY`, offline): K steps played, put back, run again, compared (the cars,
    seeds, nodes and the port's blocks). 1000 checks of 8 steps over `kills`, `two`, `lives`, `koth`, `four`,
    `laquake`, `tour` (every level, the bosses), `dt_player`, `secret`, `arena_off`: all equal. Since request 54 the
    node list is every loaded model file's (made once a load, kept on the host: `dmd_list`), so a node an object takes
    later is in every state. No state reaches back past a round's start (`rbs_new_round`).
  - **Two machines** (`tools/test/net_lockstep.py`, `TM2PC_NET_ROLLBACK=1 --delay 0`, `TM2PC_NET_LAG_MS=100`): 224
    hashes checked over 1800 steps, no desync; 9 rollbacks on the host (up to the window), 34 on the joiner.
  - **Three machines through the lobby** (`TM2PC_NET_ROLLBACK=1 TM2PC_NET_LAG_MS=60 NET_KILLS=none
    tools/test/net_lobby.py --players 3`): 598 hashes checked over 2400 steps, no desync, 16 rollbacks on the host.
    (The state logs differ from the first predicted step: a log line is written before a correction; the confirmed
    steps' hashes are the check.)
  - **The port's own per-step state:** rollback also restores the kills' attribution (`feat_hooks.c`), the match
    tally (Gun Game's levels come from it) and the stats (`rbs_keep`; the kill cam's replays keep their "counted once"
    rule); real-time counters stay live across a batch (`rbs_live`: the Stats screen's seconds); a step run again
    shows no kill feed line or mode message (the first run did: a kill only the corrected steps have is not shown, one
    only the predicted steps had stays shown, as the decomp's sound rule). 440 re-run checks over `kills`, `feed2`,
    `jugg_car`, `hunted_teams`, `gun_specials` equal.
  - **Players leaving, coming back, dropping in** (PR #355): applied as the step begins, live and re-run
    (`slot_events`); a join's or resync's state made at a confirmed step (the host stops predicting while one is owed);
    the kills' attribution and the tally sent with every state.
  - **A round's end** (request 55): while `portRollbackEndDue()` says the next step ends the round and an input before
    it is predicted, the machine waits (window 0): every machine ends it at the same step (3 machines at 80 ms: step
    448 on all).
  - **The decomp's whole table between machines** (`rbs_hash`, `TM2PC_NET_FULLHASH`, for tests until request 57):
    less each machine's own words (the pads' buffers, sound, the views' cameras, rigs and the sky, the buffer flip,
    `portHumanLocal`, the HUD texts, the human records' camera bytes) three machines hash equal, lockstep and rollback;
    after a drop-in two more words still differ (request 57). The default compares the cars, the seed and the nodes.
    The hash takes under 1 ms (the spans less the masks made once; 17 ms word by word).
  - **The kill cam:** a re-run's inputs and hashes go to their own frames, snapshots of guessed steps are forgotten:
    the final replay matches the battle frame for frame on every machine.
  - **Catch-up:** a machine 2 steps or more behind the host runs a released step at once, unseen (`ses_behind`).
  - **Rollback is the netcode** (2026-10-23, the user's: it replaces lockstep, no option): on whenever the build has the
    decomp's rollback pieces (`netplay_uses_rollback`; lockstep only without them, tests: `TM2PC_NET_LOCKSTEP=1`),
    input delay 1 step (50 ms; lockstep's was 2). Host migration needed nothing more (mid-match and before the start).
    Tested in this mode: 3-player matches (0 and 80 ms), rejoin, drop-in, the host leaving, a playlist, Co-op
    Tournament (each level from the host's state: the frame hook still sees a battle left), Survival.
  - **The whole table's hash between machines** (2026-10-23, after requests 57-58): the default. Every mode (3 and 4
    players, 0-120 ms, rejoin, drop-in, the host leaving, Survival, Co-op Tournament, a playlist): no desync.
  - **Fixed on the way:** a machine coming back gets every input since its state (the inputs sent as far back as the
    ring holds: it waited for steps the others no longer sent), and a step it catches up runs as a step run again (the
    live frame hook expects a drawing between two steps).
- **Never online** (the user's, 2026-10-17): Minion's volley and invisibility. The host's rules go through
  `battle_online_rules` (`BATTLE_ONLINE_NEVER_ENERGY`, battle.h) before they are sent; Host Server shows both rows
  locked OFF.

## 3. Desync detection and recovery

- Every confirmed step (all inputs known) each machine hashes its state (minus render-only areas); every 10th hash goes
  to the host with the step number. A machine whose hash differs from the host's is out of sync.
- **Recovery:** the host sends its state of a recent confirmed step (compressed, a few hundred KB, over the reliable
  channel); the machine loads it and replays its inputs from there. The game pauses for that player a moment
  ("Resyncing..."), not for everyone. Repeated desyncs are logged with both states for us to fix the cause.
- The same transfer lets a player **join a match in progress** (a joining player replaces a computer car: request 33).

## 4. Voice chat

- In the lobby and in the game: push to talk or open mic (the lobby's Voice chat row), per player mute.
- Captured with SDL3's audio input, encoded with **Opus** (BSD license; needs downloading: asked first), 20 ms frames,
  sent through the host like inputs but on their own channel (never resent: a lost voice packet is a short gap). Each
  speaker's stream mixed into the game's audio output; the lobby's microphone icon from it.
- Voice is never part of the simulation.
- **Built** (2026-10-20, `src/net/voice_io.c`): push to talk (Controls: "Push to talk", a key, V by default, and a
  controller button, none by default), the microphone open only while it is held; each speaker played on an audio
  stream of their own; a muted machine dropped as its frames come (the lobby's mute and the Players menu's are one);
  the server's Voice chat setting off: nobody sends. In a match, whoever is talking gets a note at the top right under
  the kill feed (a green microphone and the name), and the Players menu marks them. Since built: the lobby's own
  microphone marks, a voice volume (Audio), and open mic (Audio: push to talk or open mic, with a microphone gate).

## 5. Four split-screen players on one machine

Built into the slots (1): a machine joins with 1-4 local players, each takes a slot, the server counts all of them
toward its 16. Each local player signs in with their profile (their name online). The host's own machine the same.

## 6. Playlist and votes

- The host sets up to 16 games (game type, arena, settings) on the Playlist page (built: menus).
- After each round: three games from the playlist drawn at random (by the host; the shared seed), every player votes
  (the vote screen, a few seconds), the most votes wins (ties: the host's random pick), and the next match loads at
  once for everyone. Only the host ending the playlist goes back to the lobby.
- All of it host messages on the reliable channel: the draw, the votes, the result, the next match's full setup.

## 7. Vote kick, mute, friends

- **Vote kick:** from the lobby (built: menus) or in the game; more than half the other players agree within 30 s ->
  the host removes the player (a kicked player can't rejoin that server for the session). The host can kick directly.
- **Mute in the game:** hold the scoreboard button, then move a highlight on the scoreboard (D-pad, or the mouse) and
  press a button (Square, or right click) to mute or unmute that player's voice and chat. Also a **Players** entry in the
  online pause menu (in place of Photo mode, which online play can't have), listing everyone with Mute / Vote kick.
- **Friends (built 2026-10-21):** each profile gets a random id (5 bytes: its **friend code**, 8 letters and digits,
  "XXXX-XXXX") and a 16-byte secret the first time it goes online (the profile's `[online]`; no key pair: no crypto
  library to download; the master keeps the first secret it sees for an id and refuses others). The game says where
  it is through the masters (`src/net/friends.c`; the Friends page and the browser from their own socket, a server's
  machines from the lobby's: hosting, or in a server at the host's address) every 4 s while in Network Play's online
  pages or an internet server; the master (`tools/master/accounts.c`) answers with the friends (online, in which
  server) and the requests, and keeps players, friendships and requests on disk (`tm2_master.db`: written every 30 s
  while changed, and when stopped). Network Play > Online > **Friends**: your code, Add a friend (by code), Accept /
  Decline, your friends and where they are (in a server: X joins it); a server's player page: Send friend request;
  the browser marks servers with friends. Tests: `tools/net_sim/master_friends_test` (the master alone), and two
  copies with `TM2PC_TEST_FRIEND_ALL=1`. The design before it: the master server knows a player by the public key and the name. A friend request, accepted on the
  other side, puts each in the other's friends list (kept by the master server). Friends: a friend icon by the name in
  the lobby and on the scoreboard, servers with friends in another color in the browser, and a Friends page (online or
  not, in which server; Join, which fails as any join would if the server is full).
  **Marks built** (2026-10-23): a sky-blue person after a friend's name in the lobby's cells and on the in-game
  scoreboard, and a friend's server named in sky blue with the person by the lock in the browser. Pictures:
  `TM2PC_TEST_ALL_FRIENDS=1` (every other player a friend) with `TM2PC_NET_MENU_SHOT=<folder>` (the lobby, browser and
  scoreboard as BMPs) through `tools/test/net_lobby.py`.

## 8. Online Co-op Tournament, Custom Tournament, Survival

The same netcode with up to 4 human slots (the lobby already caps these at 4); the host runs the tournament's flow
(stages, the globe, the final battle) and sends each next step. Saving a run online: the host's.

## 9. Host migration

- Every machine holds the whole match, so any can carry on as the host. When the host leaves (or drops for 5 s), the
  others pause ("Host left: moving the server..."), the new host is the player who joined first among those left (or
  the best connection: decided by the same rule everywhere, no vote), everyone agrees on the last step all of them have
  confirmed, the new host continues the relay from there, and the match resumes. Inputs after that step are re-sent.
- The catch is reachability: the clients must reach the new host. Through the master server's hole punching and
  relay this works like a first join; on LAN directly.
- **Built** (2026-10-20, `src/net/lobby.c`, `game_tm2.c`): the lobby's state carries every machine's address as the host
  hears it, so each machine knows the others'. The host silent 2 s: the machine with the lowest number left (the one
  there longest) takes over, in the lobby and in the match (the session's own rule, the same everywhere). In the match
  the game holds on its last frame with a box: "HOST DROPPED / Finding a new host...", then "Transferring host to
  NAME...", then "NEW HOST: NAME" over the game a moment; in the lobby a chat line ("Host dropped. NAME is the host
  now."). The new host answers the LAN browser and takes new machines (a free node; the welcome says which node hosts).
  The playlist lived on the old host: after a move the server plays single matches (back to the lobby). Kick bans
  stay with the old host. A host leaving as the match starts, before its state went out (found 2026-10-21 with 4
  players: everyone waited at step 0): the new host's own battle, at the same step, goes to the others instead.
  Tests: `lobby_test` (host leaves, the next takes over, the LAN browser finds it, a new
  machine joins it and chats), `net_lobby.py --players 3 --quit-host 82` (battles equal, both back in the lobby).
- **A player leaving mid-match** (built 2026-10-20): every machine reaches the same step where that player's input
  ends; from it the car is out (destroyed, no one's kill, no life left) and "NAME LEFT THE MATCH" shows. The user's
  rule: a player who took a CPU / OPEN spot gets a computer car in their place. Built 2026-10-21 with decomp request 50
  (`portHumanHandOver`): a computer driver takes the car over as it is (place, health, weapons), on the player's team,
  with their lives left; "NAME LEFT: CPU DRIVES ON" shows. Not in Survival or a Co-op Tournament (the computer cars are
  the enemy there), nor for a car that is down at that moment: those leave the match as before.
- **Coming back** (built 2026-10-20): a player whose machine dropped joins the server again and goes straight back into
  the match on, as the same player (the host knows the machine by its own number, or by the player's names). Its
  battle starts and runs a moment (its own start-up), then it asks for its old slots; the host sends its state and from
  a step every machine has, its car is in again (away from the others, its lives as when it left). The others wait a
  moment ("WAITING FOR PLAYERS") while the state goes. Found on the way: the port's kept blocks (replays and states)
  stopped at 64 of 77 (KEEP_MAX: 128 now); a state is never taken while a replay shows; a returning machine's
  inputs from before its start are refused; the host follows each machine's latest report of what it knows. A car a
  computer driver took over goes back to its player as it is (`portHumanReturn`); if it is down just then, the player
  gets it once it comes back.
  Test: `net_lobby.py --players 3 --quit-last 82 --rejoin 70` (the battle the host's, frame for frame).
- **Joining while a match is on** (built 2026-10-20): the machine waits in the lobby ("A match is on: you play in the
  next one") and plays the next one (a playlist's next match too). **Dropping in** (built 2026-10-21, decomp request 33's
  `TM2_PORT_CAR_DROPIN`): in a Challenge or Versus match with CPU / OPEN computer cars, the host gives a new machine's
  players the next human records and sends everyone the start with them in it; the machine loads the match, takes the
  host's state, and at the step its slots begin every machine removes a CPU / OPEN computer car for good
  (`portCarRemove`) and starts the player on its car, fresh, at the start farthest from the others
  (`portCarStartHuman`), with the match's lives ("NAME JOINED THE MATCH"). For now the player drives that computer
  car's type (a car of their own pick would need `portCarLoadGroupLive` with its files read from the disc). Survival
  and the Co-op Tournament still wait for the next match. Found on the way: a machine asked into a match as it
  finished connecting threw the ask away; a state from the host overwrote the joiner's button tables (written again
  after every state); a newcomer's table copied before it arrived (taken again at its step).
  Test: `net_lobby.py --players 3 --late 60 --dropin` (frame for frame, no desync).

## 10. Versus Tournament

The user's rules (2026-10-21):
- **Stages:** 6 or 8, the Custom Tournament's setup (`docs/CUSTOM_TOURNAMENT.md`), set by the host; the defaults the
  game's tournament: its 8 arenas in order. The stages run straight into each other (no vote, no lobby between them);
  every player keeps their car for the whole tournament.
- **Each stage:** Last Car Standing, 1 life each; nobody sits out a stage. Computer cars: the host's choice (the
  setup's enemies per stage, 0 for players only).
- **Points:** by finishing place in each stage (the order the players are knocked out; the last one standing first),
  no points for kills; the most points after the last stage wins (the "TM2 Original" ruleset: the winner's ending,
  `PORTRAIT_UNLOCKS.md`). Which places score: to settle with the user (the top 3 or 5).
- **Boss stages:** with the default setup stages 4 (Minion) and 8 (Dark Tooth): the boss is driven by a player, the
  one lowest in points at that time (a tie: one of the bottom players at random).

## 11. Online stats and the website

- **Built 2026-10-21:** every machine sends its result (`MASTER_RESULT`: the match's key, the same on every machine;
  each signed-in player's id, car, win, kills, deaths, damage); the master counts it once two machines agree (one
  machine alone, or a mismatch: nothing), per player and car, in `tm2_master.db` on each master (each counts every
  result: each holds the whole). Not counted: modded games (the user's, 2026-10-21; Vanilla Plus counts; no mods exist
  yet) and guests. The website reads the file later.
- (The plan:) At each match's end the host sends the result to the master server: the ruleset (only vanilla rules count: no mods,
  Vanilla Plus as the online default), the arena, mode, and per player (by public key): the car, kills, deaths, the
  damage, win or loss. Each client sends its own copy; results that disagree are dropped (a host can't invent them).
- Stored per player and per car (so an overpowered car shows: wins, kills and deaths per car across all players).
- The website reads the same database: global totals, per car, per player (by name), top players. The website is a
  separate project when we get there.

## 12. What is built when

| Step | What | Needs the game? |
|---|---|---|
| A | `src/net/`: sockets, connections (reliable + unreliable channels), the protocol's messages, the rollback engine with its own time sync and desync checks, and `tools/net_sim`: many simulated players over a fake network (delay, jitter, loss) running a tiny test game, proving rollback, desync detection and recovery, host migration and joining late without the game. **Built** (PR #298), then voice (Opus) and the server's messages (chat, the playlist vote, vote kick): `net_sim` runs 10 scenarios | no |
| B | Decomp request 44 (what the game needs for rollback: step boundary, state, determinism) | no (the decomp chat) |
| C | The master server (server list, hole punching, relay, friends, stats) as a small program run on this PC first | no |
| D | The game hooked to the engine: inputs per slot from the network, save/load state, the determinism fixes (`HARDENING.md` 3-4), the lobby's messages; LAN first (two copies on one PC, then two PCs). **Lockstep milestone built** (2026-10-19, `src/net/game_tm2.c`): `TM2PC_NET=host:PORT` / `join:HOST:PORT`, each step waits for both machines' inputs (the kill cam's input seam, `killcam_input`), the cars' hash compared every 8th step; two copies on this PC over UDP, a 2-player Challenge Match, player 1 on the host and player 2 on the joiner: 1,338 steps the same on both, 0 desyncs in 150 checks. **The joiner starts from the host's state** (RAM, the port's gameplay blocks, VRAM and the texture banks, sound RAM: 50.9 MB, 3.6 MB packed, at the battle's first step), so its own menus need not match: with another seed (other opponents chosen) the battle ran the same, 1,554 steps and the same screenshots (`tools/test/net_lockstep.py --join TM2PC_SEED=99`). **The lobby** (`src/net/lobby.c`, Network Play's menus, 2026-10-19): host, join by address, players, cars, teams, ready, chat, kick, the host's Start into the same match on every machine and back to the lobby after it (two copies through the menus: equal battles, 0 desyncs). Next: more players (the decomp's local-player map), then rollback with decomp request 48 | yes |
| E | Voice (Opus), the playlist vote, kicks, mute, friends, co-op modes, host migration, stats | partly |
| F | Versus Tournament | yes |

Decisions for the user (not needed for A-C): where the master server runs (a small rented server, about $5 a month,
and a domain name for the website later), and whether to use Opus for voice (a download).
