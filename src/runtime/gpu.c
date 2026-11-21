/* GPU front end: GP0/GP1 ports, GPUSTAT, DMA channel 2.
 *
 * Handles the protocol (command lengths, polylines, image transfer phases,
 * ordering-table DMA) and hands complete operations to the backend
 * (gpu_backend.h), which owns VRAM and draws. Execution is synchronous: the
 * GPU is always idle and ready when the game looks. Command framing follows
 * psx-spx. */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "runtime.h"
#include "gpu_backend.h"

void hdtex_vram_written(int x, int y, int w, int h); /* vplus/hd_textures.c: a sharper texture written over */

const GpuBackend *gpu_backend = &gpu_null_backend;

static uint32_t cmd[16];       /* current command being collected */
static uint32_t cmd_addr[16];  /* each word's guest address (DMA lists; 0 from the port) */
static uint32_t word_addr;     /* the address of the word being written (gpu_dma) */
static int cmd_len, cmd_need;
static int poly_line;          /* inside a polyline, until the terminator word */
static uint32_t poly_op, poly_color, poly_vertex, poly_pending_color;
static int poly_want_color;    /* gouraud polyline: next word is a color */
static struct { int active, x, y, w, h, n, i; uint16_t *pix; int bank; } upload, download;
/* the texture bank of a CPU transfer (the decomp's portVramUploadBank, TEXTURE_BANKS.md 6.5): read when the transfer
 * starts and kept with it (the game sets the word just before LoadImage / StoreImage and clears it after DrawSync) */
static int transfer_bank(void)
{
    static uint32_t word = 1;
    if (word == 1) word = port_sym("portVramUploadBank");
    if (!word || !gpu_backend->upload_bank) return 0;
    return (int)(mem_r32(&M.cpu, word) & 0x7F);
}
static uint32_t display_enabled;
static uint32_t gpustat_draw;  /* texpage and draw-mode bits shown in GPUSTAT */
static uint32_t last_e2;       /* (TM2PC_POLY_TRACE: the texture window last set) */
static uint32_t dma_dir;
uint64_t gpu_commands;

int gpu_select_backend(const GpuBackend *b)
{
    gpu_backend = b;
    return b->init();
}

static int command_length(uint32_t c)
{
    uint32_t op = c >> 24;
    if (op >= 0x20 && op < 0x40) {
        int n = (op & 8) ? 4 : 3, tex = (op >> 2) & 1, gour = (op >> 4) & 1;
        return 1 + n * (1 + tex) + (gour ? n - 1 : 0);
    }
    if (op >= 0x40 && op < 0x60)
        return (op & 0x10) ? 4 : 3; /* first segment; polylines continue */
    if (op >= 0x60 && op < 0x80) {
        int size = (op >> 3) & 3, tex = (op >> 2) & 1;
        return 2 + tex + (size == 0);
    }
    if (op >= 0x80 && op < 0xA0) return 4;
    if (op >= 0xA0 && op < 0xE0) return 3;
    if (op == 0x02) return 3;
    return 1;
}

/* ---- the PS1 GPU's size limit ----
 * The GPU skips a triangle when two of its vertices are more than 1023
 * pixels apart horizontally or 511 vertically, and draws a quad as two
 * triangles (v0 v1 v2, v1 v2 v3), each checked on its own. Games rely on
 * it: half-clipped ground and sky polygons near the camera simply vanish. */
static int vx(uint32_t w) { return ((int32_t)(w << 21)) >> 21; }
static int vy(uint32_t w) { return ((int32_t)(w << 5)) >> 21; }

static int too_big(uint32_t a, uint32_t b)
{
    int dx = vx(a) - vx(b), dy = vy(a) - vy(b);
    return dx > 1023 || dx < -1023 || dy > 511 || dy < -511;
}

static int tri_too_big(uint32_t a, uint32_t b, uint32_t c)
{
    return too_big(a, b) || too_big(b, c) || too_big(a, c);
}

int64_t gpu_culled_prims;
uint32_t gpu_ot_start; /* the ordering table being drawn (its DrawOTag start) */
static uint32_t gpu_ot_prev; /* (tests: the node before, TM2PC_OT_COPY_LOG) */

/* Precise vertices (gte.c): a polygon whose vertex words all have floats is
 * sent with them (gte_precise_on: 1 subpixel positions, 2 also their depth,
 * for perspective-correct textures). The size limit stays on the words. */
int gte_precise_lookup(uint32_t addr, uint32_t word, float out[3]);
extern int gte_precise_on;
static int gpu_ot_world;

/* The depth buffer (PSY-Z patch 20; the decomp's HIDDEN_AREAS.md 5 (a)): a view's 3D polygons with precise depth
 * (wobble none) are drawn with a depth test, so a surface behind another one is hidden whatever their order-table
 * slots (the PS1 sorted each polygon by its deepest corner: a big wall or ground polygon could paint over what was in
 * front of it), and the PS1's size limit is not needed for them (they are clipped by the graphics card). Polygons
 * without precise depth, and the sky's slots, are drawn in order as before, untested. */
static int gpu_ot_slot = -1; /* the order-table slot being drawn (its entry's index; -1 not an order table) */
static int depth_active(void)
{
    static int env = -1; /* tests: TM2PC_DEPTH=0 / 1 overrides the setting */
    if (env == -1) env = getenv("TM2PC_DEPTH") ? atoi(getenv("TM2PC_DEPTH")) : -2;
    extern int gpu_depth_on; /* (gpu_psyz.c: the setting, read between frames) */
    return gpu_ot_world && gte_precise_on >= 2 && gpu_backend->polygon_depth && (env >= 0 ? env : gpu_depth_on);
}

