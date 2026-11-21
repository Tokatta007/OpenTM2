/* PSY-Z GPU backend (third_party/psyz): VRAM and rendering on the graphics
 * card through SDL3, in a window.
 *
 * Drawing and state commands go to PSY-Z's GP0 queue (Psyz_GpuWriteGP0) and
 * are flushed at VBlank or when the queue fills. VRAM transfers use its
 * Draw_* image functions; GP1 goes to Psyz_GpuDisplayCommand. Presenting
 * happens in Psyz_VideoVSync(0), which also handles window events; pacing
 * stays with the runtime (vsync mode "limitless").
 *
 * Resolution: PSY-Z renders at a multiple of the PS1's resolution
 * (settings_video, F9 to cycle); "auto" follows the window height. The
 * PS1's dithering is turned off when upscaled, where it shows as a grid. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "gpu_backend.h"
#include "settings.h"
#include <psyz/gpu.h>
#include <psyz/video.h>

typedef struct { short x, y, w, h; } Ps1Rect; /* libgpu RECT (PS1_RECT in PSY-Z) */
void Draw_LoadImage(Ps1Rect *rect, unsigned long *p);
void Draw_StoreImage(Ps1Rect *rect, unsigned long *p);
void Draw_MoveImage(Ps1Rect *rect, unsigned int x, unsigned int y);
void Draw_ClearImage(Ps1Rect *rect, unsigned char r, unsigned char g, unsigned char b);

static unsigned queued; /* GP0 words since the last flush */
int gpu_depth_on = 1; /* the hidden surfaces setting (gpu.c), taken between frames as the wobble */

static void flush(void)
{
    while (Psyz_GpuExeque() > 0)
        ;
    queued = 0;
}

/* The window (settings_video.window_mode). Runs with any TM2PC_ variable set (tests, debugging) stay
 * in a window and don't save F4's switch. */
extern char **environ;
static int dev_run(void)
{
    static int dev = -1;
    if (dev < 0) {
        dev = 0;
        for (char **e = environ; e && *e; e++) if (!strncmp(*e, "TM2PC_", 6)) dev = 1;
    }
    return dev;
}
static void apply_window_mode(void)
{
    Psyz_VideoSetFullscreenMode(settings_video.display_w, settings_video.display_h, settings_video.display_hz);
    Psyz_VideoSetWindowMode(dev_run() ? 0 : settings_video.window_mode);
}

/* The display modes exclusive fullscreen can use (the window's display): largest first, one per size and whole
 * refresh rate. Returns the count. */
int video_display_modes(int *w, int *h, float *hz, int max)
{
    int windows = 0, n = 0, count = 0;
    SDL_Window **ws = SDL_GetWindows(&windows);
    SDL_DisplayID id = ws && windows > 0 ? SDL_GetDisplayForWindow(ws[0]) : 0;
    SDL_free(ws);
    if (!id) id = SDL_GetPrimaryDisplay();
    SDL_DisplayMode **modes = id ? SDL_GetFullscreenDisplayModes(id, &n) : NULL;
    for (int i = 0; modes && i < n && count < max; i++) {
        const SDL_DisplayMode *m = modes[i];
        int dup = 0;
        for (int k = 0; k < count && !dup; k++)
            dup = w[k] == m->w && h[k] == m->h && (int)(hz[k] + 0.5f) == (int)(m->refresh_rate + 0.5f);
        if (dup) continue;
        w[count] = m->w, h[count] = m->h, hz[count] = m->refresh_rate;
        count++;
    }
    SDL_free(modes);
    return count;
}

int video_auto_scale(void) /* Resolution Auto's multiple of the PS1's 240 lines now */
{
    PsyzSize s = Psyz_VideoGetDisplaySize();
    int n = s.h / 240;
    return n < 1 ? 1 : n > PSYZ_INTERNAL_RES_MAX ? PSYZ_INTERNAL_RES_MAX : n;
}

