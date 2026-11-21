/* Minimal runtime for testing recompiled functions in isolation: dispatch,
 * a few BIOS functions, and logging for everything a function test should not reach. */
#include <stdio.h>
#include <stdlib.h>
#include "psx_cpu.h"

int rc_log_unexpected = 1;
unsigned rc_unexpected_count;
#define BIOS_RAND_SEED 0x9010u /* kernel RAM word holding the rand() seed */

static void unexpected(const char *what, uint32_t a, uint32_t b)
{
    rc_unexpected_count++;
    if (rc_log_unexpected)
        fprintf(stderr, "runtime: %s %08X %08X\n", what, a, b);
}

static RcFn find_function(uint32_t addr)
{
    unsigned lo = 0, hi = rc_function_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (rc_functions[mid].addr == addr)
            return rc_functions[mid].fn;
        if (rc_functions[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

static uint32_t guest_strlen(CPU *c, uint32_t s)
{
    uint32_t n = 0;
    while (mem_r8(c, s + n))
        n++;
    return n;
}

static void bios(CPU *c, uint32_t table, uint32_t num)
{
    uint32_t *R = c->r, a0 = R[4], a1 = R[5], a2 = R[6];
    if (table == 0xA0) {
        switch (num) {
        case 0x2F: { /* rand: the BIOS LCG */
            uint32_t seed = mem_r32(c, BIOS_RAND_SEED) * 0x41C64E6Du + 0x3039u;
            mem_w32(c, BIOS_RAND_SEED, seed);
            R[2] = (seed >> 16) & 0x7FFF;
            return;
        }
        case 0x30: mem_w32(c, BIOS_RAND_SEED, a0); return;                            /* srand */
        case 0x0E: R[2] = (int32_t)a0 < 0 ? (uint32_t)-(int32_t)a0 : a0; return;   /* abs */
        case 0x2A: for (uint32_t i = 0; i < a2; i++) mem_w8(c, a0 + i, mem_r8(c, a1 + i)); /* memcpy */
                   R[2] = a0; return;
        case 0x2B: for (uint32_t i = 0; i < a2; i++) mem_w8(c, a0 + i, a1);         /* memset */
                   R[2] = a0; return;
        case 0x1B: R[2] = guest_strlen(c, a0); return;                               /* strlen */
        case 0x19: { uint32_t i = 0, ch;                                             /* strcpy */
                     do { ch = mem_r8(c, a1 + i); mem_w8(c, a0 + i, ch); i++; } while (ch);
                     R[2] = a0; return; }
        case 0x17: case 0x18: {                                                      /* strcmp, strncmp */
            uint32_t i = 0, x, y;
            for (;; i++) {
                if (num == 0x18 && i >= a2) { R[2] = 0; return; }
                x = mem_r8(c, a0 + i); y = mem_r8(c, a1 + i);
                if (x != y || !x) break;
            }
            R[2] = (uint32_t)((int32_t)x - (int32_t)y);
            return;
        }
        case 0x3F: R[2] = 0; return;                                                  /* printf */
        }
    } else if (table == 0xB0 && num == 0x3F) {                                       /* puts */
        R[2] = 0;
        return;
    }
    unexpected("bios", table, num);
    R[2] = 0;
}

void rc_call(CPU *c, uint32_t addr)
{
    RcFn f = find_function(addr);
    if (f)
        f(c);
    else if (addr == 0xA0 || addr == 0xB0 || addr == 0xC0)
        bios(c, addr, c->r[9]);
    else
        unexpected("call to unknown address", addr, c->r[31]);
}

void rc_jump(CPU *c, uint32_t addr) { rc_call(c, addr); }
void rc_bad_jump(CPU *c, uint32_t site, uint32_t target) { unexpected("jump table target", site, target); }
void rc_untranslated(CPU *c, uint32_t addr) { unexpected("untranslated function", addr, 0); }
void rc_syscall(CPU *c, uint32_t site) { unexpected("syscall", site, c->r[4]); }
void rc_break(CPU *c, uint32_t site, uint32_t code) { unexpected("break", site, code); }
uint32_t rc_io_read(CPU *c, uint32_t addr, int size) { unexpected("io read", addr, (uint32_t)size); return 0; }
void rc_io_write(CPU *c, uint32_t addr, uint32_t v, int size) { unexpected("io write", addr, v); }
uint32_t cop0_read(CPU *c, int reg) { return c->cop0[reg & 31]; }
void cop0_write(CPU *c, int reg, uint32_t v) { c->cop0[reg & 31] = v; }
void cop0_rfe(CPU *c) { unexpected("rfe", 0, 0); }