int gpu_order_ties; /* vplus/podium.c: its scene's near-ties inside a slot to the later polygon (send_polygon) */
int gpu_shadow_ground; /* vplus/fixes.c: portVisual bit 10 on (decomp request 102, RENDER_ANSWERS R10.2) */
/* a car's shadow (see-through, at its own slot or nearer): its depth test pulled this far toward the eye (w units, 8 a
 * world unit) so it stays over the ground under it: 2 slots. With 5 (until 2026-11-08) the shadow showed through its
 * own car's body wherever that came within 5 units of it: Roadkill's rear window and spoiler as he bounced (the user's
 * trailer take). Tests: TM2PC_SHADOW_PULL=w units */
static float shadow_pull(float wide)
{
    static float env = -2;
    if (env == -2) env = getenv("TM2PC_SHADOW_PULL") ? (float)atof(getenv("TM2PC_SHADOW_PULL")) : -1;
    return env >= 0 ? env : 2 * wide;
}
int gpu_depth_enabled(void) /* vplus/fixes.c: the depth test is on for a view's 3D (wobble none and the setting) */
{
    int ot_world = gpu_ot_world;
    gpu_ot_world = 1;
    int on = depth_active();
    gpu_ot_world = ot_world;
    return on;
}

/* An effect quad built from projected points plus whole-pixel offsets (the model emitter, 800CDC54: a point sprite
 * is its projected centre +- a size, a camera-facing beam its two projected ends +- a sideways step): those points
 * are the exact middles of corner pairs, and their projections have floats (by value). The corners keep their
 * offsets and take the points' subpixel places. Only in a view's 3D order table (a HUD quad is no effect). */
static int mid_floats(uint32_t a, uint32_t b, float out[3])
{
    int sx = vx(a) + vx(b), sy = vy(a) + vy(b);
    if ((sx & 1) || (sy & 1)) return 0;
    uint32_t word = (uint32_t)(sx / 2 & 0xFFFF) | (uint32_t)(sy / 2) << 16;
    if (!gte_precise_lookup(0, word, out)) return 0;
    float dx = out[0] - (float)(sx / 2), dy = out[1] - (float)(sy / 2);
    static int tr = -1;
    if (tr < 0) tr = getenv("TM2PC_PRECISE_TRACE") ? atoi(getenv("TM2PC_PRECISE_TRACE")) : 0;
    if (tr >= 4 && (dx > 3 || dx < -3 || dy > 3 || dy < -3)) fprintf(stderr, "effect mid far: %d,%d -> %.2f,%.2f\n", sx / 2, sy / 2, out[0], out[1]);
    extern int gte_precise_sat;
    if (!gte_precise_sat && (dx > 3 || dx < -3 || dy > 3 || dy < -3)) return 0; /* (the GTE's own rounding: up to ~1.5) */
    /* (nearer than H/2 the GTE's place is saturated, wrong and jumping as the camera moves: a tree beside the road
     * shook; the true place is used, the corners keeping their offsets) */
    out[0] = dx;
    out[1] = dy;
    return 1;
}
static int effect_quad(const uint32_t *w, const int *vtx, float xyw[12])
{
    uint32_t v[4] = {w[vtx[0]], w[vtx[1]], w[vtx[2]], w[vtx[3]]};
    float p[3], q[3];
    int k, beam = mid_floats(v[0], v[1], p) && mid_floats(v[2], v[3], q);
    if (!beam) {
        if (vx(v[0]) + vx(v[3]) != vx(v[1]) + vx(v[2]) || vy(v[0]) + vy(v[3]) != vy(v[1]) + vy(v[2])) return 0;
        if (!mid_floats(v[0], v[3], p)) return 0;
        q[0] = p[0], q[1] = p[1], q[2] = p[2];
    }
    /* A point sprite stands for a ball (a pickup's name, smoke, a spark), not a card at its middle's depth: with the
     * depth buffer it is tested its own half-size nearer, so a pickup's turning model no longer covers its name and
     * flickers through it (the PS1 drew the name in a nearer slot). */
    float near = 0;
    unsigned hh = M.cpu.gte_ctrl[26] & 0xFFFF;
    if (!beam && gte_precise_on >= 2 && hh) {
        int sw = abs(vx(v[0]) - vx(v[3])), sh = abs(vy(v[0]) - vy(v[3]));
        near = (float)(sw > sh ? sw : sh) / 2 * p[2] / (float)hh;
        /* a small one (a letter of a pickup's name, a spark): at least 96 nearer, the name's letters each their own
         * sprite, half a letter too little to clear the turning pickup (the user's FIRE behind its rocket, 2026-10-29) */
        if (sw <= 24 && sh <= 24 && near < 96) near = 96;
        if (near > p[2] / 2) near = p[2] / 2;
    }
    for (k = 0; k < 4; k++) {
        const float *d = k < 2 ? p : q;
        xyw[k * 3] = (float)vx(v[k]) + d[0];
        xyw[k * 3 + 1] = (float)vy(v[k]) + d[1];
        xyw[k * 3 + 2] = gte_precise_on < 2 ? 1 : p[2] - near; /* (one depth: a camera-facing quad is flat, and a beam's q may be another vertex's: a sideways bent pickup) */
    }
    extern unsigned gte_precise_effects;
    gte_precise_effects++;
    return 1;
}
/* A camera-facing sprite of the C's emitter (its four corners put at one depth: a pickup's name, a spark) tested its
 * own half-size nearer, as effect_quad's: the pickup's turning model drew over its name (the user's HOMING behind its
 * rocket, 2026-10-29: with the C's exact sprite corners the effect path, and its bias, was never taken) */