static int init(void)
{
    Psyz_SetTitle("Twisted Metal 2");
    apply_window_mode();
    Psyz_VideoSetVsyncMode(PSYZ_VSYNC_LIMITLESS);
    return 1;
}

static void command(const uint32_t *words, int count)
{
    if (queued + (unsigned)count > 0x3000) /* PSY-Z's queue holds 0x4000 words */
        flush();
    for (int i = 0; i < count; i++)
        Psyz_GpuWriteGP0(words[i]);
    queued += (unsigned)count;
}

/* a polygon with its precise vertices (PSY-Z patch 12): one entry per polygon, in order */
static void polygon(const uint32_t *words, int count, const float *xyw)
{
    Psyz_GpuPushPrecise(xyw, xyw ? ((words[0] >> 24) & 8 ? 4 : 3) : 0);
    command(words, count);
}

static void polygon_depth(const uint32_t *words, int count, const float *xyw, int depth, float bias)
{
    Psyz_GpuPushPreciseDepth(xyw, xyw ? ((words[0] >> 24) & 8 ? 4 : 3) : 0, depth, bias);
    command(words, count);
}
static void clear_depth(void)
{
    flush();
    Psyz_GpuClearDepth();
}

static void upload(int x, int y, int w, int h, const uint16_t *p)
{
    flush();
    Ps1Rect r = {(short)x, (short)y, (short)w, (short)h};
    if (x + w <= 1024 && y + h <= 512) {
        Draw_LoadImage(&r, (unsigned long *)p);
        return;
    }
    /* wrapping transfer: split per row (rare) */
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            Ps1Rect px = {(short)((x + i) & 1023), (short)((y + j) & 511), 1, 1};
            Draw_LoadImage(&px, (unsigned long *)&p[j * w + i]);
        }
}

static void download(int x, int y, int w, int h, uint16_t *p)
{
    flush();
    Ps1Rect r = {(short)x, (short)y, (short)w, (short)h};
    Draw_StoreImage(&r, (unsigned long *)p);
}

void gpu_vram_read(int x, int y, int w, int h, uint16_t *p) { download(x, y, w, h, p); } /* (vplus/hud_styles.c) */

static void copy(int sx, int sy, int dx, int dy, int w, int h)
{
    flush();
    Ps1Rect r = {(short)sx, (short)sy, (short)w, (short)h};
    Draw_MoveImage(&r, (unsigned)dx, (unsigned)dy);
}

static void fill(int x, int y, int w, int h, uint32_t c)
{
    flush();
    Ps1Rect r = {(short)x, (short)y, (short)w, (short)h};
    Draw_ClearImage(&r, (unsigned char)c, (unsigned char)(c >> 8), (unsigned char)(c >> 16));
}

/* Display state from GP1, for the 24-bit mode PSY-Z does not draw (TM2's
 * movies use it): each VBlank the displayed area is read back, decoded from
 * packed RGB888 and handed to PSY-Z to show instead (psyz-02 patch). */
static struct {
    int x, y, width, height, rgb24, shown;
} display = {0, 0, 320, 240, 0, 0};

static void gp1(uint32_t word)
{
    flush();
    Psyz_GpuDisplayCommand(word);
    static int gp1_log = -1;
    static uint32_t last_seen[256];
    if (gp1_log < 0) gp1_log = getenv("TM2PC_GP1_LOG") != NULL;
    if (gp1_log && last_seen[word >> 24] != word) { /* tests: each display command when it changes (the display area, its range) */
        last_seen[word >> 24] = word;
        unsigned long long rt_vblanks_now(void); /* main.c */
        fprintf(stderr, "gp1: %08X at %llu\n", word, rt_vblanks_now());
    }
    switch (word >> 24) {
    case 0x00: display.rgb24 = 0; break;
    case 0x05: display.x = (int)(word & 0x3FF); display.y = (int)((word >> 10) & 0x1FF); break;
    case 0x08: {
        static const int widths[4] = {256, 320, 512, 640};
        display.width = (word & 0x40) ? 368 : widths[word & 3];
        display.height = (word & 0x24) == 0x24 ? 480 : 240; /* 480 lines need interlace */
        display.rgb24 = (int)((word >> 4) & 1);
        break;
    }
    }
}

