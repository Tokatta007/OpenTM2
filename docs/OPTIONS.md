# Options pages

The port's own menus, in the style of the game's: a title at the top left, white
labels with option values in orange (as the game's ON/OFF and EASY/MEDIUM/HARD), the
copper highlight bar behind the chosen label, a description of the chosen row, and the
button legend box at the bottom. Code: `src/ui` (`options.c` pages, `draw.c` pictures,
`text.c` font).

They **replace the game's Options screen**: choosing Options on the title menu opens
them, and backing out returns to the title menu (`80133C08` is wrapped and returns the
next screen, as the game's shell screens do; runs without live input, such as headless
tests and tapes, keep the original screen). As in the game's own submenus, the title
menu's select sound and the menu music loop start as the pages open (two VBlanks are run
first) and the CD music is quiet until the title menu starts it again; a new CD start
drops the audio PSY-Z had read ahead and fades in (`psyz-06-host-audio.patch`), so
nothing of the old position is heard. **F1** also opens them anywhere, for
example during a match (on a gamepad: the touchpad or guide button); O backs out of them,
there is no key that closes them at once. The game is stopped while
they are open. X (or Enter) selects or changes a value, Left/Right change a value, O (or
Backspace) goes back; O on the first page closes. Changes are saved to `tm2pc.ini` when
the pages close: only the sections that changed are rewritten, the rest of the file is
kept as it is.

## Pages