static void sprite_nearer(float xyw[12])
{
    float w0 = xyw[2], x0 = xyw[0], x1 = xyw[0], y0 = xyw[1], y1 = xyw[1];
    if (w0 <= 0) return;
    for (int i = 1; i < 4; i++) {
        float d = xyw[i * 3 + 2] - w0;
        if (d > w0 * 1e-4f || d < -w0 * 1e-4f) return; /* (not flat on the screen: a beam, a card turned) */
        if (xyw[i * 3] < x0) x0 = xyw[i * 3];
        if (xyw[i * 3] > x1) x1 = xyw[i * 3];
        if (xyw[i * 3 + 1] < y0) y0 = xyw[i * 3 + 1];
        if (xyw[i * 3 + 1] > y1) y1 = xyw[i * 3 + 1];
    }
    unsigned hh = M.cpu.gte_ctrl[26] & 0xFFFF;
    if (!hh) return;
    float sw = x1 - x0, sh = y1 - y0, near = (sw > sh ? sw : sh) / 2 * w0 / (float)hh;
    if (sw <= 24 && sh <= 24 && near < 96) near = 96;
    if (near > w0 / 2) near = w0 / 2;
    for (int i = 0; i < 4; i++) xyw[i * 3 + 2] = w0 - near;
}

/* tests: TM2PC_POLY_TRACE=AAAAAAAA,BBBBBBBB, the polygon holding both words followed through polygon() and
 * send_polygon() (decomp request 116: the crosswalk tile reaching the GPU undrawn) */
static int poly_trace;
#define PTRACE(...) do { if (poly_trace) { unsigned long long rt_vblanks_now(void); \
    fprintf(stderr, "poly trace: v%llu slot %d ", rt_vblanks_now(), gpu_ot_slot); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* try: with the depth test only (1 drawn; 0 nothing done, the caller goes on as before) */