/* Every area a 24-bit picture (a movie) was shown from: cleared after it
 * (gpu_rgb24_clear), so its frames can't show again on another screen. */
static int r24_x0 = 1024, r24_y0 = 512, r24_x1, r24_y1;
static void fill(int x, int y, int w, int h, uint32_t c);
void gpu_rgb24_clear(void)
{
    if (r24_x1 > r24_x0 && r24_y1 > r24_y0) fill(r24_x0, r24_y0, r24_x1 - r24_x0, r24_y1 - r24_y0, 0);
    r24_x0 = 1024; r24_y0 = 512; r24_x1 = r24_y1 = 0;
}
static void show_rgb24(void)
{
    int w = display.width, h = display.height, hw = w * 3 / 2;
    if (display.x + hw > 1024 || display.y + h > 512)
        return;
    if (display.x < r24_x0) r24_x0 = display.x;
    if (display.y < r24_y0) r24_y0 = display.y;
    if (display.x + hw > r24_x1) r24_x1 = display.x + hw;
    if (display.y + h > r24_y1) r24_y1 = display.y + h;
    uint16_t *raw = malloc((size_t)hw * h * 2);
    unsigned char *rgb = malloc((size_t)w * h * 3);
    if (raw && rgb) {
        download(display.x, display.y, hw, h, raw);
        for (int i = 0; i < hw * h; i++) { /* VRAM bytes in order: R G B R G B ... */
            rgb[i * 2] = (unsigned char)raw[i];
            rgb[i * 2 + 1] = (unsigned char)(raw[i] >> 8);
        }
        Psyz_VideoSetDisplayRGB24(rgb, w, h);
        display.shown = 1;
    }
    free(raw);
    free(rgb);
}

/* ---- resolution ---- */

static void apply_resolution(void)
{
    int n = settings_video.resolution; /* 0 auto, 1..8 */
    if (n == 0) { /* auto: as many times the PS1's 240 lines as fit the window */
        PsyzSize s = Psyz_VideoGetDisplaySize();
        n = s.h / 240;
    }
    if (n < 1) n = 1;
    if (n > PSYZ_INTERNAL_RES_MAX) n = PSYZ_INTERNAL_RES_MAX;
    if ((unsigned)n != Psyz_VideoGetInternalResolution())
        Psyz_VideoSetInternalResolution((unsigned)n);
    int dither = settings_video.dithering;
    Psyz_VideoSetDitheringMode(dither == 2 || (dither == 0 && n > 1) ? PSYZ_DITHER_OFF : PSYZ_DITHER_AUTO);
}

void video_settings_changed(void) { apply_resolution(); apply_window_mode(); }

