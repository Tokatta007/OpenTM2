# Performance tools

A plain sampling profiler for the running game. No install is needed: Python with `ctypes`, and the llvm-mingw `llvm-nm`
the build uses.

1. Start a run that holds the moment to measure, e.g. a scripted battle with `TM2PC_BENCH=V0,V1` (unpaced; it logs
   `bench: N VBlanks in S s`) and `TM2PC_FRAME_TIMES=30`. Each VBlank over 30 ms is logged, split into the game's code,
   the drawing lists (`gpu_dma`), the GPU's VBlank, the port's VBlank work and pacing.
2. `python tools/perf/sampler.py PID SECONDS out.txt [all]`: suspends each thread about 1,000 times a second and writes
   `tid module address` lines (the exe's addresses as offsets from its base). With `all` it also writes each sample's
   stack words.
3. `python tools/perf/symtop.py build/port-dev/tm2pc-hooks.exe out.txt [N]`: the hottest functions of the two busiest
   threads.
4. `python tools/perf/waits.py build/port-dev/tm2pc-hooks.exe out.txt`: for samples taken with `all`, what the main
   thread waits in (outside the exe, or in `SDL_GetAtomicInt`), with the exe's callers from the stack.

Found with them (2026-11-18): PSY-Z cycled its vertex buffers on every flush. SDL's D3D12 backend then looked through a
growing list of buffers at each cycle, about a third of the main thread at the farthest draw distance. The fix is
`third_party/patches/psyz-26-vertex-ring.patch`.