static int send_polygon(const uint32_t *w, int n, const int *vtx, int nv, const uint32_t *addr, int try)
{
    PTRACE("send try %d precise %d", try, gte_precise_on);
    if (!gte_precise_on || !gpu_backend->polygon) { if (try) return 0; gpu_backend->command(w, n); return 1; }
    void port_precise_ring(void); /* vplus/port_config.c: the C's own subpixel places */
    port_precise_ring();
    float xyw[12];
    int ok = addr != 0;
    extern int gte_precise_by_value;
    gte_precise_by_value = gpu_ot_world;
    extern int gte_precise_found_put;
    int put = 1, put_far = 0; /* (every corner put: a sprite of the C's emitter; a put corner far from its pixel) */
    int byval = 0; /* (TM2PC_PRECISE_SHOW=2: a corner found by its word's value) */
    for (int i = 0; ok && i < nv; i++) {
        extern int gte_precise_why;
        ok = gte_precise_lookup(addr[vtx[i]], w[vtx[i]], &xyw[i * 3]);
        byval |= ok && gte_precise_why;
        put &= gte_precise_found_put;
        if (ok && gte_precise_found_put && (fabsf(xyw[i * 3] - (float)vx(w[vtx[i]])) > 4 || fabsf(xyw[i * 3 + 1] - (float)vy(w[vtx[i]])) > 4))
            put_far = 1;
        if (gte_precise_on < 2) xyw[i * 3 + 2] = 1;
    }
    /* A model's polygon with one corner the C emitter put far from its pixel: the emitter's place of a sprite corner
     * left at that packet word, the word since a model's vertex (car select's scene drawn again in between frames). The
     * corner pulled to the sprite: Sweet Tooth's roof spiking up to his sign, the user's 2026-10-29. Only a sprite whose
     * every corner the emitter put may be that far: this one as the words have it */
    if (ok && put_far && !put) {
        ok = 0;
        static int told;
        if (told++ < 10) fprintf(stderr, "gpu: a polygon's corner had a sprite's place (op %02X): drawn as its words\n", w[0] >> 24);
    }
    int effect = 0;
    if (ok && put && nv == 4 && gpu_ot_world) effect = 1; /* (a sprite or beam the C emitter put: as flat, the same limit) */
    if (!ok && addr && nv == 4 && gpu_ot_world) ok = effect = effect_quad(w, vtx, xyw);
    else if (ok && put && nv == 4 && gpu_ot_world && gte_precise_on >= 2 && ((w[0] >> 24) & 4)) sprite_nearer(xyw); /* (textured:
                                                                                * not the hill's box, put at its own depths) */
    PTRACE("corners ok %d put %d far %d byval %d effect %d world %d xyw %.1f,%.1f,%.1f %.1f,%.1f,%.1f %.1f,%.1f,%.1f %.1f,%.1f,%.1f",
           ok, put, put_far, byval, effect, gpu_ot_world, xyw[0], xyw[1], xyw[2], xyw[3], xyw[4], xyw[5], xyw[6], xyw[7], xyw[8],
           nv == 4 ? xyw[9] : 0, nv == 4 ? xyw[10] : 0, nv == 4 ? xyw[11] : 0);
    if (try && !ok) return 0;
    /* an effect quad (smoke, a tree, sparks) is flat on the screen, not clipped at the eye: one at the camera is as big
     * as the GTE's clamped places make it (the PS1 dropped it by the size limit). Drawn whole it covered the view: the
     * black bars by Los Angeles's tunnels, Paris's green sky, Antarctica's grey blocks. The size limit stays for them,
     * the C emitter's too: a grass beam of Holland's beside the eye flashed across the view's top left (the user's
     * 2026-10-31) */
    if (try && effect && (tri_too_big(w[vtx[0]], w[vtx[1]], w[vtx[2]]) || tri_too_big(w[vtx[1]], w[vtx[2]], w[vtx[3]]))) {
        static int big = -1; /* tests: TM2PC_BIG_EFFECT_LOG=1, each effect quad the size limit keeps */
        if (big < 0) big = getenv("TM2PC_BIG_EFFECT_LOG") ? atoi(getenv("TM2PC_BIG_EFFECT_LOG")) : 0; /* (2: drawn anyway) */
        unsigned long long rt_vblanks_now(void);
        if (big) fprintf(stderr, "big effect: v%llu op %02X %d,%d %d,%d %d,%d %d,%d w %.1f\n", rt_vblanks_now(), w[0] >> 24,
                         vx(w[vtx[0]]), vy(w[vtx[0]]), vx(w[vtx[1]]), vy(w[vtx[1]]), vx(w[vtx[2]]), vy(w[vtx[2]]),
                         vx(w[vtx[3]]), vy(w[vtx[3]]), xyw[2]);
        PTRACE("effect quad kept by the size limit (returns %d)", big == 2);
        if (big != 2) return 0;
    }
    static int show = -1; /* tests: TM2PC_PRECISE_SHOW=1, the polygons without precise vertices tinted red */
    if (show < 0) show = getenv("TM2PC_PRECISE_SHOW") ? atoi(getenv("TM2PC_PRECISE_SHOW")) : 0;
    if (show >= 2 && ok && gpu_ot_world && (show == 3 ? ((w[0] >> 24) & 4) != 0 : effect || byval)) {
        /* (2: effect quads blue, corners found by value green; 3: each textured polygon tinted by its page and palette) */
        static uint32_t tint[16];
        int gour = (w[0] >> 24) & 0x10;
        for (int i = 0; i < n && i < 16; i++) tint[i] = w[i];
        uint32_t h = ((w[gour ? 5 : 4] >> 16) * 2654435761u) ^ ((w[2] >> 16) * 40503u);
        h = (h ^ (h >> 13)) * 2246822519u;
        tint[0] = show == 3 ? (tint[0] & 0xFE000000u) | (h & 0xFFFFFFu) | 0x404040u
                            : (tint[0] & 0xFF000000u) | (effect ? 0xFF0000u : 0x00FF00u);
        w = tint;
    }
    int depth = 0;
    if (ok && depth_active() && gpu_ot_slot >= 0 && gpu_ot_slot < 4090) /* (4090..4095: the sky and backdrops, drawn first) */
        depth = (w[0] >> 24) & 2 ? 2 : 1; /* a see-through polygon is tested, not written */
    PTRACE("depth %d (active %d)", depth, depth_active());
    if (try && !depth) return 0;
    float push = 0; /* (w units: the depth test's extra distance, the order table's bias) */
    static int old_bias = -1; /* (tests: TM2PC_OLD_BIAS=1, no bias push) */
    if (old_bias < 0) old_bias = getenv("TM2PC_OLD_BIAS") != NULL;
    if (depth) {
        /* a far mesh the decomp's GTE range pass scaled by 1/2^k (FAR_DISTANCE.md 2) has its depths 2^k too near, its
         * slot still in world units (its shift lowered by k): the slot's distance against the deepest corner's (w is
         * 8 units a world unit) gives 2^k back. Near slots (< 1,024: a world unit each) hold a decal's bias of up to 32
         * slots; a scaled mesh is at least 256 world units away, so only a ratio of 1.6 or more is a scale. */
        int s = gpu_ot_slot;
        float zs = s < 1024 ? (float)s : s < 1792 ? (float)((s - 1024) * 4 + 1024) : s < 3328 ? (float)((s - 1792) * 8 + 4096)
                                                                                          : (float)((s - 3328) * 32 + 16384);
        float hi = xyw[2];
        for (int i = 1; i < nv; i++) if (xyw[i * 3 + 2] > hi) hi = xyw[i * 3 + 2];
        if (hi > 0 && zs >= 256 && zs * 8 >= hi * 1.6f) {
            float r = zs * 8 / hi, scale = r < 3 ? 2.0f : r < 6 ? 4.0f : 8.0f;
            for (int i = 0; i < nv; i++) xyw[i * 3 + 2] *= scale;
        } else if (hi > 0 && !old_bias) {
            /* a slot farther than the polygon: the order table's bias (Amazonia's lava +10 / +31 slots under the stone
             * path at 0, Antarctica's ice +20..+31; decomp RENDER_ANSWERS.md R4.0). Retail drew it that much farther,
             * so the depth test takes it that much farther too: the coplanar layer under keeps under, where a quarter
             * unit a slot lost to the rounding between two tilings (the black patches on the lava, the user's
             * 2026-10-30). Two slots' leeway for the slot's rounding and the in-between frames; at most 32 slots */
            float wide = s < 1024 ? 8.0f : s < 1792 ? 32.0f : s < 3328 ? 64.0f : 256.0f, over = zs * 8 - hi;
            /* (opaque only: a see-through polygon's slot can be farther for another reason, every car's shadow sorted
             * behind its own car, portVisual bit 3; pushed, the shadows went under the road) */
            if (over > 2 * wide && depth == 1) push = over < 32 * wide ? over : 32 * wide;
            /* and a slot nearer than the polygon: a bias in front (every car's shadow, -2..-7 entries so the blob wins
             * over the road under the car; decals). Pulled toward the eye that much, at most 8 slots: a shadow on a
             * tiled roof's rising seams stays over it (Axel's on Roof Tops, the user's 2026-10-30; RENDER_ANSWERS R5.4) */
            else if (-over > 2 * wide) push = depth == 2 && gpu_shadow_ground ? -shadow_pull(wide) : -over < 8 * wide ? over : -8 * wide;
            /* (with portVisual bit 10 a shadow sorts 2 entries in front of its own nearest corner, its entry nearer than
             * its corners by its whole depth: shadow_pull's fixed slots, not the whole pull, R10.2) */
            /* a see-through polygon at its own slot or farther: as a car's shadow, which bit 3 sorts behind its car
             * (its slot no longer its data's -2..-7 entries in front): tested shadow_pull nearer, that bias given back, so
             * it stays over a road rising a little under it */
            if (depth == 2 && over > -wide) push = -shadow_pull(wide);
        }
    }
    static long long dlog = -1; /* tests: TM2PC_DEPTH_LOG=<VBlank>, from then each depth-tested polygon's slot and corners */
    if (dlog < 0) dlog = getenv("TM2PC_DEPTH_LOG") ? atoll(getenv("TM2PC_DEPTH_LOG")) : 0;
    unsigned long long rt_vblanks_now(void);
    if (dlog && depth && rt_vblanks_now() >= (unsigned long long)dlog) {
        float lo = xyw[2], hi = xyw[2];
        for (int i = 1; i < nv; i++) {
            if (xyw[i * 3 + 2] < lo) lo = xyw[i * 3 + 2];
            if (xyw[i * 3 + 2] > hi) hi = xyw[i * 3 + 2];
        }
        fprintf(stderr, "depth poly: v%llu slot %d w %.1f..%.1f op %08X%s", rt_vblanks_now(), gpu_ot_slot, lo, hi, w[0], try ? " whole" : "");
        for (int i = 0; i < nv; i++) fprintf(stderr, " %.1f,%.1f,%.1f", xyw[i * 3], xyw[i * 3 + 1], xyw[i * 3 + 2]);
        fputc('\n', stderr);
    }
    if (depth == 1 && gpu_order_ties) {
        /* (the podium's scene, vplus/podium.c) inside one slot near-ties go to the later polygon, as the PS1's order drew
         * them: each next opaque polygon of the slot a half depth unit nearer, at most 8 (a world unit). Outlaw 2's side
         * guns and the door under them shared a slot and swapped every frame as the camera moved (the user's 2026-11-11) */
        static int tie_slot = -1, tie_n;
        tie_n = gpu_ot_slot == tie_slot ? tie_n + 1 : 0;
        tie_slot = gpu_ot_slot;
        push -= 0.5f * (float)(tie_n < 16 ? tie_n : 16);
    }
    if (depth) {
        /* near-ties go to the nearer slot, as the PS1's order drew them (a decal linked nearer than its wall, or a
         * painting a hair behind its wall): each slot pushes a quarter of a depth unit farther; a slot is 1-32 world
         * units (8-256 depth units), so two surfaces only that much apart are drawn by their slots */
        PTRACE("drawn with the depth test (bias %.2f, push %.1f)", (float)gpu_ot_slot * 0.25f + push, push);
        gpu_backend->polygon_depth(w, n, xyw, depth, (float)gpu_ot_slot * 0.25f + push);
        return 1;
    }
    extern unsigned gte_precise_stats[6];
    gte_precise_stats[4]++;
    gte_precise_stats[5] += ok;
    static int trace = -1; /* tests: TM2PC_PRECISE_TRACE=2, each polygon drawn without its floats */
    if (trace < 0) trace = getenv("TM2PC_PRECISE_TRACE") ? atoi(getenv("TM2PC_PRECISE_TRACE")) : 0;
    if (trace >= 5 && !addr) {
        unsigned long long rt_vblanks_now(void);
        fprintf(stderr, "precise no address: op %02X v%llu ot %d", w[0] >> 24, rt_vblanks_now(), gpu_ot_world);
        for (int i = 0; i < nv; i++) fprintf(stderr, " %d,%d", vx(w[vtx[i]]), vy(w[vtx[i]]));
        fputc('\n', stderr);
    }
    if (trace >= 2 && !ok && addr && (gpu_ot_world || trace >= 3)) { /* (2: the views' 3D only) */
        unsigned long long rt_vblanks_now(void);
        fprintf(stderr, "precise miss: op %02X", w[0] >> 24);
        if (trace >= 5) fprintf(stderr, " v%llu", rt_vblanks_now());
        for (int i = 0; i < nv; i++) {
            float f[3];
            extern int gte_precise_why;
            int got = gte_precise_lookup(addr[vtx[i]], w[vtx[i]], f);
            fprintf(stderr, " [%08X %d,%d%s%d]", addr[vtx[i]], vx(w[vtx[i]]), vy(w[vtx[i]]), got ? " ok" : " x", gte_precise_why);
            int gte_precise_stored(uint32_t addr, uint32_t *word, float out[3]);
            uint32_t sw;
            if (trace >= 5 && gte_precise_stored(addr[vtx[i]], &sw, f)) fprintf(stderr, "(was %d,%d %.1f,%.1f)", vx(sw), vy(sw), f[0], f[1]);
        }
        fputc('\n', stderr);
    }
    if (show && !ok) {
        static uint32_t red[16];
        for (int i = 0; i < n && i < 16; i++) red[i] = w[i];
        red[0] = (red[0] & 0xFF000000u) | 0x0000FFu;
        gpu_backend->polygon(red, n, 0);
        return 1;
    }
    PTRACE("drawn without the depth test (precise %d)", ok);
    gpu_backend->polygon(w, n, ok ? xyw : 0);
    return 1;
}