static void resolution_vblank(void)
{
    /* the driver's vsync: on with Uncapped (frames held to the display's refresh, vplus/interp.c), and with Original
     * and 60 on a display of 59 Hz or more (the port paces itself at the PS1's 59.83 Hz; without vsync its frames tore,
     * the tear crawling as 59.83 drifts against the display's 60: the user's 4K 60 Hz screen). Off on a slower one (50
     * Hz: vsync would hold the game to it) and in tests (TM2PC_INTERP_HZ, TM2PC_NO_VSYNC). */
    static int vsync_on = -1;
    static float display_hz;
    static unsigned hz_asked;
    if (!display_hz || ++hz_asked % 600 == 0) { /* (now and then: the window may move to another display) */
        const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        display_hz = m && m->refresh_rate > 0 ? m->refresh_rate : 60;
    }
    static int no_vsync = -1; /* (read once: not each VBlank) */
    if (no_vsync < 0) no_vsync = getenv("TM2PC_INTERP_HZ") || getenv("TM2PC_NO_VSYNC");
    int want = !no_vsync && (settings_video.frame_rate == 2 || display_hz >= 59.0f);
    if (want != vsync_on) {
        vsync_on = want;
        Psyz_VideoSetVsyncMode(want ? PSYZ_VSYNC_ON : PSYZ_VSYNC_LIMITLESS);
    }
    static unsigned count;
    static bool f9_held;
    const bool *keys = SDL_GetKeyboardState(0);
    bool f9 = keys && keys[SDL_SCANCODE_F9];
    if (f9 && !f9_held) { /* F9: auto, 1x, 2x .. 8x, auto */
        settings_video.resolution = (settings_video.resolution + 1) % (PSYZ_INTERNAL_RES_MAX + 1);
        apply_resolution();
        if (settings_video.resolution) fprintf(stderr, "resolution %dx\n", settings_video.resolution);
        else fprintf(stderr, "resolution auto (%ux)\n", Psyz_VideoGetInternalResolution());
    }
    f9_held = f9;
    extern int gte_precise_on;
    void gte_precise_vblank(void);
    gte_precise_on = settings_video.wobble % 3; /* (gte.c, gpu.c: between frames, the GP0 queue just flushed) */
    gpu_depth_on = settings_video.depth;
    gte_precise_vblank();
    if (getenv("TM2PC_PRECISE_DEBUG") && count % 120 == 0) {
        extern unsigned gte_precise_stats[6];
        unsigned *t = gte_precise_stats;
        fprintf(stderr, "precise: mode %d, projected %u (ok %u), stored %u (kept %u), polygons %u (precise %u)\n",
                gte_precise_on, t[0], t[1], t[2], t[3], t[4], t[5]);
        for (int i = 0; i < 6; i++) t[i] = 0;
    }
    int wm = Psyz_VideoGetWindowMode(); /* F4: kept for next time */
    if (!dev_run() && wm != settings_video.window_mode) {
        settings_video.window_mode = wm;
        settings_save_section("video");
    }
    static int test_window = -1; /* tests: TM2PC_TEST_WINDOW=1280x720, the window's size (once, when shown) */
    if (test_window < 0 && count == 120) { /* (the renderer gives the window its own size when it first shows it) */
        int w = 0, h = 0, n = 0;
        test_window = getenv("TM2PC_TEST_WINDOW") && sscanf(getenv("TM2PC_TEST_WINDOW"), "%dx%d", &w, &h) == 2;
        SDL_Window **ws = test_window ? SDL_GetWindows(&n) : NULL;
        if (ws && n > 0) {
            bool ok = SDL_SetWindowSize(ws[0], w, h);
            SDL_SyncWindow(ws[0]);
            fprintf(stderr, "test: window %dx%d (%d windows, %s)\n", w, h, n, ok ? "set" : SDL_GetError());
        }
        SDL_free(ws);
        /* trailer footage: TM2PC_WINDOW_TITLE, the window's title (OBS finds the copy to record among several) */
        ws = getenv("TM2PC_WINDOW_TITLE") ? SDL_GetWindows(&n) : NULL;
        if (ws && n > 0) SDL_SetWindowTitle(ws[0], getenv("TM2PC_WINDOW_TITLE"));
        SDL_free(ws);
        /* trailer footage: TM2PC_TEST_DISPLAY=3840x2160, the window borderless fullscreen on the display of that size */
        int dw = 0, dh = 0, nd = 0;
        ws = getenv("TM2PC_TEST_DISPLAY") && sscanf(getenv("TM2PC_TEST_DISPLAY"), "%dx%d", &dw, &dh) == 2 ? SDL_GetWindows(&n) : NULL;
        SDL_DisplayID *ds = ws && n > 0 ? SDL_GetDisplays(&nd) : NULL;
        for (int i = 0; ds && i < nd; i++) {
            const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(ds[i]);
            int pw = m ? (int)(m->w * m->pixel_density + 0.5f) : 0, ph = m ? (int)(m->h * m->pixel_density + 0.5f) : 0;
            fprintf(stderr, "test: display %u %dx%d (%dx%d pixels)\n", (unsigned)ds[i], m ? m->w : 0, m ? m->h : 0, pw, ph);
            if (!m || ((m->w != dw || m->h != dh) && (pw != dw || ph != dh))) continue;
            SDL_SetWindowPosition(ws[0], SDL_WINDOWPOS_CENTERED_DISPLAY(ds[i]), SDL_WINDOWPOS_CENTERED_DISPLAY(ds[i]));
            SDL_SyncWindow(ws[0]);
            SDL_SetWindowFullscreenMode(ws[0], NULL);
            bool ok = SDL_SetWindowFullscreen(ws[0], true);
            SDL_SyncWindow(ws[0]);
            fprintf(stderr, "test: borderless on display %u (%dx%d, %s)\n", (unsigned)ds[i], dw, dh, ok ? "set" : SDL_GetError());
            break;
        }
        SDL_free(ds);
        SDL_free(ws);
    }
    { /* the mouse pointer: hidden after 3 seconds without moving, back when it moves */
        static float mx = -1, my = -1;
        static unsigned still;
        float x, y;
        SDL_GetMouseState(&x, &y);
        if (x != mx || y != my) {
            if (still >= 180) SDL_ShowCursor();
            still = 0;
            mx = x; my = y;
        } else if (still < 180 && ++still == 180) {
            SDL_HideCursor();
        }
    }
    static int rw = -1, rh; /* tests: TM2PC_TEST_RESIZE=960x720@2000, the window resized at that frame */
    static unsigned rat;
    if (rw < 0) {
        rw = 0;
        if (getenv("TM2PC_TEST_RESIZE") && sscanf(getenv("TM2PC_TEST_RESIZE"), "%dx%d@%u", &rw, &rh, &rat) != 3) rw = 0;
    }
    static int tres = -1; /* tests: TM2PC_TEST_RES=6@3000, the resolution changed at that frame (as in Options) */
    static unsigned tres_at;
    if (tres < 0 && (!getenv("TM2PC_TEST_RES") || sscanf(getenv("TM2PC_TEST_RES"), "%d@%u", &tres, &tres_at) != 2)) tres = 0;
    if (tres > 0 && count == tres_at) {
        settings_video.resolution = tres;
        video_settings_changed();
        fprintf(stderr, "test: resolution %dx\n", tres);
    }
    if (rw > 0 && count == rat) {
        int n = 0;
        SDL_Window **ws = SDL_GetWindows(&n);
        if (ws && n > 0) { SDL_SetWindowSize(ws[0], rw, rh); SDL_SyncWindow(ws[0]); fprintf(stderr, "test: window resized to %dx%d\n", rw, rh); }
        SDL_free(ws);
    }
    if (count++ % 30 == 0) /* follow window size changes (auto) */
        apply_resolution();
}

