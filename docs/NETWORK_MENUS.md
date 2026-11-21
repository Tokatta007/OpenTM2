# Network Play: the menus

The title's Network Play opens these pages (ui/options.c, "Network Play's menus"). **Hosting and joining are real**
(2026-10-19, `src/net/lobby.c`): Create server opens the server on UDP port 27520 (`TM2PC_NET_PORT`), **Join by
address** (Join Server's page: an IP, and `:port`) joins it; the lobby shows every machine's players (each one of the server's room: a machine whose players do not fit
is refused), their cars,
teams and ready marks (a joiner's Start: ready), chat, kick and the vote to kick, leaving. The host's **Start**
starts the match on every machine (2 players for now, one a machine: each machine's own player on its pad 1; a
player without a car gets one at random from the host; the match's random picks the same everywhere, the match's
seed); the joiners start from the host's state, then lockstep. After the results: every machine back in the server's
lobby. **The host leaving** (2026-10-20): the machine there longest carries on as the host (a chat line in the lobby; in a
match the game holds with "HOST DROPPED", then "NEW HOST: NAME"); a player leaving mid-match: their car is out at the same
step on every machine; a machine joining while a match is on plays the next one (ONLINE_PLAN.md 9). lobby. **LAN's server browser is real** (2026-10-19): the browser asks the network (a broadcast on UDP 27521), each hosting
machine answers with its listing (name, host, game, arena, players, ping); X joins (another build: refused, an
update). A player's team: chosen in car select (a joiner's car select under the host's game, its teams). **The playlist** (2026-10-19): up
to 16 whole matches (Add a match: a copy of the server's game; each one edited on the Challenge Match pages: its
game type, arena, opponents, teams, rules), one marked first. Start plays the first; after each match the vote,
over the match's end (its last frame, dimmed, who won at the top), never back to the lobby between: 3 of the others
at random (never the one just played) as cards (the arena's picture, the game type, its mode and rules), every player
one vote (a machine's players each their own: P1-P4 marks), the votes counted as they come, 15 s or less (everyone
voted: at once); the winner framed green, the others greyed, 1.5 s later the match on every machine (a tie, or no
votes: one of the highest at random). Without the vote: the next in order. No Rematch online: without a playlist, the lobby again. Still to
come: the online server browser (the master server), more players (the local-player map: each machine
shows only its own players), Rematch and the playlist. `TM2PC_TEST_SERVERS`
fills the browser with sample servers for tests; `TM2PC_NET_LOCAL=1` keeps a test on 127.0.0.1.

## The flow

1. **Sign in:** as Local Play's Multiplayer, 1 to 4 players (players 2-4 press Start to join; everyone ready, player 1
   Start). Each signed-in player plays online from this PC.
2. **Online / LAN.**
3. **Join Server: the server browser**, a table (`draw_server_browser`).
   - Columns: server, arena, game (its game type), status (in game / lobby), players, ping, mods (yes / no; locked
     servers marked). Nine rows a page, "PAGE 1 OF N".
   - Under it the highlighted server's details: its arena's card, status, game mode, game type, arena, players, host
     and ping, and last its mods (NONE, or MODS (N): each one on a line); beside them who is there, in two columns with
     their portraits (a team game: in their team's color). The right stick scrolls both.
   - Full servers' players and pings of 150 ms or more in gold; sorted by players (or ping, name).
   - The totals at the top: servers and players online.
   - X joins, Square refreshes, Triangle the filters (sort, hide full servers, hide private servers, max ping, game
     mode, mods; compact rows).
4. **Host Server** (compact rows).
   - Game: **Co-op Tournament**, **Survival** (4 players together, the game's own computer cars), **Challenge Match**,
     **Versus Tournament**, Left and Right. **Single Match / Playlist** (Challenge Match only, else grayed out): a
     playlist is Challenge Matches one after another, a vote for the next game (3 of the list at random), up to 16
     entries (a game type, built in or the host's own, and an arena). **Game settings** opens the chosen one's page
     (the Challenge Match pages or the playlist, the tournament's difficulty, Survival's arena; Done or O comes back;
     a playlist match's pages back to the playlist).
   - Server: name (the name grid), players (2-16; the co-op games: 2-4), Public / Private (private: shown in the
     browser, joining needs the password; online only), voice chat.
   - Computer cars (grayed out for the co-op games): fixed CPU cars (CPU: always in, each one a player's spot less, so
     4 of them make 12 players at most), fill CPU cars (a number, NONE-15, default 15: that many of the empty spots
     CPU / OPEN, the rest OPEN; a player joining takes one's place); teams
     (players choose / auto-balance; Challenge Match only).
   - **Create server** (once on the server: **Back to server**).
5. **The lobby** (`draw_lobby_screen`; wide screens: the panels stretched to the sides, up to 16:9).
   - The game (top left): PLAYLIST or SINGLE MATCH, the arena's card, the game mode and type, arena, teams, players,
     fixed computer cars, and every rule that differs from the game's (two columns). The panel is as tall as that.
   - The chat (under it): the newest at the bottom, the right stick scrolls back. Left from the players' first column
     highlights it, X (or Square anywhere) opens the keyboard over the players (the chat and the game still in view):
     the name grid, or type on a PC keyboard (Enter or Start sends).
   - The players (right): 16 cells in two columns of 8, by team (each team's players counted at the top), in each
     team (or for everyone): the players, CPU / OPEN, OPEN, CPU (fixed), then CLOSED (beyond the players' room; the
     co-op games: 4 slots, the rest closed). This PC's players: P1-P4 in gold under the name. A player's
     cell: the portrait, the name in the team's color, the host and a microphone (talking; muted: struck through)
     under the name, the car's plate on the right ("?": random). Solid once the player has chosen a car.
   - A joined server: everyone there (a sample's list for now), in their teams' colors (the teams it uses: Holland
     Hunt's green and yellow), this PC's players on its smallest teams, the open slots spread over its teams.
   - The arrows move between the cells; X on a player opens their page: you (your team when the players choose, your
     voice chat), another (mute; the host: kick; the others: vote to kick; send a friend request, to come).
   - Triangle: the game's car select, every local player at once (the co-op games: no team colors), then the lobby
     with the cars in place. The menus' music: one copy (car select starts its own).
   - Local Play's lobby is the same screen without the chat: the computer cars with their drivers and cars, X a car's
     next team (team games).
   - The host: Select edits the game (the playlist when it's on), Start starts it (no ready check). O leaves.
6. After a game: back to the lobby (to come with the network).

## To come with the network

- The server list, joining and hosting for real (`docs/HARDENING.md` 3-4: one ruleset and seed from the host).
- The players' cars and the host's settings sent to everyone; Start; back to the server after the game; the playlist
  and its vote.
- Chat and voice; the Versus Tournament; mods listed per server and downloaded (`docs/MODDING.md` 10).

Tests: `net_join` (Online, Join, a sample server, a chat line, a player muted and a vote to kick), `net_host` (LAN,
Host, Challenge Match, Create server, car select, Edit game, Start, your page, Leave); `TM2PC_NET_SCRIPT` drives these
pages. Lobby pictures: `--ui-shot 46,...` with `TM2PC_TEST_SERVERS=1` and `TM2PC_LOBBY_HOST`, `_TEAMS`, `_FIXED`,
`_MAX`, `_BOTS`, `_READY`, `_TYPING` (the keyboard open with that text), `_JOIN` (the sample server joined: 10 Hong Kong
Showdown, 8 Holland Hunt).