/* Send a polygon command, applying the size limit. */
static void polygon(const uint32_t *w, int n)
{
    uint32_t op = w[0] >> 24;
    int quad = (op & 8) != 0, tex = (op >> 2) & 1, gour = (op >> 4) & 1;
    int nv = quad ? 4 : 3, at = 1;
    int vtx[4], uv[4], col[4];
    for (int i = 0; i < nv; i++) {
        col[i] = i == 0 ? 0 : (gour ? at++ : 0);
        vtx[i] = at++;
        uv[i] = tex ? at++ : -1;
    }
    static uint32_t trace_a, trace_b; /* (TM2PC_POLY_TRACE, above send_polygon) */
    static int trace_on = -1;
    if (trace_on < 0) trace_on = getenv("TM2PC_POLY_TRACE") && sscanf(getenv("TM2PC_POLY_TRACE"), "%x,%x", &trace_a, &trace_b) == 2;
    poly_trace = 0;
    if (trace_on) {
        int a = 0, b = 0;
        for (int i = 1; i < n; i++) a |= w[i] == trace_a, b |= w[i] == trace_b;
        poly_trace = a && b;
    }
    if (poly_trace) {
        unsigned long long rt_vblanks_now(void);
        fprintf(stderr, "poly trace: v%llu slot %d world %d reached:", rt_vblanks_now(), gpu_ot_slot, gpu_ot_world);
        for (int i = 0; i < n; i++) fprintf(stderr, " %08X", w[i]);
        fprintf(stderr, " addr %06X draw mode %04X window %08X\n", cmd_addr[0] & 0xFFFFFF, gpustat_draw, last_e2);
        /* TM2PC_POLY_TRACE=A,B,1: that polygon drawn untextured magenta (its place and depth kept): is it its texels? */
        static int magenta = -1;
        if (magenta < 0) magenta = strchr(getenv("TM2PC_POLY_TRACE"), ',') && strchr(strchr(getenv("TM2PC_POLY_TRACE"), ',') + 1, ',') != NULL;
        if (magenta && tex && !gour) {
            static uint32_t mw[5], ma[5];
            static const int mv[4] = {1, 2, 3, 4};
            mw[0] = ((uint32_t)(op & ~0x07u) << 24) | 0xFF00FFu; /* (flat, untextured, opaque) */
            for (int i = 0; i < nv; i++) mw[1 + i] = w[vtx[i]], ma[1 + i] = cmd_addr[vtx[i]];
            fprintf(stderr, "poly trace: drawn untextured magenta\n");
            if (depth_active() && send_polygon(mw, 1 + nv, mv, nv, ma, 1)) return;
            send_polygon(mw, 1 + nv, mv, nv, ma, 0);
            return;
        }
    }
    /* with the depth test, a polygon with precise depth is drawn whole (no size limit: clipped by the graphics card) */
    if (depth_active() && send_polygon(w, n, vtx, nv, cmd_addr, 1)) return;
    PTRACE("not drawn whole (depth active %d): the size-limit path", depth_active());
    int gte_precise_stored(uint32_t addr, uint32_t *word, float out[3]);
    static int drop_log = -1; /* tests: TM2PC_DROP_LOG=1, each polygon of a view's 3D the size limit cuts (whole or half) */
    if (drop_log < 0) drop_log = getenv("TM2PC_DROP_LOG") != NULL;
    if (drop_log && gpu_ot_slot >= 0 && (quad ? tri_too_big(w[vtx[0]], w[vtx[1]], w[vtx[2]]) || tri_too_big(w[vtx[1]], w[vtx[2]], w[vtx[3]])
                                              : tri_too_big(w[vtx[0]], w[vtx[1]], w[vtx[2]]))) {
        unsigned long long rt_vblanks_now(void);
        fprintf(stderr, "size drop: v%llu slot %d op %02X depth %d", rt_vblanks_now(), gpu_ot_slot, op, depth_active());
        for (int i = 0; i < nv; i++) {
            float f[3] = {0, 0, 0};
            uint32_t sw;
            int got = cmd_addr[vtx[i]] && gte_precise_stored(cmd_addr[vtx[i]], &sw, f);
            int gte_precise_lookup(uint32_t addr, uint32_t word, float out[3]);
            extern int gte_precise_why;
            int found = gte_precise_lookup(cmd_addr[vtx[i]], w[vtx[i]], f);
            fprintf(stderr, " %d,%d%s[%s%d w %.1f addr %06X]", vx(w[vtx[i]]), vy(w[vtx[i]]), got ? "" : "(no float)", found ? "found" : "why",
                    gte_precise_why, f[2], cmd_addr[vtx[i]] & 0xFFFFFF);
        }
        fputc('\n', stderr);
    }
    if (!quad) {
        if (tri_too_big(w[vtx[0]], w[vtx[1]], w[vtx[2]])) { gpu_culled_prims++; return; }
        send_polygon(w, n, vtx, nv, cmd_addr, 0);
        return;
    }
    int bad_a = tri_too_big(w[vtx[0]], w[vtx[1]], w[vtx[2]]);
    int bad_b = tri_too_big(w[vtx[1]], w[vtx[2]], w[vtx[3]]);
    if (!bad_a && !bad_b) { send_polygon(w, n, vtx, nv, cmd_addr, 0); return; }
    PTRACE("size limit: halves too big %d %d", bad_a, bad_b);
    if (bad_a && bad_b) { gpu_culled_prims++; return; }
    /* draw only the surviving triangle, as a 3-vertex command */
    static const int tris[2][3] = {{0, 1, 2}, {1, 2, 3}};
    const int *t = tris[bad_a ? 1 : 0];
    uint32_t out[12], out_addr[12] = {0};
    int k = 0, out_vtx[3];
    out[k++] = ((op & ~8u) << 24) | ((col[t[0]] ? w[col[t[0]]] : w[0]) & 0xFFFFFF);
    for (int j = 0; j < 3; j++) {
        if (j > 0 && gour) out[k++] = w[col[t[j]]] & 0xFFFFFF;
        out_vtx[j] = k;
        out_addr[k] = cmd_addr[vtx[t[j]]];
        out[k++] = w[vtx[t[j]]];
        if (tex) /* CLUT rides on the first UV word, texture page on the second */
            out[k++] = (w[uv[t[j]]] & 0xFFFF) | (j < 2 ? (w[uv[j]] & 0xFFFF0000u) : 0);
    }
    gpu_culled_prims++;
    send_polygon(out, k, out_vtx, 3, out_addr, 0);
}