uint64_t gpu_last_present_ns; /* vplus/interp.c (uncapped): when a frame was last shown */
extern volatile unsigned rt_heartbeat; /* main.c: the freeze watchdog */
static void present_log(void) /* tests: TM2PC_PRESENT_LOG, the time between frames shown (ms) */
{
    static int on = -1;
    if (on < 0) on = getenv("TM2PC_PRESENT_LOG") != NULL;
    uint64_t now = SDL_GetTicksNS();
    if (on && gpu_last_present_ns) fprintf(stderr, "present: %.1f ms\n", (double)(now - gpu_last_present_ns) / 1e6);
    gpu_last_present_ns = now;
    rt_heartbeat++;
}
void gpu_psyz_present(void) /* vplus/interp.c (uncapped): what was drawn, shown now (no VBlank) */
{
    flush();
    Psyz_VideoVSync(0);
    present_log();
}
static void vblank(void)
{
    flush();
    resolution_vblank();
    if (display.rgb24) {
        show_rgb24();
    } else if (display.shown) {
        Psyz_VideoSetDisplayRGB24(NULL, 0, 0);
        display.shown = 0;
    }
    int interp_owns_present(void); /* vplus/interp.c: Uncapped, a battle's wait: shown on the display's clock instead */
    if (!interp_owns_present()) {
        Psyz_VideoVSync(0);
        present_log();
    } else if (SDL_GetTicksNS() - gpu_last_present_ns > 33000000ull) {
        /* Uncapped: the pacing wait shows the frames (interp_extra), but a VBlank over its time leaves no wait (and
         * tests' TM2PC_BENCH none at all). Nothing was shown then: every VBlank's drawing went into the same command
         * buffer, a frame shown only when the pacing clock gave up (a quarter second behind), and unpaced the buffer
         * grew until SDL's D3D12 backend ran out of memory and crashed (2026-11-18, 4 players at Farthest: 8 GB
         * in 30 s). Two VBlanks with nothing shown: this VBlank's own frame now. */
        Psyz_VideoVSync(0);
        present_log();
    }
    void rt_live_shot(void); /* tests: TM2PC_LIVE_SHOT (main.c) */
    rt_live_shot();
}

