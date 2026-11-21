/* The A/B check: a function of the retail game against its version from the decomp's own source.
 *
 * The decomp builds the whole game from its C (at the retail layout); the recompiler can translate
 * chosen functions of that build too (--alt-exe / --alt-funcs: F_x_alt beside the retail F_x_base).
 * With TM2PC_AB set, each call of such a function runs both on the same machine state:
 *
 *   1. the state kept (RAM, scratchpad, the CPU);
 *   2. the source version run, its hardware writes noted and not done (a sandbox), its result kept;
 *   3. the state put back, the retail version run for real (its hardware writes noted and done);
 *   4. the two results compared: RAM, scratchpad, the registers a caller may read (v0, v1, sp and
 *      the saved s0-s7, fp, ra) and the hardware writes in order.
 *
 * The game goes on with the retail result, so a difference is reported (stderr: the function, the
 * call, the first differing address and its two values) without spreading. Calls inside an A/B call
 * run once (their own check is skipped). Functions with host side effects (the port's wraps) are not
 * eligible (the recompiler refuses them). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

#define RAM_BYTES GUEST_RAM_BYTES
#define IO_MAX 1024

int rc_io_capture; /* hw: 1 hardware writes noted (and done), 2 noted only (core.c rc_io_write) */
typedef struct { uint32_t addr, value; int size; } IoWrite;
static IoWrite io[2][IO_MAX];
static int io_n[2], io_side;
void ab_io_note(uint32_t addr, uint32_t value, int size)
{
    if (io_n[io_side] < IO_MAX) io[io_side][io_n[io_side]++] = (IoWrite){addr, value, size};
}

int ab_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("TM2PC_AB") != NULL;
    return on;
}

static struct { uint32_t addr; unsigned long long calls, differ; } stats[64];
static int stats_n;
static void ab_summary(void)
{
    for (int i = 0; i < stats_n; i++)
        fprintf(stderr, "a/b: %08X: %llu calls, %llu different\n", stats[i].addr, stats[i].calls, stats[i].differ);
}

static int report(uint32_t addr, unsigned long long call, const char *what)
{
    static int reports;
    if (reports++ < 200) fprintf(stderr, "a/b: %08X call %llu: %s\n", addr, call, what);
    return 1;
}

void ab_run(CPU *c, void (*base)(CPU *), void (*alt)(CPU *), uint32_t addr)
{
    static int inside;
    static uint8_t *ram_before, *ram_alt;
    static uint8_t scr_before[1024], scr_alt[1024];
    if (inside) { base(c); return; }
    if (!ram_before) {
        ram_before = malloc(RAM_BYTES);
        ram_alt = malloc(RAM_BYTES);
        if (!ram_before || !ram_alt) { base(c); return; }
        atexit(ab_summary);
    }
    int s = 0;
    while (s < stats_n && stats[s].addr != addr) s++;
    if (s == stats_n && stats_n < 64) stats[stats_n++].addr = addr;
    unsigned long long call = s < 64 ? ++stats[s].calls : 0;
    inside = 1;
    /* 1, 2: the source version in a sandbox */
    memcpy(ram_before, M.ram, RAM_BYTES);
    memcpy(scr_before, M.scratch, sizeof scr_before);
    CPU cpu_before = *c;
    io_side = 1;
    io_n[1] = 0;
    rc_io_capture = 2;
    alt(c);
    rc_io_capture = 0;
    memcpy(ram_alt, M.ram, RAM_BYTES);
    memcpy(scr_alt, M.scratch, sizeof scr_alt);
    CPU cpu_alt = *c;
    static int broken = -1; /* tests: TM2PC_AB_BREAK=N spoils every Nth source result (the check must report it) */
    if (broken < 0) broken = getenv("TM2PC_AB_BREAK") ? atoi(getenv("TM2PC_AB_BREAK")) : 0;
    if (broken > 0 && call % (unsigned)broken == 0) cpu_alt.r[2] ^= 1;
    /* 3: the retail version for real */
    memcpy(M.ram, ram_before, RAM_BYTES);
    memcpy(M.scratch, scr_before, sizeof scr_before);
    *c = cpu_before;
    io_side = 0;
    io_n[0] = 0;
    rc_io_capture = 1;
    base(c);
    rc_io_capture = 0;
    inside = 0;
    /* 4: the comparison */
    int differ = 0;
    char line[160];
    if (memcmp(M.ram, ram_alt, RAM_BYTES)) {
        uint32_t o = 0, n = 0, first = 0;
        for (o = 0; o < RAM_BYTES; o++)
            if (M.ram[o] != ram_alt[o]) { if (!n++) first = o; }
        snprintf(line, sizeof line, "RAM: %u bytes differ, first %08X: retail %02X, source %02X", n, 0x80000000u + first,
                 M.ram[first], ram_alt[first]);
        differ |= report(addr, call, line);
    }
    if (memcmp(M.scratch, scr_alt, sizeof scr_alt)) differ |= report(addr, call, "scratchpad differs");
    static const int regs[] = {2, 3, 16, 17, 18, 19, 20, 21, 22, 23, 29, 30, 31};
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        if (c->r[regs[i]] != cpu_alt.r[regs[i]]) {
            snprintf(line, sizeof line, "register %d: retail %08X, source %08X", regs[i], c->r[regs[i]], cpu_alt.r[regs[i]]);
            differ |= report(addr, call, line);
        }
    int n = io_n[0] < io_n[1] ? io_n[0] : io_n[1];
    for (int i = 0; i < n; i++)
        if (io[0][i].addr != io[1][i].addr || io[0][i].value != io[1][i].value) {
            snprintf(line, sizeof line, "hardware write %d: retail %08X=%X, source %08X=%X", i, io[0][i].addr, io[0][i].value,
                     io[1][i].addr, io[1][i].value);
            differ |= report(addr, call, line);
            break;
        }
    if (io_n[0] != io_n[1]) {
        snprintf(line, sizeof line, "hardware writes: retail %d, source %d", io_n[0], io_n[1]);
        differ |= report(addr, call, line);
    }
    if (differ && s < 64) stats[s].differ++;
    if (s < 64 && (call & (call - 1)) == 0) /* now and then (1, 2, 4, 8 ... calls): how it goes */
        fprintf(stderr, "a/b: %08X: %llu calls, %llu different\n", addr, call, stats[s].differ);
}
