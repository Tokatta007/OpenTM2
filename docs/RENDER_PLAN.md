# Render plan: widescreen, draw distance, 60 fps, no wobble

Research notes (2026-10-07) for four in-battle features. Sources: the decomp's `docs/WIDESCREEN.md`,
`DRAW_DISTANCE.md` and `MAIN_LOOP.md` (these already answer most of what the port asked), its
C in `src/semantic`, and the port's `src/runtime`, `src/ui/options.c`, `tools/recomp/recompile.py` and
PSY-Z (`third_party/psyz`, patches in `third_party/patches`). Nothing here was run.

Tags: **CONFIRMED** = seen in code or data. **LIKELY** = inferred, check before relying on it.

## Summary

| Feature | Approach | Main work | Effort | Blocked by |
|---|---|---|---|---|
| Draw distance | raise the far value `D_80180DAC` (4800) | one runtime write; then the caps | 0.5 day, +3-5 days for caps | nothing for step 1 |
| Widescreen | anamorphic Hor+: squeeze the camera's x axis, widen the cull planes, show the frame stretched (patch 09) | view table patch, HUD un-stretch | 1-1.5 weeks | nothing |
| No wobble | PGXP-style shadow vertices: float SX/SY/Z from the GTE carried to PSY-Z; perspective-correct UVs | recompiler, `gte.c`, `gpu.c`, new PSY-Z patch | 1.5-2 weeks | decomp request R1 (near pass) |
| 60 fps | keep the 20 Hz simulation, redraw each VBlank with interpolated transforms | main loop in port C, state snapshot/lerp | 3-4 weeks (60), +1 week (uncapped) | decomp request R3 |

Suggested order: draw distance step 1, widescreen, no wobble, 60 fps (R3 can be asked now so it is
answered by then). Section 6 has the details.

## 0. What the game does today (CONFIRMED unless tagged)

**Frame and projection.**
- Every 3D view draws into a 320x240 frame; the two frame buffers sit side by side in VRAM at x = 0 and
  x = 320 (`viewCreate` 80155888).
- All projection numbers come from the view-mode table `8017EC44` (9 rows of 96 bytes): H at +0x08, view
  w/h at +0x18/+0x1C (OFX = w/2, OFY = h/2), the axis scales {X, Y, Z} = {±4096, 3690, 3690} at +0x28,
  and the cull planes at +0x30 (right, left, near) and +0x48 (top, bottom, far).
- Modes: 1 = 1 player (H 228, side planes ±35°), 2/3 = horizontal split, 4 = rear-view mirror (101x30 at
  211,20, axis X -4096), 5/6 = vertical split.
- `viewProc` (80155E80) sets GTE H (`ctc2 $26`, `sub_8014A274`) and OFX/OFY (`SetGeomOffset` 8014A25C)
  every frame for each view. It also reads the axis scale row every frame. The cull planes are copied
  only at view creation, by `viewLoadMode` 80155B30.
- Software projection (tracers 800D8230, lightning 801084D8, effect sprites 80110BE4, particles
  80112F6C, debris 8014C19C) uses the same H, center and camera matrix, so it follows any table change.

**Culling and far clip.**
- The level model, the 20 world models and the camera-attached sky model are walked by `renderWalkModel`
  (801526CC, hand-written asm). Opcodes 0/1 test a sphere against the 5 planes plus the far test: `depth
  > far << shift + radius` → cull.
- far = `D_80180DAC` (renderFarDistanceDefault) = **4800** world units, copied to `D_80180DA8` and
  ctx+0x10 each pass by `renderScene` 80152284. Its setter 8015165C has no callers. The mirror uses an
  immediate 500 (801522CC).
- Objects (cars, pickups, props): `csUpdMat` 801491E8 → `csSphereVisible` 80149344 tests the side, top
  and bottom planes only. There is no far test for objects, and per-node culling is off for them.
- LOD (opcode 2) and camera-region switches (opcode 11) use thresholds and planes stored in the level's
  DMD data, not in the EXE.
- Caps: 280 mesh draw entries per view (`slti` at 801528EC, array `dword_801AC810`), 198/200 matrix
  slots, and a 0x17CF0-byte packet buffer per frame. The OT mapping in the emitter `800CDC54` reaches
  about 40,959 units.
- The GTE limits bite first: SZ saturates at 65535 coordinate units, which is 8191 world units at the
  default world shift 3; IR1/IR2 saturate at ±32767.
- There is no fog: DQA/DQB are only set by InitGeom, and `DPCS` is never used.

**Rasterization ("wobble").**
- The emitter `800CDC54` runs RTPT/RTPS + NCLIP and writes SXY0-2 straight into the primitives with
  `swc2`. In the generated C this is `mem_w32(c, ea, gte_read_data(c, 12..14))` (8 sites,
  `build/*/gen/funcs_00.c`).
- Two other sites read SXY with `mfc2` into registers (sprites and beams).
- Near polygons (otz < 70, or 20 in split screen) go to `renderNearPolys` 80154690 (hand-written asm),
  which subdivides them "at screen-space midpoints". **LIKELY** this is done in integer screen
  coordinates; to check (R1).
- The port's GTE is `src/runtime/gte.c`, register-exact: `rtp()` produces integer SX/SY saturated to
  -1024..1023. PSY-Z's own `libgte.c` is not used by game code.
