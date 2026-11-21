/* Run one recompiled function from a captured entry state.
 *
 *   fixture_runner IN OUT
 *
 * IN:  u32 function, 34 u32 (r0..r31, hi, lo), 64 u32 GTE (data, control),
 *      1024 bytes scratchpad, 2 MB RAM.
 * OUT: the same layout (without the function word) after the call.
 * Exit code: 0, or 2 if the runtime reported something unexpected. */
#include <stdio.h>
#include <stdlib.h>
#include "psx_cpu.h"

extern unsigned rc_unexpected_count;

static int read_all(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: fixture_runner IN OUT\n");
        return 1;
    }
    static uint8_t ram[0x200000], scratch[0x400];
    uint32_t func, cpu[34], gte[64];
    FILE *in = fopen(argv[1], "rb");
    if (!in || !read_all(in, &func, 4) || !read_all(in, cpu, sizeof cpu) || !read_all(in, gte, sizeof gte)
        || !read_all(in, scratch, sizeof scratch) || !read_all(in, ram, sizeof ram)) {
        fprintf(stderr, "fixture_runner: cannot read %s\n", argv[1]);
        return 1;
    }
    fclose(in);
    CPU c = {0};
    memcpy(c.r, cpu, 32 * 4);
    c.hi = cpu[32];
    c.lo = cpu[33];
    memcpy(c.gte_data, gte, 32 * 4);
    memcpy(c.gte_ctrl, gte + 32, 32 * 4);
    c.ram = ram;
    c.scratch = scratch;
    c.r[0] = 0;

    rc_call(&c, func);

    memcpy(cpu, c.r, 32 * 4);
    cpu[32] = c.hi;
    cpu[33] = c.lo;
    memcpy(gte, c.gte_data, 32 * 4);
    memcpy(gte + 32, c.gte_ctrl, 32 * 4);
    FILE *out = fopen(argv[2], "wb");
    if (!out)
        return 1;
    fwrite(cpu, 1, sizeof cpu, out);
    fwrite(gte, 1, sizeof gte, out);
    fwrite(scratch, 1, sizeof scratch, out);
    fwrite(ram, 1, sizeof ram, out);
    fclose(out);
    return rc_unexpected_count ? 2 : 0;
}
