/* Interface between recompiled PSX code (tools/recomp/recompile.py) and the
 * runtime: CPU state, guest memory access, R3000 arithmetic rules, and the
 * services the generated code calls (dispatch, BIOS, GTE, COP0, I/O). */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct CPU {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t gte_data[32], gte_ctrl[32];
    uint32_t cop0[32];
    uint32_t pc;       /* entry point; recompiled code does not track it */
    uint8_t *ram;      /* 2 MB main RAM */
    uint8_t *scratch;  /* 1 KB scratchpad (data cache) */
} CPU;

typedef void (*RcFn)(CPU *c);
typedef struct RcFunction { uint32_t addr; RcFn fn; } RcFunction;
extern const RcFunction rc_functions[];
extern const unsigned rc_function_count;

/* Function entry marker emitted at the start of every recompiled function.
 * Empty unless built with -DRC_TRACE_ENTRY (records the last functions
 * entered, for diagnosing hangs). */
#ifdef RC_TRACE_ENTRY
extern uint32_t rc_trace_ring[256];
extern unsigned rc_trace_pos;
#define RC_ENTER(a) (rc_trace_ring[rc_trace_pos++ & 255] = (a))
#else
#define RC_ENTER(a) ((void)0)
#endif

/* Debugging: -DRC_MEM_WATCH (with RC_TRACE_ENTRY) reports every write to the range TM2PC_WATCH=addr,len
 * (main.c: the value, the functions last entered). */
#ifdef RC_MEM_WATCH
extern uint32_t rc_watch_lo, rc_watch_n;
void rc_watch_hit(uint32_t a, uint32_t v, int size);
#define RC_WATCH(a, v, s) do { if (((a) & 0x1FFFFFFFu) - rc_watch_lo < rc_watch_n) rc_watch_hit(a, v, s); } while (0)
#else
#define RC_WATCH(a, v, s) ((void)0)
#endif

/* Runtime services (runtime.c). */
void rc_call(CPU *c, uint32_t addr);   /* indirect call: function start or BIOS A0/B0/C0 */
void rc_jump(CPU *c, uint32_t addr);   /* indirect tail jump (BIOS wrappers) */
void rc_bad_jump(CPU *c, uint32_t site, uint32_t target);
void rc_untranslated(CPU *c, uint32_t addr);
void rc_syscall(CPU *c, uint32_t site);
void rc_break(CPU *c, uint32_t site, uint32_t code);
uint32_t rc_io_read(CPU *c, uint32_t addr, int size);
void rc_io_write(CPU *c, uint32_t addr, uint32_t value, int size);
uint32_t cop0_read(CPU *c, int reg);
void cop0_write(CPU *c, int reg, uint32_t value);
void cop0_rfe(CPU *c);
uint32_t gte_read_data(CPU *c, int reg);
uint32_t gte_read_ctrl(CPU *c, int reg);
void gte_write_data(CPU *c, int reg, uint32_t value);
void gte_write_ctrl(CPU *c, int reg, uint32_t value);
void gte_command(CPU *c, uint32_t cmd);
void gte_store_data(CPU *c, uint32_t ea, int reg); /* swc2 (keeps precise vertices, gte.c) */

/* The guest's RAM: the PS1's 2 MB and more for the port, 16 MB (the decomp's port memory 0x80200000-0x80BFFFFF, the
 * port's own 0x80C00000-0x80DFFFFF and rollback's node list from 0x80E00000 (net/rb_state.c); a draw list's links
 * are 24 bits: everything drawn stays below 16 MB) */
#define GUEST_RAM_BYTES 0x01000000u

/* Guest memory: KUSEG/KSEG0/KSEG1 views of RAM (GUEST_RAM_BYTES)
 * and the scratchpad are direct; everything else goes to rc_io_*. */
static inline uint8_t *mem_direct(CPU *c, uint32_t a, int size)
{
    if (a >= 0xC0000000u)
        return 0;
    uint32_t p = a & 0x1FFFFFFFu;
    if (p < GUEST_RAM_BYTES)
        return c->ram + p;
    if (p >= 0x1F800000u && p + size <= 0x1F800400u)
        return c->scratch + (p - 0x1F800000u);
    return 0;
}