| Page | Contents | Saved in |
|---|---|---|
| Options | Profile (from the title menu; the signed-in name at the right), Gameplay, Video, Audio, Controls, Vanilla Plus, Movies (from the title menu), Mods (Mod Cars in Random: on, the default, lets Random computer drivers be the mod characters that are on in Challenge Match, Custom Tournament and Survival, offline; `[mods] random_characters`; then each mod's switch by kind) | |
| Profiles | Every profile (the signed-in one marked), New profile; the chosen profile's picture at the right. X signs in, Square opens its page | `[profile] name` (the one signed in) |
| A profile's page | Done (back to the profiles), Portrait (a grid of every driver's three portraits), Rename, Delete (hold X for 3.5 seconds) | `profiles/<name>.ini` |
| Gameplay | Difficulty (Easy, Medium, Hard; locked when opened with F1: change it from the title menu), World map (on, off: the globe between tournament levels) | `[gameplay] difficulty, world_map` |
| Video | Resolution (auto, 1x-8x; each shown with its lines, 3X (720P), Auto with what it is now), Dithering (auto, on, off), Window (window, borderless, fullscreen; F4 switches, `psyz-11-window-mode.patch`), Display Mode (shown with Fullscreen only: Desktop, or one of the display's modes, resolution and refresh rate; `display_mode = desktop` or `2560x1440@144`; `psyz-14-fullscreen-mode.patch`), Widescreen (on, off), Wobble (ps1, smooth, none; `psyz-12-precise-vertices.patch`), Hidden surfaces (depth, the default: a depth buffer, what is in front hides what is behind it; ps1: drawn back to front; depth needs wobble none; `psyz-20-depth-buffer.patch`, RENDER_PLAN.md), Menu sides (extension, blur, black), Renderer (Direct3D 12, the default, or Vulkan; `renderer = direct3d12 / vulkan`, at the next start; no Vulkan on the PC: Direct3D 12; Wine/Proton: Vulkan either way; `SDL_GPU_DRIVER` in the environment still decides), Draw distance (Original 4,800 units as the PS1; Far 16,000 and Farthest 32,000 through the decomp's far distance, `portFarDistance`, the sharp model bands kept 2x / 4x farther, the mirror as far; in a battle F3 shows the renderer's capacity: matrix slots, meshes, packets, now and at most, `vplus/render_stats.c`, `TM2PC_RENDER_STATS=1` logs it), Split screen (2-3 players: side by side, the default, or top and bottom; no longer "by arena". Select in a
battle switches it and the choice is saved, `[video] split`), Customize HUD | `[video]` |
| Video › Customize HUD | Each player's (kept in their profile): Profile, Preview, Style, Layout, Size, Turbo, Speedometer, Lives, Rear-view mirror, Radar & health raised, Radar, Energy bar, Turbo shown as, Target, Messages, Message text; Enemy markers (a small triangle over each other car in its radar color, or its team's, red while your missiles are locked on it), Marker health (a thin green/yellow/red bar over it), Marker names (Players: other players' profile names; All cars: the computer cars' too, by car; Off), Lock-on reticle (TM2012's four red arcs round the car your special, fire or homing missiles would home on). All off by default (optional extras); the reticle shows by itself while a missile fired now would home on a car. The markers and the reticle grow and shrink with the distance (`vplus/world_marks.c`, drawn in `vplus/hud_styles.c`) | `[hud]` in the profile, `[video] hud_*` for guests (`hud_enemy_markers`, `hud_marker_health`, `hud_marker_names = players/all/off`, `hud_lock_on`) |
| Audio | Music and Sound effects volume, 0-100% in steps of 10, as green level bars like the game's Audio screen; Menu sounds (on, off) | `[audio] music, effects, menu_sounds` |
| Audio › Custom Music | Each arena and the menus, in two columns: the playback (Square cycles Default, Random, In order, Song) and, for Song, the song (Left/Right: the music folder's files). The playback sits just past the track names and the song beside it, as long as the window allows: on a wide screen past the page into the sides, cut short (…) only at the window's edge | `[music]` |
| Controls | Controllers (below); Player: whose controls the page shows (1 or 2); Controller type (Auto: from the connected controller, PlayStation, Xbox: the button names and colors shown); Layout (the game's four: Standard, Run 'N Gun, Control Freak, Deadly Force; a new one resets that player's controller buttons; shown as Custom once a button differs from the layout's. The keyboard keys are separate: the layouts don't touch them); then, in two columns, each of the 12 driving actions' controller button and keyboard key, and Next / Previous song (a key, Page Down / Page Up, and optionally a button on player 1's controller, none by default so a song never changes by accident; gray for player 2). X, then press a button on a controller or a key; or Left/Right for the button | `[controls] keypad1, player1_type, p1_accelerate, p1_accelerate_key, next_song, next_song_key ...` |
| Controls › Controllers | Player 1 and Player 2: one of the connected controllers, or none; below, every connected controller, its dot lighting up while any of its buttons is pressed, so you can tell which is which | `[controls] player1_controller, player2_controller` |
| Vanilla Plus | Mode (Vanilla Plus: all on, Original: all off; shows Custom when mixed), then every change of `src/vplus` under its category, each ON/OFF; changes decided but not made yet are listed gray with SOON (`planned_changes` in `options.c`) | `[vanilla_plus]` |

**The game's own options** (Difficulty, Map, Keypad 1, Keypad 2) live in its settings
block at `8016475C` (+36 difficulty 0-2, +37 map 1 = on, +38/+39 keypad layout 0-3; the
order checked on the game's own Options screen). The game's Map option is the world globe
between tournament levels (`sub_801334E8` skips it when 0), not the radar: the decomp's
`OPTIONS_CONTROLS.md`, and checked (the radar shows either way). The saved values are written into it
each time the title screen (`8012DF84`) starts, and at once when changed on these pages.
As in the original, Easy ends the tournament after level 4 (the STOP screen); a password
for level 5 or later entered on Easy plays on Medium. Passwords carry no difficulty: a
tournament continued by password plays at the current setting. Tournament saves in the
profile (planned) will keep their difficulty, and it will not be changeable for them.

**Volumes** are the port's, not the game's Audio screen levels: music scales the CD
audio and music files (`cd_audio.c`; movies keep theirs) and the SPU voices playing the
menus' music loop (sample `EF4C`, the submenus' music); sound effects scale every other
SPU voice (PSY-Z's per-voice gain, `psyz-06-host-audio.patch`), checked each VBlank. Both
use the square of the setting, so the steps sound even.

**Descriptions** stay short: one plain line, or none where the label says it all
(Mode, the keypads); they sit just above the legend (two lines at most), so the list
gets the room above.

**Menu sounds**: the pages play the game's own sounds, keyed on the SPU directly with
the settings the game's menus use: the cursor sound (sample `1A70`) when moving or
changing a value, the explosion (`1264`) with a copy one octave lower for select, back and
close.

**Layout**: the legend box frames the page. Titles, labels and the description start at
its inner left edge, values end at its inner right edge, whatever the background. The
legend shows X = SELECT and O = BACK, or the Left/Right arrows = CHANGE on a value.
Long lists (the Vanilla Plus changes) use smaller rows, at the headers' size, and scroll;
copper arrows at the right show when there are more rows above or below. The background is drawn once
per page and window size and reused, so a change on the page redraws in a few
milliseconds.

**Lettering**: Averia Serif Libre Bold drawn 2% heavier, 12% wider and 4.5% more widely
spaced, which brings it closer to the game's labels at the same capital height;
descriptions use the font as it is. `--ui-shot 99,...` draws a sample.

**Movies** (from the title menu): a grid of portraits, each with its name below: the Intro (Calypso with
the globe), every driver's ending (in car select's order, their first portrait; dimmed until earned,
with how to earn it; a secret driver's a "?" card and "???"), and the Credits (Sweet Tooth's head,
cut out of the cover art on black: `ui/credits_portrait.bmp`). The intro and the
endings play through the game's own movie routine (`8012B898`, with the winner movies' table
`801648C4`) and come back to the list; the credits are the game's credits screen (`80136A3C`).

**Buttons** (`src/controls/bindings.c`): the game turns a layout into action bindings
at race start (`sub_801206F4`, the 43-row table `D_80162F64`; the decomp's
`OPTIONS_CONTROLS.md`). That function is wrapped: after the layout, each action the player
gave another button is bound again (`sub_80150F70(action, button, port)`) and the bindings
made active (`sub_80150FC0(port)`). The actions are the twelve the car reads
(`car_input.c`): accelerate 0x18, brake 0x19, steer left 0x17, steer right 0x16, forward
0x1A, reverse 0x1B, tight turn 0x1C, turbo 0x1D, machine gun 0x1E (R2 in Standard), fire weapon 0x1F (the special
weapon, L2),
previous weapon 0x21 (up the TM2 list; left on TM:Black and TM:2012), next weapon 0x20 (down it; right). The game's
own names for 0x21 / 0x20 are "cycle next" / "cycle previous" and its layouts put 0x21 on R1; the port's defaults trade
the two buttons (Standard: L1 previous, R1 next; Run 'N Gun: Circle previous, Triangle next), bound after the game's
layout every time. Buttons are the PlayStation's: gamepads map
onto them by position (`input.c`). Keyboard keys belong to actions instead: while a key is
held, its action's button is pressed, so a key keeps its action when the action gets
another button; one key serves one action. Face buttons are shown as the PlayStation's
symbols (drawn: green triangle, red circle, blue cross, pink square), also in the legend,
or for an Xbox controller as its letters in their colors (A green, B red, X blue, Y
yellow) and LB, RB, LT, RT, VIEW, MENU, LS, RS.

**What reaches the game** (`src/vplus/humans.c`): every human plays with one button table, the Standard layout's
alone, on every machine. A player's own layout, buttons and keys are used only on their own PC, to turn the buttons
they hold into the ones Standard has for the same actions (the actions the car reads only). Online, then, no player's
table travels and none can differ between machines, and a change in Controls applies from the next step, mid-battle and
online too. Because of that an action may also have L3, R3 or a direction of the right stick (RS UP, RS DOWN, RS LEFT,
RS RIGHT: flick the stick when Controls asks for a button) or of the left stick (LS UP ... LS RIGHT). A left stick
direction with nothing of its own is the D-pad's, as always; given an action, an energy attack or an extra control, it is
that in a battle and no longer the D-pad's (in the menus the stick still moves through them).

**Analog steering and Analog throttle** follow whatever steering, accelerate, brake, Forward and Reverse are on: a
stick direction's tilt or a trigger's press; a button or a key is full. The D-pad's Left and Right (and the left stick
while it is the D-pad's) steer by the left stick's tilt as before; Forward / Reverse on the D-pad are always full. Each
PC sends the amounts (0..4096), not the buttons, so any binding is the same number on every machine online.

The controller's own Select, L1 and R1 go with the input
separately: L1 + R1's jump and Select's buttons are those, whatever actions they have.

**Jump on L1 + R1** has **Jump-Weapon Switch Safety** under it (gray while the jump is off): Off, 50, 100 or 150 ms
(`jump_safety`), how long a lone L1 or R1 waits before switching weapons; the other shoulder in time makes it a jump and
nothing switches, and a tap let go sooner switches then. The game switches the moment one shoulder goes down, so without
it a jump's first shoulder switched the weapon. **Energy attacks** has a **Jump** row too (none by default).

**Quit game** (with the song keys): Escape by default, and no controller button unless one is set; it opens "QUIT
OPENTM2?", which takes the keyboard (Y / N, the arrows, Enter) or any controller (the D-pad, Cross picks, Circle is
No); while it is open the game gets no keys or buttons.

**Custom music** (`music.c`): In order plays the folder's files in name order, one per
track start, with one position shared by every track set to it, so songs follow on from
arena to arena. Page Down / Page Up play the next or previous song on a track set to
Random or In order, mid-game too: the file is decoded on a thread and swapped in at the
next VBlank, fading in (`cd_audio_skip`); previous goes back through the songs played.

**The pause menu** (Start in a battle) is the port's own, drawn as the game's: the
paused frame darkened by half, BATTLE PAUSED and the items in the game's own in-battle
fonts and colors (`ui/gamefont.bmp`/`.txt`: UAFONT1.TIM and the executable's glyph
tables, drawn by `src/ui/gamefont.c` with each game pixel scaled to the window), at the
game's positions, with OPTIONS added between RESUME and QUIT, the chosen one lighter, and
the tournament's password when the game shows one. It wraps `80129528` and
keeps the game's steps: the music stops and comes back on Resume, the cursor and select
sounds are the game's, Quit shows BATTLE ABORTED and ends the battle, and O resumes.
OPTIONS opens the pages that can change during a battle: Video, Audio (without Custom
Music) and Controls; F1 in a battle opens the same. Runs without live input (tests,
tapes) keep the game's pause; `TM2PC_PAUSE_SCRIPT` drives it in tests as
`TM2PC_UI_SCRIPT` does the other menus.
**Controls** gets rebindable Next and Previous song keys (and gamepad buttons for them); **Controls** gets choosing
the button for each action (decomp topic 4: the binding table), steering sensitivity,
easier special moves and jump on L1+R1; **Mods** (cars, skins, arenas, textures, game
modes). Changes that sit best on another page (for example easier special moves under
Controls) are still Vanilla Plus changes: they are listed there too and follow the Mode.

**Frame rate** (Video): 60 FPS (the default) draws the battle every frame, the cars and the camera
moving smoothly between the game's 20 steps a second (`vplus/interp.c`, RENDER_PLAN.md 5); Original
shows each step three times. The game plays the same either way.

**Local Play** opens the sign-in first, as Network Play's: 1 to 4 players (players 2-4 press Start
to join), then the 1 Player menu (one) or the 2/3/4 Players menu. O on that menu goes back to the
sign-in, O there to the title. (Tests: `TM2PC_SIGNIN_ONE` keeps players 2-4 out for the 1-player
scenarios.)

**Who is signed in:** the 1 Player and 2/3/4 Players menus show, at the top right, each
player's portrait, P1-P4 and their profile's name (a guest: GUEST and a "?" card); one column for
1-2 players, two for 3-4; a long name runs on to the screen's edge (a wide screen: further), cut only
there or before the next column (`draw_signed_in`; scenarios `menu2`, `menu4`).

## Profiles

The profile screen comes first, before the title menu, on the title picture as the
intro movie leaves it (darkened under the text, the hair burning; its song plays): the
profiles in `profiles/` next to the executable, the one signed in last chosen, and New
profile. X signs in (and the controller that pressed it becomes player 1's: the game
reads only player 1's controller, these menus read every one) and goes on to the title menu; Square opens the profile's page; O
does nothing here (someone signs in first). O on the title menu comes back here. From
the title menu's Options it is Options > Profile (on the glass), where X switches
profile and O goes back.

A profile's page: Done (back to the profiles; a new profile comes here to choose its
portrait), Portrait, Rename and Delete. Delete asks first: the page dims, and X held
for 3.5 seconds fills a bar and deletes (letting go starts the bar over; O goes back).

A profile keeps player 1's controls (layout, buttons, keys, song keys), its picture
and its progress (docs/PROFILES.md). The Controls page shows whose they are: the
profile for player 1, "Guest (this PC)" for player 2, whose controls stay in
`tm2pc.ini` until players 2-4 get profiles (in the Local and Network Play lobby).

**Stats** (Options, after Movies; from the title): a
profile's counts, view only. Profile (Left/Right: any profile's, the signed-in one first); Play: Local, Online or
All (the two added; Survival's best round the higher); then matches played and won, Survival's best round,
tournaments started and won, time in matches, kills, deaths, falls, freezes, pedestrians, kills by each weapon,
specials and rams, and pickups. They are kept in the profile's `[stats]` section (`local_<count>` and
`online_<count>`, with their own check; edited counts are cleared), counted by `src/profile/feats.c` with the
feats: `stat_add` counts into the set of the play it is (`stats_online`: online play, when it comes). A profile from
before the stats starts its local set from its feats' counters. Test: the `stats` scenario (a match won by player 1).

**Names** (up to 16 letters, digits, spaces, `-`, `.`, `_`) are entered on a letter
grid with the controller (X types, O or Square erases, O on an empty name goes back,
Start or DONE takes it, SHIFT switches capitals: the first letter is a capital, then
small letters, as on a phone) or typed on the keyboard (Shift for a capital,
Backspace, Enter, Escape to go back). The keyboard's menu keys (X, C, Z) are letters
there.

**Portraits**: three of each driver: the two car-select portraits (`ui/portraits.bmp`)
and the art of the driver's info screen (`ui/portraits_hq.bmp`, much sharper). Portrait
opens a grid, a column per driver in car select's order (Roadkill first; Sweet Tooth and Minion after
Shadow, or last with Secret cars last) and a row per portrait; X takes an unlocked one (a
the one in use is framed when the grid opens); a locked one is dark, with how it is earned below
(nothing is written for unlocked ones). The first of each is free while the driver can be
played; the second and third are, for now, earned with the driver's ending (how is still
to be decided). Sweet Tooth and Minion stay secret until unlocked: "???" and a black
card. A new profile starts with a free one, at random.

