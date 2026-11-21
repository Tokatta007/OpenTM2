"""Static inventory of a PlayStation executable for static recompilation.

    python tools/recomp/inventory.py [--exe EXE] [--funcs ida_funcs.tsv] [--names ida_names.tsv]
                                     [--json docs/recomp_inventory.json]

Defaults read Twisted Metal 2 (SCUS-94306) from references/TM2-Decomp:
bin/SCUS_94306R.EXE and the IDA function and name lists in config/US/.

Reports what a recompiler and its runtime have to handle: function
boundaries and gaps, call targets that are not known functions, branches
that leave their function, switch jump tables, other indirect jumps and
calls, functions whose address is taken (callbacks, pointer tables), BIOS
calls, hardware register access, GTE and COP0 use, callback registrations,
and the sources of randomness and time. IDA names are hints only; the code
decides. Prints a summary and writes the details as JSON.
"""
import argparse
import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mips import Insn, RA, has_delay_slot  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DECOMP = ROOT / "references/TM2-Decomp"

# BIOS function names by table and number (psx-spx, "BIOS Function Summary").
BIOS = {
    0xA0: {0x00: "open", 0x01: "lseek", 0x02: "read", 0x03: "write", 0x04: "close", 0x05: "ioctl",
           0x06: "exit", 0x08: "getc", 0x0E: "abs", 0x0F: "labs", 0x09: "putc", 0x13: "setjmp", 0x14: "longjmp", 0x15: "strcat",
           0x16: "strncat", 0x17: "strcmp", 0x18: "strncmp", 0x19: "strcpy", 0x1A: "strncpy",
           0x1B: "strlen", 0x1C: "index", 0x1D: "rindex", 0x1E: "strchr", 0x1F: "strrchr",
           0x25: "toupper", 0x26: "tolower", 0x27: "bcopy", 0x28: "bzero", 0x29: "bcmp",
           0x2A: "memcpy", 0x2B: "memset", 0x2C: "memmove", 0x2D: "memcmp", 0x2E: "memchr",
           0x2F: "rand", 0x30: "srand", 0x31: "qsort", 0x33: "malloc", 0x34: "free",
           0x37: "calloc", 0x38: "realloc", 0x39: "InitHeap", 0x3A: "_exit", 0x3F: "printf",
           0x41: "LoadTest", 0x42: "Load", 0x43: "Exec", 0x44: "FlushCache", 0x47: "gpu_send_dma",
           0x48: "SendGP1Command", 0x49: "GPU_cw", 0x4A: "GPU_cwp", 0x4B: "send_gpu_linked_list",
           0x4D: "GetGPUStatus", 0x70: "_bu_init", 0x71: "CdInit", 0x72: "CdRemove", 0x78: "CdAsyncSeekL",
           0x7C: "CdAsyncGetStatus", 0x7E: "CdAsyncReadSector", 0x81: "CdAsyncSetMode",
           0x9F: "SetMemSize", 0xA0: "WarmBoot", 0xA1: "SystemErrorBootOrDiskFailure",
           0xA2: "EnqueueCdIntr", 0xA3: "DequeueCdIntr", 0xA7: "bu_callback_okay",
           0xAB: "_card_info", 0xAC: "_card_async_load_directory"},
    0xB0: {0x00: "alloc_kernel_memory", 0x01: "free_kernel_memory", 0x02: "init_timer",
           0x03: "get_timer", 0x04: "enable_timer_irq", 0x05: "disable_timer_irq",
           0x06: "restart_timer", 0x07: "DeliverEvent", 0x08: "OpenEvent", 0x09: "CloseEvent",
           0x0A: "WaitEvent", 0x0B: "TestEvent", 0x0C: "EnableEvent", 0x0D: "DisableEvent",
           0x0E: "OpenThread", 0x0F: "CloseThread", 0x10: "ChangeThread", 0x12: "InitPad",
           0x13: "StartPad", 0x14: "StopPad", 0x15: "OutdatedPadInitAndStart",
           0x16: "OutdatedPadGetButtons", 0x17: "ReturnFromException",
           0x18: "SetDefaultExitFromException", 0x19: "SetCustomExitFromException",
           0x20: "UnDeliverEvent", 0x32: "open", 0x33: "lseek", 0x34: "read", 0x35: "write",
           0x36: "close", 0x37: "ioctl", 0x38: "exit", 0x3F: "puts", 0x42: "firstfile",
           0x43: "nextfile", 0x44: "rename", 0x45: "erase", 0x46: "undelete", 0x47: "AddDrv",
           0x48: "DelDrv", 0x4A: "InitCard", 0x4B: "StartCard", 0x4C: "StopCard",
           0x4E: "write_card_sector", 0x4F: "read_card_sector", 0x50: "allow_new_card",
           0x56: "GetC0Table", 0x57: "GetB0Table", 0x5B: "ChangeClearPad"},
    0xC0: {0x00: "EnqueueTimerAndVblankIrqs", 0x01: "EnqueueSyscallHandler", 0x02: "SysEnqIntRP",
           0x03: "SysDeqIntRP", 0x07: "InstallExceptionHandlers", 0x08: "SysInitMemory",
           0x0A: "ChangeClearRCnt", 0x0C: "InitDefInt", 0x12: "InstallDevices"},
}

