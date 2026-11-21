# Recompilation inventory: Twisted Metal 2 (SCUS-94306)

What a static recompiler and its runtime have to handle in the retail
executable, found by `tools/recomp/inventory.py` (2026-10-03). The tool reads
`references/TM2-Decomp/bin/SCUS_94306R.EXE` and the decomp's IDA lists
(`config/US/ida_funcs.tsv`, `ida_names.tsv`). It prints a summary and writes
the details, function by function, to
[`recomp_inventory.json`](recomp_inventory.json). IDA names are used as hints
only: library membership and every count below come from the code itself.

**Verdict: TM2 is an easy recompilation target.**
- It is one executable, with no code loaded from disc at run time.
- The control flow is plain: no calls to unknown addresses, no branches between functions, and no branches in delay slots.
- Every switch jump table is found automatically.
- The game code never touches hardware directly; only Sony's SDK code does.
- The geometry coprocessor (GTE) is used through 8 commands only.

## The executable

| | |
|---|---|
| Load address, size | `800CDC54`, 735,232 bytes (to `80181454`) |
| Entry point | `8013878C` (`start`) |
| Global pointer (`gp`) | `801809D4`, set once by `start` |
| Code | `800CDC54..80162460` (data islands inside, e.g. `800CE910..800D1318`) |
| Initialized data | `80162460..80181454` (127 KB) |
| BSS | cleared by `start` up to `801FAD48`; stack at `801FFFF0` |

## Results

| Check | Result | What it means for the recompiler |
|---|---|---|
| Code loaded at run time | **None.** `FlushCache` has one caller, the SDK's `_patch_gte` (startup BIOS patch). The only addresses used below the executable (`80019C40..`) are file buffers given to `fileioLoadFileIntoRam`. | Recompile one executable; no overlays. |
| Functions | 1,775 from IDA. IDA's 19 eight-byte "functions" after `80162460` sit inside data and are dropped. | One C function per PSX function. |
| Missed functions | 122 gaps between functions (22 KB). 79 contain code; 77 of those are referenced from nowhere and are dead code. One is a real function: `8015FCA0`, in a handler table at `800D1220`. The other referenced block, `8013FF1C..80140410`, is the code `_patch_gte` copies into the BIOS at startup; it never runs in place. | Add `8015FCA0` (the recompiler does). The BIOS patch is not needed: the runtime provides the BIOS. |
| Calls (`jal`) | 7,587, all to function starts inside the executable | Direct C calls. |
| Branches | None leaves its function; none sits in a delay slot | Plain per-function translation. |
| Switch jump tables | 50 tables, 631 entries, all found by tracking constants. IDA named 49; the extra one is at `800D110C` (`SsSetTickMode`). | C `switch` statements. |
| Tables built at run time | 2, in the 3D model renderers `8014EA60` and `801526CC`. Each reads a 1-byte primitive type and jumps through a table built by setup code at `8014E9B4..` and `80153464..`. Each has 10 possible targets, all known statically. | `switch` over the 10 labels; log any other target. |
| Indirect calls (`jalr`) | 163 (82 in game code, 81 in SDK code) | Look up a table of all function start addresses. |
| Functions whose address is taken | 111 (77 game, 34 SDK). The game ones are mostly menu and screen handlers stored in data tables. Function-pointer arrays: the libgpu driver table `8017C2D8` (15 entries) and the libetc interrupt table `8017E04C` (3). | Covered by the lookup table above. |
| GTE | 28 functions: 12 game, the rest libgte. Commands: MVMVA 21, RTPT 5, RTPS 4, SQR 4, NCLIP 2, NCCS 2, GPF 1, OP 1. | A small GTE core. The 9 RTPS/RTPT sites are where sub-pixel precision and widescreen projection hook in. |
| COP0 | Only `InitGeom` (turns the GTE on) | Nothing to emulate. |
| `syscall` | Only `EnterCriticalSection` / `ExitCriticalSection` | Provided by the runtime. |
| `break` | 539 checks the compiler inserted (292 divide-by-zero, 247 overflow), plus one after `main` returns | Turn them into a logged trap. |
| Hardware registers | 73 functions, **all SDK code**. They reach the hardware through 102 address variables in data. | The game itself never touches hardware, so replaced SDK functions leave recompiled game code with plain memory access. |
| BIOS | 33 wrapper functions (below) | Provided by the runtime. |

## SDK libraries linked in