/* 24-bit bottom-up BMP from an RGB888 buffer */
static int write_bmp(const char *path, const unsigned char *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int stride = (w * 3 + 3) & ~3;
    unsigned size = 54u + (unsigned)(stride * h);
    unsigned char hdr[54] = {'B', 'M'};
    hdr[2] = (unsigned char)size; hdr[3] = (unsigned char)(size >> 8); hdr[4] = (unsigned char)(size >> 16); hdr[5] = (unsigned char)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (unsigned char)w; hdr[19] = (unsigned char)(w >> 8);
    hdr[22] = (unsigned char)h; hdr[23] = (unsigned char)(h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    unsigned char *row = calloc(1, (size_t)stride);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            const unsigned char *p = rgb + ((size_t)y * w + x) * 3;
            row[x * 3] = p[2]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[0];
        }
        fwrite(row, 1, (size_t)stride, f);
    }
    free(row);
    fclose(f);
    return 1;
}

static int capture(const char *path, int whole_vram)
{
    flush();
    int w = 0, h = 0;
    unsigned char *rgb = whole_vram ? Psyz_VideoAllocVramDump(&w, &h) : Psyz_VideoAllocCapturedFrame(&w, &h);
    if (!rgb) return 0;
    int ok = write_bmp(path, rgb, w, h);
    free(rgb);
    return ok;
}

/* texture banks (PSY-Z patch 15): a bank's transfers, whole (the game's banked rectangles never wrap) */
void Psyz_VideoBankLoadImage(int bank, int x, int y, int w, int h, const unsigned short *pix);
void Psyz_VideoBankStoreImage(int bank, int x, int y, int w, int h, unsigned short *pix);
int Psyz_VideoBanksSupported(void);
static void upload_bank(int bank, int x, int y, int w, int h, const uint16_t *p)
{
    flush();
    if (x + w <= 1024 && y + h <= 512) Psyz_VideoBankLoadImage(bank, x, y, w, h, p);
}
static void download_bank(int bank, int x, int y, int w, int h, uint16_t *p)
{
    flush();
    if (x + w <= 1024 && y + h <= 512) Psyz_VideoBankStoreImage(bank, x, y, w, h, p);
}
static int banks(void) { return Psyz_VideoBanksSupported(); }

const GpuBackend gpu_psyz_backend = {"psyz", init, command, upload, download, copy, fill, gp1, vblank, capture, polygon,
                                     upload_bank, download_bank, banks, polygon_depth, clear_depth};