# Hardware register blocks (psx-spx "Memory Map").
HW = [
    (0x1F801000, 0x1F801024, "memctrl"), (0x1F801040, 0x1F801050, "pad/memcard (SIO0)"),
    (0x1F801050, 0x1F801060, "serial (SIO1)"), (0x1F801060, 0x1F801064, "ram size"),
    (0x1F801070, 0x1F801078, "interrupt control"), (0x1F801080, 0x1F801100, "DMA"),
    (0x1F801100, 0x1F801130, "timers"), (0x1F801800, 0x1F801804, "CD-ROM"),
    (0x1F801810, 0x1F801818, "GPU"), (0x1F801820, 0x1F801828, "MDEC"),
    (0x1F801C00, 0x1F802000, "SPU"), (0x1F802000, 0x1F803000, "expansion 2"),
    (0x1F800000, 0x1F800400, "scratchpad"), (0xFFFE0130, 0xFFFE0134, "cache control"),
]

# Library guesses from IDA names (prefix or exact). SDK code is verified by
# address clustering in the report, not by these names alone.
LIBS = [
    ("libgpu", ("DrawOTag", "LoadImage", "StoreImage", "MoveImage", "ClearImage", "ClearOTag", "DrawPrim",
                "PutDrawEnv", "GetDrawEnv", "PutDispEnv", "GetDispEnv", "SetDraw", "SetDisp", "SetTexWindow",
                "SetDefDrawEnv", "SetDefDispEnv", "LoadTPage", "LoadClut", "GetTPage", "GetClut", "Dump",
                "AddPrim", "TermPrim", "SetSemiTrans", "SetShadeTex", "ResetGraph", "SetGraph", "DrawSync",
                "checkRECT", "FntLoad", "GetODE", "_otc", "_clr", "_dws", "_drs", "_ctl", "_getctl", "_cwb",
                "_cwc", "_param", "_addque", "_exeque", "_reset", "_sync", "set_alarm", "get_alarm",
                "_version", "_status", "get_mode", "get_ofs", "get_tw", "get_dx", "GPU_cw")),
    ("libgte", ("RotMatrix", "rsin", "rcos", "sin_1", "EigenMatrix", "IsIdMatrix", "MatrixNormal", "InitGeom",
                "VectorNormal", "SetRotMatrix", "SetLightMatrix", "SetColorMatrix", "SetTransMatrix",
                "RotTrans", "NormalColor", "OuterProduct", "TransposeMatrix", "ratan2", "_patch_gte",
                "MulRotMatrix", "SetBackColor", "SetGeomOffset", "SetGeomScreen")),
    ("libgs", ("GsGet",)),
    ("libetc", ("VSync", "v_wait", "ResetCallback", "InterruptCallback", "DMACallback", "VSyncCallback",
                "StopCallback", "RestartCallback", "CheckCallback", "GetIntrMask", "SetIntrMask",
                "startIntr", "trapIntr", "setIntr", "stopIntr", "restartIntr", "PadInit", "PadRead")),
    ("libcd", ("Cd", "CD_", "cd_read", "cb_read", "getintr", "_cmp", "def_cbsync", "callback")),
    ("libcd (stream)", ("St", "data_ready_callback", "init_ring_status")),
    ("libpress", ("DecDCT", "MDEC_", "timeout", "ivlc_")),
    ("libspu", ("Spu", "_spu", "_Spu")),
    ("libsnd", ("Ss", "_Ss", "SpuVm", "vmNoise", "note2pitch", "SePitchBend", "SetAutoVol", "SetAutoPan",
                "SeVib", "SetVib")),
    ("libapi", ("InitPAD", "StartPAD", "StopPAD", "ChangeClear", "EnterCritical", "ExitCritical",
                "ReturnFromException", "ResetEntryInt", "HookEntryInt", "FlushCache", "OpenEvent",
                "CloseEvent", "EnableEvent", "DisableEvent", "TestEvent", "DeliverEvent", "SetRCnt",
                "GetRCnt", "StartRCnt", "StopRCnt", "ResetRCnt", "setjmp", "InitHeap")),
    ("libc", ("memcpy", "memset", "memmove", "memchr", "strcpy", "strlen", "strcmp", "strncmp", "rand",
              "srand", "printf", "sprintf", "abs", "free", "exit", "__main", "__builtin_delete")),
    ("startup", ("start",)),
    ("sdk (other)", ("mem2mem", "dma_execute")),
]