Counted from IDA names and the address ranges around them (81 unnamed
functions were assigned to their neighbors' library).

| Library | Functions | Bytes | Approach for the runtime |
|---|---:|---:|---|
| Game code | 1,351 | 488,744 | Recompile |
| libgpu (drawing) | 74 | 13,344 | Recompile. Emulate the GPU command ports and DMA, interpreting the drawing commands on the PC renderer. |
| libgte, libgs (geometry) | 39 | 7,912 | Recompile (pure CPU + GTE) |
| libetc, libapi (VSync, callbacks, events, timers, pads) | 46 | 3,696 | **Reimplement:** they are interrupt plumbing. |
| libcd + streaming | 62 | 17,492 | **Reimplement** at the API level (`CdSearchFile`, `CdRead`, `CdControl`, `CdGetToc`, streaming) on the disc image. CD audio playback goes here (the custom music folder hook). |
| libpress (MDEC movie decoder) | 19 | 3,496 | Reimplement `DecDCTin`/`out` in software. |
| libspu (sound chip) | 61 | 21,104 | Emulate the SPU registers in software (22 functions touch it). |
| libsnd (sequencer) | 103 | 26,656 | Recompile; it runs on libspu. |
| libc, startup | 18 | 3,364 | BIOS / C library versions |

OpenPete uses PsyCross (Sony's SDK API reimplemented on PC). Its source
will show which of these lines it already covers.

**BIOS calls** (wrapper → number of call sites):
- **C library:** rand 298, printf 59, memcpy 39, puts 16, exit 7, abs 5, memset 5, srand 3, strcpy 2, strlen 2, strncmp 2, memchr 1, strcmp 1, free 1, InitHeap 1, setjmp 1.
- **System:** GPU_cw 1, FlushCache 1, CdRemove 1, ChangeClearPad 3, ChangeClearRCnt 5, ReturnFromException 2, SetDefaultExitFromException 1, SetCustomExitFromException 2.
- **Events:** DeliverEvent 4, OpenEvent 2, CloseEvent 2, EnableEvent 2, DisableEvent 2, TestEvent 2.
- **Pads:** InitPad 1, StartPad 1, StopPad 1.

## Callbacks and interrupts

The runtime calls these where the hardware would interrupt. With one game
thread, that happens at fixed points: `VSync()`, `DrawSync()`, and CD or
DMA completion.

| Source | Handler |
|---|---|
| VSync | `cbvsync` `8015ECD8` (game: sound/XA repeat), `_SsTrapIntrVSync` and `_SsSeqCalledTbyT_1per2` (libsnd sequencer tick), `trapIntrVSync` (libetc) |
| Root counter 2 | `SysClkIntHandler` `801553A8`, registered by `InitTimer` `8015510C` (`OpenEvent(RCnt2, …)`, `SetRCnt(RCnt2, 0xFFFF, interrupt mode)`) |
| DMA | `_exeque` (libgpu drawing queue), `trapIntrDMA` |
| CD | `callback` (libcd), `data_ready_callback` (streaming), `DecDCTout` callbacks (movies) |

16 more registrations pass a handler computed at run time. The trace saw 417
registrations; every handler was one of the functions above, plus
`strCallback` `801380D8` (movie streaming) and `_spu_FiDMA` `801473B4`
(sound DMA).

**The game clock is the root counter, not VSync.** `GetCurTics` counts
`SysClkIntHandler` interrupts, so its rate must match the hardware:
target `0xFFFF` at system clock / 8, about 64.6 interrupts per second.
Measured in the emulator (`tools/recomp/trace_runtime.py`, 1,200 frames of the
Quake Zone combat tape): 1.080 interrupts per VBlank, which is 64.6 per
second at the NTSC field rate of 59.83 Hz.

The per-frame timestep comes from this clock. In gameplay it was **3 ticks in
1,074 of 1,075 frames**. Frames alternated between 3 and 2 VBlanks (716 and
358), about 22 frames per second. A fixed 3-tick step for online play matches
what the game already does.

## Time and randomness (for online play)

| Source | Use |
|---|---|
| `rand` | 298 calls in 85 functions, **all game code** (none in the SDK). The most calls are in `80125A18` (26), `8014BAD4` (17), `80116FF8` (12), `800E75F4` (11) and `800D5F94` (10). |
| `srand` | 3 calls: boot `801197D4`, level setup `80119C78`, round reset `8011A03C`, each seeded with `GetCurTics()` |
| `GetCurTics` | 26 calls in 11 functions, including the car update `800DC88C` (4), the camera `80102BE8` (2), the world simulation `8011A700` (2), `rtMainLoop` (4) and `80151138` (6) |
| `VSync` | 38 calls: 16 are `VSync(-1)` (reading the counter, mostly the front end), 21 have a non-constant argument |
| `SetFrameStart` | 2 callers: `rtMainLoop` and `80122B34` |

All of these come from the runtime (BIOS `rand`, the root counter, VSync),
so the PC build controls every source by construction. For online play:
- `srand` gets the lobby's seed;
- `GetCurTics` reads from the fixed game tick, so the car update and camera see the same time on every machine;
- `rand` callers are split into game-state and visual-only streams (the list requested in `DECOMP_REQUESTS.md`).

## Requirements for the recompiler

1. **Input:** the retail executable and this inventory: 1,775 functions plus the 2 missed regions, 50 jump tables, 2 run-time tables.
2. **Output:** one C function per PSX function, with direct calls for `jal`.
   - Indirect calls and jumps go through a lookup table of every function start.
   - Switches cover the jump tables.
   - An unknown target stops with a log message.
3. **Memory:** 2 MB of RAM, the 1 KB scratchpad and the hardware register range. Only SDK functions reach the hardware range. Where those functions are reimplemented, game code needs only the fast path.
4. **GTE:** the 8 commands above plus register moves.
5. **Runtime services:** BIOS functions, events and callbacks, root counter 2 and VSync timing, CD on a disc image, MDEC, the SPU, and GPU command interpretation.
6. **Checking:** game state compared with retail DuckStation at each `SetFrameStart`, using the harness and input tapes the decomp already has.

## Open items

- **Missed function:** add `8015FCA0` to the function list (`config/US/ida_funcs.tsv`, a decomp request).
- **Run-time targets:** the trace (boot, menus, 1,075 frames of combat) reached 65 of the 165 indirect jump and call sites, with 67 different targets. All are function starts except one: `_patch_gte` jumps to the BIOS through `B0` (`80140FA0`). The other 100 sites, mostly front end and movies, are still to be traced.
- **`8015D7F8`:** touches the SPU directly and sits between the libspu reverb functions. Presumed SDK; confirm.
- **OpenPete:** compare with its recompiler and runtime when its source arrives (expected about 2026-10-14).
