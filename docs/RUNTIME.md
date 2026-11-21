# tm2pc runtime: design and status

`tm2pc` runs the recompiled game (`tools/recomp/recompile.py`, generated at
build time from the retail executable) on a PC "machine" in `src/runtime/`.

## Build and run

```sh
cmake -S . -B build/port -G Ninja -DCMAKE_C_COMPILER=<llvm-mingw clang>
cmake --build build/port
build/port/tm2pc.exe "path/to/Twisted Metal 2 (USA).cue" --frames 1200
```

CMake and Ninja come with the Visual Studio Build Tools; clang is currently
OpenPete's bundled llvm-mingw. Initialize `third_party/psyz` and its
`external/SDL` submodule first. Options:
- `--frames N`: stop after N frame boundaries (`SetFrameStart` calls).
- `--tape FILE`: decoded pad words per frame boundary.
- `--tape-vblank FILE`: one decoded pad word per VBlank, for menus that poll the pads between frame boundaries (the pause menu).
- `--preset original|vanilla_plus`, `--settings FILE`: Vanilla Plus changes (docs/VANILLA_PLUS.md). Windowed runs read `tm2pc.ini` next to the executable.
- `--trace`: log CD access and game messages.
- `--frame-log FILE`: VBlanks and game ticks per frame.
- `--headless`: no window and no pacing, using the null GPU backend (for tests and fast runs). The default opens a window with the PSY-Z renderer and runs at 59.83 Hz.
- `--shots 40,100 --shot-dir DIR`: save the displayed frame and all of VRAM as BMP at those frame boundaries.
- Debugging stray writes: a build with `-DCMAKE_C_FLAGS="-DRC_TRACE_ENTRY -DRC_MEM_WATCH"` (e.g. `build/port-watch`,
  the hybrid's settings) reports every write to `TM2PC_WATCH=hexaddr,len`: the value, the native code address
  (`llvm-addr2line`) and the last four game functions entered (retail addresses). `exit()` prints its caller.

## Design

| Layer | Approach | Files |
|---|---|---|
| Game code, libgpu, libgte, libspu, libsnd, libetc internals | Recompiled | `build/port/gen/` |
| Overrides | Host versions of selected functions, listed in `src/overrides.json`. `wrap:` keeps the original callable. | `hle.c` |
| VSync | Replaced. Emulated time advances here, one VBlank at a time. | `hle.c` |
| libcd | Replaced at the API level. Reads files from the disc image; the track table and CD-audio commands are recorded. | `hle.c`, `cd.c` |
| Movies | moviecdPlayMovie replaced: STR demux, MDEC decode (versions 1 and 2), XA audio, shown through the game's PutDispEnv | `movie.c`, `mdec.c` |
| BIOS | A0/B0/C0 functions, events, critical sections, the interrupt hook | `bios.c` |
| Interrupts | `trapIntr` entry and `ReturnFromException` unwinding (details below) | `core.c` |
| Hardware registers | Interrupt controller, DMA (GPU lists, OT clear, SPU), root counters, SPU registers and RAM | `hw.c` |
| GPU | Front end: GP0/GP1 framing, polylines, image-transfer phases, DMA linked lists, an always-ready GPUSTAT. A backend owns VRAM and draws: PSY-Z (`third_party/psyz`, SDL3 GPU with Direct3D 12/Vulkan, in a window), or the null backend (VRAM in memory, no drawing). | `gpu.c`, `gpu_backend.h`, `gpu_psyz.c`, `gpu_null.c` |
| GTE | Register-level, per instruction | `gte.c` |

**How interrupts are delivered.** The recompiled code doesn't count cycles.
Interrupts happen at points where the game waits or looks:
- **VSync:** each VBlank first raises root counter 2 (the game clock, 1.080 per VBlank on average), then the VBlank interrupt.
- **Hardware register reads**, i.e. polling.
- **ExitCriticalSection.**
- **DMA completion:** this raises the DMA interrupt, which drives libgpu's drawing queue.

Each delivery works like the BIOS:
1. Fire root-counter events. `SysClkIntHandler` is the game clock.
2. Acknowledge them, unless `ChangeClearRCnt` turned that off.
3. Enter libetc's `trapIntr` on the stack and `gp` from the `HookEntryInt` jmp_buf.

`ReturnFromException` simply returns, since `trapIntr` ends right after
calling it. (A C `longjmp` there fails on 64-bit Windows with
`0xC00000FF`: SEH unwinding through the recompiled frames.) libsnd's
chained VBlank handler and libgpu's DMA-driven queue therefore run exactly
as on hardware.

## Status (2026-10-03)

Running the Quake Zone combat tape (headless):
- **Boot:** the font, sound bank and logo screens load, the SingleTrac logo and title movies play, and the title screen comes up with CD music.
- **Menus:** 1 Player, car select, then the world map.
- **Gameplay:** Los Angeles loads with all 8 cars, and 1,000 frames of gameplay run with about 1,000 drawing commands per frame.
- **Clean:** zero unexpected runtime events, and 1,200 frames take 0.8 seconds headless.
- **Rendering (windowed, PSY-Z):** the title screen, menus, car select, and gameplay with the full HUD, in real time.

Known differences and open work:
- **Frame pacing:** gameplay matches retail, 3 VBlanks per frame (20 frames per second). The game's frame limiter (`800D1628`) waits with VSync(0) until the time since the frame started is 3 fields: root counter 2 converted to 60 Hz fields and rounded, against a cap of 20 frames per second (`gp+20`) and a base of 60 (`0x801A5184`). Measured in retail DuckStation: about 212,270 counter units per frame, which is 3.00 VBlanks. (An earlier note here said retail mixed 2 and 3 VBlanks; that was a measurement error.) Smoother PC presentation, such as interpolated rendering between the 20 Hz frames, is a separate Phase 2 feature.
- **Rendering gaps:** on car select, the driver portrait overlaps the "SELECT CAR" text, and the health bar looks empty in gameplay. Both are to be investigated. Rendering is at native resolution, with no PGXP or widescreen yet.
- **Controller input:** in a window without `--tape`, keyboard and gamepads drive the pads (`input.c`, SDL3). Every device is presented to the game as a digital pad, the only kind TM2 accepts. Keyboard: the driving keys belong to actions and can be changed on the Controls page (player 1 by default: W accelerate, C brake, A/D steer, Up arrow forward, S reverse, X tight turn, Left Ctrl turbo, Space machine gun, Left Shift fire (special) weapon, Q/E previous/next weapon); a key presses whatever button its action has. Fixed for the menus: the arrow keys (D-pad), Enter Start, Backspace Select, X Cross, C Circle, Z Square.

  Any SDL gamepad (DualShock 4, DualSense, Xbox) maps by button position; the left stick also works as the D-pad. Controllers are picked up when plugged in, even while the game runs. Virtual devices with no product ID are ignored: Logitech G HUB, for example, presents a mouse as an "XInput Controller". `--list-controllers` shows what SDL detects. PSY-Z hotkeys: Escape quits, F4 toggles fullscreen, F6 shows the VRAM debug view. F1 (or a gamepad's touchpad or guide button) opens the Options pages ([OPTIONS.md](OPTIONS.md)). Page Down / Page Up: next / previous song on custom music set to Random or In order.
- **Sound:** SPU registers, sound RAM and SPU DMA go to PSY-Z's register-level SPU (`hw.c`), which SDL plays on its audio thread. Access takes the audio lock. The SPU has 48 voices (`psyz-17-spu-voices.patch`): the PS1's 24 and 24 more, whose registers are at 0x1F802C00..0x1F802DFF, laid out as the SPU's first 0x200 bytes (voice 24 first; key on/off, reverb on and ENDX at the same offsets, bit n for voice 24+n). The game uses them once the decomp's sound library has 48 voices (DECOMP_REQUESTS.md 34). `TM2PC_SPU_EXT_TEST=1` plays each voice n on voice 24+n instead (a headless `--wav` run is byte-identical to one without).
- **CD music:** TM2 plays music with `CdlPlay` at an absolute disc position (its `.DA` files point into audio tracks 2–11). `cd_audio.c` streams those sectors into the SPU's CD input, loops at the end of a track, and handles Pause, Stop, Mute and Demute. With Setmode Report on, the drive sends a position report about every 10 sectors to the CdReadyCallback; `hle.c` delivers one every 8 VBlanks (absolute positions only). The game keeps that position to loop the song and to resume it after the pause menu, which plays from the saved second (it sends no Pause command; it mutes the CD volume). GetlocP (0x11) returns the same position. CdGetToc gives track starts in whole seconds, as libcd (GetTD) does; with frames the game's range check rejected every report and restarted the song. `--wav FILE` records a headless run's audio; `TM2PC_NO_CDDA=1` turns music off to check sound effects alone. `TM2PC_SPU_TRACE=1` logs every SPU key on and key off with the VBlank, the voice and its sample address, and uploads to sound RAM; `=2` also the volumes written to voices 20-23 (the port's menu sounds).
- **PSY-Z patches:** our fixes to PSY-Z are in `third_party/patches/psyz-NN-*.patch`. At each configure CMake resets the submodule's tracked files to the pinned commit and applies the patches in name order, so `third_party/psyz` shows as modified; change PSY-Z only through new patch files. `psyz-01-spu-voice-end.patch`: a voice whose sample ends now reads as free (envelope 0, ENDX set) as on hardware. Before, its last level stayed in VOLUMEX, so libsnd never reused it, and after a few dozen one-shot sounds new ones were dropped (weapons, menu sounds).
- **Movies:** `movie.c` replaces `moviecdPlayMovie` (801384C4). It reads the STR file from the disc image and joins the video sectors into frames. `mdec.c` decodes them (bitstream to run-level codes, dequantization with libpress's table, exact IDCT, YCbCr to RGB). Frames go into the game's two VRAM buffers through the GPU port, and the game's own PutDispEnv and SetDispMask show them. XA ADPCM sectors (one in 8, stereo, 37.8 kHz) are decoded into the SPU's CD input (`cd_audio.c` resamples to 44.1 kHz). Emulated time follows the stream (150 sectors a second, 15 frames a second). After each frame the game's skip callback runs as on hardware (Start or X skip). TM2 shows its movies in the 24-bit display mode, which PSY-Z does not draw. `gpu_psyz.c` tracks the display mode from GP1 and, in 24-bit mode, reads the displayed area back each VBlank, unpacks RGB888 and passes it to PSY-Z (`psyz-02-display-rgb24.patch`: `Psyz_VideoSetDisplayRGB24`, shown in place of the display area and in screenshots).
- **Resolution:** PSY-Z renders at a multiple of the PS1's resolution (internal resolution, up to 8x), set by `[video] resolution` in `tm2pc.ini`: `auto` (the default) picks as many times 240 lines as fit the window or screen (1080p: 4x, 1440p: 6x), or a fixed 1 to 8. F9 cycles auto, 1x .. 8x while playing. The PS1's dithering is turned off when upscaled, where it shows as a grid (`dithering = auto`; `on` or `off` force it). Screenshots are taken at the rendered resolution (`psyz-03-capture-scaled.patch`). Movies are 320x240 video, shown the same way at any resolution and scaled smoothly to the window (`psyz-04-video-filter.patch`); game graphics stay pixel-sharp.
- **Console and log:** the game is a windowed program without a console window of its own. Started from a terminal it prints there; with its output redirected (tests) it writes to that; started from Explorer or a shortcut, its messages go to `tm2pc.log` next to the executable.
- **Custom music:** the `[music]` section of `tm2pc.ini` (`src/music/music.c`) sets each music track of the disc, by its `.DA` name, to `default` (the disc), a file, `random` (a random file from the music folder each time the track starts) or `random:FOLDER`. The music folder is `music` next to the executable unless `folder =` says otherwise. Files can be WAV (through SDL: PCM, float or ADPCM), MP3, FLAC or Ogg Vorbis (`decoders.c`: dr_mp3, dr_flac and stb_vorbis, public domain, in `third_party/dr_libs` and `third_party/stb` with their commits in `COMMIT`), at any rate, mono or stereo. They are converted to 44.1 kHz stereo and played through the SPU's CD input, so the game's music volume and fades apply. The file loops. The position reported to the game stays inside the disc track, so the game does not restart it, and after the pause menu it continues where it stopped. Tracks: FRWYSUB Hollywood, DISH Moscow, PARIS Paris, LAVA Amazonia, RFNEW New York, GLACIER Antarctica, DNMSWP Holland, HONG Hong Kong, TMLEVELS probably the Dark Tooth boss, TMLOGO title screen and menus, BLANK silence. PARIS, GLACIER and HONG are named from the file names and TMLEVELS is unconfirmed. `tools/disc/extract_music.py` saves every track as a WAV in `extracted/music` to listen to. The options page (planned) will edit the same settings. `settings.c` reads the file and passes each section to its module.
- **Pause-menu explosion (Resume, Quit):** it sounds thinner than in the other menus in retail too. The explosion is the same sound everywhere; in the menus a music loop (program 0x1B) is playing under it or starts with it, and its beat gives the bass. The Vanilla Plus change `pause_quit_boom` adds a copy of the explosion one octave lower instead (see VANILLA_PLUS.md).
- **Players:** every connected gamepad is opened; which are players 1 and 2 is chosen on Options › Controls › Controllers (by name, `[controls] player1_controller`; auto: in the order found). The keyboard is player 1 (player 2 can be given keys too). Each gamepad shows its player number (DualSense player LEDs) and a light bar color, red for player 1 and blue for player 2 (green and yellow are kept for players 3 and 4). Port 2 reads as plugged in while a second gamepad is connected (with `--tape`: if the tape has player 2 input).
- **Memory card:** not modeled; TM2 keeps progress in passwords. Profiles (`src/profile`, docs/PROFILES.md) record earned endings and the unlocks they give.
- **Overlay (PSY-Z patch 07):** `Psyz_VideoSetOverlay(rgb, w, h, rects, n)` copies rectangles of a window-sized RGB picture over each presented frame (plain blits, opaque), for drawing at the window's resolution over the game's own screens (the car select portraits). Captures include them, mapped into the game's picture.