CALLBACK_SETTERS = {  # name -> argument register index that holds the handler
    "VSyncCallback": 0, "VSyncCallbacks": 1, "DrawSyncCallback": 0, "DMACallback": 1,
    "InterruptCallback": 1, "CdDataCallback": 0, "DecDCToutCallback": 0, "StSetRing": None,
    "OpenEvent": 3, "SysEnqIntRP": 1, "CdSyncCallback": 0, "CdReadyCallback": 0, "CdReadCallback": 0,
}


def lib_of(name):
    if not name or name.startswith(("sub_", "nullsub_", "loc_")):
        return None
    base = name.rsplit("_", 1)[0] if name.rsplit("_", 1)[-1].isdigit() else name
    for lib, keys in LIBS:
        for k in keys:
            if base == k or (base.startswith(k) and len(k) >= 2 and not k.islower()) or base == k:
                return lib
            if k.startswith("_") and base.startswith(k):
                return lib
    return None


def load_tsv(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        next(f)
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3:
                rows.append((int(parts[0], 16), int(parts[1]), parts[2]))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=str(DECOMP / "bin/SCUS_94306R.EXE"))
    ap.add_argument("--funcs", default=str(DECOMP / "config/US/ida_funcs.tsv"))
    ap.add_argument("--names", default=str(DECOMP / "config/US/ida_names.tsv"))
    ap.add_argument("--json", default=str(ROOT / "docs/recomp_inventory.json"))
    args = ap.parse_args()

    raw = Path(args.exe).read_bytes()
    if raw[:8] != b"PS-X EXE":
        sys.exit("not a PS-X EXE")
    pc0, gp0, t_addr, t_size = struct.unpack_from("<4I", raw, 0x10)
    b_addr, b_size = struct.unpack_from("<2I", raw, 0x28)
    sp0 = struct.unpack_from("<I", raw, 0x30)[0]
    text = raw[0x800:0x800 + t_size]
    t_end = t_addr + t_size

    def word(a):
        return struct.unpack_from("<I", text, a - t_addr)[0] if t_addr <= a <= t_end - 4 else None

    funcs = sorted(load_tsv(args.funcs))
    names = load_tsv(args.names)
    fstart = {a: (n * 4, name) for a, n, name in funcs}
    starts = sorted(fstart)
    name_at = {a: n for a, _, n in names}
    for a, (_, n) in fstart.items():
        name_at.setdefault(a, n)

    def func_of(addr):
        import bisect
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < starts[i] + fstart[starts[i]][0]:
            return starts[i]
        return None

    rep = {"exe": {"path": str(Path(args.exe).name), "entry": pc0, "gp0": gp0, "text_addr": t_addr,
                   "text_size": t_size, "text_end": t_end, "bss_addr": b_addr, "bss_size": b_size,
                   "stack": sp0}}

    # ---- function boundaries and gaps
    gaps, overlaps = [], []
    for a, b in zip(starts, starts[1:]):
        end = a + fstart[a][0]
        if end < b:
            gaps.append((end, b))
        elif end > b:
            overlaps.append((a, b))
    last_end = starts[-1] + fstart[starts[-1]][0]
    # IDA defines a few 8-byte "functions" inside the data after the code.
    code_end = max(a + s for a, (s, _) in fstart.items() if s > 16)
    false_funcs = [a for a in starts if a >= code_end]
    for a in false_funcs:
        del fstart[a]
    starts = [a for a in starts if a < code_end]
    gaps = [(g0, g1) for g0, g1 in gaps if g1 <= code_end]
    code_bytes = sum(s for s, _ in fstart.values())

    # gp: the entry code loads it with lui/addiu.
    gp = None
    for x in range(pc0, pc0 + 0x200, 4):
        i = Insn(x, word(x))
        if i.rt == 28 and i.kind == "lui":
            gp = i.imm << 16
        elif i.rt == 28 and i.name == "addiu" and gp is not None and i.rs == 28:
            gp = (gp + i.simm) & 0xFFFFFFFF
            break

    # Hardware register addresses kept in data (how the SDK reaches the hardware).
    hw_vars = {}
    for x in range(code_end, t_end - 3, 4):
        v = word(x)
        for lo, hi, blk in HW:
            if lo <= v < hi and blk != "scratchpad":
                hw_vars[x] = (v, blk)

    # ---- decode every function
    per = {}
    jal_targets = Counter()
    jal_sites = defaultdict(list)
    cross_branches, unknown_calls, mid_calls = [], [], []
    jr_sites, jalr_sites = [], []
    addr_taken_code = defaultdict(set)  # function -> sites building its address
    hw_access = defaultdict(lambda: defaultdict(set))
    heap_consts = defaultdict(set)
    bios_wrappers = {}
    gte_total, gte_funcs = Counter(), {}
    cop0_funcs, syscall_funcs = {}, {}
    branch_in_delay = []
    calls_with_a = defaultdict(list)  # target -> [(site, {reg: const})]
    all_consts = defaultdict(set)
    const_tables = []
    break_codes = Counter()
    hw_via_vars = set()

    for a in starts:
        size, fname = fstart[a]
        ins = [Insn(x, word(x)) for x in range(a, a + size, 4) if word(x) is not None]
        consts = {28: gp} if gp is not None else {}
        indexed = {}   # reg -> table base (const + index)
        loaded = {}    # reg -> table address it was loaded from
        stats = Counter()
        gte = Counter()
        for k, i in enumerate(ins):
            if has_delay_slot(i) and k + 1 < len(ins) and has_delay_slot(ins[k + 1]):
                branch_in_delay.append(i.addr)
            # effective address of a load/store, before the load overwrites its base
            ea = None
            if i.kind in ("load", "store") and i.rs in consts:
                ea = (consts[i.rs] + i.simm) & 0xFFFFFFFF
            # constants: lui / addiu / ori chains
            if i.kind == "lui":
                consts[i.rt] = i.imm << 16
            elif i.name == "addiu" and i.rs == 0:
                consts[i.rt] = i.simm & 0xFFFFFFFF
            elif i.name == "addiu" and i.rs in consts:
                consts[i.rt] = (consts[i.rs] + i.simm) & 0xFFFFFFFF
            elif i.name == "ori" and i.rs in consts:
                consts[i.rt] = consts[i.rs] | i.imm
            else:
                w = i.writes()
                if i.kind == "alu" and i.op == 0 and i.funct == 0x21 and i.rd:  # addu
                    one = (i.rs in consts) != (i.rt in consts)
                    base = consts.get(i.rs, consts.get(i.rt)) if one else None
                    if base is not None and base != gp:
                        indexed[i.rd] = base
                        consts.pop(i.rd, None)
                        w = None
                elif i.name == "lw" and i.rs in indexed:
                    loaded[i.rt] = (indexed[i.rs] + i.simm) & 0xFFFFFFFF
                    consts.pop(i.rt, None)
                    w = None
                if w is not None:
                    consts.pop(w, None)
                    indexed.pop(w, None)
                    loaded.pop(w, None)
            if i.kind in ("lui", "imm") and consts.get(i.rt) is not None and i.rt != 28:
                all_consts[consts[i.rt]].add(i.addr)
            if i.kind in ("lui", "imm"):
                v = consts.get(i.rt)
                if v is not None:
                    if v in fstart and i.kind == "imm":
                        addr_taken_code[v].add(i.addr)
                    if 0x80010000 <= v < t_addr and i.kind == "imm":
                        heap_consts[v].add(i.addr)
            if ea is not None:
                all_consts[ea].add(i.addr)
                for lo, hi, blk in HW:
                    if lo <= ea < hi and blk != "scratchpad":
                        hw_access[a][blk].add(ea)
                if 0x80010000 <= ea < t_addr:
                    heap_consts[ea].add(i.addr)
                if ea in hw_vars:
                    hw_access[a][hw_vars[ea][1]].add(hw_vars[ea][0])
                    hw_via_vars.add(a)
            if i.kind == "jal":
                jal_targets[i.target] += 1
                jal_sites[i.target].append(i.addr)
                snap = {r: consts[r] for r in (4, 5, 6, 7) if r in consts}
                if k + 1 < len(ins):  # argument set in the delay slot
                    d = ins[k + 1]
                    if d.name == "addiu" and d.rs == 0:
                        snap[d.rt] = d.simm & 0xFFFFFFFF
                    elif d.name in ("addiu", "ori") and d.rs in consts:
                        snap[d.rt] = ((consts[d.rs] + d.simm) if d.name == "addiu" else (consts[d.rs] | d.imm)) & 0xFFFFFFFF
                calls_with_a[i.target].append((i.addr, snap))
                if i.target not in fstart:
                    owner = func_of(i.target)
                    (mid_calls if owner else unknown_calls).append((i.addr, i.target, owner))
                stats["calls"] += 1
            elif i.kind in ("branch", "j", "branchlink"):
                if not (a <= i.target < a + size):
                    cross_branches.append((i.addr, i.target, i.name, func_of(i.target)))
            elif i.kind == "jr":
                if i.rs != RA:
                    if i.rs == 10 and consts.get(10) in (0xA0, 0xB0, 0xC0):
                        num = None
                        d = ins[k + 1] if k + 1 < len(ins) else None
                        if d and d.name == "addiu" and d.rs == 0 and d.rt == 9:
                            num = d.imm
                        elif 9 in consts:
                            num = consts[9]
                        tbl = consts[10]
                        bios_wrappers[a] = (tbl, num, BIOS.get(tbl, {}).get(num, "?"))
                    elif i.rs in loaded:
                        const_tables.append((i.addr, a, loaded[i.rs]))
                    else:
                        jr_sites.append((i.addr, a, i.rs))
                stats["returns" if i.rs == RA else "jr"] += 1
            elif i.kind == "jalr":
                jalr_sites.append((i.addr, a, i.rs, consts.get(i.rs)))
                stats["jalr"] += 1
            elif i.kind == "gte":
                gte[i.name] += 1
                gte_total[i.name] += 1
            elif i.kind == "gte_move":
                gte["moves"] += 1
            elif i.kind == "cop0":
                cop0_funcs.setdefault(a, Counter())[i.name] += 1
            elif i.kind == "syscall":
                syscall_funcs.setdefault(a, Counter())["syscall"] += 1
            elif i.kind == "break":
                code = (i.word >> 16) & 0x3FF
                kind = {7: "divide by zero", 6: "overflow"}.get(code, f"break {code}")
                break_codes[kind] += 1
                if code not in (6, 7):
                    syscall_funcs.setdefault(a, Counter())[kind] += 1
        if gte:
            gte_funcs[a] = dict(gte)
        per[a] = {"name": fname, "size": size, "stats": dict(stats)}

    # ---- jump tables (IDA names jpt_<site>)
    jpt = {}
    for a, _, n in names:
        if n.startswith("jpt_"):
            try:
                jpt[int(n[4:], 16)] = a
            except ValueError:
                pass
    tables, unresolved = [], []
    found = [(site, fn, jpt.get(site), "ida") for site, fn, reg in jr_sites]
    found += [(site, fn, tbl, "constant") for site, fn, tbl in const_tables]
    regs = {site: reg for site, fn, reg in jr_sites}
    for site, fn, tbl, how in found:
        size = fstart[fn][0]
        if tbl is None:
            unresolved.append((site, fn, regs[site]))
            continue
        entries = []
        x = tbl
        while True:
            w = word(x)
            if w is None or not (fn <= w < fn + size) or (x != tbl and x in name_at and not name_at[x].startswith("jpt_") is False):
                break
            entries.append(w)
            x += 4
            if x in jpt.values() and x != tbl:
                break
        tables.append({"site": site, "function": fn, "table": tbl, "entries": len(entries),
                       "targets": sorted(set(entries)), "found_by": how})

    # Tables for the unresolved sites: runs of 4+ data words pointing into the function.
    candidates = {}
    for site, fn, reg in unresolved:
        size = fstart[fn][0]
        runs_, cur_ = [], []
        for x in range(code_end, t_end - 3, 4):
            if fn <= word(x) < fn + size:
                if cur_ and x != cur_[-1] + 4:
                    runs_.append(cur_)
                    cur_ = []
                cur_.append(x)
        if cur_:
            runs_.append(cur_)
        candidates[site] = [(r_[0], len(r_)) for r_ in runs_ if len(r_) >= 4]
    # Tables built at run time: code constants (lui/addiu) pointing inside the function.
    runtime_targets = {}
    for site, fn, reg in unresolved:
        size = fstart[fn][0]
        runtime_targets[site] = sorted((v, sorted(refs)) for v, refs in all_consts.items()
                                       if fn < v < fn + size and v % 4 == 0)

    # ---- function addresses stored in data (pointer tables, callback structs)
    data_ranges = [(last_end, t_end)] + gaps
    in_tables = set()
    for t in tables:
        in_tables.update(range(t["table"], t["table"] + 4 * t["entries"], 4))
    data_ptrs = defaultdict(list)
    for lo, hi in data_ranges:
        for x in range((lo + 3) & ~3, hi - 3, 4):
            if x in in_tables:
                continue
            w = word(x)
            if w in fstart:
                data_ptrs[w].append(x)
    runs, cur = [], []
    for x in sorted(x for xs in data_ptrs.values() for x in xs):
        if cur and x == cur[-1] + 4:
            cur.append(x)
        else:
            if cur:
                runs.append(cur)
            cur = [x]
    if cur:
        runs.append(cur)
    ptr_tables = [{"addr": r[0], "count": len(r), "name": name_at.get(r[0])} for r in runs if len(r) >= 2]

    # ---- code in gaps between IDA functions: referenced from anywhere?
    data_words = defaultdict(list)
    for x in range(code_end, t_end - 3, 4):
        data_words[word(x)].append(x)
    for g0, g1 in gaps:
        for x in range(g0, g1, 4):
            data_words[word(x)].append(x)
    gap_code = []
    for g0, g1 in gaps:
        rets = [x for x in range(g0, g1, 4) if word(x) == 0x03E00008]
        if not rets:
            continue
        refs = sorted({r for x in range(g0, g1, 4) for r in list(all_consts.get(x, ())) + data_words.get(x, [])
                       if not (g0 <= r < g1)})
        gap_code.append({"start": g0, "end": g1, "returns": len(rets), "refs": refs})

    # ---- library classification
    lib = {}
    for a in starts:
        lib[a] = lib_of(fstart[a][1])
    for a in starts:
        if a in bios_wrappers:
            lib[a] = lib[a] or "libapi (BIOS wrapper)"
    # unnamed functions between two functions of the same library
    inferred = 0
    for k, a in enumerate(starts):
        if lib[a] is None and not fstart[a][1].startswith("nullsub_"):
            prev = next((lib[starts[j]] for j in range(k - 1, -1, -1) if lib[starts[j]]), None)
            nxt = next((lib[starts[j]] for j in range(k + 1, len(starts)) if lib[starts[j]]), None)
            prev_named = next((starts[j] for j in range(k - 1, -1, -1) if lib[starts[j]]), None)
            base_lib = lambda x: x.split(" (")[0] if x else x  # noqa: E731
            if prev and base_lib(prev) == base_lib(nxt) and prev_named is not None and a - prev_named < 0x800:
                prev = base_lib(prev)
                lib[a] = prev + " (inferred)"
                inferred += 1
    lib_counts = Counter((l or "game").split(" (")[0] for l in lib.values())
    lib_bytes = Counter()
    for a in starts:
        lib_bytes[(lib[a] or "game").split(" (")[0]] += fstart[a][0]

    # ---- callbacks registered through SDK setters
    by_name = {fstart[a][1]: a for a in starts}
    for a, (tbl, num, n) in bios_wrappers.items():
        by_name.setdefault(n, a)
    callbacks = []
    for setter, argi in CALLBACK_SETTERS.items():
        tgt = by_name.get(setter)
        if tgt is None or argi is None:
            continue
        for site, snap in calls_with_a.get(tgt, []):
            h = snap.get(4 + argi)
            callbacks.append({"setter": setter, "site": site, "caller": func_of(site),
                              "handler": h, "handler_name": name_at.get(h) if h else None})

    # ---- randomness and time
    def callers(fname_list):
        out = {}
        for n in fname_list:
            a = by_name.get(n)
            if a is not None:
                out[n] = {"addr": a, "sites": sorted(jal_sites.get(a, []))}
        return out
    rand_wrappers = [a for a, (t, num, _) in bios_wrappers.items() if t == 0xA0 and num in (0x2F, 0x30)]
    time_sources = callers(["rand", "srand", "GetCurTics", "GetRCnt", "VSync", "SetFrameStart", "CdGetSector",
                            "rtUpdateFieldCounter"])
    rand_callers = Counter(func_of(s) for s in time_sources.get("rand", {}).get("sites", []))
    vsync_args = Counter()
    if "VSync" in by_name:
        for site, snap in calls_with_a.get(by_name["VSync"], []):
            v = snap.get(4)
            vsync_args["unknown" if v is None else (v - (1 << 32) if v & 0x80000000 else v)] += 1
    timer_hw = sorted(f for f, blocks in hw_access.items() if "timers" in blocks)

    # ---- assemble report
    hx = lambda v: f"{v:08X}" if isinstance(v, int) else v  # noqa: E731
    rep["exe"]["gp"] = gp
    rep["exe"]["code_end"] = code_end
    rep["functions"] = {"count": len(starts), "code_bytes": code_bytes, "first": hx(starts[0]),
                        "false_functions_in_data": [hx(x) for x in false_funcs],
                        "gap_code": [{"start": hx(g["start"]), "end": hx(g["end"]), "returns": g["returns"],
                                      "refs": [hx(x) for x in g["refs"]]} for g in gap_code],
                        "last_end": hx(code_end), "data_after_code": t_end - code_end,
                        "gaps": [[hx(g0), hx(g1), g1 - g0] for g0, g1 in gaps],
                        "overlaps": [[hx(x), hx(y)] for x, y in overlaps]}
    rep["calls"] = {"jal_sites": sum(jal_targets.values()),
                    "to_unknown_address": [[hx(s), hx(t)] for s, t, _ in unknown_calls],
                    "into_function_middle": [[hx(s), hx(t), hx(o)] for s, t, o in mid_calls],
                    "outside_exe": [[hx(s), hx(t)] for s, t, _ in unknown_calls if not (t_addr <= t < t_end)]}
    rep["cross_function_branches"] = [[hx(s), hx(t), n, hx(o)] for s, t, n, o in cross_branches]
    rep["branch_in_delay_slot"] = [hx(x) for x in branch_in_delay]
    rep["jump_tables"] = [{"site": hx(t["site"]), "function": hx(t["function"]), "table": hx(t["table"]),
                           "entries": t["entries"], "distinct_targets": len(t["targets"]),
                           "found_by": t["found_by"]} for t in tables]
    rep["unresolved_jr"] = [{"site": hx(s), "function": hx(f), "reg": r,
                             "candidate_tables": [[hx(x), n] for x, n in candidates.get(s, [])],
                             "runtime_table_targets": [hx(v) for v, _ in runtime_targets.get(s, [])],
                             "built_by": sorted({hx(x) for _, refs in runtime_targets.get(s, []) for x in refs})}
                            for s, f, r in unresolved]
    rep["jalr_sites"] = [{"site": hx(s), "function": hx(f), "reg": r, "const": hx(c) if c is not None else None}
                         for s, f, r, c in jalr_sites]
    taken = set(addr_taken_code) | set(data_ptrs)
    rep["address_taken_functions"] = [{"addr": hx(a), "name": fstart[a][1],
                                       "code_refs": sorted(hx(x) for x in addr_taken_code.get(a, ())),
                                       "data_refs": sorted(hx(x) for x in data_ptrs.get(a, ()))}
                                      for a in sorted(taken)]
    rep["pointer_tables"] = [{"addr": hx(t["addr"]), "count": t["count"], "name": t["name"]} for t in ptr_tables]
    rep["bios_wrappers"] = [{"addr": hx(a), "ida_name": fstart[a][1], "table": f"{t:02X}",
                             "number": f"{n:02X}" if n is not None else None, "bios_name": bn,
                             "call_sites": len(jal_sites.get(a, []))}
                            for a, (t, n, bn) in sorted(bios_wrappers.items())]
    rep["hardware_access"] = [{"function": hx(f), "name": fstart[f][1], "lib": lib[f],
                               "blocks": {b: sorted(hx(x) for x in xs) for b, xs in blocks.items()}}
                              for f, blocks in sorted(hw_access.items())]
    rep["gte"] = {"commands": dict(gte_total.most_common()), "functions": len(gte_funcs),
                  "game_functions": sum(1 for f in gte_funcs if not lib[f])}
    rep["cop0"] = [{"function": hx(f), "name": fstart[f][1], "ops": dict(c)} for f, c in sorted(cop0_funcs.items())]
    rep["break_codes"] = dict(break_codes)
    rep["hardware_vars"] = {"count": len(hw_vars), "functions_using": len(hw_via_vars)}
    rep["syscall_break"] = [{"function": hx(f), "name": fstart[f][1], "ops": dict(c)}
                            for f, c in sorted(syscall_funcs.items())]
    rep["libraries"] = {"functions": dict(lib_counts), "bytes": dict(lib_bytes), "inferred": inferred}
    rep["callbacks"] = [{**c, "site": hx(c["site"]), "caller": hx(c["caller"]),
                         "handler": hx(c["handler"]) if c["handler"] is not None else None} for c in callbacks]
    rep["heap_region_refs"] = {"count": len(heap_consts),
                               "addresses": sorted(hx(a) for a in heap_consts)[:200]}
    rep["determinism"] = {
        "sources": {n: {"addr": hx(v["addr"]), "call_sites": len(v["sites"]),
                        "sites": [hx(s) for s in v["sites"]]} for n, v in time_sources.items()},
        "rand_bios_wrappers": [hx(a) for a in rand_wrappers],
        "rand_calling_functions": len(rand_callers),
        "vsync_arguments": {str(k): v for k, v in vsync_args.items()},
        "timer_register_functions": [hx(f) for f in timer_hw],
    }
    rep["functions_table"] = [{"addr": hx(a), "name": fstart[a][1], "size": fstart[a][0], "lib": lib[a],
                               **({"gte": gte_funcs[a]} if a in gte_funcs else {}),
                               **({k: v for k, v in per[a]["stats"].items() if k in ("jr", "jalr")})}
                              for a in starts]

    Path(args.json).parent.mkdir(parents=True, exist_ok=True)
    with open(args.json, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(rep, indent=1) + "\n")
    summary(rep, lib, starts, fstart)
    print(f"\nJSON: {args.json}")


