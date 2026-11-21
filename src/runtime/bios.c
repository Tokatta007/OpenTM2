/* BIOS functions (A0/B0/C0 tables) and system calls, as used by TM2.
 * Names and numbers: psx-spx "BIOS Function Summary". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

#define BIOS_RAND_SEED 0x9010u /* kernel RAM word holding the rand() seed */

extern uint32_t hook_jmpbuf;
void rt_return_from_exception(void);

static uint32_t guest_strlen(CPU *c, uint32_t s)
{
    uint32_t n = 0;
    while (mem_r8(c, s + n))
        n++;
    return n;
}

static void guest_print(CPU *c, uint32_t s)
{
    static const char reloc[] = "Relocatable"; /* (the game's "no VRAM room for a car texture": always shown) */
    int warn = 1;
    for (uint32_t i = 0; i < sizeof reloc - 1 && warn; i++) warn = mem_r8(c, s + i) == (uint8_t)reloc[i];
    if (!M.trace && !warn)
        return;
    char buf[256];
    uint32_t i = 0;
    for (; i < sizeof buf - 1; i++)
        if (!(buf[i] = (char)mem_r8(c, s + i)))
            break;
    buf[i] = 0;
    fprintf(stderr, "[game] %s\n", buf);
}

static uint32_t open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func)
{
    for (int i = 0; i < 16; i++)
        if (!M.events[i].open) {
            M.events[i] = (Event){cls, spec, mode, func, 1, 0, 0};
            return 0xF1000000u | i;
        }
    return 0xFFFFFFFFu;
}

static Event *event_of(uint32_t h)
{
    uint32_t i = h & 0xFFFF;
    return (h & 0xFF000000u) == 0xF1000000u && i < 16 && M.events[i].open ? &M.events[i] : 0;
}

/* A hardware or software event happened: run INTR-mode handlers, mark the rest ready. */
void rt_events_fire(uint32_t cls, uint32_t spec)
{
    for (int i = 0; i < 16; i++) {
        Event *e = &M.events[i];
        if (!e->open || !e->enabled || e->cls != cls || !(e->spec & spec))
            continue;
        if (e->mode == 0x1000 && e->func)
            guest_call(e->func, 0, 0, 0, 0);
        else
            e->ready = 1;
    }
}

