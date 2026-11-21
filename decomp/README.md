# The port's changes to the game's C

The port's own changes to the decomp's source (`references/TM2-Decomp`, which is never written),
built into the decomp's port variant. See `docs/HYBRID.md`, "Our changes in C".

- `files/`: new files, copied into the working copy: `include/semantic/port_hooks.h` (the settings
  block `portConfig` the host fills, the port functions' declarations) and the port units
  `src/port/*.c` (the port's own functions: built into `.port_text` with `--port-unit`).
- `patches/`: changes to the decomp's files. Every change to the game's C is under
  `#ifdef TM2_PORT_HOOKS`, which only this build defines, so without it the code is the decomp's.

```
python tools/decomp/wc.py sync     # copy the decomp to build/decomp-wc, apply this overlay
python tools/decomp/wc.py build    # the variant + the decomp's proof against E0 -> build/decomp-wc/out/
                                   # (~45 s: units cached by their preprocessed source, work/pv_cache;
                                   #  the first build after a sync of new decomp sources ~4.5 min)
python tools/decomp/wc.py diff     # after editing the working copy: write patches/ back
```

Then a port build with `TM2_EXE`, `TM2_INVENTORY`, `TM2_ADDRMAP` and `TM2_PORT_JSON` pointing into
`build/decomp-wc/out/` (`build/port-hooks`).

The decomp's pipeline (its request 27: `--port-define TM2_PORT_HOOKS`, `--port-unit`, group `one`)
takes changes inside existing game functions (laid out at their new size), new functions and their
constants in port units (`.port_text`, `.port_rodata`), and new storage in its port unit
(`src/semantic/port_variant_arrays.c`, listed in `config/US/port_variant_arrays.tsv`). The recompiler
translates the port's functions with the game's (`--port-json`: the sections' `.bin` files).

| Change | Function | Was | Checked |
|---|---|---|---|
| AI vs AI: every live computer car updates | `aiCarShouldUpdate` 8011B7D8 | `vp_AiCarShouldUpdate` (vplus/fixes.c) | a 2-player team match: equal to the retail port's for 1248 frames; the host's wrap off and the C off, it differs from frame 231 |
| AI vs AI: a computer car's aimed shot steers at its own target (its target computer car, else the nearest human still in) | `weaponFindTargetAhead` 80109F94, `weaponFindTargetBehind` 8010A2AC call `portAiAim` (`src/port/port_ai.c`, the first port function) | `ai_aim` (vplus/fixes.c), now with the same integer distance and all 4 humans | a 2-player team match: equal on retail, `port-hooks` and its host wraps for 1846 frames; the aim rule off, it differs from frame 314 |
| The quarter views' rows of the view-mode table (9-12) | `viewModeTable`'s initial data (`port_variant_arrays.c`) | `quarter_rows` (vplus/four.c, at boot) | 4 players: equal to the play build, frames and screenshot, with the host's rows off |
| Views 3-4's copies of the sky | `viewSetFollowObj` 80156344 (`portConfig.humans`) | `vp_ViewSetFollowObj` (vplus/four.c) | 4 players: equal, frames and screenshot, with the host's copies off |
| Chase-camera offsets by view shape (3-4 players), the guard for views not made | `viewCarCameraUpdate` 80102BE8 (`portConfig.humans`, `.left`) | `vp_CarCameraUpdate` (vplus/four.c) | 4 players, 2 players, kills: equal, frames and screenshots |
| Players 3-4's radars (every player and computer car on every radar, the game's 1-in-5 rule for other heights) | `portRadarPlot` (`src/port/port_radar.c`), from `rtMainLoop`; `worldUpdate`'s own N-human radar off | `four_radars` (vplus/four.c, now the same rule) | 3 and 4 players with `TM2PC_TEST_SOLID_RADAR`: equal, frames and screenshots |
| Views 3-4: their frame records, order tables, the bigger packet buffers; their drawing | `portViewBuffersInit` (end of `shellInitMachine`), `portViewsDraw` (`rtMainLoop`), `src/port/port_views.c` | `vp_DoubleBufferInit`, `vp_RenderScene` (vplus/four.c) | the same |
| Split screen's views 2..N (2-4 players) in one path, each as the game's view 2 (streaks included; its updates once per view 1-2) | `portViewsDraw` in place of `rtMainLoop`'s view-2 code; `streaksDraw` | `vp_RenderScene`, `vp_WeaponDropsUpdate`, `vp_WeaponFlameUpdate` (vplus/four.c) | 2, 3 (solid radar), 4 players and kills: equal |
| Split screen's display (2-4 players): views 2..N on the screen, each with its HUD | `portViewsPresent` in place of `rtMainLoop`'s split display | `four_present` (vplus/four.c; the mirrors after the last HUD: `four_hud_done`) | 2 (and mirrors), 3, 4 players, kills: equal |