def summary(r, lib, starts, fstart):
    e, f, c = r["exe"], r["functions"], r["calls"]
    print(f"EXE {e['path']}: text {e['text_addr']:08X}..{e['text_end']:08X} ({e['text_size']} bytes), "
          f"entry {e['entry']:08X}, stack {e['stack']:08X}")
    print(f"functions {f['count']}, {f['code_bytes']} code bytes, code ends {f['last_end']}, "
          f"{f['data_after_code']} bytes of data after; gaps {len(f['gaps'])} "
          f"({sum(g[2] for g in f['gaps'])} bytes), overlaps {len(f['overlaps'])}")
    print(f"gp {e['gp']:08X}; real code ends {e['code_end']:08X}; IDA false functions in data "
          f"{len(f['false_functions_in_data'])}; gaps with code {len(f['gap_code'])} "
          f"(referenced: {sum(1 for g in f['gap_code'] if g['refs'])})")
    print(f"jal sites {c['jal_sites']}; to unknown addresses {len(c['to_unknown_address'])} "
          f"(outside EXE {len(c['outside_exe'])}); into function middles {len(c['into_function_middle'])}")
    print(f"branches leaving their function {len(r['cross_function_branches'])}; "
          f"branch in delay slot {len(r['branch_in_delay_slot'])}")
    jt = r["jump_tables"]
    print(f"jump tables {len(jt)} ({sum(t['entries'] for t in jt)} entries; "
          f"{sum(1 for t in jt if t.get('found_by') == 'constant')} found by constant tracking); "
          f"jr without a static table {len(r['unresolved_jr'])}: "
          + ", ".join(f"{u['site']} ({len(u['runtime_table_targets'])} run-time table targets)"
                      for u in r["unresolved_jr"]) + "; "
          f"jalr sites {len(r['jalr_sites'])}")
    print(f"address-taken functions {len(r['address_taken_functions'])}; pointer tables in data {len(r['pointer_tables'])}")
    print(f"BIOS wrappers {len(r['bios_wrappers'])}: " + ", ".join(
        f"{b['table']}:{b['number']} {b['bios_name']}" for b in r["bios_wrappers"]))
    print(f"libraries (functions): {r['libraries']['functions']}  inferred {r['libraries']['inferred']}")
    print(f"libraries (bytes): {r['libraries']['bytes']}")
    print(f"GTE: {r['gte']['functions']} functions ({r['gte']['game_functions']} game), {r['gte']['commands']}")
    print(f"break codes {r['break_codes']}; hardware pointer variables {r['hardware_vars']}")
    print(f"COP0 users: {[(x['function'], x['name']) for x in r['cop0']]}")
    print(f"syscall/break: {[(x['function'], x['name'], x['ops']) for x in r['syscall_break']]}")
    hw = Counter()
    for h in r["hardware_access"]:
        for b in h["blocks"]:
            hw[(b, "sdk" if h["lib"] else "game")] += 1
    print(f"hardware access (functions by block): {dict(hw)}")
    print(f"callbacks registered: {len(r['callbacks'])}")
    for cb in r["callbacks"]:
        print(f"   {cb['setter']} at {cb['site']} (in {cb['caller']}) -> {cb['handler']} {cb['handler_name'] or ''}")
    d = r["determinism"]
    print("time/randomness: " + ", ".join(f"{n} {v['call_sites']} sites" for n, v in d["sources"].items()))
    print(f"   rand callers in {d['rand_calling_functions']} functions; VSync args {d['vsync_arguments']}; "
          f"timer-register functions {d['timer_register_functions']}")
    print(f"heap-region (80010000..text) references: {r['heap_region_refs']['count']}")


if __name__ == "__main__":
    main()
