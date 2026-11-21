/* The renderer behind the GPU front end (gpu.c).
 *
 * gpu.c handles the GP0/GP1 protocol (command framing, polylines, image
 * transfer phases, DMA lists). A backend owns VRAM and draws:
 *   gpu_null.c  VRAM in host memory, no drawing (headless runs, tests)
 *   gpu_psyz.c  PSY-Z: VRAM and rendering on the graphics card, SDL3 window
 * Pick one with gpu_select_backend() before the game starts. */
#pragma once
#include <stdint.h>

typedef struct GpuBackend {
    const char *name;
    int (*init)(void);
    /* A complete drawing or state command (GP0 0x20-0x7F, 0xE1-0xE6). */
    void (*command)(const uint32_t *words, int count);
    /* VRAM transfers (16-bit PS1 pixels, row-major). */
    void (*upload)(int x, int y, int w, int h, const uint16_t *pixels);
    void (*download)(int x, int y, int w, int h, uint16_t *pixels);
    void (*copy)(int sx, int sy, int dx, int dy, int w, int h);
    void (*fill)(int x, int y, int w, int h, uint32_t rgb24);
    /* Display control (GP1). */
    void (*gp1)(uint32_t word);
    /* A VBlank happened: finish drawing and present. */
    void (*vblank)(void);
    /* Save the displayed frame (or all of VRAM) as a BMP; 0 if unsupported. */
    int (*capture)(const char *bmp_path, int whole_vram);
    /* Optional: a polygon command (GP0 0x20-0x3F) with precise vertices, x y w
     * per vertex (NULL: none, as command). */
    void (*polygon)(const uint32_t *words, int count, const float *xyw);
    /* Optional: texture banks (the decomp's TEXTURE_BANKS.md 6.5): transfers into / out of bank b (1..; 0 the VRAM),
     * and whether the renderer has them (NULL: none, the game keeps them off). */
    void (*upload_bank)(int bank, int x, int y, int w, int h, const uint16_t *pixels);
    void (*download_bank)(int bank, int x, int y, int w, int h, uint16_t *pixels);
    int (*banks)(void);
    /* Optional: a depth buffer (PSY-Z patch 20): polygon with its depth test (0 none, 1 test and write, 2 test only),
     * its bias (that much farther, in its depth's units: ties), and emptying the buffer before a view (NULL: none, polygons in command order as the PS1). */
    void (*polygon_depth)(const uint32_t *words, int count, const float *xyw, int depth, float bias);
    void (*clear_depth)(void);
} GpuBackend;

extern const GpuBackend gpu_null_backend;
extern const GpuBackend gpu_psyz_backend;
extern const GpuBackend *gpu_backend;

int gpu_select_backend(const GpuBackend *b);