- PSY-Z draws in VRAM space at an integer internal resolution. Its `Vertex` has `short x, y`
  (`sdl3_gpu.c`), and `psx.vert.glsl` outputs `w = 1`, so texturing is affine. Neither PSY-Z nor the
  port has PGXP or a precision option.

**Frame pacing.**
- `rtMainLoop` 800D1318 runs one simulation step per iteration. It waits with VSync until `rtFrameTime`
  (`D_801809E8`, 20 tics, or 15 when `gameGetMode() == 2`) has passed.
- `timerGetFrameTicks` 8015531C returns 3 (VBlanks per frame), so the game runs at **20 fps**, not 30
  (RUNTIME.md, measured in DuckStation).
- In the port, each emulated VBlank (`rt_vblank`, `hle.c`) presents through `Psyz_VideoVSync(0)` and is
  paced to 59.94 Hz in `rt_pace_vblank` (`main.c`). So each game frame is presented 3 times.
- About 34 game source files scale by `timerGetFrameTicks()`, and much per-frame logic does not. A 60 Hz
  simulation would therefore change gameplay and break the retail-identical, deterministic simulation
  that the netcode needs. Agreed with the user (2026-10-04): interpolation, not a time-based simulation.

## 1. What already exists in the port (CONFIRMED)

- **PSY-Z patch 09** (`psyz-09-game-aspect.patch`): `Psyz_VideoSetGameAspect(a)` presents the 320x240
  game picture stretched to aspect `a`, centered (0 = normal). This is the "show stretched" half of
  anamorphic widescreen.
- **`split_wide_apply`** (`src/ui/options.c`, car select split screen): sets the game aspect to the
  content width, and writes the axis X scale of view mode 1 (`0x8017ECCC`) to `4096 · (4/3) / aspect`,
  back to 4096 when off. This is the same anamorphic technique, only without the plane widening (car
  select needs none).
  - Mode 1 is also the 1-player battle row, so battle code and car select must each set the value they
    need.
  - The comment there calls mode 1 "the shell scenes' view mode". The decomp gives mode 7 for shell
    scenes (801222F0). **LIKELY** car select's 3D cars use mode 1; to recheck.
- **`ui_ViewProc`** (wrap of 80155E80): already adjusts OFX/OFY after viewProc (the car select offsets).
  This is the natural hook for per-frame projection changes.
- **`ui_MainLoop`** (wrap of 800D1318) wraps a whole round, not an iteration. Hooks on HUD (`vp_HudDraw`
  800F8370) and pause (`ui_PauseMenu` 80129528) exist.
- **GPU path:**
  - `gpu_dma` walks the OT in guest RAM, so every vertex word's guest address is known.
  - `gpu.c` applies the PS1 size limit (1023/511) and sends GP0 words to PSY-Z (`gpu_psyz.c`:
    `Psyz_GpuWriteGP0`).
  - Execution is synchronous: after `DrawOTag` the frame is drawn, and the buffers can be reused at
    once.
- **Internal resolution:** `apply_resolution` (`gpu_psyz.c`) picks `auto` as window height / 240 (1080p:
  4x). F9 cycles it.
- **Guest RAM is 8 MB**: the PS1's 2 MB plus 6 MB free for the port, which is room for relocated, larger
  buffers.
- **Recompiler:** overrides are function-level only (`src/overrides.json`). There is no per-instruction
  patch mechanism, so EXE immediates such as 280 and 500 cannot be changed without one.
- **OpenPete** (Spyro recomp) has interpolation, widescreen and supersampling. Its source is expected
  around 2026-10-14 (memory note). It is a design reference, not a blocker.

## 2. Draw distance

**Step 1: far value (CONFIRMED mechanism).**
- Write `D_80180DAC` and `D_80180DA8` (32-bit) at match start, from a `[video] draw_distance` setting,
  for example in the existing `vp_LevelStart` (801334C0) wrap or the round start in `ui_MainLoop`.
- Steps: retail 4800, then 6000 and 8000. Optionally raise the mirror's 500. That is an immediate at
  801522CC: it needs the instruction patch below, or a wrap of `renderInitState` 801521D0 that rewrites
  ctx+0x10 when the view is mode 4.
- Keep the value ≤ about 8000 at world shift 3. Beyond that the GTE clamps SZ, so polygons sort wrongly
  and project wrongly.
- Client-side only: **LIKELY**. The far test is in the model walker, and the only reader of an object's
  visible flag (+292) in decomp C is `renderScene`. Asm callers are not checked (R2).

**Step 2: caps.**
- More level meshes hit the 280-entry cap per view and the packet buffer. Extra meshes are dropped
  silently, which looks like holes in the level.
- Add an instruction-patch list to `recompile.py`, e.g. `src/patches.json` {addr: new word}, checked
  against the original word. Then:
  - raise the `slti 280` (801528EC) and `198`/`200` (80152C18, 80152354, ...) caps;
  - relocate `dword_801AC810`, the matrix arrays `801F8AB8`/`801AEB18` and the packet buffers
    `801B600C`/`801CDD4C` (0x17CF0) into port RAM above 2 MB.
- This needs every instruction that forms those addresses (lui/addiu pairs, and ctx fields set from
  them) and every size check. Ask the decomp (R2) or scan the EXE with a tool.
