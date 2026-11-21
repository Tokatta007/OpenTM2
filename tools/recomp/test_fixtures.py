"""Test recompiled functions against fixtures captured from retail.

    python tools/recomp/test_fixtures.py RUN_DIR [--build] [--cc CLANG] [--jobs 8]

Regenerates and builds the recompiled code with the fixture runner when
--build is given (or the runner is missing), then runs every fixture in
RUN_DIR/fixtures: the recompiled function starts from the captured entry
state and must reproduce the captured return state.

Checked strictly: all RAM, the scratchpad, v0 and the registers the MIPS
calling convention preserves (s0-s7, gp, sp, fp, ra). Differences in other
registers are reported but not failures (BIOS code and the C library
leave different values in scratch registers). Fixtures captured while an
interrupt ran are reported separately: their handlers changed memory too.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fixture_io  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/recomp"
RUNTIME = Path(__file__).resolve().parent / "runtime"
CORE = ROOT / "src/runtime"  # psx_cpu.h, gte.c
DEFAULT_CC = os.environ.get("RECOMP_CC", "clang")  # (llvm-mingw's)
STRICT_REGS = [2, 16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31]
NAMES = ("zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra "
         "hi lo").split()


def build(cc):
    subprocess.run([sys.executable, str(Path(__file__).parent / "recompile.py")], check=True)
    obj = BUILD / "obj"
    obj.mkdir(parents=True, exist_ok=True)
    sources = sorted((BUILD / "gen").glob("*.c")) + [RUNTIME / "runtime_stub.c", CORE / "gte.c", RUNTIME / "fixture_runner.c"]
    with ThreadPoolExecutor(8) as ex:
        procs = list(ex.map(lambda s: subprocess.run(
            [cc, "-O1", "-w", "-c", "-I", str(CORE), "-I", str(BUILD / "gen"), str(s),
             "-o", str(obj / (s.stem + ".o"))], capture_output=True, text=True), sources))
    for s, p in zip(sources, procs):
        if p.returncode:
            sys.exit(f"compile {s.name} failed:\n{p.stderr[:2000]}")
    exe = BUILD / "fixture_runner.exe"
    subprocess.run([cc, "-o", str(exe)] + [str(obj / (s.stem + ".o")) for s in sources], check=True)
    return exe


def ranges(a, b, limit=8):
    out, i, n = [], 0, len(a)
    while i < n and len(out) < limit:
        if a[i] != b[i]:
            k = i
            while k < n and a[k] != b[k]:
                k += 1
            out.append((i, k - i))
            i = k
        else:
            # skip quickly to the next difference in 4 KB blocks
            blk = i // 4096 * 4096 + 4096
            if a[i:blk] == b[i:blk]:
                i = blk
            else:
                i += 1
    return out


def run_one(exe, path):
    fx = fixture_io.read(path)
    cpu, gte, scratch, ram = fx["entry"]
    with tempfile.TemporaryDirectory() as td:
        fin, fout = Path(td) / "in.bin", Path(td) / "out.bin"
        fin.write_bytes(struct.pack("<I", fx["func"]) + struct.pack("<34I", *cpu) + struct.pack("<64I", *gte)
                        + scratch + ram)
        try:
            p = subprocess.run([str(exe), str(fin), str(fout)], capture_output=True, text=True, timeout=10)
        except subprocess.TimeoutExpired:
            # Usually a wait loop for something only an interrupt handler or
            # the hardware provides; needs the full runtime, not a fixture.
            return {"file": path.name, "func": f"{fx['func']:08X}", "interrupted": fx["interrupted"],
                    "result": "timeout"}
        if p.returncode not in (0, 2) or not fout.exists():
            return {"file": path.name, "func": f"{fx['func']:08X}", "result": "crash", "detail": p.stderr[-500:]}
        data = fout.read_bytes()
    got_cpu = struct.unpack_from("<34I", data, 0)
    got_gte = struct.unpack_from("<64I", data, 136)
    got_scratch = data[392:392 + 1024]
    got_ram = data[1416:1416 + 0x200000]
    want_cpu, want_gte, want_scratch, want_ram = fx["exit"]
    strict = [NAMES[r] for r in STRICT_REGS if got_cpu[r] != want_cpu[r]]
    loose = [NAMES[r] for r in range(34) if r not in STRICT_REGS and r not in (0, 26, 27) and got_cpu[r] != want_cpu[r]]
    ram_diff = ranges(want_ram, got_ram)
    scratch_diff = ranges(want_scratch, got_scratch)
    ok = not strict and not ram_diff and not scratch_diff
    return {"file": path.name, "func": f"{fx['func']:08X}", "interrupted": fx["interrupted"],
            "result": "pass" if ok else "fail", "unexpected_runtime_events": p.returncode == 2,
            "runtime_log": p.stderr.strip().splitlines()[:5],
            "strict_regs": {n: [f"{want_cpu[NAMES.index(n)]:08X}", f"{got_cpu[NAMES.index(n)]:08X}"] for n in strict},
            "other_regs": loose,
            "ram_diff": [[f"{0x80000000 + o:08X}", n] for o, n in ram_diff],
            "scratch_diff": [[f"{0x1F800000 + o:08X}", n] for o, n in scratch_diff],
            "gte_diff": sum(1 for x, y in zip(got_gte, want_gte) if x != y)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir", type=Path)
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--cc", default=DEFAULT_CC)
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()
    exe = BUILD / "fixture_runner.exe"
    if args.build or not exe.exists():
        exe = build(args.cc)
    files = sorted((args.run_dir / "fixtures").glob("*.fix"))
    with ThreadPoolExecutor(args.jobs) as ex:
        results = list(ex.map(lambda f: run_one(exe, f), files))
    clean = [r for r in results if not r.get("interrupted")]
    irq = [r for r in results if r.get("interrupted")]
    summary = {
        "fixtures": len(results),
        "clean": dict(Counter(r["result"] for r in clean)),
        "interrupted": dict(Counter(r["result"] for r in irq)),
        "functions_passing": len({r["func"] for r in clean if r["result"] == "pass"}),
        "functions_failing": sorted({r["func"] for r in clean if r["result"] != "pass"}),
    }
    out = args.run_dir / "test_results.json"
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps({"summary": summary, "results": results}, indent=1) + "\n")
    print(json.dumps(summary, indent=1))
    for r in clean:
        if r["result"] != "pass":
            print(r["func"], r["result"], "regs", r.get("strict_regs"), "ram", r.get("ram_diff", [])[:4],
                  "scratch", r.get("scratch_diff", [])[:3], r.get("runtime_log", [])[:2], r.get("detail", ""))
    print("->", out)


if __name__ == "__main__":
    main()
