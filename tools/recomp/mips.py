"""Minimal MIPS R3000A (MIPS I) decoder for static analysis.

Decodes just enough to find control flow, constants built from lui, memory
accesses, coprocessor 0/2 use and system calls. Not a full disassembler.
"""

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
RA, SP, GP = 31, 29, 28

LOADS = {0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr", 0x32: "lwc2"}
STORES = {0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr", 0x3A: "swc2"}
BRANCHES = {0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz"}
REGIMM = {0x00: "bltz", 0x01: "bgez", 0x10: "bltzal", 0x11: "bgezal"}

# GTE commands by the low 6 bits of the COP2 command word.
GTE = {0x01: "RTPS", 0x06: "NCLIP", 0x0C: "OP", 0x10: "DPCS", 0x11: "INTPL", 0x12: "MVMVA",
       0x13: "NCDS", 0x14: "CDP", 0x16: "NCDT", 0x1B: "NCCS", 0x1C: "CC", 0x1E: "NCS",
       0x20: "NCT", 0x28: "SQR", 0x29: "DCPL", 0x2A: "DPCT", 0x2D: "AVSZ3", 0x2E: "AVSZ4",
       0x30: "RTPT", 0x3D: "GPF", 0x3E: "GPL", 0x3F: "NCCT"}


class Insn:
    __slots__ = ("addr", "word", "op", "rs", "rt", "rd", "sa", "funct", "imm", "simm", "target", "kind", "name")

    def __init__(self, addr, word):
        self.addr = addr
        self.word = word
        self.op = word >> 26
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.funct = word & 63
        self.imm = word & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = None
        self.kind, self.name = classify(self)

    def writes(self):
        """The general register this instruction writes, or None."""
        op = self.op
        if op == 0:
            if self.funct in (0x08, 0x0C, 0x0D, 0x18, 0x19, 0x1A, 0x1B, 0x11, 0x13):
                return None  # jr, syscall, break, mult/div, mthi/mtlo
            return self.rd or None
        if op == 1:
            return RA if self.rt in (0x10, 0x11) else None
        if op == 3:
            return RA
        if 0x08 <= op <= 0x0F or op in LOADS and op != 0x32:
            return self.rt or None
        if op in (0x10, 0x12) and self.rs in (0, 2):  # mfc/cfc
            return self.rt or None
        return None


def classify(i):
    op = i.op
    if op == 0:
        f = i.funct
        if f == 0x08:
            return "jr", "jr"
        if f == 0x09:
            return "jalr", "jalr"
        if f == 0x0C:
            return "syscall", "syscall"
        if f == 0x0D:
            return "break", "break"
        return "alu", "special"
    if op == 1:
        name = REGIMM.get(i.rt, "regimm?")
        i.target = (i.addr + 4 + (i.simm << 2)) & 0xFFFFFFFF
        return ("branchlink" if i.rt in (0x10, 0x11) else "branch"), name
    if op in (2, 3):
        i.target = ((i.addr + 4) & 0xF0000000) | ((i.word & 0x3FFFFFF) << 2)
        return ("j" if op == 2 else "jal"), ("j" if op == 2 else "jal")
    if op in BRANCHES:
        i.target = (i.addr + 4 + (i.simm << 2)) & 0xFFFFFFFF
        return "branch", BRANCHES[op]
    if op == 0x0F:
        return "lui", "lui"
    if op in (0x09, 0x0D):
        return "imm", "addiu" if op == 0x09 else "ori"
    if 0x08 <= op <= 0x0E:
        return "alu", "imm"
    if op in LOADS:
        return "load", LOADS[op]
    if op in STORES:
        return "store", STORES[op]
    if op == 0x10:
        if i.rs == 0x10 and i.funct == 0x10:
            return "cop0", "rfe"
        return "cop0", {0: "mfc0", 4: "mtc0"}.get(i.rs, "cop0?")
    if op == 0x12:
        if i.rs >= 0x10:
            return "gte", GTE.get(i.word & 0x3F, f"cop2_{i.word & 0x3F:02x}")
        return "gte_move", {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}.get(i.rs, "cop2?")
    return "other", f"op{op:02x}"


def has_delay_slot(i):
    return i.kind in ("jr", "jalr", "j", "jal", "branch", "branchlink")