Below the drivers' rows come stills from the movies (`ui/movie_portraits.bmp`), each
driver's ending's after the other in the same order, then Calypso's, then other people's ("OTHER - n"); the
grid shows three rows and scrolls (arrows at its right). Names: "OUTLAW 2 - PORTRAIT 2" for a driver's three, "GRASSHOPPER - ENDING 4",
"CALYPSO - INTRO 2" for a still (counted among that person's of that kind). Only the drivers' first portraits are free. Locked stills of an ending show as a black "?" card (the ending stays a surprise); the intro's are grayed, their goal still to come; an ending's
come with that ending (Sweet Tooth's and Minion's stay "???" until the driver is
unlocked). The picks are in `tools/art/movie_portraits.json` (movie, frame, crop x, who).

## Title screen

The port draws the title too (`ui_TitleScreen`, wrapping `8012DF84`; runs without live
input keep the game's): the PC version's title picture (`ui/title.bmp`) with LOCAL PLAY,
NETWORK PLAY and OPTIONS in the menu lettering, the copper bar behind the chosen one,
which rises a little as the game's items bob. For now they lead where 1 Player, 2 Player
and Options did (`8012E908`, `8012F5F8`, the Options pages). Around it the game's own
steps are kept: its music starts the same way (the music object's slot 2, CD volume),
picking an item plays the select explosion, starts the submenus' music loop (sound 27) and
stops the music.
Sweet Tooth's hair burns as on the PC title: its 19 frames (`ui/hair.bmp`) cycle over
the picture at 30 a second, placed over the hair painted into it.

Left alone, the game shows the attract demo after 2020 VBlanks (about 34 seconds). Here
the title waits for its song to end instead: if nothing was pressed while it played, the
demo starts as the song ends (about 58 seconds for the disc's title track; as long as a
custom title song); a press gives the song another play. Without music: 60 seconds. The
sides of a wide screen take the title picture's own near-black.

## Wide screens

Everything is laid out on the game's 640x480 screen, scaled to the window height and
centered. `[video] menu_sides` sets what fills the sides on screens wider than 4:3:

- `extension` (default): the shattered glass continued past the edges (the glass
  picture mirrored outwards, at full brightness),
- `blur`: a blurred, darkened copy of the page's background,
- `black`: black bars.

These pages use it now; the game's own menus are to follow.

## Pictures and font

Made from the game's art on your machine and placed next to the executable (keep them
out of the repository):

    py tools/art/ui_assets.py --out build/port/ui

| File | From |
|---|---|
| `ui/options.bmp` | the Options screen (burning car) without its title and legend (`clean_menus.py`) |
| `ui/glass.bmp` | the PC version's GLASS2: the shattered glass with nothing on it |
| `ui/audio.bmp` | the PS1 Audio screen (speakers and flame in the middle) without its title and legend: the Audio page's background |
| `ui/portraits.bmp` | profile portraits: each driver's two car-select portraits (CARSEL.TPC, 64x128), in car-select order |
| `ui/movie_portraits.bmp`, `.txt` | portraits from the movies: stills cropped 120x240 (`movie_portraits.py`, from the picks in `tools/art/movie_portraits.json`; needs ffmpeg, which reads the disc's STR movies; skipped without it) |
| `ui/portraits_hq.bmp` | each driver's third portrait: 240x480 of the art on the driver's info screen (BIOS/<xx>INFO.TIM) |
| `ui/title.bmp`, `ui/hair.bmp` | the PC title picture and the 19 frames of Sweet Tooth's burning hair (TITLE.TPC) |
| `ui/network.bmp` | the Network background: the PC network screen with only its glass, flame and cable (`net_background.py`, [MENU_ART.md](MENU_ART.md)); for the Network Play screens to come |
| `ui/bar.bmp` | the PS1 highlight bar (OPTIONS.TMS): the copper bands of the longest bar, its inside made the bar's own dark so it stretches to any label |

The Options and Audio pictures show the same shattered glass as GLASS2, pixel for pixel,
but with lifted blacks; they are taken to GLASS2's levels (a tone curve per channel
fitted on the clean plate; on the artwork only the black level), so every page, and the
glass continuing past its edges, has the same brightness.

Without them the pages still work, on black with a plain drawn bar.

The font is **Averia Serif Libre Bold** (SIL Open Font License, `assets/fonts`, copied
next to the executable by the build), free and close to Flexure, the game's own menu
typeface (licensing a commercial font for a game is not straightforward). It is drawn with stb_truetype (`third_party/stb`, public domain) at the
window's resolution, with a black outline and drop shadow like the game's labels.

## How it is shown

The page is composed on the CPU at the window's size and shown through PSY-Z's 24-bit
display (`Psyz_VideoSetDisplayRGB24`); `psyz-05-display-fill.patch` lets that picture
fill the window at its own aspect ratio instead of 4:3. It is redrawn only when something
changes.

## Testing

`--ui-shot page,row,WxH,file.bmp` draws one page to a BMP and exits (page 0 Options,
1 Gameplay, 2 Video, 3 Audio, 4 Controls, 5 Vanilla Plus, 6 Custom Music, 7 Controllers,
8 Profiles, 9 the signed-in profile's page, 94 the profiles at startup (on the title picture),
95 deleting, 96 the portrait grid (row: the portrait chosen), 97 entering a name, 98 the
title, 99 a font sample), with the settings file's values,
as opened from the title menu:

    build/port/tm2pc.exe --headless --settings tm2pc.ini --ui-shot 5,2,2560x1080,vplus.bmp

`TM2PC_UI_SCRIPT="f1,x,down,right,o,o"` presses those buttons (f1 x o start select up
down left right; "-" waits), from VBlank 600, or as soon as the title menu's Options
opens, instead of reading the devices, in a windowed run. With `--tape` driving the game
to the title menu's Options, it tests the replaced screen end to end.
