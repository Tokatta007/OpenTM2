/* Core services for recompiled code: dispatch, guest calls, exceptions
 * (interrupt delivery through the BIOS hook the SDK installs), COP0. */
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

Machine M;

/* After HookEntryInt(jmp_buf), the SDK's startIntr continues past its setjmp
 * call into trapIntr. Recompiled code cannot resume mid-function, so map the
 * jmp_buf's return address to the function that code calls. */
static const struct { uint32_t resume, handler; } hook_entry[] = {
    {0x801454F0u, 0x80145564u}, /* libetc startIntr -> trapIntr */ /* retail-address */
};

uint32_t hook_jmpbuf; /* set by HookEntryInt (B0:19) */

void rt_unexpected(const char *what, uint32_t a, uint32_t b)
{
    M.unexpected++;
    if (M.unexpected < 200)
        fprintf(stderr, "runtime: %s %08X %08X (ra %08X)\n", what, a, b, M.cpu.r[31]);
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

void rc_call(CPU *c, uint32_t addr)
{
    RcFn f = find_function(addr);
    if (f)
        f(c);
    else if (addr == 0xA0 || addr == 0xB0 || addr == 0xC0)
        bios_call(c, addr, c->r[9]);
    else
        rt_unexpected("call to unknown address", addr, c->r[31]);
}

void rc_jump(CPU *c, uint32_t addr) { rc_call(c, addr); }
void rc_bad_jump(CPU *c, uint32_t site, uint32_t target) { rt_unexpected("jump table target", site, target); }
void rc_untranslated(CPU *c, uint32_t addr) { rt_unexpected("untranslated function", addr, 0); }
void rc_syscall(CPU *c, uint32_t site) { bios_syscall(c); }
void rc_break(CPU *c, uint32_t site, uint32_t code)
{
    if (code == 0x1C00 || code == 0x1800) /* compiler divide-by-zero / overflow checks */
        rt_unexpected(code == 0x1C00 ? "divide by zero" : "overflow", site, code);
    else
        rt_unexpected("break", site, code);
}

uint32_t rc_io_read(CPU *c, uint32_t addr, int size) { return hw_read(addr, size); }
extern int rc_io_capture; /* abcheck.c: the A/B check notes hardware writes (2: and holds them back) */
void ab_io_note(uint32_t addr, uint32_t value, int size);
void rc_io_write(CPU *c, uint32_t addr, uint32_t v, int size)
{
    if (rc_io_capture) { ab_io_note(addr, v, size); if (rc_io_capture == 2) return; }
    hw_write(addr, v, size);
}
uint32_t cop0_read(CPU *c, int reg) { return c->cop0[reg & 31]; }
void cop0_write(CPU *c, int reg, uint32_t v) { c->cop0[reg & 31] = v; }
void cop0_rfe(CPU *c) { }

/* Call a guest function as an interrupt or callback would: all registers
 * are preserved for the interrupted code. */
uint32_t guest_call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    CPU *c = &M.cpu;
    uint32_t saved[32], hi = c->hi, lo = c->lo;
    memcpy(saved, c->r, sizeof saved);
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;
    c->r[29] = (c->r[29] - 0x400) & ~7u;
    c->r[31] = 0;
    rc_call(c, fn);
    uint32_t v0 = c->r[2];
    memcpy(c->r, saved, sizeof saved);
    c->hi = hi;
    c->lo = lo;
    return v0;
}

/* As guest_call, with a fifth argument (on the stack, at sp + 16, as the
 * MIPS calling convention puts it). */
uint32_t guest_call5(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    CPU *c = &M.cpu;
    uint32_t saved[32], hi = c->hi, lo = c->lo;
    memcpy(saved, c->r, sizeof saved);
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;
    c->r[29] = (c->r[29] - 0x400) & ~7u;
    mem_w32(c, c->r[29] + 16, a4);
    c->r[31] = 0;
    rc_call(c, fn);
    uint32_t v0 = c->r[2];
    memcpy(c->r, saved, sizeof saved);
    c->hi = hi;
    c->lo = lo;
    return v0;
}

/* BIOS ReturnFromException (B0:17). On hardware it does not return. Its
 * only caller, libetc trapIntr, ends right after the call, so returning
 * normally gets back to exception() the same way. (No longjmp: on 64-bit
 * Windows the C library one unwinds through the recompiled frames with SEH
 * and fails with 0xC00000FF.) The other call, on trapIntr's "libetc not
 * initialized" path, would continue into the handler; it is logged. */
void rt_return_from_exception(void)
{
    if (!M.in_interrupt)
        rt_unexpected("ReturnFromException outside an exception", 0, 0);
}

/* One exception: BIOS event handling, then the SDK's hooked handler. */
static void exception(void)
{
    CPU *c = &M.cpu;
    uint32_t saved[32], hi = c->hi, lo = c->lo;
    memcpy(saved, c->r, sizeof saved);
    M.in_interrupt = 1;

    /* BIOS: root counter interrupts deliver their events (counter 2 is the
     * game clock), then are acknowledged unless ChangeClearRCnt said not to
     * (libetc does that when it handles the counter itself). */
    for (int t = 0; t < 3; t++) {
        uint32_t bit = 1u << (IRQ_TMR0 + t);
        if (M.i_stat & bit) {
            rt_events_fire(0xF2000000u | (uint32_t)t, 0x0002u);
            if (M.rcnt_autoclear[t])
                M.i_stat &= ~bit;
        }
    }

    if (hook_jmpbuf) {
        uint32_t resume = mem_r32(c, hook_jmpbuf + 0), handler = 0;
        for (unsigned i = 0; i < sizeof hook_entry / sizeof hook_entry[0]; i++)
            if (ga(hook_entry[i].resume) == resume) /* (retail addresses: where they are in this build) */
                handler = ga(hook_entry[i].handler);
        if (handler) {
            c->r[29] = mem_r32(c, hook_jmpbuf + 4);  /* the hook's own stack */
            c->r[30] = mem_r32(c, hook_jmpbuf + 8);
            c->r[28] = mem_r32(c, hook_jmpbuf + 44);
            c->r[31] = 0;
            rc_call(c, handler);
        } else {
            rt_unexpected("unknown HookEntryInt resume address", resume, hook_jmpbuf);
            M.i_stat = 0;
        }
    } else {
        M.i_stat = 0; /* no handler installed: the BIOS acknowledges */
    }

    M.in_interrupt = 0;
    memcpy(c->r, saved, sizeof saved);
    c->hi = hi;
    c->lo = lo;
}

void rt_raise_irq(int irq)
{
    M.i_stat |= 1u << irq;
    rt_deliver_pending();
}

void rt_deliver_pending(void)
{
    int guard = 0;
    while (!M.critical && !M.in_interrupt && (M.i_stat & M.i_mask) && guard++ < 16)
        exception();
}

/* As guest_call, with fifth to seventh arguments (on the stack at sp + 16, + 20, + 24). */
uint32_t guest_call7(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
{
    CPU *c = &M.cpu;
    uint32_t saved[32], hi = c->hi, lo = c->lo;
    memcpy(saved, c->r, sizeof saved);
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;
    c->r[29] = (c->r[29] - 0x400) & ~7u;
    mem_w32(c, c->r[29] + 16, a4);
    mem_w32(c, c->r[29] + 20, a5);
    mem_w32(c, c->r[29] + 24, a6);
    c->r[31] = 0;
    rc_call(c, fn);
    uint32_t v0 = c->r[2];
    memcpy(c->r, saved, sizeof saved);
    c->hi = hi;
    c->lo = lo;
    return v0;
}