static void line(const uint32_t *w, int n)
{
    int gour = (w[0] >> 28) & 1; /* op & 0x10 */
    uint32_t a = w[1], b = w[gour ? 3 : 2];
    if (too_big(a, b)) { gpu_culled_prims++; return; }
    gpu_backend->command(w, n);
}

static void begin_transfer(void)
{
    int x = cmd[1] & 1023, y = (cmd[1] >> 16) & 511;
    int w = ((cmd[2] - 1) & 1023) + 1, h = (((cmd[2] >> 16) - 1) & 511) + 1;
    if ((cmd[0] >> 24) < 0xC0) {
        upload = (typeof(upload)){1, x, y, w, h, w * h, 0, malloc((size_t)w * h * 2), transfer_bank()};
    } else {
        free(download.pix);
        download = (typeof(download)){1, x, y, w, h, w * h, 0, malloc((size_t)w * h * 2), transfer_bank()};
        if (download.bank) gpu_backend->download_bank(download.bank, x, y, w, h, download.pix);
        else gpu_backend->download(x, y, w, h, download.pix);
    }
}

static void execute(void)
{
    uint32_t op = cmd[0] >> 24;
    gpu_commands++;
    if (op == 0x02) { /* fill rectangle: x in 16-pixel steps */
        int x = cmd[1] & 0x3F0, y = (cmd[1] >> 16) & 0x1FF;
        int w = ((cmd[2] & 0x3FF) + 15) & ~15, h = (cmd[2] >> 16) & 0x1FF;
        if (w && h) {
            gpu_backend->fill(x, y, w, h, cmd[0] & 0xFFFFFF);
            hdtex_vram_written(x, y, w, h);
        }
    } else if (op >= 0x80 && op < 0xA0) { /* VRAM to VRAM */
        int w = ((cmd[3] - 1) & 1023) + 1, h = (((cmd[3] >> 16) - 1) & 511) + 1;
        gpu_backend->copy(cmd[1] & 1023, (cmd[1] >> 16) & 511, cmd[2] & 1023, (cmd[2] >> 16) & 511, w, h);
        hdtex_vram_written(cmd[2] & 1023, (cmd[2] >> 16) & 511, w, h);
    } else if (op >= 0xA0 && op < 0xE0) {
        begin_transfer();
    } else {
        if (op == 0xE1)
            gpustat_draw = (gpustat_draw & ~0x87FFu) | (cmd[0] & 0x7FF) | ((cmd[0] & 0x800) << 4);
        else if (op == 0xE2)
            last_e2 = cmd[0];
        else if (op == 0xE6)
            gpustat_draw = (gpustat_draw & ~0x1800u) | ((cmd[0] & 3) << 11);
        if (op >= 0x20 && op < 0x40)
            polygon(cmd, cmd_len);
        else if (op >= 0x40 && op < 0x60)
            line(cmd, cmd_len);
        else
            gpu_backend->command(cmd, cmd_len);
    }
}