static inline uint32_t mem_r8(CPU *c, uint32_t a)
{
    uint8_t *p = mem_direct(c, a, 1);
    return p ? *p : rc_io_read(c, a, 1) & 0xFF;
}
static inline uint32_t mem_r16(CPU *c, uint32_t a)
{
    uint8_t *p = mem_direct(c, a, 2);
    uint16_t v;
    if (!p)
        return rc_io_read(c, a, 2) & 0xFFFF;
    memcpy(&v, p, 2);
    return v;
}
static inline uint32_t mem_r32(CPU *c, uint32_t a)
{
    uint8_t *p = mem_direct(c, a, 4);
    uint32_t v;
    if (!p)
        return rc_io_read(c, a, 4);
    memcpy(&v, p, 4);
    return v;
}
static inline void mem_w8(CPU *c, uint32_t a, uint32_t v)
{
    RC_WATCH(a, v, 1);
    uint8_t *p = mem_direct(c, a, 1);
    if (p) *p = (uint8_t)v; else rc_io_write(c, a, v & 0xFF, 1);
}
static inline void mem_w16(CPU *c, uint32_t a, uint32_t v)
{
    RC_WATCH(a, v, 2);
    uint8_t *p = mem_direct(c, a, 2);
    uint16_t h = (uint16_t)v;
    if (p) memcpy(p, &h, 2); else rc_io_write(c, a, h, 2);
}
static inline void mem_w32(CPU *c, uint32_t a, uint32_t v)
{
    RC_WATCH(a, v, 4);
    uint8_t *p = mem_direct(c, a, 4);
    if (p) memcpy(p, &v, 4); else rc_io_write(c, a, v, 4);
}

/* Unaligned word access (little-endian R3000 rules). */
static inline uint32_t mem_lwl(CPU *c, uint32_t a, uint32_t rt)
{
    uint32_t sh = (a & 3) * 8, w = mem_r32(c, a & ~3u);
    return (rt & (0x00FFFFFFu >> sh)) | (w << (24 - sh));
}
static inline uint32_t mem_lwr(CPU *c, uint32_t a, uint32_t rt)
{
    uint32_t sh = (a & 3) * 8, w = mem_r32(c, a & ~3u);
    return sh ? (rt & (0xFFFFFF00u << (24 - sh))) | (w >> sh) : w;
}
static inline void mem_swl(CPU *c, uint32_t a, uint32_t rt)
{
    uint32_t sh = (a & 3) * 8, w = mem_r32(c, a & ~3u);
    mem_w32(c, a & ~3u, sh == 24 ? rt : (w & (0xFFFFFF00u << sh)) | (rt >> (24 - sh)));
}
static inline void mem_swr(CPU *c, uint32_t a, uint32_t rt)
{
    uint32_t sh = (a & 3) * 8, w = mem_r32(c, a & ~3u);
    mem_w32(c, a & ~3u, sh ? (w & (0x00FFFFFFu >> (24 - sh))) | (rt << sh) : rt);
}

/* R3000 division: defined results for division by zero and overflow. */
static inline void cpu_div(CPU *c, uint32_t n, uint32_t d)
{
    int32_t a = (int32_t)n, b = (int32_t)d;
    if (b == 0) {
        c->lo = a >= 0 ? 0xFFFFFFFFu : 1u;
        c->hi = n;
    } else if (n == 0x80000000u && b == -1) {
        c->lo = 0x80000000u;
        c->hi = 0;
    } else {
        c->lo = (uint32_t)(a / b);
        c->hi = (uint32_t)(a % b);
    }
}
static inline void cpu_divu(CPU *c, uint32_t n, uint32_t d)
{
    if (d == 0) {
        c->lo = 0xFFFFFFFFu;
        c->hi = n;
    } else {
        c->lo = n / d;
        c->hi = n % d;
    }
}