void bios_call(CPU *c, uint32_t table, uint32_t num)
{
    uint32_t *R = c->r, a0 = R[4], a1 = R[5], a2 = R[6], a3 = R[7];
    if (table == 0xA0) {
        switch (num) {
        case 0x0E: R[2] = (int32_t)a0 < 0 ? (uint32_t) - (int32_t)a0 : a0; return; /* abs */
        case 0x13: /* setjmp: ra, sp, fp, s0-s7, gp */
            mem_w32(c, a0 + 0, R[31]);
            mem_w32(c, a0 + 4, R[29]);
            mem_w32(c, a0 + 8, R[30]);
            for (int i = 0; i < 8; i++)
                mem_w32(c, a0 + 12 + 4 * i, R[16 + i]);
            mem_w32(c, a0 + 44, R[28]);
            R[2] = 0;
            return;
        case 0x17: case 0x18: { /* strcmp, strncmp */
            uint32_t i = 0, x = 0, y = 0;
            for (;; i++) {
                if (num == 0x18 && i >= a2) { R[2] = 0; return; }
                x = mem_r8(c, a0 + i); y = mem_r8(c, a1 + i);
                if (x != y || !x) break;
            }
            R[2] = (uint32_t)((int32_t)x - (int32_t)y);
            return;
        }
        case 0x19: { uint32_t i = 0, ch; /* strcpy */
            do { ch = mem_r8(c, a1 + i); mem_w8(c, a0 + i, ch); i++; } while (ch);
            R[2] = a0; return; }
        case 0x1B: R[2] = guest_strlen(c, a0); return;
        case 0x2A: for (uint32_t i = 0; i < a2; i++) mem_w8(c, a0 + i, mem_r8(c, a1 + i)); R[2] = a0; return;
        case 0x2B: for (uint32_t i = 0; i < a2; i++) mem_w8(c, a0 + i, a1); R[2] = a0; return;
        case 0x2E: /* memchr */
            R[2] = 0;
            for (uint32_t i = 0; i < a2; i++)
                if (mem_r8(c, a0 + i) == (a1 & 0xFF)) { R[2] = a0 + i; break; }
            return;
        case 0x2F: {
            uint32_t seed = mem_r32(c, BIOS_RAND_SEED) * 0x41C64E6Du + 0x3039u;
            mem_w32(c, BIOS_RAND_SEED, seed);
            extern int rbs_rand_log, rbs_resimulating; /* (tests: net/rb_state.c's re-run check, every caller) */
            if (rbs_rand_log) fprintf(stderr, "rand %c %08x\n", rbs_resimulating ? 'R' : 'L', R[31]);
            extern int32_t rbs_step_running; /* (tests: TM2PC_RAND_STEPS=LO-HI, every caller in those network steps) */
            static int lo = -2, hi;
            if (lo == -2) { lo = -1; if (getenv("TM2PC_RAND_STEPS")) sscanf(getenv("TM2PC_RAND_STEPS"), "%d-%d", &lo, &hi); }
            if (lo >= 0 && rbs_step_running >= lo && rbs_step_running <= hi)
                fprintf(stderr, "randstep %d %c %08x %08x\n", (int)rbs_step_running, rbs_resimulating ? 'R' : 'L', R[31], seed);
            R[2] = (seed >> 16) & 0x7FFF;
            return;
        }
        case 0x30: mem_w32(c, BIOS_RAND_SEED, a0); return;
        case 0x34: return;                       /* free */
        case 0x39: return;                       /* InitHeap */
        case 0x3F: guest_print(c, a0); R[2] = 0; return; /* printf: format string only */
        case 0x44: return;                       /* FlushCache */
        case 0x49: gpu_write_gp0(a0); return;    /* GPU_cw */
        case 0x72: return;                       /* CdRemove */
        }
    } else if (table == 0xB0) {
        switch (num) {
        case 0x07: rt_events_fire(a0, a1); R[2] = 1; return;        /* DeliverEvent */
        case 0x08: R[2] = open_event(a0, a1, a2, a3); return;       /* OpenEvent */
        case 0x09: { Event *e = event_of(a0); if (e) e->open = 0; R[2] = 1; return; }
        case 0x0B: { Event *e = event_of(a0); R[2] = e && e->ready; if (e) e->ready = 0; return; }
        case 0x0C: { Event *e = event_of(a0); if (e) e->enabled = 1; R[2] = 1; return; }
        case 0x0D: { Event *e = event_of(a0); if (e) e->enabled = 0; R[2] = 1; return; }
        case 0x12: /* InitPad(buf1, size1, buf2, size2) */
            M.pad_buf[0] = a0; M.pad_len[0] = a1; M.pad_buf[1] = a2; M.pad_len[1] = a3;
            R[2] = 1; return;
        case 0x13: case 0x14: R[2] = 1; return;                     /* StartPad, StopPad */
        case 0x17: rt_return_from_exception(); return;
        case 0x18: hook_jmpbuf = 0; return;                          /* SetDefaultExitFromException */
        case 0x19: hook_jmpbuf = a0; return;                         /* SetCustomExitFromException */
        case 0x38: fprintf(stderr, "game called exit(%d) (from %08X)\n", (int)a0, R[31]); exit((int)a0);
        case 0x3F: guest_print(c, a0); R[2] = 0; return;             /* puts */
        case 0x5B: R[2] = 0; return;                                  /* ChangeClearPad */
        case 0x56: R[2] = 0x00000674u; return;                       /* GetC0Table (kernel RAM) */
        case 0x57: R[2] = 0x00000874u; return;                       /* GetB0Table (kernel RAM) */
        }
    } else if (table == 0xC0) {
        switch (num) {
        case 0x0A: /* ChangeClearRCnt(counter, flag): BIOS acknowledges that counter's IRQ */
            if (a0 < 4) { R[2] = (uint32_t)M.rcnt_autoclear[a0]; M.rcnt_autoclear[a0] = (int)a1; }
            else R[2] = 0;
            return;
        }
    }
    rt_unexpected("bios", table, num);
    R[2] = 0;
}

/* syscall: a0 = 1 EnterCriticalSection, 2 ExitCriticalSection. */
void bios_syscall(CPU *c)
{
    switch (c->r[4]) {
    case 1:
        c->r[2] = !M.critical;
        M.critical = 1;
        break;
    case 2:
        M.critical = 0;
        rt_deliver_pending();
        break;
    default:
        rt_unexpected("syscall", c->r[4], 0);
    }
}