- Debug aid: log ctx+0x3C (the mesh count) per frame to see when the cap is reached.

**Step 3: beyond ~8000 units (optional, large).**
- `gte.c` "extended range" mode: keep MAC3 (unclamped depth) and compute SX/SY from MAC1/MAC2 instead of
  the saturated IR1/IR2. Retail behavior stays the default.
- The emitter reads SZ as 16-bit registers, so this only helps if the emitter (`800CDC54`) is replaced
  by C that uses the wide depth. The decomp has reference C (`reference/model_emitter_800CDC54.c`).
- The OT mapping is good to about 40,959 units; beyond that, add segments.
- **LIKELY** level DMD data has finite top LOD ranges (opcode 2) on some pieces, which would make them
  vanish regardless of far. Fix: a data dump per arena (R2), then treat the last LOD as unbounded in a C
  walker, or scale d2 down.

**Risks:**
- More geometry means more draw time. Harmless on PC.
- Retail arenas may show unfinished backs or edges at longer range (data). Test every arena.
- The sky shell (camera-attached model, group 2) also uses far. Keep it, it is fine.

## 3. Widescreen (Hor+)

**Status: done** (`src/vplus/wide.c`, `[video] widescreen`, on by default, Options > Video > Widescreen). Anamorphic
as below, k = the window's aspect over 4:3 (up to 3), set when a battle's views are made and again when the window
changes mid-battle (`wide_vblank`: the rows, and each live view's side planes in its camera object, m40 at +0x40). The HUD is laid out for the view as seen and squeezed back (`hud_layout.c`, and the split
mirror in `mirrors.c`); the pause screen, results and photo mode widen their captured frame (`wide_capture`).
Tests: `compare_builds.py` scenarios `wide1`, `wide2`, `wide4` (a 1280x720 window, `TM2PC_TEST_WINDOW`), `wide_resize`
(widened to 21:9 mid-battle, `TM2PC_TEST_RESIZE`).

**Recommended: anamorphic, with a 320-wide frame in VRAM.**
- A true 427-wide frame (WIDESCREEN.md §6) needs both buffers in VRAM: 2 × 427 = 854 columns.
- **LIKELY** arena textures already use VRAM x ≥ 640 in rows 0..239. Moving the second buffer would
  overwrite them (check with F6 or `Psyz_VideoAllocVramDump` during a match).
- The anamorphic route has the same field of view, keeps the VRAM layout and reuses patch 09.

For a display aspect A (16:9 = 1.778; factor k = A / (4/3)):

1. **Projection (axis X, read each frame by viewProc).**
   - Set +0x28 of modes 1, 2, 3, 5, 6 to `4096 / k` (16:9: 3072), and mode 4 to `-4096 / k`.
   - The rows are at `8017EC44 + 96·mode + 0x28` (mode 1 = 8017ECCC). Also set the initial copy
     `D_8017EC24` (viewAxisMatrixTemplate) if it is used before the first viewProc.
   - H, OFX and OFY stay as they are. Everything projected (GTE and software) follows, because both use
     the same camera matrix.
2. **Cull planes (copied at view creation).**
   - Patch +0x30/+0x38 (a[0], a[1]) of each row before `viewLoadMode` 80155B30 runs. A new wrap of
     80155B30 can patch the row of the mode it loads, then call the original.
   - Formula (WIDESCREEN.md §6): α = atan(k · halfW / H), store (∓4096·cos α, 4096·sin α).
     - 16:9: mode 1 (∓2991, 2799); modes 2/3 the same; modes 5/6 (∓3241, 2505).
     - 21:9: mode 1 (∓2586, 3176).
   - Fallback: (0, 4095) disables lateral culling (watch the 280 cap).
   - Restore retail rows when the option is off, and for car select (which shares mode 1).
3. **Present.**
   - `Psyz_VideoSetGameAspect(A)` while a match is shown, and 0 otherwise (menus, movies, results).
   - Set it from `ui_MainLoop` and clear it on exit. Coordinate with `split_wide_apply`.
4. **Resolution.**
   - The frame is stretched by k horizontally. In `apply_resolution`, pick the multiplier from the shown
     width too: n = max(window_h / 240, ceil(window_w / 320)), up to 8. At 1080p 16:9 that is 6x instead
     of 4x, which keeps horizontal detail.
   - **LIKELY** later: a PSY-Z patch with separate x/y internal scales would avoid the oversampling. Not
     needed first.
5. **HUD and 2D (the anamorphic catch).**
   - The HUD is drawn into the same frame (step 21 of the loop, after `DrawOTag`), so it is stretched by
     k.
   - Fix it in `gpu.c` with a "2D remap" active while the HUD functions run. The GPU is synchronous, and
     the HUD's primitives reach `gpu.c` inside these calls. Set a flag in the wraps of 800F8370,
     800F9084, 800F9D48 and 80129528.
   - Remap rule: per primitive, from the center of its x bounding box (0..320), pick the anchor: 0
     (left, center < 107), 160 (center) or 320 (right, center > 213). Then map every x to
     anchor + (x - anchor) / k. The HUD keeps its proportions and hugs the screen edges. The anchoring
     groups of WIDESCREEN.md §6C can override the rule per element.
   - Full-screen quads (flash 800DC45C, red tint 8011335C, lost-a-life 8012940C) are drawn 0..320 and so
     are stretched to the full width by themselves. They need no change, but the remap must leave them
     alone: exempt any primitive that spans the whole 0..320 width.
   - The mirror: its 3D window (mode 4) and its HUD frame (800FB8AC) are both in frame space. Leave both
     unremapped: the mirror shows k times wider on screen with correct proportions (axis -4096/k). Or
     move its window and frame right by the same amount.
   - Remapped sprites land on fractional pixels. At ≥ 2x internal resolution this is fine; at 1x it
     shimmers.
   - Long term the port's own HUD (overlay, patch 07) replaces this.
