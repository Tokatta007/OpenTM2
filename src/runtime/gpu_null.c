/* Null GPU backend: VRAM in host memory, drawing commands only counted.
 * Used for headless runs and tests. */
#include <string.h>
#include "gpu_backend.h"

static uint16_t vram[512][1024];
unsigned long long gpu_null_draws;

static int init(void) { return 1; }
static void command(const uint32_t *words, int count) { (void)words; (void)count; gpu_null_draws++; }

static void upload(int x, int y, int w, int h, const uint16_t *p)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            vram[(y + j) & 511][(x + i) & 1023] = p[j * w + i];
}

static void download(int x, int y, int w, int h, uint16_t *p)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            p[j * w + i] = vram[(y + j) & 511][(x + i) & 1023];
}

static void copy(int sx, int sy, int dx, int dy, int w, int h)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            vram[(dy + j) & 511][(dx + i) & 1023] = vram[(sy + j) & 511][(sx + i) & 1023];
}

static void fill(int x, int y, int w, int h, uint32_t c)
{
    uint16_t v = (uint16_t)(((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            vram[(y + j) & 511][(x + i) & 1023] = v;
}

static void gp1(uint32_t word) { (void)word; }
static void vblank(void) { }

static int capture(const char *path, int whole_vram) { (void)path; (void)whole_vram; return 0; }

const GpuBackend gpu_null_backend = {"null", init, command, upload, download, copy, fill, gp1, vblank, capture};
