"""Static recompiler prototype: translate the PSX executable's functions to C.

    python tools/recomp/recompile.py [--exe EXE] [--inventory docs/recomp_inventory.json]
                                     [--out build/recomp/gen] [--files 16]

Every function in the inventory becomes `void F_xxxxxxxx(CPU *c)` operating
on the register file in `CPU` and on guest memory through the accessors in
tools/recomp/runtime/psx_cpu.h. Translation rules:

- Branch delay slots run in hardware order: a branch condition (or a jump
  register) is read first, then the delay slot runs, then control moves.
- `jal` becomes a direct C call; `jr ra` a return.
- Switch jump tables (from the inventory) and the two run-time tables in
  the model renderers become `switch` statements over local labels.
- Other `jr`/`jalr` targets go through `rc_jump`/`rc_call`, which look up
  function starts and route A0/B0/C0 to the BIOS.
- div/divu follow the R3000's results for division by zero and overflow.
- Load delay slots are not modeled (compiled code does not rely on them).

Overrides (--overrides, JSON {"8014510C": "hle_VSync", ...}) replace a
function's body with a call to a host function `void name(CPU *c)` that the
runtime provides; callers and the dispatch table are unchanged. A value
"wrap:name" keeps the original as `F_xxxxxxxx_orig` for the host function
to call (hooks around a function).

Writes C files, a dispatch table (dispatch.c) and a list of functions.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mips import Insn  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
EXTRA_FUNCTIONS = {0x8015FCA0: 0x8015FCE4}  # missed by IDA; in the handler table at 800D1220


def R(n):
    return "0" if n == 0 else f"R[{n}]"


def W(n, expr):
    """Assignment to a register; writes to $zero are dropped (expression still evaluated)."""
    return f"(void)({expr});" if n == 0 else f"R[{n}] = {expr};"


EA_DELTA = {}  # --rebase: instruction -> how far the array it addresses moved


def ea_of(i):
    d = EA_DELTA.get(i.addr)
    if d:  # an address in a moved array (tools/recomp/rebase.py)
        return f"R[{i.rs}] + {i.simm} + 0x{d & 0xFFFFFFFF:08X}u /* moved */" if i.rs else f"0x{(i.simm + d) & 0xFFFFFFFF:08X}u"
    return f"R[{i.rs}] + {i.simm}" if i.rs else f"(uint32_t){i.simm}"


def load(i):
    ea = ea_of(i)
    fn = {"lb": "(uint32_t)(int32_t)(int8_t)mem_r8", "lbu": "mem_r8", "lh": "(uint32_t)(int32_t)(int16_t)mem_r16",
          "lhu": "mem_r16", "lw": "mem_r32"}
    if i.name in fn:
        return W(i.rt, f"{fn[i.name]}(c, {ea})")
    if i.name == "lwl":
        return W(i.rt, f"mem_lwl(c, {ea}, {R(i.rt)})")
    if i.name == "lwr":
        return W(i.rt, f"mem_lwr(c, {ea}, {R(i.rt)})")
    if i.name == "lwc2":
        return f"gte_write_data(c, {i.rt}, mem_r32(c, {ea}));"
    raise ValueError(i.name)


def store(i):
    ea = ea_of(i)
    fn = {"sb": "mem_w8", "sh": "mem_w16", "sw": "mem_w32", "swl": "mem_swl", "swr": "mem_swr"}
    if i.name in fn:
        return f"{fn[i.name]}(c, {ea}, {R(i.rt)});"
    if i.name == "swc2":
        return f"gte_store_data(c, {ea}, {i.rt});"  # (gte.c: precise vertices kept by address)
    raise ValueError(i.name)


def simple(i):
    """C for a non-control-flow instruction."""
    op, rs, rt, rd = i.op, i.rs, i.rt, i.rd
    if op == 0:
        f = i.funct
        sh = i.sa
        table = {
            0x00: lambda: W(rd, f"{R(rt)} << {sh}"),
            0x02: lambda: W(rd, f"{R(rt)} >> {sh}"),
            0x03: lambda: W(rd, f"(uint32_t)((int32_t){R(rt)} >> {sh})"),
            0x04: lambda: W(rd, f"{R(rt)} << ({R(rs)} & 31)"),
            0x06: lambda: W(rd, f"{R(rt)} >> ({R(rs)} & 31)"),
            0x07: lambda: W(rd, f"(uint32_t)((int32_t){R(rt)} >> ({R(rs)} & 31))"),
            0x10: lambda: W(rd, "c->hi"),
            0x11: lambda: f"c->hi = {R(rs)};",
            0x12: lambda: W(rd, "c->lo"),
            0x13: lambda: f"c->lo = {R(rs)};",
            0x18: lambda: f"{{ int64_t p = (int64_t)(int32_t){R(rs)} * (int32_t){R(rt)}; "
                          f"c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}",
            0x19: lambda: f"{{ uint64_t p = (uint64_t){R(rs)} * {R(rt)}; "
                          f"c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}",
            0x1A: lambda: f"cpu_div(c, {R(rs)}, {R(rt)});",
            0x1B: lambda: f"cpu_divu(c, {R(rs)}, {R(rt)});",
            0x20: lambda: W(rd, f"{R(rs)} + {R(rt)}"),
            0x21: lambda: W(rd, f"{R(rs)} + {R(rt)}"),
            0x22: lambda: W(rd, f"{R(rs)} - {R(rt)}"),
            0x23: lambda: W(rd, f"{R(rs)} - {R(rt)}"),
            0x24: lambda: W(rd, f"{R(rs)} & {R(rt)}"),
            0x25: lambda: W(rd, f"{R(rs)} | {R(rt)}"),
            0x26: lambda: W(rd, f"{R(rs)} ^ {R(rt)}"),
            0x27: lambda: W(rd, f"~({R(rs)} | {R(rt)})"),
            0x2A: lambda: W(rd, f"(int32_t){R(rs)} < (int32_t){R(rt)}"),
            0x2B: lambda: W(rd, f"{R(rs)} < {R(rt)}"),
            0x0C: lambda: f"rc_syscall(c, 0x{i.addr:08X}u);",
            0x0D: lambda: f"rc_break(c, 0x{i.addr:08X}u, {(i.word >> 6) & 0xFFFFF}u);",
        }
        if f in table:
            return table[f]()
        if i.word == 0:
            return ";"
        # no R3000 instruction (MIPS II traps, 64-bit shifts, ...): a data word inside a function, never run (crt0's
        # stack word after its break: 0x0096A530 since decomp request 89, 0x0096B078 since request 96)
        return f"rc_break(c, 0x{i.addr:08X}u, 0x{i.word:08X}u);"
    if op in (0x08, 0x09):
        return W(rt, f"{R(rs)} + 0x{(i.simm + EA_DELTA.get(i.addr, 0)) & 0xFFFFFFFF:08X}u")
    if op == 0x0A:
        return W(rt, f"(int32_t){R(rs)} < {i.simm}")
    if op == 0x0B:
        return W(rt, f"{R(rs)} < 0x{i.simm & 0xFFFFFFFF:08X}u")
    if op == 0x0C:
        return W(rt, f"{R(rs)} & 0x{i.imm:04X}u")
    if op == 0x0D:
        if EA_DELTA.get(i.addr):
            return W(rt, f"({R(rs)} | 0x{i.imm:04X}u) + 0x{EA_DELTA[i.addr] & 0xFFFFFFFF:08X}u /* moved */")
        return W(rt, f"{R(rs)} | 0x{i.imm:04X}u")
    if op == 0x0E:
        return W(rt, f"{R(rs)} ^ 0x{i.imm:04X}u")
    if op == 0x0F:
        return W(rt, f"0x{i.imm << 16:08X}u")
    if i.kind == "load":
        return load(i)
    if i.kind == "store":
        return store(i)
    if op == 0x10:
        if i.name == "mfc0":
            return W(rt, f"cop0_read(c, {rd})")
        if i.name == "mtc0":
            return f"cop0_write(c, {rd}, {R(rt)});"
        if i.name == "rfe":
            return "cop0_rfe(c);"
    if op == 0x12:
        if i.name == "mfc2":
            return W(rt, f"gte_read_data(c, {rd})")
        if i.name == "cfc2":
            return W(rt, f"gte_read_ctrl(c, {rd})")
        if i.name == "mtc2":
            return f"gte_write_data(c, {rd}, {R(rt)});"
        if i.name == "ctc2":
            return f"gte_write_ctrl(c, {rd}, {R(rt)});"
        if i.kind == "gte":
            return f"gte_command(c, 0x{i.word & 0x1FFFFFF:07X}u); /* {i.name} */"
    raise ValueError(f"unhandled {i.name} ({i.word:08X}) at {i.addr:08X}")


def cond(i):
    rs, rt = R(i.rs), R(i.rt)
    return {"beq": f"{rs} == {rt}", "bne": f"{rs} != {rt}", "blez": f"(int32_t){rs} <= 0",
            "bgtz": f"(int32_t){rs} > 0", "bltz": f"(int32_t){rs} < 0", "bgez": f"(int32_t){rs} >= 0",
            "bltzal": f"(int32_t){rs} < 0", "bgezal": f"(int32_t){rs} >= 0"}[i.name]


def translate(fn, size, word, tables, runtime_tables, starts):
    end = fn + size
    ins = {a: Insn(a, word(a)) for a in range(fn, end, 4)}
    labels = set()
    for i in ins.values():
        if i.kind in ("branch", "branchlink", "j") and fn <= i.target < end:
            labels.add(i.target)
    for site, tgts in list(tables.items()) + list(runtime_tables.items()):
        if fn <= site < end:
            labels.update(t for t in tgts if fn <= t < end)
    out = [f"void F_{fn:08X}(CPU *c)", "{", "    uint32_t *R = c->r;", f"    RC_ENTER(0x{fn:08X}u);"]
    a = fn
    while a < end:
        i = ins[a]
        if a in labels:
            out.append(f"L_{a:08X}:;")
        d = ins.get(a + 4)
        delay = simple(d) if d is not None and i.kind in ("branch", "branchlink", "j", "jal", "jr", "jalr") else None
        if delay is not None and a + 4 in labels:
            # Another branch targets this delay slot: give it its own entry,
            # which runs the instruction and falls through to a + 8.
            entry = f"    if (0) {{ L_{a + 4:08X}:; {delay} }}"
        else:
            entry = None
        if i.kind in ("branch", "branchlink"):
            link = f" R[31] = 0x{a + 8:08X}u;" if i.kind == "branchlink" else ""
            if fn <= i.target < end and i.kind == "branch":
                out.append(f"    {{ int t = {cond(i)}; {delay} if (t) goto L_{i.target:08X}; }}")
            else:  # branch-and-link or a branch leaving the function: a call
                out.append(f"    {{ int t = {cond(i)};{link} {delay} if (t) rc_call(c, 0x{i.target:08X}u); }}")
            if entry:
                out.append(entry)
            a += 8
            continue
        if i.kind == "j":
            if fn <= i.target < end:
                out.append(f"    {delay} goto L_{i.target:08X};")
            else:
                out.append(f"    {delay} rc_call(c, 0x{i.target:08X}u); return;")
            if entry:
                out.append(entry)
            a += 8
            continue
        if i.kind == "jal":
            call = f"F_{i.target:08X}(c);" if i.target in starts else f"rc_call(c, 0x{i.target:08X}u);"
            out.append(f"    R[31] = 0x{a + 8:08X}u; {delay} {call}")
            if entry:
                out.append(entry)
            a += 8
            continue
        if i.kind == "jr":
            if i.rs == 31:
                out.append(f"    {delay} return;")
            elif a in tables or a in runtime_tables:
                tgts = sorted(set((tables.get(a) or runtime_tables.get(a))))
                cases = " ".join(f"case 0x{t:08X}u: goto L_{t:08X};" for t in tgts if fn <= t < end)
                out.append(f"    {{ uint32_t t = {R(i.rs)}; {delay} switch (t) {{ {cases} "
                           f"default: rc_bad_jump(c, 0x{a:08X}u, t); return; }} }}")
            else:
                out.append(f"    {{ uint32_t t = {R(i.rs)}; {delay} rc_jump(c, t); return; }}")
            if entry:
                out.append(entry)
            a += 8
            continue
        if i.kind == "jalr":
            link = f" R[{i.rd}] = 0x{a + 8:08X}u;" if i.rd else ""
            out.append(f"    {{ uint32_t t = {R(i.rs)};{link} {delay} rc_call(c, t); }}")
            if entry:
                out.append(entry)
            a += 8
            continue
        out.append(f"    {simple(i)}")
        a += 4
    out.append("}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=str(ROOT / "references/TM2-Decomp/bin/SCUS_94306R.EXE"))
    ap.add_argument("--inventory", default=str(ROOT / "docs/recomp_inventory.json"))
    ap.add_argument("--out", default=str(ROOT / "build/recomp/gen"))
    ap.add_argument("--files", type=int, default=16)
    ap.add_argument("--overrides", help="JSON map of function address to host function name")
    ap.add_argument("--rebase", help="src/rebase.json (tools/recomp/rebase.py): arrays at new addresses")
    ap.add_argument("--alt-exe", help="a second executable at the same layout (the decomp's build from its source): "
                                      "the functions of --alt-funcs from it too, as F_x_alt, for the A/B check (abcheck.c)")
    ap.add_argument("--alt-funcs", default="", help="addresses (hex, comma separated) or @file with one per line")
    ap.add_argument("--port-json", help="the decomp's TM2_SOURCE_PORT.json: its port sections (data loaded, BSS zeroed) "
                                         "for the runtime, beside the executable's image (gen/exe_image.c)")
    ap.add_argument("--addrmap", help="the decomp's addrmap.tsv for an executable at a new layout (its port variant): the "
                                      "overrides' retail addresses translated, and gen/addrmap.c for the port's own (ga())")
    args = ap.parse_args()

    raw = Path(args.exe).read_bytes()
    t_addr, t_size = struct.unpack_from("<2I", raw, 0x18)
    imm = {}
    if args.rebase:
        rb = json.loads(Path(args.rebase).read_text(encoding="utf-8"))
        EA_DELTA.update({int(k, 16): v for k, v in rb["ea_delta"].items()})
        imm = {int(k, 16): v for k, v in rb["imm"].items()}

    # the decomp variant's port sections (--port-json): storage, and the port's own code (.port_text) and its
    # constants and jump tables (.port_rodata), outside the PS-X EXE at the port base. Each from its .bin.
    port_secs = []  # (address, size, bytes or b"" for zeroed storage)
    if args.port_json:
        pj = json.loads(Path(args.port_json).read_text(encoding="utf-8"))
        bins = Path(args.port_json).parent / Path(pj["executable"]).stem
        for sec in pj.get("port_sections", []):
            n = int(sec["size"])
            if not n:
                continue
            data = b""
            if not sec.get("nobits"):
                data = (bins / (sec["name"].lstrip(".") + ".bin")).read_bytes()
                assert len(data) == n, (sec["name"], len(data), n)
            port_secs.append((int(sec["address"], 16), n, data))

    def word(a):
        if not t_addr <= a < t_addr + t_size:  # port code / its tables
            for pa, n, data in port_secs:
                if data and pa <= a < pa + n:
                    return struct.unpack_from("<I", data, a - pa)[0]
        w = struct.unpack_from("<I", raw, 0x800 + a - t_addr)[0]
        if a in imm:  # a bound of two players (rebase.py)
            w = (w & 0xFFFF0000) | (imm[a] & 0xFFFF)
        return w

    inv = json.loads(Path(args.inventory).read_text(encoding="utf-8"))
    funcs = {int(f["addr"], 16): f["size"] for f in inv["functions_table"]}
    if not args.addrmap:  # (a build at a new layout: its own lists have it, at its new address)
        for s, e in EXTRA_FUNCTIONS.items():
            funcs[s] = e - s
    starts = set(funcs)
    tables = {}
    for t in inv["jump_tables"]:
        base = int(t["table"], 16)
        tables[int(t["site"], 16)] = [word(base + 4 * k) for k in range(t["entries"])]
    runtime_tables = {int(u["site"], 16): [int(x, 16) for x in u["runtime_table_targets"]]
                      for u in inv["unresolved_jr"]}

    amap = []  # (retail start, retail end, candidate start)
    if args.addrmap:
        for line in Path(args.addrmap).read_text(encoding="utf-8").splitlines():
            f = line.split("\t")
            if len(f) >= 5 and not line.startswith("#") and f[0] != "r_start" and f[0] != f[1]:
                r0, r1, c0, c1 = (int(x, 16) for x in f[:4])
                # a function the variant rewrote (code_entry) may be shorter or longer than retail's: the row
                # holds over the shorter of the two (past it, the retail range or the new one has other code)
                n = min(r1 - r0, c1 - c0) if c1 > c0 else r1 - r0
                amap.append((r0, r0 + n, c0, f[4].startswith("port")))
        # the port rows (arrays it enlarged, moved out) lie inside the rows of the data around them:
        # those are split around them, so the map is a plain tiling (ga()'s binary search)
        base = sorted((r0, r1, c0) for r0, r1, c0, port in amap if not port)
        for p0, p1, pc in sorted((r0, r1, c0) for r0, r1, c0, port in amap if port):
            for i, (r0, r1, c0) in enumerate(base):
                if r0 <= p0 < r1:
                    assert p1 <= r1, f"port row {p0:08X}-{p1:08X} crosses {r0:08X}-{r1:08X}"
                    base[i:i + 1] = [x for x in ((r0, p0, c0), (p0, p1, pc), (p1, r1, c0 + p1 - r0)) if x[0] < x[1]]
                    break
            else:
                base.append((p0, p1, pc))
                base.sort()
        amap = base

    def to_new(a):
        for r0, r1, c0 in amap:
            if r0 <= a < r1:
                return c0 + a - r0
        return a

    def to_retail(a):  # (a build at a new layout: a function's retail address, which the port's C names it by)
        for r0, r1, c0 in amap:
            if c0 <= a < c0 + (r1 - r0):
                return a - c0 + r0
        return a

    def orig_name(a):  # a wrap's original, by its retail name: our C calls it so, whatever is at that address now
        return f"F_{to_retail(a):08X}_orig"

    overrides = {}
    if args.overrides:
        overrides = {to_new(int(k, 16)): v for k, v in json.loads(Path(args.overrides).read_text(encoding="utf-8")).items()
                     if not k.startswith("_")}
        unknown = [f"{a:08X}" for a in overrides if a not in funcs]
        if unknown:
            sys.exit(f"overrides for unknown functions: {unknown}")

    alt = set()
    alt_word = None
    if args.alt_exe and args.alt_funcs:
        spec = args.alt_funcs
        if spec.startswith("@"):
            spec = ",".join(line.split("#")[0].strip() for line in Path(spec[1:]).read_text(encoding="utf-8").splitlines())
        alt = {int(x, 16) for x in spec.split(",") if x.strip()}
        bad = sorted(a for a in alt if a not in funcs or a in overrides)
        if bad:
            sys.exit("--alt-funcs: unknown or overridden functions: " + ", ".join(f"{a:08X}" for a in bad))
        alt_raw = Path(args.alt_exe).read_bytes()
        if struct.unpack_from("<2I", alt_raw, 0x18) != (t_addr, t_size):
            sys.exit("--alt-exe: not the same layout (load address and size)")

        def alt_word(a):
            return struct.unpack_from("<I", alt_raw, 0x800 + a - t_addr)[0]

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    order = sorted(funcs)
    per = (len(order) + args.files - 1) // args.files
    proto = "\n".join(f"void F_{a:08X}(CPU *c);" for a in order)
    hosts = sorted({v.split(":", 1)[-1] for v in overrides.values()})
    proto += "\n" + "\n".join(f"void {n}(CPU *c);" for n in hosts)
    proto += "\n" + "\n".join(f"void {orig_name(a)}(CPU *c);" for a, v in sorted(overrides.items())
                              if v.startswith("wrap:"))
    proto += "\n" + "\n".join(f"void F_{a:08X}_base(CPU *c);\nvoid F_{a:08X}_alt(CPU *c);" for a in sorted(alt))
    proto += "\nint ab_enabled(void);\nvoid ab_run(CPU *c, void (*base)(CPU *), void (*alt)(CPU *), uint32_t addr);"
    (out / "funcs.h").write_text("#pragma once\n#include \"psx_cpu.h\"\n" + proto + "\n", encoding="utf-8", newline="\n")
    errors = 0
    for k in range(args.files):
        chunk = order[k * per:(k + 1) * per]
        if not chunk:
            break
        parts = ['#include "funcs.h"', ""]
        for a in chunk:
            if a in overrides:
                host = overrides[a].split(":", 1)[-1]
                parts.append(f"void F_{a:08X}(CPU *c) {{ {host}(c); }}")
                parts.append("")
                if not overrides[a].startswith("wrap:"):
                    continue
                parts.append(translate(a, funcs[a], word, tables, runtime_tables, starts)
                             .replace(f"void F_{a:08X}(CPU *c)", f"void {orig_name(a)}(CPU *c)", 1))
                parts.append("")
                continue
            if a in alt:  # the A/B check (abcheck.c): this one, or both compared
                parts.append(f"void F_{a:08X}(CPU *c) {{ if (ab_enabled()) ab_run(c, F_{a:08X}_base, F_{a:08X}_alt, 0x{a:08X}u); "
                             f"else F_{a:08X}_base(c); }}")
                parts.append("")
                parts.append(translate(a, funcs[a], word, tables, runtime_tables, starts)
                             .replace(f"void F_{a:08X}(CPU *c)", f"void F_{a:08X}_base(CPU *c)", 1))
                parts.append("")
                continue
            try:
                parts.append(translate(a, funcs[a], word, tables, runtime_tables, starts))
            except ValueError as e:
                errors += 1
                print(f"F_{a:08X}: {e}")
                parts.append(f"void F_{a:08X}(CPU *c) {{ rc_untranslated(c, 0x{a:08X}u); }}")
            parts.append("")
        (out / f"funcs_{k:02d}.c").write_text("\n".join(parts), encoding="utf-8", newline="\n")
    alt_parts = ['#include "funcs.h"', "", "/* the --alt-exe versions (the A/B check) */", ""]
    for a in sorted(alt):
        alt_tables = {site: [alt_word(int(t["table"], 16) + 4 * k) for k in range(t["entries"])]
                      for t in inv["jump_tables"] for site in [int(t["site"], 16)]}
        alt_parts.append(translate(a, funcs[a], alt_word, alt_tables, runtime_tables, starts)
                         .replace(f"void F_{a:08X}(CPU *c)", f"void F_{a:08X}_alt(CPU *c)", 1))
        alt_parts.append("")
    (out / "funcs_alt.c").write_text("\n".join(alt_parts), encoding="utf-8", newline="\n")
    am = ['#include <stdint.h>', "", "/* the retail address map (--addrmap): retail start, end, new start; none: the retail layout */",
          "const uint32_t rc_addrmap[][3] = {"]
    am += [f"    {{0x{r0:08X}u, 0x{r1:08X}u, 0x{c0:08X}u}}," for r0, r1, c0 in amap] or ["    {0, 0, 0},"]
    am += ["};", f"const unsigned rc_addrmap_count = {len(amap)};", ""]
    (out / "addrmap.c").write_text("\n".join(am), encoding="utf-8", newline="\n")
    # a build at a new layout boots from its own image (the disc's is retail's), with the port sections
    img = ['#include <stdint.h>', "", "/* the executable's image when it is not the disc's (a build at a new layout) */"]
    if amap:
        img.append(f"const unsigned rc_exe_image_size = {len(raw)};")
        img.append("const uint8_t rc_exe_image[] = {")
        img += ["    " + ",".join(str(b) for b in raw[k:k + 64]) + "," for k in range(0, len(raw), 64)]
        img.append("};")
    else:
        img += ["const unsigned rc_exe_image_size = 0;", "const uint8_t rc_exe_image[1] = {0};"]
    secs = port_secs  # (the port code's bytes too: harmless in RAM, its .port_rodata needed there)
    img.append("/* the port sections: address, size, data (none: zeroed) */")
    for k, (a, n, d) in enumerate(secs):
        img.append(f"static const uint8_t port_sec_{k}[] = {{{','.join(str(b) for b in d) or '0'}}};")
    img.append("const struct { uint32_t addr, size, has_data; const uint8_t *data; } rc_port_sections[] = {")
    img += [f"    {{0x{a:08X}u, {n}u, {1 if d else 0}u, port_sec_{k}}}," for k, (a, n, d) in enumerate(secs)] or ["    {0, 0, 0, 0},"]
    img += ["};", f"const unsigned rc_port_section_count = {len(secs)};"]
    # the port's settings block for its own changes in the game's C (decomp/ overlay, -DTM2_PORT_HOOKS): 0 if none
    pcfg = 0
    if args.port_json:
        pcfg = next((int(a["address"], 16) for a in pj.get("port_arrays", []) if a["name"] == "portConfig"), 0)
    # the rows of the view-mode table in a build at a new layout: the port array's (the decomp's variant has 16), or retail's 9
    vrows = 9
    if args.port_json:
        vrows = next((int(a["size"]) // 96 for a in pj.get("port_arrays", []) if a["name"] == "viewModeTable"), 9)
    # the far-LOD switch (decomp FAR_LOD.md, request 30): 0 if none
    flod = 0
    if args.port_json:
        flod = next((int(a["address"], 16) for a in pj.get("port_arrays", []) if a["name"] == "portFarLod"), 0)
    # the rollback snapshot's and hash's spans of guest RAM (decomp ONLINE_ROLLBACK.md 12.4, the build's
    # rollback_regions.tsv): the ranges, minus the "no" rows inside them, plus the "yes" rows outside them; the BIOS
    # rand() seed (0x80009010, the port's HLE: the game's stream) in both
    rb_spans = {'snap': [], 'hash': []}
    rb_ptrs = []  # the `ptr` rows (request 57): a pointer word, and the bytes at the object it names that are hash no
    if args.port_json:
        rr = Path(args.port_json).with_suffix('') / 'rollback_regions.tsv'
        if rr.exists():
            rows = []
            for line in rr.read_text(encoding='utf-8').splitlines():
                f = line.split('\t')
                if line.startswith('#') or len(f) < 6 or f[0] == 'address' or f[3] == 'dmd':
                    continue
                if f[3] == 'ptr':
                    if f[5] != 'yes':
                        rb_ptrs.append((int(f[0], 16), int(f[2].rsplit(':', 1)[1], 16)))
                    continue
                a, n = int(f[0], 16), int(f[1], 16)
                if n and 0x80000000 <= a < 0x80C00000:
                    rows.append((a, n, f[3], f[4] == 'yes', f[5] == 'yes'))
            for col, key in ((3, 'snap'), (4, 'hash')):
                ranges = [(a, a + n) for a, n, kind, *yes in rows if kind == 'range' and yes[col - 3]]
                inside = lambda a: any(r0 <= a < r1 for r0, r1 in ranges)
                spans = sorted(ranges)
                holes = sorted((a, a + n) for a, n, kind, *yes in rows if kind != 'range' and not yes[col - 3] and inside(a))
                extra = sorted((a, a + n) for a, n, kind, *yes in rows if kind != 'range' and yes[col - 3] and not inside(a))
                spans_out = []
                for r0, r1 in spans:
                    at = r0
                    for h0, h1 in holes:
                        if h1 <= at or h0 >= r1:
                            continue
                        if h0 > at:
                            spans_out.append((at, h0))
                        at = max(at, h1)
                    if at < r1:
                        spans_out.append((at, r1))
                spans_out = sorted(spans_out + extra + [(0x80009010, 0x80009014)])
                merged = []
                for a0, a1 in spans_out:
                    if merged and a0 <= merged[-1][1]:
                        merged[-1] = (merged[-1][0], max(merged[-1][1], a1))
                    else:
                        merged.append((a0, a1))
                rb_spans[key] = merged
    # port functions the host calls (the interpolated frames: portViewsPresent): from the build's symbols.tsv, 0 if none
    pfun = {}
    psyms = {}  # the port's storage and functions in the build (decomp request 32: car groups, 16 cars, the swap), by name
    if args.port_json:
        sym = Path(args.port_json).with_suffix('') / 'symbols.tsv'
        if sym.exists():
            for line in sym.read_text(encoding='utf-8').splitlines():
                f = line.split('\t')
                if len(f) >= 3 and f[2] in ('portViewsPresent',):
                    pfun[f[2]] = int(f[0], 16)
                if len(f) >= 3 and not line.startswith('#') and (f[2].startswith('port') or f[2] in ('sessionPortOpponentCars', 'sessionPortHumanCars')):
                    psyms[f[2]] = int(f[0], 16)
    # the port's own functions in the build (functions.tsv kind port: its .port_text, the retail bodies the variant moved
    # there, e.g. weaponTornadoUpdateOne): by address range, for return-address matches (the kill feed's attacks)
    pfuncs = []
    if args.port_json:
        ft = Path(args.port_json).with_suffix('') / 'functions.tsv'
        if ft.exists():
            for line in ft.read_text(encoding='utf-8').splitlines():
                f = line.split('\t')
                if len(f) >= 7 and not line.startswith('#') and f[6] == 'port':
                    pfuncs.append((int(f[0], 16), int(f[2]), f[4]))
    # the computer-car records the build has: the port array aiCars (0x854 each), else retail's 9
    ai_max = 9
    if args.port_json:
        ai_max = next((int(a["size"]) // 0x854 for a in pj.get("port_arrays", []) if a["name"] == "aiCars"), 9)
    # a HUD's radar record (0x70 + 0x40 per blip: retail 12 blips; the variant's TM2_MAX_RADAR_BLIPS)
    radar_bytes = 0x370
    pair_stride = 15  # the car-pair tables' row (retail: 15 slots; the variant's TM2_MAX_CAR_SLOTS)
    projectiles = 16  # fxRecords (980 bytes each): retail 16, the variant's TM2_MAX_PROJECTILES
    if args.port_json:
        projectiles = next((int(a["entries"]) for a in pj.get("port_arrays", []) if a["name"] == "fxRecords"), 16)
    shields = 4  # weaponShieldPool's entries: { objects[N]; records[N] } (retail 4; the variant's one per car)
    if args.port_json:
        shields = next((int(a["entries"]) for a in pj.get("port_arrays", []) if a["name"] == "weaponShieldPool"), 4)
    if args.port_json:
        pair_stride = next((int(round(int(a["entries"]) ** 0.5)) for a in pj.get("port_arrays", []) if a["name"] == "carPairVectors"), 15)
    if args.port_json:
        radar_bytes = next((int(a["entry_bytes"]) for a in pj.get("port_arrays", []) if a["name"] == "hudRadars"), 0x370)
    img += [f"const uint32_t rc_port_config = 0x{pcfg:08X}u;", f"const unsigned rc_view_mode_rows = {vrows};",
            f"const uint32_t rc_port_far_lod = 0x{flod:08X}u;",
            f"const uint32_t rc_port_views_present = 0x{pfun.get('portViewsPresent', 0):08X}u;",
            f"const unsigned rc_ai_cars_max = {ai_max};",
            f"const unsigned rc_hud_radar_bytes = 0x{radar_bytes:X}u;",
            f"const unsigned rc_car_pair_stride = {pair_stride};",
            f"const unsigned rc_shield_count = {shields};",
            f"const unsigned rc_projectile_count = {projectiles};",
            "const struct { const char *name; uint32_t addr; } rc_port_symbols[] = {"]
    img += [f'    {{"{n}", 0x{a:08X}u}},' for n, a in sorted(psyms.items())] or ['    {"", 0},']
    img += ["};", f"const unsigned rc_port_symbol_count = {len(psyms)};",
            "const struct { const char *name; uint32_t addr, size; } rc_port_functions[] = {"]
    img += [f'    {{"{n}", 0x{a:08X}u, {sz}u}},' for a, sz, n in sorted(pfuncs)] or ['    {"", 0, 0},']
    img += ["};", f"const unsigned rc_port_function_count = {len(pfuncs)};"]
    img += ["const struct { uint32_t addr, size; } rc_rb_ptrs[] = {"]
    img += [f"    {{0x{a:08X}u, 0x{n:X}u}}," for a, n in rb_ptrs] or ["    {0, 0},"]
    img += ["};", f"const unsigned rc_rb_ptrs_count = {len(rb_ptrs)};"]
    for key in ('snap', 'hash'):
        img += [f"const struct {{ uint32_t addr, size; }} rc_rb_{key}[] = {{"]
        img += [f"    {{0x{a0:08X}u, 0x{a1 - a0:X}u}}," for a0, a1 in rb_spans[key]] or ["    {0, 0},"]
        img += ["};", f"const unsigned rc_rb_{key}_count = {len(rb_spans[key])};"]
    img.append("")
    (out / "exe_image.c").write_text("\n".join(img), encoding="utf-8", newline="\n")
    disp = ['#include "funcs.h"', "", "const RcFunction rc_functions[] = {"]
    disp += [f"    {{0x{a:08X}u, F_{a:08X}}}," for a in order]
    disp += ["};", f"const unsigned rc_function_count = {len(order)};", ""]
    if amap:  # the port's C names functions by their retail address: those names for the moved ones
        moved = {}
        for r0, r1, c0 in amap:
            if r0 != c0:
                for a in order:
                    if c0 <= a < c0 + (r1 - r0) and a - c0 + r0 not in funcs:
                        moved[a - c0 + r0] = a
        disp.append("/* retail names of moved functions (the port's C calls some by them) */")
        for r, a in sorted(moved.items()):
            disp.append(f"void F_{r:08X}(CPU *c) {{ F_{a:08X}(c); }}")
        disp.append("")
    (out / "dispatch.c").write_text("\n".join(disp), encoding="utf-8", newline="\n")
    print(f"{len(order)} functions -> {out} ({args.files} files), {errors} untranslated, "
          f"{len(overrides)} overridden")


if __name__ == "__main__":
    main()