6. **Split screen.**
   - Rows 2/3 and 5/6 above. In vertical split each half is 160 wide: its axis scale and planes per the
     table.
   - The split HUDs are remapped like the 1-player HUD, with a center group around the divider.

**Not affected (CONFIRMED, WIDESCREEN.md §4/§6D):**
- the emitter (it has no screen-x test);
- particle and tracer bounds;
- the weapon auto-target cone (it is gameplay, around the car, not the screen);
- the near plane, OT and far test.
- The gpu.c size limit (1023 px) is unchanged, because the frame is still 320 wide.

**Risks:**
- Gameplay could change only if game code reads the visible flag (+292) set by the widened planes (R2).
  If it does, online play must lock the aspect, or run the plane test only for drawing.
- Remap heuristics may misplace a HUD element that spans groups. Check every HUD layout, the pause menu
  and the messages.
- The PAR question (the 0.9 Y/Z axis, WIDESCREEN.md §2): keep today's look (axis 4096 = emulator look).
  Optionally offer "TV proportions" (axis × 0.9, which combines with 1/k).

## 4. No wobble (subpixel vertices, perspective-correct textures)

**The last wobble, and why split screen showed more (2026-10-12).** `TM2PC_PRECISE_TRACE` counted 15% of 1P and
21% of 2P polygons still drawn from whole pixels. A split view has half the pixels (a quarter in 4P), so each
whole-pixel snap is twice as large on screen: the same misses look worse there. The misses and the fixes:
- **Near the camera** (most of them): a corner nearer than H/2 (the GTE's divide saturates) or past +-1024
  (clamped) was refused, so the ground and walls beside the camera kept the PS1's stretched places. With
  depth (wobble none) such a corner now gets its true place; PSY-Z draws `(x*w, y*w, 0, w)`, so a corner
  behind the camera (w < 0) is clipped by the GPU at w = 0 and the polygon is drawn as it really is. The
  integer check allows 4 px plus 1/64 of the distance from the centre (`gte.c` `rtp`). Smooth (no depth)
  keeps refusing them. A clamped word is not put in `by_value` (many corners share it).
- **Effect sprites in C** (`fxSpritesDraw`: smoke, fire, explosions, flames on burning cars): projected with
  integer C, not the GTE. In the variant (overlay `world_collapse.c`) the same places are computed in 1/16
  pixel and queued with `PORT_PRECISE_PUT` (`port_hooks.h`: a ring at 0x80680000, `portConfig.precise`);
  `port_precise_ring` (`vplus/port_config.c`) puts them in the shadow table before the next polygon is drawn.
- **The model emitter's sprites and beams** (800CDC54, hand-written assembly: pickups and their labels,
  the arenas' fires): a point sprite is its projected centre +- a size, a beam its two projected ends +- a
  sideways step. Those points are exact middles of corner pairs and their projections are in `by_value`,
  so `effect_quad` (`gpu.c`) gives the corners their points' subpixel offsets (up to 3 px, the GTE's own
  rounding). Only for quads in a view's 3D order table (`port_ot_is_world`: the game's four, the in-between
  frames', views 3-4's), never a HUD's.
- `by_value`: 262,144 entries, kept 10 VBlanks (was 65,536 and 4: a split screen projects twice as many
  and builds its frame over more VBlanks).
- Left at whole pixels: the HUD, off-screen and zero-size quads, a fixed overlay, and big effects right at
  the camera (their GTE point saturated). 2P world misses went from 8,031 to 1,725 per run.