void gpu_write_gp0(uint32_t w)
{
    if (upload.active) {
        upload.pix[upload.i++] = (uint16_t)w;
        if (upload.i < upload.n)
            upload.pix[upload.i++] = (uint16_t)(w >> 16);
        if (upload.i >= upload.n) {
            if (upload.bank) {
                gpu_backend->upload_bank(upload.bank, upload.x, upload.y, upload.w, upload.h, upload.pix);
            } else {
                static int log = -1; /* tests (TM2PC_VRAM_LOG): each upload into the cars' palettes and textures (x 768 on) */
                if (log < 0) log = getenv("TM2PC_VRAM_LOG") != NULL;
                if (log && upload.x + upload.w > 768 && upload.y < 200)
                    fprintf(stderr, "vram: vblank %llu upload %d,%d %dx%d\n", (unsigned long long)M.vblanks, upload.x, upload.y,
                            upload.w, upload.h);
                vp_vram_upload(upload.x, upload.y, upload.w, upload.h, upload.pix);
                gpu_backend->upload(upload.x, upload.y, upload.w, upload.h, upload.pix);
                hdtex_vram_written(upload.x, upload.y, upload.w, upload.h);
            }
            free(upload.pix);
            upload.pix = 0;
            upload.active = 0;
        }
        return;
    }
    if (poly_line) {
        if ((w & 0xF000F000u) == 0x50005000u) { /* terminator */
            poly_line = 0;
            return;
        }
        uint32_t seg[4];
        if (!(poly_op & 0x10)) { /* flat: every word is the next vertex */
            seg[0] = (poly_op & ~8u) << 24 | (poly_color & 0xFFFFFF);
            seg[1] = poly_vertex; seg[2] = w;
            line(seg, 3);
            poly_vertex = w;
        } else if (poly_want_color) {
            poly_pending_color = w;
            poly_want_color = 0;
        } else {
            seg[0] = (poly_op & ~8u) << 24 | (poly_color & 0xFFFFFF);
            seg[1] = poly_vertex; seg[2] = poly_pending_color & 0xFFFFFF; seg[3] = w;
            line(seg, 4);
            poly_color = poly_pending_color; poly_vertex = w;
            poly_want_color = 1;
        }
        return;
    }
    if (cmd_len == 0) {
        cmd_need = command_length(w);
        if ((w >> 24) == 0x00 || (w >> 24) == 0x01)
            return;
    }
    cmd_addr[cmd_len] = word_addr;
    cmd[cmd_len++] = w;
    if (cmd_len >= cmd_need) {
        uint32_t op = cmd[0] >> 24;
        if (op >= 0x48 && op < 0x60 && (op & 8)) { /* polyline: first segment, then continue */
            cmd[0] = (cmd[0] & 0x00FFFFFFu) | ((op & ~8u) << 24);
            gpu_commands++;
            line(cmd, cmd_len);
            poly_line = 1;
            poly_op = op;
            poly_color = (op & 0x10) ? cmd[2] : cmd[0];
            poly_vertex = cmd[cmd_len - 1];
            poly_want_color = 1;
            cmd_len = 0;
            return;
        }
        execute();
        cmd_len = 0;
    }
}

void gpu_write_gp1(uint32_t w)
{
    switch (w >> 24) {
    case 0x00: /* reset */
        cmd_len = 0; upload.active = download.active = poly_line = 0;
        display_enabled = 0; gpustat_draw = 0;
        break;
    case 0x01: cmd_len = 0; upload.active = poly_line = 0; break; /* reset command buffer */
    case 0x03: display_enabled = !(w & 1); break;
    case 0x04: dma_dir = w & 3; break;
    default: break;
    }
    gpu_backend->gp1(w);
}

uint32_t gpu_read_data(void)
{
    if (!download.active)
        return 0;
    uint32_t v = download.pix[download.i++];
    if (download.i < download.n)
        v |= (uint32_t)download.pix[download.i++] << 16;
    if (download.i >= download.n)
        download.active = 0;
    return v;
}

