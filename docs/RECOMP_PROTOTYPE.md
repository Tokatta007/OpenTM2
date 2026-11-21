# Recompiler prototype: status and testing

A first static recompiler for SCUS-94306, built on the inventory
(`docs/RECOMP_INVENTORY.md`), and a way to test it function by function
against the real game before any runtime (graphics, sound, CD) exists.

## Pieces

| File | What it does |
|---|---|
| `tools/recomp/recompile.py` | Translates every function to C: `void F_xxxxxxxx(CPU *c)`. Delay slots run in hardware order, jump tables become `switch`, indirect calls and jumps go through a lookup of all function starts (A0/B0/C0 go to the BIOS), and division follows the R3000's rules. Output: `build/recomp/gen/` (16 C files, a dispatch table). |
| `src/runtime/psx_cpu.h` | The CPU state and guest memory access (RAM mirrors, scratchpad, I/O), unaligned loads and stores, division. |
| `tools/recomp/runtime/runtime_stub.c` | A minimal runtime for tests. It provides dispatch and the simple BIOS functions (`rand`/`srand` on the BIOS seed in kernel RAM, `memcpy`, `memset`, string functions, `abs`, `printf`, `puts`), and logs anything else. |
| `tools/recomp/runtime/fixture_runner.c` | Runs one recompiled function from a captured state. |
| `tools/recomp/trace_runtime.py` | Traces retail in the harness emulator: clock rate, ticks per frame, indirect call targets, callback registrations. |
| `tools/recomp/capture_fixtures.py`, `fixture_io.py` | Capture fixtures from retail: registers, GTE registers, scratchpad and RAM on entry to a function and on return. |
| `tools/recomp/test_fixtures.py` | Builds the code and runs every fixture through it, comparing with the captured return state. |

Compiler: clang from llvm-mingw (`RECOMP_CC`, default OpenPete's bundled
copy). All 1,776 functions translate with no gaps and compile in about 8
seconds.

## How functions are tested

1. `capture_fixtures.py` runs retail with an input tape. On the first call of each selected function it records the full state on entry and again when the function returns to its caller with the same stack pointer.
   - Selected: game functions whose direct call tree has no GTE command, no hardware access, and only the BIOS functions listed above.
   - A fixture is flagged when an interrupt ran in between, because the handler then changed memory as well.
2. `test_fixtures.py` starts the recompiled function from the entry state and compares the result with the return state.
   - **Strict:** all 2 MB of RAM, the scratchpad, `v0`, and the registers the calling convention preserves (`s0`–`s7`, `gp`, `sp`, `fp`, `ra`).
   - **Reported only:** other registers, because BIOS code leaves different values in scratch registers. Example: the BIOS `memset` leaves an address in `v1`.

## Results (2026-10-03, Quake Zone combat tape, 900 frames)

| | Fixtures |
|---|---:|
| Captured (one per function) | 655 |
| **Pass: identical RAM, scratchpad and preserved registers** | **633** |
| Fail | 0 |
| Timeout: the function waits for hardware (GPU, CD) that the test runtime does not provide, e.g. `screenInit`, `screenClearScreen`, `Load4BitClut` | 18 |
| Interrupted during capture (not comparable) | 4 |

Every function that completed reproduced the PS1 exactly. That covers the
translation of every instruction these functions use, including delay slots,
jump tables, unaligned access, multiply/divide, and calls through
`rand`/`srand` with the BIOS seed in kernel RAM.

### GTE functions (2026-10-03)

With the register-level GTE (`src/runtime/gte.c`), `capture_fixtures.py --gte` selects the 186 functions whose call tree uses the GTE (and no hardware). Over 600 frames it captured 314 fixtures, up to 3 calls per function. **All 310 clean fixtures pass (113 functions)**, including the main renderer, the model renderers, `RotTrans` and `MulRotMatrix0`; 4 were interrupted.

The capture needs the emulator's GTE registers. DuckStation reports them only to debugger clients that have read its target description, so `capture_fixtures.py` requests it first. Without that, all GTE registers read as zero, and functions that rely on matrices loaded earlier wrongly appear to fail.

## Not covered yet

- **Functions that reach hardware.** These need the real runtime: GPU, SPU, CD, MDEC, timers and interrupts.
- **Load delay slots** are not modeled. No fixture has needed them.
- **The 100 indirect-call sites the trace did not reach** (mostly front end and movies).

## Running it

```sh
python tools/recomp/recompile.py
python tools/recomp/test_fixtures.py <capture folder>/fix-NNN --build
```

The fixtures and traces were captured from the retail game in an emulator harness that is not part of this
repository (a 900-frame capture: about an hour and 500 MB).
