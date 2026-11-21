# First-time setup: the game's pictures from the player's own discs

The port's menu pictures (`ui/`, about 75 MB) are made from the game's art, so no release can carry them. Today
`tools/art/ui_assets.py` and its helpers (about 3,200 lines of Python) make them from the PS1 disc image and the 1997
PC version's files. The user's decision (2026-10-21): the game makes them itself, in C, the first time it starts.

## What the player points the game to

- **The PS1 disc: `.cue` + `.bin`** (the game needs it already). Not `.iso`: the PS1 disc's movies and music are Mode 2
  (XA) sectors and CD audio tracks, which an ISO's 2048-byte data sectors cannot hold. (`.chd` could come later.)
- **The PC version: its disc as `.iso`, or its installed folder** (the one with `TM2.EXE`). The PC disc is data only, so
  an ISO holds everything used here.

## The flow

1. At start, before the game's window (as the updater): when `ui/` is missing or older than the game's builder, a
   "First-time setup" window asks for the two, each with a Browse button (the PS1 one filled in when the ini has it).
   It recommends both: the PC version's pictures are twice as sharp. Under the PS1 box: "Extract the .cue and all its
   .bin files into one folder first, then choose the .cue." (2026-11-20). Build checks that every `FILE` in the `.cue`
   is beside it ("<name> is missing: extract it beside the .cue"), and says what to do for a `.bin` without its `.cue`
   or a `.zip` / `.7z` / `.rar` still packed.
2. **Build**: a progress bar while it reads both, cleans the menu screens and writes `ui/`. The paths go into
   `tm2pc.ini`; the game starts.
3. **PS1 only** (the user's): the pictures from the PS1 disc alone, at its resolution, with a note that the PC version
   makes them sharper; Options then offers "Game discs: add the PC version", which opens the same window again.

Nothing made from either disc leaves the player's PC.

## Where each picture comes from

| From | Pictures |
|---|---|
| PC version | title and Sweet Tooth's hair, the menu backgrounds (options, audio, glass, single), car select, car info, the arenas' pictures and cards, the line-ups, the network background, the portraits |
| PS1 disc | the highlight bar, the in-battle font, the opponents' plates, the HUD's weapon icons, the Santa car, the movie portraits (from the PS1's movies, by the game's own MDEC decoder) |
| Shipped (ours to give) | the Challenge modes' icons (game-icons.net, CC BY 3.0, with their credit) |
| Made by the game | the Customize HUD previews (a battle frame the game captures itself the first time) |
| Changed | the Movies menu's Credits picture: the 1996 advertising art is Sony's, on neither disc, so not ours to ship; instead a crop of the credits' own background (SHELLDB2/CREDIT0N), from the player's disc; plain "CREDITS" without it |

## The PC version's sharper art (audit 2026-10-21)

Every TIM / TMS / TPC on the PS1 disc against the PC version's, by name and image index (PC larger in pixels):

| Area | Sharper on the PC |
|---|---|
| Arenas (`LEVELDB/*.TPC` vs `TMS/*.TMS`) | about half of every arena's textures: 1,411 images in 11 arenas; `HONGKONG.TPC` (388) has no PS1 twin by that name |
| Cars (`CARSDB/*.TPC`) | 63 textures over the 15 cars |
| Menus and screens | title, car select, level select, the line-ups (SHOWOPS), options, audio, history, world map, the 14 car info screens, the 7 credits pictures, the studio logos, CARCARDS (PC only) |

The menu pictures already use the PC's. **In a battle the game still draws every car and arena texture from the PS1
disc.** The renderer can already draw a sharper picture over a texture in VRAM (PSY-Z patch 10, used today for the
drivers' names on car select), so the PC's textures can be matched to the PS1's as the game loads them.

## Order of work

1. Done: the base in C (`src/assets/discfs.c`, `tim.c`): the PC disc's ISO 9660 (or a folder), TIM / TPC / PCX.
2. Done: the builder (`src/assets/uibuild.c`, about 2 s) and the setup window (`src/runtime/setup.c`).
   - Both discs: every picture byte for byte the Python's (`tools/test/ui_build_check.py`), except the movie stills (the
     game's own MDEC decoder, not ffmpeg's: each within an average of about 1 per color) and the new Credits picture.
   - PS1 only: the PC steps run on the PS1 disc (its 320x240 screens doubled, its packs' pictures mapped to the PC's by
     table, the glass voted from its menu screens, the rule above its button legends painted out). Left out, needing
     the PC version: the network background, the Antarctica Santa (online: the host's comes with its state), the arena
     name cards. The co-op duel screen: from the PS1's own COWIN, its new lines set from hand-picked pieces of its lines.
   - `ui/setup.txt` (`version N`, `pc 0|1`): a newer builder (`UI_BUILD_VERSION`) makes the pictures again from the
     discs in `tm2pc.ini` (`disc`, `pc_version`) without a question. A `ui/` made by the Python tools is kept.
   - Options, Game discs: the game restarts with `--setup` into the window (to add the PC version later).
   - Tests: `TM2PC_SETUP_PS1`, `TM2PC_SETUP_PC` (`-`: none), `TM2PC_SETUP_AUTO=1`, `TM2PC_SETUP_SHOT=file.bmp`.
   - Online, Santa (PC files) and PS1-only players: no desync on any path, since every machine plays from the host's
     state (its RAM, VRAM and sound RAM) at a match's start and at a drop-in or rejoin. Tested 2026-10-21 on Antarctica
     (`TM2PC_TEST_NO_SANTA=1` on players 2 and 3): the start, a drop-in mid-match, and the host (the only one with
     Santa) leaving mid-match: 0 desyncs, the battles equal frame for frame.
3. Done: the Customize HUD previews are painted by the menu (src/ui/options.c draw_hud_preview: the HUD's parts and the
   TM:Black and 2012 styles, on a plain grey view, the user's choice): nothing captured, nothing made at setup.
4. Done (2026-10-21): the PC's sharper arena textures in battles. Matched by content (tools/asset_check/hd_audit.c:
   the packs' orders differ, so the first audit's "1,411 by name and index" was wrong): most PC arena textures are
   the PS1's at the same size; 108 are the same picture at twice the size, checked by eye with the user
   (tools/art/hd_picks.txt; the PC's changed effects and details left out). Cars: none sharper. The setup writes
   `ui/hd_arena.bin` (each PS1 texture's CRC-32, the PC picture); `src/vplus/hd_textures.c` finds them as the arena's
   pack is parsed, checks VRAM holds them, and draws their colors from the PC picture (PSY-Z patch 19: 208 slots,
   colors-only: see-through, semi-transparency and edges stay the PS1's). Written over later (an animation, a
   state): checked again, the PS1's if changed. Video, Textures: Sharpest or PlayStation. Display only (a battle
   the same either way: tested on Hong Kong, 39 textures, and Paris, 31).