uint32_t gpu_read_status(void)
{
    /* ready for commands (26), ready to send VRAM (27), ready for DMA (28) */
    uint32_t s = gpustat_draw | (1u << 26) | (1u << 27) | (1u << 28) | (dma_dir << 29);
    if (!display_enabled) s |= 1u << 23;
    if (M.vblanks & 1) s |= 1u << 31;
    if (dma_dir == 2) s |= 1u << 25;
    return s;
}

volatile uint32_t gpu_walking; /* main.c's freeze watchdog: the order table being walked (0 none) */
static void gpu_dma_inner(uint32_t madr, uint32_t bcr, uint32_t chcr);
uint64_t gpu_dma_ns; /* runtime/hle.c: the time drawing lists took since the last VBlank (the slow frames' log) */
void gpu_dma(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    uint64_t t = SDL_GetTicksNS();
    gpu_dma_inner(madr, bcr, chcr);
    gpu_dma_ns += SDL_GetTicksNS() - t;
}
static void gpu_dma_inner(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    uint32_t a = madr & (GUEST_RAM_BYTES - 4);
    uint32_t mode = (chcr >> 9) & 3;
    if (mode == 2) { /* linked list (ordering table) */
        gpu_ot_start = madr;
        int port_ot_is_world(uint32_t madr); /* vplus/port_config.c */
        gpu_ot_world = port_ot_is_world(madr);
        static int otlog = -1;
        if (otlog < 0) otlog = getenv("TM2PC_OT_LOG") != NULL;
        if (otlog) fprintf(stderr, "ot walk %08X%s\n", madr, gpu_ot_world ? " (3D)" : "");
        if (depth_active() && gpu_backend->clear_depth) gpu_backend->clear_depth(); /* (each view's 3D its own) */
        int port_ot_slot(uint32_t addr); /* vplus/port_config.c */
        /* a list that comes back to a packet it passed (a packet linked twice) would be drawn again and again, a million
         * times: the game frozen (the user's Gun Game at a respawn, 2026-10-29?). Brent's check: the walk's mark moved
         * to the node at each power of two; met again, the list loops: the walk ends there (logged) */
        uint32_t mark = 0xFFFFFFFFu, power = 1, steps = 0;
        gpu_walking = madr;
        gpu_ot_prev = 0;
        for (int guard = 0; guard < 0x100000; guard++) {
            if (a == mark) {
                static int told;
                if (told++ < 8) fprintf(stderr, "gpu: the order table at %08X loops (node %08X): its walk ended\n", madr, 0x80000000u | a);
                break;
            }
            if (++steps == power) { mark = a; power <<= 1; steps = 0; }
            int slot = gpu_ot_world ? port_ot_slot(0x80000000u | a) : -1;
            if (slot >= 0 || !gpu_ot_world) gpu_ot_slot = slot; /* (a packet: the slot whose entry linked to it) */
            uint32_t head = mem_r32(&M.cpu, 0x80000000u | a);
            uint32_t n = head >> 24;
            static int hide_slot = -2; /* tests: TM2PC_HIDE_SLOT=N, the 3D order table's entry N not drawn */
            if (hide_slot == -2) hide_slot = getenv("TM2PC_HIDE_SLOT") ? atoi(getenv("TM2PC_HIDE_SLOT")) : -1;
            if (hide_slot >= 0 && gpu_ot_world && gpu_ot_slot == hide_slot) n = 0;
            static int bad_log = -1; /* tests (decomp request 105's run 2): a node sending a VRAM copy (GP0 0x80), the node
                                      * that linked to it, the list's base: the first 12 */
            if (bad_log < 0) bad_log = getenv("TM2PC_OT_COPY_LOG") ? 400 : 0;
            for (uint32_t i = 1; bad_log > 0 && i + 3 <= n; i++) { /* (any word of the packet: an impossible copy, no height or width) */
                uint32_t w0 = mem_r32(&M.cpu, 0x80000000u | ((a + 4 * i) & (GUEST_RAM_BYTES - 4)));
                uint32_t sz = mem_r32(&M.cpu, 0x80000000u | ((a + 4 * (i + 3)) & (GUEST_RAM_BYTES - 4))); /* (h << 16 | w) */
                if ((w0 >> 24) != 0x80 || !((sz >> 16) == 0 || (sz & 0xFFFF) == 0 || ((sz >> 16) == 1 && (sz & 0xFFFF) > 64))) continue;
                bad_log--;
                fprintf(stderr, "ot copy: vblank %llu list %08X (%s) node %08X from %08X, %u words, the copy at word %u:",
                        (unsigned long long)M.vblanks, madr, gpu_ot_world ? "3D" : "2D", 0x80000000u | a, gpu_ot_prev, n, i);
                for (uint32_t k = 0; k <= n && k < 12; k++) fprintf(stderr, " %08X", mem_r32(&M.cpu, 0x80000000u | ((a + 4 * k) & (GUEST_RAM_BYTES - 4))));
                fprintf(stderr, "\n");
                break;
            }
            gpu_ot_prev = 0x80000000u | a;
            for (uint32_t i = 1; i <= n; i++) {
                word_addr = 0x80000000u | ((a + 4 * i) & (GUEST_RAM_BYTES - 4));
                gpu_write_gp0(mem_r32(&M.cpu, word_addr));
            }
            word_addr = 0;
            if ((head & 0xFFFFFF) == 0xFFFFFF) /* the end (only this: with 16 MB a link may have bit 23) */
                break;
            a = head & (GUEST_RAM_BYTES - 4);
        }
        gpu_ot_slot = -1;
        gpu_walking = 0;
        return;
    }
    uint32_t words = mode == 0 ? ((bcr & 0xFFFF) ? (bcr & 0xFFFF) : 0x10000) : (bcr & 0xFFFF) * (bcr >> 16);
    for (uint32_t i = 0; i < words; i++, a = (a + 4) & (GUEST_RAM_BYTES - 4)) {
        if (chcr & 1)
            gpu_write_gp0(mem_r32(&M.cpu, 0x80000000u | a));
        else
            mem_w32(&M.cpu, 0x80000000u | a, gpu_read_data());
    }
}