- Tests: `TM2PC_PRECISE_TRACE=2` lists only the views' 3D misses (with the reason per vertex), `=3` all;
  `TM2PC_PRECISE_SHOW=1` tints them red; `TM2PC_OT_LOG` the order tables drawn. The comparisons set
  `TM2PC_NO_PRECISE_C=1` (the C's sprites as the plain build): one, two, four, teams, interp2 equal.

**Car select's shiver (fixed 2026-10-11):** the turning car had 1-10 of its polygons a frame drawn
from the rounded words. Its projections near the camera were refused: the float and the GTE's
integer differ by 2-3 pixels there (the GTE's rounding), more than the 2-pixel check allowed, now 4.
Also, a vertex word with no float by its address (copied with ordinary stores) takes one of the same
value projected in the last 4 VBlanks (`by_value`, 65536 entries). `TM2PC_PRECISE_TRACE=1` (per
VBlank: polygons without floats, and why), `=2` (each one), `=3` (each projection refused).

**Status: done** (2026-10-11; Options > Video > Wobble, `[video] wobble = ps1 | smooth | none`, default none).
As planned below, with these differences:
- `gte.c`: the float FIFO beside SXY0-2 is filled from the exact products (double), valid when sf is 12, the
  divide does not saturate and the float lands within 2 pixels of the integer result. `swc2` is
  `gte_store_data` (recompiler); the shadow table (8 MB of guest words, 20 bytes each, made on first use)
  keeps {word, VBlank, x, y, depth}. A lookup needs the same word, stored at most 10 VBlanks before (a frame
  is built over several VBlanks and drawn after).
- `gpu.c` passes each DMA word's address with the command; `send_polygon` sends a polygon with floats when
  all its vertices are found (about 83% of the polygons in a 1-player battle; the rest are 2D and sprites).
- PSY-Z `psyz-12-precise-vertices.patch`: float positions and a depth per vertex (`gl_Position` times w),
  `Psyz_GpuPushPrecise` queues one entry per polygon in GP0 order. GL keeps its integer path.
- The near pass (item 5): in "none" `vplus/precise.c` wraps the emitter with its near threshold (scratchpad
  +0x40) at 0, so close polygons go to the order table whole, precise and perspective-correct, instead of
  being cut from integers. "smooth" and "ps1" keep the game's cutting.
- Checks: `wobble = ps1` draws byte-identical frames to the build before (`compare_builds.py` scenario
  `wobble_ps1` against `one`); the cars' records and `rand` are identical in every mode (1P and 4P runs).
- Seams (`psyz-16-seam-edges-cutouts.patch`): separate meshes (a tire's tread and its side) meet with hairline gaps
  that the PS1's whole pixels hid. An opaque precise polygon is drawn with every edge 0.3 of a PS1 pixel further
  out (its outline offset, the corners along their bisectors; patch 13 pushed the corners from the middle, which
  left long thin polygons open). Its UVs are kept: the texture stretches with it.
- Cut-out edges (the same patch, `psx.frag.glsl`): a shape made by see-through texels (a tire's round side) is
  drawn smooth when magnified: whether a pixel is seen through is decided from the 4 texels around it (their
  coverage at a half, as a bilinear filter's), the color still a texel's own. Precise polygons only, and only
  magnified; the PS1 look (`wobble = ps1`) keeps whole texels.
- The translation's fraction (`gte.c`, `tr_frac`): the model emitter (800CDC54) builds a node's TR from two MVMVAs,
  the second shifted, dropping 12 bits; a TR written right after keeps them for the floats. Car select's cars are
  so near (~1 unit a pixel) that the rest of the rounding, in the game's integer math before it, still rocks a
  car's front a pixel as it turns: there (`ui_SceneDraw` around the carousel's draw, `gte_tr_smooth_begin`) each
  node's TR is a least-squares fit (quadratic) of its last 12 frames' (a jump starts it over), for the floats
  only. Measured with `carsel_cadence`'s 30 VBlanks: the one-frame hops are gone.
- **The depth buffer** (`psyz-20-depth-buffer.patch`, `runtime/gpu.c`; the decomp's HIDDEN_AREAS.md 5 (a), the user's
  "nothing hidden", 2026-10-26). Video > Hidden surfaces (`hidden_surfaces = depth`, the default, or `ps1`), with
  wobble none only. In a view's 3D order table every polygon with precise depth is drawn with a depth test
  (`Psyz_GpuPushPreciseDepth`): opaque ones test and write, see-through ones (GP0 bit 1) only test, the sky and
  backdrops (slots 4,090..4,095) and polygons without floats go in order as before. The buffer (D32 float, 1/w,
  nearer is greater, as big as the render target) is emptied at each view's order-table walk (`clear_depth`).
  - **No size limit for them:** a polygon with depth is drawn whole (the graphics card clips it), so the PS1's
    dropped near polygons (holes in the tunnel's walls) are back.
  - **Ties:** the PS1 drew a decal or a painting after its wall by its slot; each slot pushes the polygon a quarter of
    a depth unit farther (the shader's `zbias`), so a polygon in a nearer slot wins against one up to that much
    nearer (a slot is 8-256 depth units: real overlaps are unchanged). Paris's painting is 2 units behind its wall
    at one edge.
  - **Seams:** the 0.3-pixel outline offset moves a corner along the screen only: its 1/w is taken from the polygon's
    plane at the new place, or the offset tilts the depths (a decal sinks into its wall).
  - **See-through texels** are discarded (no depth written), not drawn with alpha 0.
  - **The GTE range pass** (decomp FAR_DISTANCE.md 2): a far mesh scaled by 1/2^k has its depths 2^k too near; its
    slot is still in world units, so `gpu.c` gets 2^k back from the slot's distance against the deepest corner's
    (a ratio of 1.6 or more; w is 8 units a world unit) and multiplies the depths. (Measured at Amazonia from above:
    202 of 4,031 polygons scaled, all by 2.)
  - An old PSY-Z overrun, found by it: a gouraud quad's last `writePacket` puts the next color word in the vertex
    after its own, so a quad at the vertex buffer's end wrote into `index_buf`'s first entries (bad indices in that
    batch, harmless on the card, a crash once the depth code read the vertices they named). `Draw_PushPrim` now
    keeps room for 5 vertices.
  - Tests: `TM2PC_DEPTH=0/1` overrides the setting; `TM2PC_DEPTH_LOG=<VBlank>` logs every depth-tested polygon
    from then (slot, corners).
  - Decomp request 72 (HIDDEN_AREAS.md) draws the hidden places (Paris's church inside, the tunnel at any distance) under
    `portVisual` bit 6; the port sets it only while the depth test is on.

The jitter has two causes:
- SX/SY are rounded to whole pixels by the GTE (the main one).
- Textures are interpolated affinely.

Rotation matrices and model vertices in 4.12 / int16 fixed point add a little, which is not fixed here.
The design is PGXP-like, written fresh. DuckStation's `cpu_pgxp.cpp` was read as a
reference only: its license does not allow copying.

1. **GTE (`gte.c`).**
   - In `rtp()`, also compute float sx = OFX/65536 + MAC1·H/SZ and sy = OFY/65536 + MAC2·H/SZ, from the
     unsaturated MACs and an exact divide, plus a float depth (MAC3).
   - Keep a 3-entry float FIFO parallel to SXY0-2 (pushed and moved with them, including mtc2/SXYP
     writes, which invalidate it).
   - The integer registers stay exact, so game logic (NCLIP, OTZ) is unchanged.
2. **Recompiler.**
   - Emit `swc2` of data registers 12-15 as `gte_store_sxy(c, ea, reg)`. The function writes the word as
     now, and records {word, sx, sy, z} in a shadow table keyed by the guest address.
   - Size: 8 MB / 4 entries × 16 bytes = 32 MB, or only the packet-buffer ranges. Regenerate, then run
     the fixtures: the integer results must be byte-identical.
   - `mfc2` sites (sprites and beams, 2 in the emitter) stay integer at first.
3. **GPU front end (`gpu.c`).**
   - `gpu_dma` passes each packet's guest address. For polygon commands, look up each vertex word's
     address in the shadow table, valid if the stored word still equals the RAM word.
   - If all vertices of the polygon are valid, send the floats with the command through a new backend
     entry, `command_ex(words, n, const float *xyz)`. Otherwise use the integer path.
   - The 1023/511 size limit keeps using the integer words, so it still culls the same primitives.
4. **PSY-Z (new `psyz-11-precise-vertices.patch`).**
   - API: `Psyz_GpuSetPreciseVertices(const float *xyw, int n)`, consumed by the next primitive in
     `Draw_PushPrim`.
   - `Vertex` becomes float x, y plus w; the vertex format changes from SHORT2 to FLOAT2 (+ FLOAT). The
     GL backend (`sdl3_gl.c`) needs it too, or stays integer.
   - In `psx.vert.glsl` output `gl_Position = vec4(ndc · w, 0, w)` when w is given. The hardware then
     interpolates UV and color perspective-correctly. 2D primitives keep w = 1 (affine, as today).
   - The texture-window and sub-pixel UV handling in `psx.frag.glsl` uses `rawUV` and `dFdx`; check it
     still behaves. Rebuild the shaders with glslang, spirv-cross and dxc.
   - Options: `[video] precise_vertices` and `perspective_textures`, each on or off. The second needs
     the first.
5. **Near-polygon pass 80154690.**
   - **LIKELY** it emits midpoint vertices as integers. Without shadow data those polygons fall back to
     integer, so close-up ground would still wobble.
   - Depending on R1: if it reprojects 3D midpoints with RTPS/swc2, it is covered automatically. If it
     averages screen coordinates, it needs a C version (or a wrap) that averages the floats and w too.
   - With perspective-correct textures the subdivision is no longer needed: another option is to skip it
     and draw the original polygon.
6. **Software-projected effects** (tracers, sprites, particles, lightning) stay integer. They are small
   and short-lived; do them later if visible.

**Risks:**
- Shadow lookups cost a hash or array access per vertex; fine on PC.
- Polygons whose words the game edits after `swc2` simply fall back to integer (the shadow is invalid).
- GTE fixture tests must stay byte-identical.
- Seams: neighbors with one float and one integer vertex can open hairline cracks. Fall back per
  polygon, not per vertex.

## 5. 60 fps (and beyond): interpolated rendering

**Made (2026-10-11), first version: `src/vplus/interp.c`, Options > Video > Frame rate (60 FPS, the default;
Original).** Unlike the plan below, the game's loop is not taken over: its step stays exactly as it is
(the battle plays the same: `interp*` and `interp*_off` scenarios have equal states frame for frame), and
the VBlanks its loop waits through after a step's drawing (`VSync(0)`, VBlanks 1 and 2 of the 3) get a
redraw from the host, as photo mode draws (R3's answers applied):
- The game shows each step one step late (a frame record draws into one buffer and displays the other),
  so a redraw in step j's wait blends j-2 to j-1, draws into this step's draw areas and shows them with
  the other record's display environment.
- Poses kept at each step's renderScene of view 1 (the object pool: +0x80 matrix, so cars keep their body
  tilt, +0xA0, +0xA8; each view's camera and its parent: angles and position) and, for the views drawn
  after the world's step (split screen's, the mirror), at their first renderScene. Snap: not drawn in
  one of the two steps, or moved more than 400 (cameras 512); a round's start clears the history.
- Every view's frame record gets the redraws' own order table and packets (high RAM 0x80600000,
  0x80620000) for the while; presented as `rtMainLoop` (split screen: the variant's `portViewsPresent`,
  `rc_port_views_present`; the players' mirrors follow its HUD wrap).
- Once per step, no-ops while redrawing: `animUpdateAll`/`animUpdateOne` (their `rand()`), the HUD's
  radar swap, message timer and marker blink, `weaponArcsDraw`, `fxSheetDisplay`; `streaksDraw` draws
  only (four.c's `streaks_draw_only`).
- Everything the redraw touches is put back: the pool, the cameras, the records, `viewProc`'s eye and
  matrices, the GTE, the scratchpad.

**Exact replays (2026-10-11).** Drawing changed the battle: the model animators (`animStepRandom`,
`animStepWeightedRandom`, `animPickPeriod`) call the game's `rand()` once per drawn view (decomp
RAND_CALLERS.md 2), so another camera, another draw distance or the kill cam's shots moved the game's random
stream. They now draw from a stream of their own (the BIOS seed swapped around them, `interp.c`). This also
makes the draw distance (and any view setting) safe for online play. The kill cam no longer changes the
views to show its shots (see killcam.c), photo mode draws with the machine kept and put back, and both
check their replays frame by frame: the team match's kill cam (`interp_killcam`) and the rewind
(`rewind`) match the battle exactly.

**Uncapped on the display's clock (2026-10-11).** With Frame rate Uncapped the driver's vsync is on and, in a
battle's waits (and the shell's scenes: car select, the title), the emulated VBlank shows nothing itself: the
pacing wait draws and shows frame after frame, each held by vsync to a refresh and blended for the moment it is
shown (`interp_extra`, `interp_owns_present`; scenes: `interp_SceneFrame` keeps each scene object's place and
turn, `scene_redraw` draws the list again with the RAM kept and put back). On the user's 100 Hz display:
100 frames a second, evenly (60 frames on 100 Hz judder: some shown once, some twice). The battle is unchanged
(scenario `uncapped`: equal states run to run). Original and 60 FPS keep vsync off and the port's own 59.83 Hz.

**The effects (2026-10-11).** Blended too: effect sprites, impact particles, the bullets' streaks (sliding along
their step's own move), the debris bursts' moving parts; the taser arcs and the screen flash are drawn on
in-between frames, their records, the sparks they spawn and the random seed put back (`fx_keep`).

Not yet: model nodes written directly (car part spinners, Mr. Slam's arm, the Eiffel tower) and sprites that
follow a car still move at 20 Hz; the HUD can run a step ahead of the world; the split car select's
cars (turned by the port, not the scene) judder on displays that are not 60 Hz. Tests: `interp`, `interp2`, `interp4`, `interp_mirror`
(four VBlanks in a row; `TM2PC_INTERP=0/1` overrides the setting; the comparisons run with
`frame_rate = original`).

Keep the simulation step exactly as retail (20 Hz, all update calls once) and draw 3 frames per step,
one per emulated VBlank. Each drawn frame has its transforms blended between the previous and current
step (t = 1/3, 2/3, 1).

1. **Own the loop iteration.**
   - Override 800D1318 with port C based on the decomp's `rt_main.c` (a close match), calling the game's
     functions with `guest_call`.
   - Keep the `ui_MainLoop` round logic around it.
   - Order per step:
     - the update calls (pad, targets, fx records, debris, lights, bullets, arena events, fx sprites,
       `worldUpdate`);
     - the update half of `streaksDraw`: `weaponDropsUpdate` 80109D58 and `weaponFlameUpdate` 8010A7A0
       once per step. Wrap them so that on redraws they return at once.
   - Then for each of the 3 VBlanks:
     - load the blended state;
     - run the draw calls (`viewProc`, `streaksDraw`, `fxSpritesDraw`, `debrisDraw`, `renderScene` and
       the second view or mirror);
     - `DrawOTag`, HUD, `VSync(0)` (one VBlank, which presents);
     - restore the real state.
2. **Do not advance animations three times.**
   - `renderFlipFrame` (80152194) flips `renderFrameParity`. `animUpdateAll` and `animUpdateOne` step an
     animation only when its done flag for the current parity is clear.
   - So call `renderFlipFrame` and `clutAnimUpdate` (80148B1C) once per step, not per redraw. The
     redraws then skip the animation steps automatically. This matters because `animStepRandom` calls
     the shared `rand()`.
   - Reusing the same OT and packet buffer for each redraw is safe: the port's GPU is synchronous.
3. **What to blend** (MAIN_LOOP.md):
   - camera objects `D_80181110[i]` and their parents (+160..+164 angles, +168..+176 position);
   - every object in the list from `csListHead` 80149438 (linked by +288);
   - the 20 world models `D_801A4E1C`;
   - the projectile, effect sprite (`D_80185144`, 150), debris, streak and impact particle records
     (layouts: R3).
   - Blend positions linearly and angles through `BoundAngle` (shortest arc), then rebuild +128 with
     `RotMatrixYXZ`. If R3 shows that some objects write +128 directly (car pitch and roll), blend those
     matrices instead (quaternion slerp, then back to 4.12).
   - **Snap** (no blend) when a position jumps more than a threshold (respawn, teleport, camera cut) or
     an object is new or removed.
4. **Side effects in the draw path** (must run once per step, or be neutralized on redraws):
   - `draw_tasers` 801084D8 calls `rand()` (bolt jitter) and spawns particles. On redraws, use a private
     visual random stream and suppress the spawns.
   - The HUD (800F8370 ...) and `pauseMenu` may count timers and read input (R3). Until checked: draw
     the HUD on redraws with timers frozen, or reuse the step's HUD primitives.
   - Arena events (#10 in MAIN_LOOP.md, e.g. the Eiffel flash) may write primitives into the OT (R3).
   - The pause menu and attract demo: no blending (t = 1).
5. **Latency.**
   - Blending previous→current shows the newest step 2 VBlanks later than retail would (about +33 ms).
   - Alternative: extrapolate (t = 4/3...) with no added latency, at the risk of overshoot. Make it an
     option after testing.
6. **Uncapped (>60 Hz), later.**
   - Present more often than the emulated VBlank: draw extra frames during the wait in `rt_pace_vblank`,
     with t from the wall clock.
   - Needs a present call outside `rt_vblank` (a small PSY-Z or `gpu_psyz.c` change) and VSync off or
     adaptive (PSY-Z already runs `PSYZ_VSYNC_LIMITLESS`).
   - Emulated time (audio, CD, IRQs) stays at 60 Hz.
7. **Determinism guard.**
   - Run the combat tapes with interpolation on and off. The simulation state (and `rand()` count) after
     N steps must be identical.
   - Add this to the Foundation determinism tests.

**Risks:**
- Missed state (some drawn thing not blended) shows as 20 Hz stutter on that thing only. Harmless, and
  found by eye.
- Missed side effects change gameplay. This is caught by the tape test.
- Draw cost triples. Fine on PC.

## 6. Order and effort

| # | Step | Effort | Notes |
|---|---|---|---|
| 1 | Draw distance step 1 (far value setting, Video page) | 0.5 day | quick win; test all arenas for pop-in and holes |
| 2 | Widescreen: axis + planes + game aspect + resolution choice | 2-3 days | 1 player first, then split |
| 3 | Widescreen: HUD remap in `gpu.c` and mirror | 3-4 days | check every HUD layout |
| 4 | Recompiler instruction patches + caps (draw distance step 2) | 3-5 days | needs R2 or an EXE scan |
| 5 | No wobble: gte.c shadow + swc2 helper + gpu.c lookup | 4-5 days | fixtures byte-identical |
| 6 | No wobble: PSY-Z patch 11 (float vertices, w, shaders) | 3-4 days | then the near pass per R1 |
| 7 | 60 fps: main loop in C, once-per-step split, camera + object blending | 1.5-2 weeks | R3 first; tape determinism test |
| 8 | 60 fps: effect records, HUD, snaps; then uncapped | 1-2 weeks | |
| 9 | Draw distance step 3 (wide GTE + emitter in C) | 1-2 weeks | only if 8000 units is not enough |

Widescreen before no wobble: anamorphic squeezing makes integer rounding slightly more visible, and no
wobble fixes that. No wobble before 60 fps: the redraws at 60 Hz show vertex snapping more clearly.

## 7. Decomp requests (drafts)

**R1. Near-polygon pass (no wobble).**
- `renderNearPolys` 80154690 and its helpers 80154E30, 80154E84, 80155088 (asm): how are the subdivided
  vertices made?
  - 3D midpoints reprojected with RTPS, or integer averages of screen coordinates?
  - Where are SX/SY, UV and colors written into the primitives (swc2, or mfc2 + sw)?
- Also: list every other site in the EXE that stores GTE SXY into primitives with `mfc2` + `sw`, or that
  edits a primitive's XY after the GTE wrote it.

**R2. Draw-distance caps and data limits.**
- Every instruction that forms the addresses or sizes of: the mesh draw list `dword_801AC810` (280 ×
  32), the matrix arrays `801F8AB8` / `801AEB18` (200 × 32), and the packet buffers `801B600C` /
  `801CDD4C` (0x17CF0). Include the scratchpad ctx fields filled from them and the bound checks. This is
  so the port can move them into larger buffers above 2 MB.
- Per arena: the world shift (`renderSetWorldShift` argument), the largest LOD max distance (opcode 2
  records) and any opcode 11 region nodes, from the DMD files.
- Who reads an object's visible flag (+292, set by `csSphereVisible`) besides `renderScene`? Is it
  gameplay-relevant (targeting, AI, sound)?

**R3. Interpolation (60 fps).**
- The record layouts (position, previous position, angles, lifetime) of:
  - the 40 projectiles (`bulletsMove` 800D7E30);
  - the 150 effect sprites (`D_80185144`);
  - debris chunks and bursts (8014B940 / 8014BF3C);
  - streak slots (800D8230);
  - impact particles (80112F6C);
  - lightning (801084D8).
- Objects whose rotation matrix +128 is written directly rather than rebuilt from the angles at +160
  (car body pitch and roll, wheels, turrets?), and per-node animation transforms.
- Camera cuts and teleports: a flag or event when the camera or a car jumps (respawn, death cam, level
  events), for snapping.
- State changes inside the draw path:
  - the HUD functions (800F8370, 800F9084, 800F9D48, and 800FADDC / 800FAEEC);
  - `clutAnimUpdate` 80148B1C;
  - `weaponFlameUpdate` 8010A7A0;
  - `pauseMenu` 80129528 (timers, input, rand);
  - whether any arena event (#10 of MAIN_LOOP.md) writes into the OT.

Widescreen needs no new request: WIDESCREEN.md covers it, apart from 800FADDC / 800FAEEC, which are
listed under R3.
