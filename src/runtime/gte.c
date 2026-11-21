/* Register-level GTE (COP2) for recompiled PSX code.
 *
 * One call per COP2 instruction: gte_command() for commands, gte_read_* /
 * gte_write_* for mfc2/cfc2/mtc2/ctc2/lwc2/swc2. Registers live in
 * CPU.gte_data / CPU.gte_ctrl as raw 32-bit words; 16-bit registers are
 * normalized on read the way the hardware does (including the sign
 * extension of H on read).
 *
 * Arithmetic, saturation and FLAG rules follow the psx-spx documentation
 * (GTE section); the structure follows the well-known MAME GTE design (smf),
 * also used by PsyCross. Checked against retail through fixtures
 * (tools/recomp/test_fixtures.py). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx_cpu.h"

#define D(c) ((c)->gte_data)
#define C(c) ((c)->gte_ctrl)
static inline int32_t lo16(uint32_t w) { return (int16_t)(w & 0xFFFF); }
static inline int32_t hi16(uint32_t w) { return (int16_t)(w >> 16); }

/* ---- register views ---- */
#define VX(c, n) ((n) < 3 ? lo16(D(c)[(n) * 2]) : IR1(c))
#define VY(c, n) ((n) < 3 ? hi16(D(c)[(n) * 2]) : IR2(c))
#define VZ(c, n) ((n) < 3 ? lo16(D(c)[(n) * 2 + 1]) : IR3(c))
#define RGBC_R(c) ((int32_t)(D(c)[6] & 0xFF))
#define RGBC_G(c) ((int32_t)((D(c)[6] >> 8) & 0xFF))
#define RGBC_B(c) ((int32_t)((D(c)[6] >> 16) & 0xFF))
#define IR0(c) lo16(D(c)[8])
#define IR1(c) lo16(D(c)[9])
#define IR2(c) lo16(D(c)[10])
#define IR3(c) lo16(D(c)[11])
#define SX(c, n) lo16(D(c)[12 + (n)])
#define SY(c, n) hi16(D(c)[12 + (n)])
#define SZ(c, n) ((int32_t)(D(c)[16 + (n)] & 0xFFFF))
/* matrices: 0 = rotation (ctrl 0-4), 1 = light (8-12), 2 = light color (16-20) */
static inline int32_t M(CPU *c, int base, int i, int j)
{
    int k = i * 3 + j; /* 0..8 */
    uint32_t w = C(c)[base + k / 2];
    return (k & 1) ? hi16(w) : lo16(w);
}
#define TRX(c) ((int32_t)C(c)[5])
#define TRY(c) ((int32_t)C(c)[6])
#define TRZ(c) ((int32_t)C(c)[7])
#define RBK(c) ((int32_t)C(c)[13])
#define GBK(c) ((int32_t)C(c)[14])
#define BBK(c) ((int32_t)C(c)[15])
#define RFC(c) ((int32_t)C(c)[21])
#define GFC(c) ((int32_t)C(c)[22])
#define BFC(c) ((int32_t)C(c)[23])
#define OFX(c) ((int32_t)C(c)[24])
#define OFY(c) ((int32_t)C(c)[25])
#define H(c) ((uint32_t)(C(c)[26] & 0xFFFF))
#define DQA(c) lo16(C(c)[27])
#define DQB(c) ((int32_t)C(c)[28])
#define ZSF3(c) lo16(C(c)[29])
#define ZSF4(c) lo16(C(c)[30])

typedef struct {
    CPU *c;
    uint32_t flag;
    int sf;          /* shift: 12 or 0 */
    int64_t mac0;    /* unshifted MAC0 */
    int64_t mac3;    /* unshifted MAC3 */
} G;

static inline void set_mac(G *g, int n, int32_t v) { D(g->c)[24 + n] = (uint32_t)v; }
static inline void set_ir(G *g, int n, int32_t v) { D(g->c)[8 + n] = (uint32_t)v; }

/* MAC1..3: 44-bit overflow check, then shift by sf. */
static int32_t A(G *g, int n, int64_t v)
{
    static const uint32_t pos[4] = {0, 1u << 30, 1u << 29, 1u << 28};
    static const uint32_t neg[4] = {0, 1u << 27, 1u << 26, 1u << 25};
    if (n == 3)
        g->mac3 = v;
    if (v > 0x7FFFFFFFFFFLL)
        g->flag |= pos[n];
    if (v < -0x80000000000LL)
        g->flag |= neg[n];
    return (int32_t)(v >> g->sf);
}

/* IR1..3 saturation: -0x8000 (0 with lm) .. 0x7FFF. */
static int32_t B(G *g, int n, int32_t v, int lm)
{
    static const uint32_t bit[4] = {0, 1u << 24, 1u << 23, 1u << 22};
    int32_t min = lm ? 0 : -0x8000;
    if (v > 0x7FFF) { g->flag |= bit[n]; return 0x7FFF; }
    if (v < min) { g->flag |= bit[n]; return min; }
    return v;
}

/* IR3 in RTPS/RTPT: the flag is checked on MAC3 >> 12 whatever sf is. */
static int32_t B3_sf(G *g, int lm)
{
    int32_t v_sf = (int32_t)(g->mac3 >> g->sf), v_12 = (int32_t)(g->mac3 >> 12);
    int32_t min = lm ? 0 : -0x8000;
    if (v_12 < -0x8000 || v_12 > 0x7FFF)
        g->flag |= 1u << 22;
    return v_sf > 0x7FFF ? 0x7FFF : v_sf < min ? min : v_sf;
}

static int32_t Cc(G *g, int n, int32_t v) /* color 0..0xFF */
{
    static const uint32_t bit[4] = {0, 1u << 21, 1u << 20, 1u << 19};
    if (v > 0xFF) { g->flag |= bit[n]; return 0xFF; }
    if (v < 0) { g->flag |= bit[n]; return 0; }
    return v;
}

static int32_t Dz(G *g, int64_t v) /* SZ3 / OTZ: 0..0xFFFF of v >> 12 */
{
    int64_t s = v >> 12;
    if (s > 0xFFFF) { g->flag |= 1u << 18; return 0xFFFF; }
    if (s < 0) { g->flag |= 1u << 18; return 0; }
    return (int32_t)s;
}

static int64_t F(G *g, int64_t v) /* MAC0 overflow */
{
    g->mac0 = v;
    if (v > 0x7FFFFFFFLL) g->flag |= 1u << 16;
    if (v < -0x80000000LL) g->flag |= 1u << 15;
    return v;
}

static int32_t Gs(G *g, int n, int64_t v) /* screen X/Y: -0x400..0x3FF */
{
    uint32_t bit = n == 1 ? 1u << 14 : 1u << 13;
    if (v > 0x3FF) { g->flag |= bit; return 0x3FF; }
    if (v < -0x400) { g->flag |= bit; return -0x400; }
    return (int32_t)v;
}

static int32_t Hq(G *g, int64_t v) /* IR0: 0..0x1000 of v >> 12 */
{
    int64_t s = v >> 12;
    if (s < 0 || s > 0x1000) g->flag |= 1u << 12;
    return s > 0x1000 ? 0x1000 : s < 0 ? 0 : (int32_t)s;
}

static uint32_t count_leading(uint32_t v) /* leading bits equal to bit 31 */
{
    if (v & 0x80000000u) v = ~v;
    uint32_t n = 0;
    while (n < 32 && !(v & (0x80000000u >> n))) n++;
    return n;
}

/* The GTE's unsigned Newton-Raphson division H / SZ3 (17-bit result). */
static uint32_t divide(G *g, uint32_t num, uint32_t den)
{
    static const uint8_t unr[257] = {
        0xff, 0xfd, 0xfb, 0xf9, 0xf7, 0xf5, 0xf3, 0xf1, 0xef, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe4, 0xe3,
        0xe1, 0xdf, 0xdd, 0xdc, 0xda, 0xd8, 0xd6, 0xd5, 0xd3, 0xd1, 0xd0, 0xce, 0xcd, 0xcb, 0xc9, 0xc8,
        0xc6, 0xc5, 0xc3, 0xc1, 0xc0, 0xbe, 0xbd, 0xbb, 0xba, 0xb8, 0xb7, 0xb5, 0xb4, 0xb2, 0xb1, 0xb0,
        0xae, 0xad, 0xab, 0xaa, 0xa9, 0xa7, 0xa6, 0xa4, 0xa3, 0xa2, 0xa0, 0x9f, 0x9e, 0x9c, 0x9b, 0x9a,
        0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8f, 0x8d, 0x8c, 0x8b, 0x8a, 0x89, 0x87, 0x86,
        0x85, 0x84, 0x83, 0x82, 0x81, 0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x75, 0x74,
        0x73, 0x72, 0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
        0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5d, 0x5c, 0x5b, 0x5a, 0x59, 0x58, 0x57, 0x56, 0x55,
        0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4f, 0x4e, 0x4d, 0x4d, 0x4c, 0x4b, 0x4a, 0x49, 0x48, 0x48,
        0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3f, 0x3f, 0x3e, 0x3d, 0x3c, 0x3c, 0x3b,
        0x3a, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2f,
        0x2e, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x2a, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
        0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a,
        0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
        0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x0d, 0x0d, 0x0c, 0x0c, 0x0b, 0x0a, 0x0a, 0x09, 0x09, 0x08, 0x08,
        0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
        0x00};
    if (num >= den * 2) {
        g->flag |= 1u << 17;
        return 0x1FFFF;
    }
    int shift = 0; /* leading zeros of the 16-bit denominator */
    while (shift < 16 && !(den & (0x8000u >> shift))) shift++;
    uint32_t d = (den << shift) & 0x7FFF;
    uint32_t r2 = unr[(d + 0x40) >> 7] + 0x101;
    uint32_t r3 = ((0x80 - (int32_t)(r2 * (d + 0x8000))) >> 8) & 0x1FFFF;
    uint32_t recip = (r2 * r3 + 0x80) >> 8;
    uint64_t q = (((uint64_t)recip * (num << shift)) + 0x8000) >> 16;
    return q > 0x1FFFF ? 0x1FFFF : (uint32_t)q;
}

/* ---- register access ---- */
uint32_t gte_read_data(CPU *c, int r)
{
    uint32_t w = D(c)[r & 31];
    switch (r & 31) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11:
        return (uint32_t)lo16(w);
    case 7: case 16: case 17: case 18: case 19:
        return w & 0xFFFF;
    case 15:
        return D(c)[14];
    case 28: case 29: {
        int32_t r5 = IR1(c) >> 7, g5 = IR2(c) >> 7, b5 = IR3(c) >> 7;
        r5 = r5 < 0 ? 0 : r5 > 0x1F ? 0x1F : r5;
        g5 = g5 < 0 ? 0 : g5 > 0x1F ? 0x1F : g5;
        b5 = b5 < 0 ? 0 : b5 > 0x1F ? 0x1F : b5;
        return (uint32_t)(r5 | g5 << 5 | b5 << 10);
    }
    default:
        return w;
    }
}

static void pf_written(int r); /* (precise vertices, below) */
void gte_write_data(CPU *c, int r, uint32_t v)
{
    pf_written(r & 31);
    switch (r & 31) {
    case 15: /* SXYP: push the screen FIFO */
        D(c)[12] = D(c)[13];
        D(c)[13] = D(c)[14];
        D(c)[14] = v;
        D(c)[15] = v;
        break;
    case 28: /* IRGB: sets IR1..3 */
        D(c)[28] = v & 0x7FFF;
        D(c)[9] = (v & 0x1F) << 7;
        D(c)[10] = ((v >> 5) & 0x1F) << 7;
        D(c)[11] = ((v >> 10) & 0x1F) << 7;
        break;
    case 29: case 31: /* read-only */
        break;
    case 30:
        D(c)[30] = v;
        D(c)[31] = count_leading(v);
        break;
    default:
        D(c)[r & 31] = v;
    }
}

uint32_t gte_read_ctrl(CPU *c, int r)
{
    uint32_t w = C(c)[r & 31];
    switch (r & 31) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30:
        return (uint32_t)lo16(w); /* H (26) is sign-extended on read too */
    default:
        return w;
    }
}

/* The translation's lost fraction (no wobble). The model emitter (800CDC54) builds a node's TR from two MVMVAs, the
 * second shifted (sf), and the shift's fraction is dropped: up close (car select's cars, ~1 unit a pixel) the
 * whole-unit TR rounds differently from frame to frame and a car's part jumps a pixel as it turns. A TR written
 * right after a shifted MVMVA (no other command between) keeps that MVMVA's fraction, for the precise projection
 * only (rtp's floats); the integer registers, and so the game, are unchanged. */
static int tr_pending;          /* the last command: a shifted MVMVA without translation */
static double tr_pending_frac[3];
static double tr_frac[3];       /* the fraction of the current TR (0: none) */

/* Car select's turning cars (no wobble). The rest of a node's translation is rounded before the emitter, in the
 * game's integer math (its world place, turned with the car each frame): up close that noise is about half a
 * pixel either way, and a car's front rocks a pixel as it turns. ui/options.c draws a car select car between
 * gte_tr_smooth_begin and _end; there each node's TR (in its order in the draw) is taken from a fit (quadratic,
 * least squares) of its last frames' TRs, for the precise projection only. A jump from the fit (another car, a
 * slide) starts the node over. In between two frames (uncapped: the scene drawn again) the fit is read there. */
extern int gte_precise_on;
#define TRS_SLOTS 4    /* (the drawn cars: one, or a car a player; the podium's scene: slot 3) */
#define TRS_NODES 256  /* (the podium's scene: every object's nodes, its three cars' among them) */
#define TRS_KEEP 12
static struct { int n; double t[TRS_KEEP], v[TRS_KEEP][3]; } trs_hist[TRS_SLOTS][TRS_NODES];
static int trs_on, trs_slot, trs_node, trs_record, trs_valid, trs_frame[TRS_SLOTS];
static double trs_t, trs_val[3];

void gte_tr_smooth_begin(int slot, double between)
{
    if (!gte_precise_on) return;
    trs_on = 1, trs_slot = slot & (TRS_SLOTS - 1), trs_node = 0, trs_valid = 0;
    trs_record = between < 0;
    if (trs_record) trs_frame[trs_slot]++;
    trs_t = trs_record ? trs_frame[trs_slot] : trs_frame[trs_slot] - 1 + between;
}
static int trs_used; /* (tests: TM2PC_TRS_LOG=1, each smoothed draw's nodes and fits used) */
void gte_tr_smooth_end(void)
{
    static int log = -1;
    if (log < 0) log = getenv("TM2PC_TRS_LOG") != NULL;
    if (log && trs_on) fprintf(stderr, "trs: slot %d frame %d nodes %d fitted %d\n", trs_slot, trs_frame[trs_slot], trs_node, trs_used);
    trs_used = 0;
    trs_on = trs_valid = 0;
}

static int trs_fit(int n, const double *t, const double (*v)[3], double at, double out[3]) /* least squares at `at` */
{
    int deg = n >= 6 ? 2 : n >= 3 ? 1 : 0;
    if (!deg) return 0;
    double s[5] = {0}, b[3][3] = {{0}};
    for (int i = 0; i < n; i++) {
        double x = t[i] - at, p[5] = {1}; /* (centered: the value at `at` is the constant term) */
        for (int k = 1; k < 5; k++) p[k] = p[k - 1] * x;
        for (int k = 0; k <= 2 * deg; k++) s[k] += p[k];
        for (int a = 0; a < 3; a++) for (int k = 0; k <= deg; k++) b[a][k] += v[i][a] * p[k];
    }
    for (int a = 0; a < 3; a++) {
        if (deg == 1) {
            double det = s[0] * s[2] - s[1] * s[1];
            if (det == 0) return 0;
            out[a] = (b[a][0] * s[2] - b[a][1] * s[1]) / det;
        } else { /* (Cramer's rule, the constant term of [s0 s1 s2; s1 s2 s3; s2 s3 s4]) */
            double det = s[0] * (s[2] * s[4] - s[3] * s[3]) - s[1] * (s[1] * s[4] - s[3] * s[2]) + s[2] * (s[1] * s[3] - s[2] * s[2]);
            if (det == 0) return 0;
            out[a] = (b[a][0] * (s[2] * s[4] - s[3] * s[3]) - s[1] * (b[a][1] * s[4] - s[3] * b[a][2]) + s[2] * (b[a][1] * s[3] - s[2] * b[a][2])) / det;
        }
    }
    return 1;
}

static void trs_node_set(const double raw[3]) /* a node's TR written (its Z, the last) */
{
    trs_valid = 0;
    if (trs_node >= TRS_NODES) return;
    __typeof__(trs_hist[0][0]) *h = &trs_hist[trs_slot][trs_node++];
    double fit[3];
    if (trs_record) {
        if (trs_fit(h->n, h->t, (const double (*)[3])h->v, trs_t, fit))
            for (int a = 0; a < 3; a++) if (fabs(raw[a] - fit[a]) > 1.5) h->n = 0; /* (a jump: over) */
        if (h->n && trs_t <= h->t[h->n - 1]) h->n = 0;
        if (h->n == TRS_KEEP) {
            memmove(h->t, h->t + 1, sizeof h->t[0] * (TRS_KEEP - 1));
            memmove(h->v, h->v + 1, sizeof h->v[0] * (TRS_KEEP - 1));
            h->n--;
        }
        h->t[h->n] = trs_t;
        memcpy(h->v[h->n], raw, sizeof h->v[0]);
        h->n++;
    }
    if (!trs_fit(h->n, h->t, (const double (*)[3])h->v, trs_t, fit)) return;
    for (int a = 0; a < 3; a++) if (fabs(raw[a] - fit[a]) > 1.5) return; /* (in between, far from it: as it is) */
    memcpy(trs_val, fit, sizeof trs_val);
    trs_valid = 1;
    trs_used++;
}

void gte_write_ctrl(CPU *c, int r, uint32_t v)
{
    if ((r & 31) >= 5 && (r & 31) <= 7) tr_frac[(r & 31) - 5] = tr_pending ? tr_pending_frac[(r & 31) - 5] : 0;
    if ((r & 31) == 7) { /* (tests: TM2PC_TR_TRACE=file, each TR as written, its fraction, per VBlank) */
        static FILE *trf; static int tro = -1;
        if (tro < 0) { tro = getenv("TM2PC_TR_TRACE") != NULL; if (tro) trf = fopen(getenv("TM2PC_TR_TRACE"), "w"); }
        extern unsigned gte_trace_gen(void);
        if (trf) fprintf(trf, "%u %.4f %.4f %.4f %d\n", gte_trace_gen(), (int32_t)C(c)[5] + tr_frac[0], (int32_t)C(c)[6] + tr_frac[1],
                         (int32_t)v + tr_frac[2], tr_pending), fflush(trf);
    }
    if (trs_on && (r & 31) >= 5 && (r & 31) <= 7) {
        trs_valid = 0;
        if ((r & 31) == 7) {
            double raw[3] = {(int32_t)C(c)[5] + tr_frac[0], (int32_t)C(c)[6] + tr_frac[1], (int32_t)v + tr_frac[2]};
            trs_node_set(raw);
        }
    }
    if ((r & 31) == 31) {
        v &= 0x7FFFF000u;
        if (v & 0x7F87E000u)
            v |= 0x80000000u;
    }
    C(c)[r & 31] = v;
}

/* ---- command helpers ---- */
static void push_color(G *g, int32_t m1, int32_t m2, int32_t m3)
{
    CPU *c = g->c;
    D(c)[20] = D(c)[21];
    D(c)[21] = D(c)[22];
    D(c)[22] = (uint32_t)(Cc(g, 1, m1 >> 4) | Cc(g, 2, m2 >> 4) << 8 | Cc(g, 3, m3 >> 4) << 16)
               | (D(c)[6] & 0xFF000000u);
}

static void mac_to_ir(G *g, int32_t m1, int32_t m2, int32_t m3, int lm)
{
    set_mac(g, 1, m1); set_mac(g, 2, m2); set_mac(g, 3, m3);
    set_ir(g, 1, B(g, 1, m1, lm)); set_ir(g, 2, B(g, 2, m2, lm)); set_ir(g, 3, B(g, 3, m3, lm));
}

/* matrix * vector + translation<<12, into MAC/IR */
static void mat_vec(G *g, int base, int64_t t1, int64_t t2, int64_t t3, int32_t x, int32_t y, int32_t z, int lm)
{
    CPU *c = g->c;
    int32_t m1 = A(g, 1, (t1 << 12) + (int64_t)M(c, base, 0, 0) * x + (int64_t)M(c, base, 0, 1) * y + (int64_t)M(c, base, 0, 2) * z);
    int32_t m2 = A(g, 2, (t2 << 12) + (int64_t)M(c, base, 1, 0) * x + (int64_t)M(c, base, 1, 1) * y + (int64_t)M(c, base, 1, 2) * z);
    int32_t m3 = A(g, 3, (t3 << 12) + (int64_t)M(c, base, 2, 0) * x + (int64_t)M(c, base, 2, 1) * y + (int64_t)M(c, base, 2, 2) * z);
    mac_to_ir(g, m1, m2, m3, lm);
}

/* light color stage: BK + LCM * IR */
static void color_stage(G *g, int lm)
{
    CPU *c = g->c;
    mat_vec(g, 16, RBK(c), GBK(c), BBK(c), IR1(c), IR2(c), IR3(c), lm);
}

/* depth cue: MAC = col + IR0 * (FC - col) with col already << 12 */
static void depth_cue(G *g, int64_t c1, int64_t c2, int64_t c3, int lm)
{
    CPU *c = g->c;
    int32_t ir0 = IR0(c);
    int32_t m1 = A(g, 1, c1 + (int64_t)ir0 * B(g, 1, A(g, 1, ((int64_t)RFC(c) << 12) - c1), 0));
    int32_t m2 = A(g, 2, c2 + (int64_t)ir0 * B(g, 2, A(g, 2, ((int64_t)GFC(c) << 12) - c2), 0));
    int32_t m3 = A(g, 3, c3 + (int64_t)ir0 * B(g, 3, A(g, 3, ((int64_t)BFC(c) << 12) - c3), 0));
    mac_to_ir(g, m1, m2, m3, lm);
}

/* ---- precise vertices (no wobble; docs/RENDER_PLAN.md section 4) ----
 * The integer registers stay exact. Beside SXY0-2, a float FIFO keeps each
 * projected vertex before rounding: x, y (screen, the same space as SXY) and
 * its depth. swc2 of SXY records it by the guest address it is stored at
 * (gte_store_data); the GPU front end looks a polygon's vertex words up there
 * (gte_precise_lookup): a polygon whose words are all found is drawn from the
 * floats. */
int gte_precise_on;                          /* set each VBlank (gpu_psyz.c) */
static struct { float x, y, w; int ok; } pf[3]; /* parallel to SXY0-2 */
typedef struct { uint32_t word, gen; float x, y, w; uint32_t put; } PreciseEntry; /* put: the port's or the C's own place */
static PreciseEntry *shadow;                 /* by guest word address (8 MB) */
static uint32_t precise_gen = 1;             /* VBlanks: entries older than 10 are stale (a frame is built over
                                                * several VBlanks and drawn the frame after) */
unsigned gte_trace_gen(void) { return precise_gen; } /* (TM2PC_TR_TRACE) */
unsigned gte_precise_stats[6]; /* (TM2PC_PRECISE_DEBUG) projections, of them ok, stores, recorded, polygons, found */
static unsigned why[4]; /* lookups failed: no address, never stored, another word, stale */
int gte_precise_why; /* (traces) the last lookup's: 0 found, 1-4 as why[] */
unsigned gte_precise_effects; /* (traces) effect quads placed from their points (gpu.c) */
void gte_precise_vblank(void)
{
    precise_gen++;
    static int trace = -1; /* tests: TM2PC_PRECISE_TRACE=1, per VBlank the polygons drawn without their floats, and why */
    if (trace < 0) trace = getenv("TM2PC_PRECISE_TRACE") != NULL;
    if (!trace) return;
    static unsigned last[6], lw[4];
    unsigned polys = gte_precise_stats[4] - last[4], found = gte_precise_stats[5] - last[5];
    if (polys && found < polys)
        fprintf(stderr, "precise trace: vblank %u, %u of %u polygons without floats (no address %u, never stored %u, other word %u, stale %u)\n",
                precise_gen, polys - found, polys, why[0] - lw[0], why[1] - lw[1], why[2] - lw[2], why[3] - lw[3]);
    memcpy(last, gte_precise_stats, sizeof last);
    memcpy(lw, why, sizeof lw);
}

/* The same floats by the vertex word's value: a word the game copied out of the GTE (mfc2) and stored with
 * an ordinary write (car select's cars, some of their polygons each frame) has no entry by its address. A
 * recent projection to the same whole pixel gives its floats (two vertices on one pixel: at most that
 * pixel apart, as the rounded word itself). */
#define BYVAL (1 << 18) /* (a split screen projects twice as many) */
static PreciseEntry by_value[BYVAL];
static uint8_t by_value_sat[BYVAL]; /* the projection was nearer than H/2: its word is the GTE's saturated place */
int gte_precise_sat; /* gpu.c: the last by-value find's */
static uint32_t by_value_slot(uint32_t word) { return (word * 2654435761u) >> 14; }
static void by_value_put(uint32_t word, float x, float y, float w, int sat)
{
    uint32_t k = by_value_slot(word);
    PreciseEntry *e = &by_value[k];
    e->word = word; e->gen = precise_gen; e->x = x; e->y = y; e->w = w; e->put = 0;
    by_value_sat[k] = (uint8_t)sat;
}
static int by_value_get(uint32_t word, float out[3])
{
    uint32_t k = by_value_slot(word);
    const PreciseEntry *e = &by_value[k];
    if (!e->gen || e->word != word || precise_gen - e->gen > 10) return 0; /* (as by address: a split screen's frame takes longer to build) */
    out[0] = e->x; out[1] = e->y; out[2] = e->w;
    gte_precise_sat = by_value_sat[k];
    return 1;
}

static void pf_push(int ok, float x, float y, float w)
{
    pf[0] = pf[1];
    pf[1] = pf[2];
    pf[2].x = x; pf[2].y = y; pf[2].w = w; pf[2].ok = ok;
}
static void pf_written(int r) /* mtc2 / lwc2 to SXY0-2 or SXYP: those floats no longer match */
{
    if (r >= 12 && r <= 14) pf[r - 12].ok = 0;
    else if (r == 15) pf_push(0, 0, 0, 0);
}

void gte_store_data(CPU *c, uint32_t ea, int r) /* swc2 */
{
    uint32_t v = gte_read_data(c, r);
    mem_w32(c, ea, v);
    if (!gte_precise_on || (r & 31) < 12 || (r & 31) > 15) return;
    int k = (r & 31) == 15 ? 2 : (r & 31) - 12;
    gte_precise_stats[2]++;
    if (!pf[k].ok) return;
    gte_precise_stats[3]++;
    if (!shadow && !(shadow = calloc(GUEST_RAM_BYTES / 4, sizeof *shadow))) return;
    PreciseEntry *e = &shadow[(ea & (GUEST_RAM_BYTES - 4)) >> 2];
    e->word = v; e->gen = precise_gen; e->x = pf[k].x; e->y = pf[k].y; e->w = pf[k].w; e->put = 0;
}

void gte_precise_put(uint32_t ea, uint32_t word, float x, float y, float w) /* the port's own 2D (hud_layout.c): exact places */
{
    if (!gte_precise_on) return;
    if (!shadow && !(shadow = calloc(GUEST_RAM_BYTES / 4, sizeof *shadow))) return;
    PreciseEntry *e = &shadow[(ea & (GUEST_RAM_BYTES - 4)) >> 2];
    e->word = word; e->gen = precise_gen; e->x = x; e->y = y; e->w = w; e->put = 1;
}

int gte_precise_found_put; /* gpu.c: the last lookup's place was put (gte_precise_put: the C emitter's sprites) */
int gte_precise_by_value = 1; /* gpu.c: only in a view's 3D order table (a HUD quad on a 3D vertex's pixel took its floats and depth: bent letters) */
static unsigned by_value_refused; /* (TM2PC_PRECISE_DEBUG: HUD vertices a 3D one's floats would have bent) */
static int by_value_ok(uint32_t word, float out[3])
{
    if (gte_precise_by_value) return by_value_get(word, out);
    float f[3];
    by_value_refused += by_value_get(word, f);
    return 0;
}
int gte_precise_lookup(uint32_t addr, uint32_t word, float out[3]) /* 1: the vertex word at addr has floats */
{
    static unsigned n;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("TM2PC_PRECISE_DEBUG") != NULL;
    if (dbg && ++n % 200000 == 0) fprintf(stderr, "precise lookups: no address %u, never stored %u, other word %u, stale %u, HUD by value refused %u (last %08X: %08X)\n", why[0], why[1], why[2], why[3], by_value_refused, addr, word);
    gte_precise_why = 0;
    gte_precise_found_put = 0;
    if (!shadow || !addr) { why[0]++; gte_precise_why = 1; return by_value_ok(word, out); }
    const PreciseEntry *e = &shadow[(addr & (GUEST_RAM_BYTES - 4)) >> 2];
    if (!e->gen) { why[1]++; gte_precise_why = 2; return by_value_ok(word, out); }
    if (e->word != word) { why[2]++; gte_precise_why = 3; return by_value_ok(word, out); }
    if (precise_gen - e->gen > 10) { why[3]++; gte_precise_why = 4; return by_value_ok(word, out); }
    out[0] = e->x; out[1] = e->y; out[2] = e->w;
    gte_precise_found_put = (int)e->put;
    return 1;
}

/* (traces: TM2PC_PRECISE_TRACE=5) the projection last stored at addr, if fresh, whatever was written over it since */
int gte_precise_stored(uint32_t addr, uint32_t *word, float out[3])
{
    if (!shadow || !addr) return 0;
    const PreciseEntry *e = &shadow[(addr & (GUEST_RAM_BYTES - 4)) >> 2];
    if (!e->gen || precise_gen - e->gen > 10) return 0;
    *word = e->word; out[0] = e->x; out[1] = e->y; out[2] = e->w;
    return 1;
}

static uint32_t rtp(G *g, int v, int lm)
{
    CPU *c = g->c;
    int32_t x = VX(c, v), y = VY(c, v), z = VZ(c, v);
    int32_t m1 = A(g, 1, ((int64_t)TRX(c) << 12) + (int64_t)M(c, 0, 0, 0) * x + (int64_t)M(c, 0, 0, 1) * y + (int64_t)M(c, 0, 0, 2) * z);
    int32_t m2 = A(g, 2, ((int64_t)TRY(c) << 12) + (int64_t)M(c, 0, 1, 0) * x + (int64_t)M(c, 0, 1, 1) * y + (int64_t)M(c, 0, 1, 2) * z);
    int32_t m3 = A(g, 3, ((int64_t)TRZ(c) << 12) + (int64_t)M(c, 0, 2, 0) * x + (int64_t)M(c, 0, 2, 1) * y + (int64_t)M(c, 0, 2, 2) * z);
    set_mac(g, 1, m1); set_mac(g, 2, m2); set_mac(g, 3, m3);
    set_ir(g, 1, B(g, 1, m1, lm));
    set_ir(g, 2, B(g, 2, m2, lm));
    set_ir(g, 3, B3_sf(g, lm));
    D(c)[16] = D(c)[17]; D(c)[17] = D(c)[18]; D(c)[18] = D(c)[19];
    D(c)[19] = (uint32_t)Dz(g, g->mac3);
    uint32_t h = divide(g, H(c), (uint32_t)SZ(c, 3));
    D(c)[12] = D(c)[13]; D(c)[13] = D(c)[14];
    int32_t sx = Gs(g, 1, F(g, (int64_t)OFX(c) + (int64_t)IR1(c) * h) >> 16);
    int32_t sy = Gs(g, 2, F(g, (int64_t)OFY(c) + (int64_t)IR2(c) * h) >> 16);
    D(c)[14] = (uint32_t)(sx & 0xFFFF) | (uint32_t)sy << 16;
    D(c)[15] = D(c)[14];
    if (gte_precise_on) { /* the same projection in floats, from the exact products (sf 12: RTPS/RTPT as games use them) */
        double tx = trs_valid ? trs_val[0] : TRX(c) + tr_frac[0], ty = trs_valid ? trs_val[1] : TRY(c) + tr_frac[1];
        double tz = trs_valid ? trs_val[2] : TRZ(c) + tr_frac[2]; /* (car select: smoothed, see trs_hist) */
        double px = (tx * 4096 + (double)M(c, 0, 0, 0) * x + (double)M(c, 0, 0, 1) * y + (double)M(c, 0, 0, 2) * z) / 4096;
        double py = (ty * 4096 + (double)M(c, 0, 1, 0) * x + (double)M(c, 0, 1, 1) * y + (double)M(c, 0, 1, 2) * z) / 4096;
        double pz = (tz * 4096 + (double)M(c, 0, 2, 0) * x + (double)M(c, 0, 2, 1) * y + (double)M(c, 0, 2, 2) * z) / 4096;
        double hh = (double)H(c);
        /* Nearer than H/2 the divide saturates and the integer place is wrong (the PS1's stretched ground under the
         * camera); off the +-1024 range it is clamped. With depth (wobble none) the true place is given anyway: the
         * renderer draws (x*w, y*w, 0, w), so a corner behind the camera (w < 0) is clipped by the GPU at w = 0, and
         * the polygon is drawn as it really is. Without depth (smooth: w 1) only the places the PS1 got right. */
        int sat = pz * 2 <= hh, persp = gte_precise_on >= 2;
        double dz = fabs(pz) < 1.0 / 64 ? (pz < 0 ? -1.0 / 64 : 1.0 / 64) : pz;
        int ok = g->sf == 12 && (!sat || persp);
        double fx = ok ? (double)OFX(c) / 65536 + px * hh / dz : 0, fy = ok ? (double)OFY(c) / 65536 + py * hh / dz : 0;
        int clamped = sx <= -1024 || sx >= 1023 || sy <= -1024 || sy >= 1023;
        /* (near the camera, as car select's cars, the GTE's rounding reaches 2-3 pixels, more very near. Just past the
         * saturation (pz under 2 H) it reaches 4-8: those corners failed the check and fell back to the GTE's, a near
         * wall's texture leaning in some frames and not others (the user's 2026-11-01: Hong Kong's temple at 60 FPS,
         * the in-between frames' eye a fraction apart). With depth the true place is given there as nearer (sat).) */
        if (ok && !sat && !clamped && !(persp && pz < 2 * hh))
            ok = fabs(fx - sx) < 4 + fabs(fx - OFX(c) / 65536.0) / 64 && fabs(fy - sy) < 4 + fabs(fy - OFY(c) / 65536.0) / 64;
        if (ok && !persp) ok = fx >= -1024 && fx < 1024 && fy >= -1024 && fy < 1024;
        /* (with depth a corner a hair behind the eye is far off the screen, -1.1 million at z -0.5: the renderer takes
         * x*w back, finite, and clips it. Rejected, its polygon fell back to the GTE's saturated corners and the size
         * limit dropped it: the ground gone at the screen's bottom, decomp RENDER_ANSWERS.md R5.2) */
        static int old_near = -1; /* (tests: TM2PC_OLD_NEAR=1, the limit as before) */
        if (old_near < 0) old_near = getenv("TM2PC_OLD_NEAR") != NULL;
        if (ok) ok = persp && !old_near ? fabs(fx) < 1e9 && fabs(fy) < 1e9 : fabs(fx) < 1e6 && fabs(fy) < 1e6;
        pz = dz;
        unsigned long long rt_vblanks_now(void);
        static int tr = -1;
        if (tr < 0) tr = getenv("TM2PC_PRECISE_TRACE") ? atoi(getenv("TM2PC_PRECISE_TRACE")) : 0;
        if (tr >= 3 && !ok) fprintf(stderr, "precise reject: %d,%d sf %d pz %.1f h %.0f fx %.2f fy %.2f v%llu\n", sx, sy, g->sf, pz,
                                    hh, fx, fy, rt_vblanks_now());
        static long long rtp_at = -2; /* tests: TM2PC_RTP_LOG_AT=<vblank>, every projection of that VBlank */
        if (rtp_at == -2) rtp_at = getenv("TM2PC_RTP_LOG_AT") ? atoll(getenv("TM2PC_RTP_LOG_AT")) : -1;
        if (rtp_at >= 0 && rt_vblanks_now() == (unsigned long long)rtp_at)
            fprintf(stderr, "rtp %d,%d ok %d sat %d clamped %d f %.2f,%.2f z %.1f op %d\n", sx, sy, ok, sat, clamped, fx, fy, pz, v);
        pf_push(ok, (float)fx, (float)fy, (float)pz);
        if (ok && !clamped) by_value_put(D(c)[14], (float)fx, (float)fy, (float)pz, sat); /* (a clamped word is shared by many) */
        gte_precise_stats[0]++;
        gte_precise_stats[1] += ok;
    }
    return h;
}

static void depth_ir0(G *g, uint32_t h)
{
    CPU *c = g->c;
    int32_t m0 = (int32_t)F(g, (int64_t)DQB(c) + (int64_t)DQA(c) * h);
    D(c)[24] = (uint32_t)m0;
    set_ir(g, 0, Hq(g, g->mac0));
}

void gte_command(CPU *c, uint32_t op)
{
    G g = {c, 0, (op >> 19) & 1 ? 12 : 0, 0, 0};
    int lm = (op >> 10) & 1;
    int v, mx, cv;
    tr_pending = 0;
    switch (op & 0x3F) {
    case 0x01: /* RTPS */
        depth_ir0(&g, rtp(&g, 0, lm));
        break;
    case 0x30: { /* RTPT */
        uint32_t h = 0;
        for (v = 0; v < 3; v++)
            h = rtp(&g, v, lm);
        depth_ir0(&g, h);
        break;
    }
    case 0x06: /* NCLIP */
        D(c)[24] = (uint32_t)(int32_t)F(&g, (int64_t)SX(c, 0) * SY(c, 1) + (int64_t)SX(c, 1) * SY(c, 2)
                                            + (int64_t)SX(c, 2) * SY(c, 0) - (int64_t)SX(c, 0) * SY(c, 2)
                                            - (int64_t)SX(c, 1) * SY(c, 0) - (int64_t)SX(c, 2) * SY(c, 1));
        if (gte_precise_on >= 2 && pf[0].ok && pf[1].ok && pf[2].ok) {
            /* A corner near or behind the eye: its whole-pixel place is saturated or clamped, so the sign above is
             * anyone's and the emitter culled a ground polygon facing the camera (New York's roof tiles, gone at the
             * screen's bottom driving across them diagonally: a tile's corner points under the camera; the user's
             * 2026-11-02). With depth the polygon is drawn from its true corners: decide from them, in homogeneous
             * coordinates (x w, y w, w), whose determinant has the facing's sign whichever side of the eye a corner is */
            int near = 0;
            for (int k = 0; k < 3; k++)
                near |= pf[k].w * 2 <= (float)H(c) || SX(c, k) <= -1024 || SX(c, k) >= 1023 || SY(c, k) <= -1024 ||
                        SY(c, k) >= 1023;
            static int old_nclip = -1; /* (tests: TM2PC_OLD_NCLIP=1, the whole-pixel sign as before) */
            if (old_nclip < 0) old_nclip = getenv("TM2PC_OLD_NCLIP") != NULL;
            if (near && !old_nclip && (pf[0].w > 0 || pf[1].w > 0 || pf[2].w > 0)) { /* (all behind: never drawn) */
                double X[3], Y[3], W[3];
                for (int k = 0; k < 3; k++) W[k] = pf[k].w, X[k] = (double)pf[k].x * W[k], Y[k] = (double)pf[k].y * W[k];
                double det = X[0] * (Y[1] * W[2] - Y[2] * W[1]) - X[1] * (Y[0] * W[2] - Y[2] * W[0]) +
                             X[2] * (Y[0] * W[1] - Y[1] * W[0]);
                int32_t m = (int32_t)D(c)[24];
                if ((det > 0 && m <= 0) || (det < 0 && m >= 0)) D(c)[24] = (uint32_t)(det > 0 ? 1 : -1);
            }
        }
        { /* tests: TM2PC_TEST_FLIP_CULL=1, every facing reversed (the back faces drawn, the front ones culled) */
            static int flip = -1;
            if (flip < 0) flip = getenv("TM2PC_TEST_FLIP_CULL") != NULL;
            if (flip) D(c)[24] = (uint32_t)-(int32_t)D(c)[24];
        }
        break;
    case 0x0C: { /* OP: cross product of the rotation diagonal with IR */
        int32_t d1 = M(c, 0, 0, 0), d2 = M(c, 0, 1, 1), d3 = M(c, 0, 2, 2);
        int32_t m1 = A(&g, 1, (int64_t)d2 * IR3(c) - (int64_t)d3 * IR2(c));
        int32_t m2 = A(&g, 2, (int64_t)d3 * IR1(c) - (int64_t)d1 * IR3(c));
        int32_t m3 = A(&g, 3, (int64_t)d1 * IR2(c) - (int64_t)d2 * IR1(c));
        mac_to_ir(&g, m1, m2, m3, lm);
        break;
    }
    case 0x10: /* DPCS */
        depth_cue(&g, (int64_t)RGBC_R(c) << 16, (int64_t)RGBC_G(c) << 16, (int64_t)RGBC_B(c) << 16, lm);
        push_color(&g, (int32_t)D(c)[25], (int32_t)D(c)[26], (int32_t)D(c)[27]);
        break;
    case 0x11: /* INTPL */
        depth_cue(&g, (int64_t)IR1(c) << 12, (int64_t)IR2(c) << 12, (int64_t)IR3(c) << 12, lm);
        push_color(&g, (int32_t)D(c)[25], (int32_t)D(c)[26], (int32_t)D(c)[27]);
        break;
    case 0x12: { /* MVMVA */
        int32_t x, y, z, m[3][3];
        int64_t t[3];
        mx = (op >> 17) & 3;
        v = (op >> 15) & 3;
        cv = (op >> 13) & 3;
        x = VX(c, v); y = VY(c, v); z = VZ(c, v);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                m[i][j] = mx < 3 ? M(c, mx * 8, i, j) : 0;
        if (mx == 3) { /* garbage matrix */
            int32_t r = RGBC_R(c) << 4;
            m[0][0] = -r; m[0][1] = r; m[0][2] = IR0(c);
            m[1][0] = m[1][1] = m[1][2] = M(c, 0, 0, 2);
            m[2][0] = m[2][1] = m[2][2] = M(c, 0, 1, 1);
        }
        t[0] = cv < 3 ? (int32_t)C(c)[5 + cv * 8] : 0;
        t[1] = cv < 3 ? (int32_t)C(c)[6 + cv * 8] : 0;
        t[2] = cv < 3 ? (int32_t)C(c)[7 + cv * 8] : 0;
        if (cv == 2) { /* far color: hardware bug, the X column only sets flags */
            int32_t m1 = A(&g, 1, (int64_t)m[0][1] * y + (int64_t)m[0][2] * z);
            int32_t m2 = A(&g, 2, (int64_t)m[1][1] * y + (int64_t)m[1][2] * z);
            int32_t m3 = A(&g, 3, (int64_t)m[2][1] * y + (int64_t)m[2][2] * z);
            B(&g, 1, A(&g, 1, (t[0] << 12) + (int64_t)m[0][0] * x), 0);
            B(&g, 2, A(&g, 2, (t[1] << 12) + (int64_t)m[1][0] * x), 0);
            B(&g, 3, A(&g, 3, (t[2] << 12) + (int64_t)m[2][0] * x), 0);
            mac_to_ir(&g, m1, m2, m3, lm);
        } else {
            int32_t m1 = A(&g, 1, (t[0] << 12) + (int64_t)m[0][0] * x + (int64_t)m[0][1] * y + (int64_t)m[0][2] * z);
            int32_t m2 = A(&g, 2, (t[1] << 12) + (int64_t)m[1][0] * x + (int64_t)m[1][1] * y + (int64_t)m[1][2] * z);
            int32_t m3 = A(&g, 3, (t[2] << 12) + (int64_t)m[2][0] * x + (int64_t)m[2][1] * y + (int64_t)m[2][2] * z);
            mac_to_ir(&g, m1, m2, m3, lm);
            if (g.sf == 12 && cv == 3 && mx < 3) { /* (its fraction, for a TR written next: see tr_frac) */
                for (int i = 0; i < 3; i++)
                    tr_pending_frac[i] = (double)(((int64_t)m[i][0] * x + (int64_t)m[i][1] * y + (int64_t)m[i][2] * z) & 4095) / 4096;
                tr_pending = 1;
            }
        }
        break;
    }
    case 0x13: case 0x16: /* NCDS, NCDT */
        for (v = 0; v < ((op & 0x3F) == 0x16 ? 3 : 1); v++) {
            mat_vec(&g, 8, 0, 0, 0, VX(c, v), VY(c, v), VZ(c, v), lm);
            color_stage(&g, lm);
            depth_cue(&g, ((int64_t)RGBC_R(c) << 4) * IR1(c), ((int64_t)RGBC_G(c) << 4) * IR2(c),
                      ((int64_t)RGBC_B(c) << 4) * IR3(c), lm);
            push_color(&g, (int32_t)D(c)[25], (int32_t)D(c)[26], (int32_t)D(c)[27]);
        }
        break;
    case 0x14: /* CDP */
        color_stage(&g, lm);
        depth_cue(&g, ((int64_t)RGBC_R(c) << 4) * IR1(c), ((int64_t)RGBC_G(c) << 4) * IR2(c),
                  ((int64_t)RGBC_B(c) << 4) * IR3(c), lm);
        push_color(&g, (int32_t)D(c)[25], (int32_t)D(c)[26], (int32_t)D(c)[27]);
        break;
    case 0x1B: case 0x3F: /* NCCS, NCCT */
        for (v = 0; v < ((op & 0x3F) == 0x3F ? 3 : 1); v++) {
            mat_vec(&g, 8, 0, 0, 0, VX(c, v), VY(c, v), VZ(c, v), lm);
            color_stage(&g, lm);
            mac_to_ir(&g, A(&g, 1, ((int64_t)RGBC_R(c) << 4) * IR1(c)), A(&g, 2, ((int64_t)RGBC_G(c) << 4) * IR2(c)),
                      A(&g, 3, ((int64_t)RGBC_B(c) << 4) * IR3(c)), lm);
            push_color(&g, (int32_t)D(c)[25], (int32_t)D(c)[26], (int32_t)D(c)[27]);
        }
        break;
    case 0x1C: /* CC */
        color_stage(&g, lm);
        mac_to_ir(&g, A(&g, 1, ((int64_t)RGBC_R(c) << 4) * IR1(c)), A(&g, 2, ((int64_t)RGBC_G(c) << 4) * IR2(c)),
                  A(&g, 3, ((int64_t)RGBC_B(c) << 4) * IR3(c)), lm);
        push_color(&g, (int32_t)D(c)[25], (int32_t)D(c)[26], (int32_t)D(c)[27]);
        break;
    case 0x1E: case 0x20: /* NCS, NCT */
        for (v = 0; v < ((op & 0x3F) == 0x20 ? 3 : 1); v++) {
            mat_vec(&g, 8, 0, 0, 0, VX(c, v), VY(c, v), VZ(c, v), lm);
            color_stage(&g, lm);
            push_color(&g, (int32_t)D(c)[25], (int32_t)D(c)[26], (int32_t)D(c)[27]);
        }
        break;
    case 0x28: /* SQR */
        mac_to_ir(&g, A(&g, 1, (int64_t)IR1(c) * IR1(c)), A(&g, 2, (int64_t)IR2(c) * IR2(c)),
                  A(&g, 3, (int64_t)IR3(c) * IR3(c)), lm);
        break;
    case 0x29: /* DCPL */
        depth_cue(&g, ((int64_t)RGBC_R(c) << 4) * IR1(c), ((int64_t)RGBC_G(c) << 4) * IR2(c),
                  ((int64_t)RGBC_B(c) << 4) * IR3(c), lm);
        push_color(&g, (int32_t)D(c)[25], (int32_t)D(c)[26], (int32_t)D(c)[27]);
        break;
    case 0x2A: /* DPCT: three times on RGB0 */
        for (v = 0; v < 3; v++) {
            uint32_t rgb0 = D(c)[20];
            depth_cue(&g, (int64_t)(rgb0 & 0xFF) << 16, (int64_t)((rgb0 >> 8) & 0xFF) << 16,
                      (int64_t)((rgb0 >> 16) & 0xFF) << 16, lm);
            push_color(&g, (int32_t)D(c)[25], (int32_t)D(c)[26], (int32_t)D(c)[27]);
        }
        break;
    case 0x2D: /* AVSZ3 */
        D(c)[24] = (uint32_t)(int32_t)F(&g, (int64_t)ZSF3(c) * (SZ(c, 1) + SZ(c, 2) + SZ(c, 3)));
        D(c)[7] = (uint32_t)Dz(&g, g.mac0);
        break;
    case 0x2E: /* AVSZ4 */
        D(c)[24] = (uint32_t)(int32_t)F(&g, (int64_t)ZSF4(c) * (SZ(c, 0) + SZ(c, 1) + SZ(c, 2) + SZ(c, 3)));
        D(c)[7] = (uint32_t)Dz(&g, g.mac0);
        break;
    case 0x3D: /* GPF */
        mac_to_ir(&g, A(&g, 1, (int64_t)IR0(c) * IR1(c)), A(&g, 2, (int64_t)IR0(c) * IR2(c)),
                  A(&g, 3, (int64_t)IR0(c) * IR3(c)), lm);
        push_color(&g, (int32_t)D(c)[25], (int32_t)D(c)[26], (int32_t)D(c)[27]);
        break;
    case 0x3E: /* GPL */
        mac_to_ir(&g, A(&g, 1, ((int64_t)(int32_t)D(c)[25] << g.sf) + (int64_t)IR0(c) * IR1(c)),
                  A(&g, 2, ((int64_t)(int32_t)D(c)[26] << g.sf) + (int64_t)IR0(c) * IR2(c)),
                  A(&g, 3, ((int64_t)(int32_t)D(c)[27] << g.sf) + (int64_t)IR0(c) * IR3(c)), lm);
        push_color(&g, (int32_t)D(c)[25], (int32_t)D(c)[26], (int32_t)D(c)[27]);
        break;
    default:
        break; /* unused opcodes: no effect on the registers modeled here */
    }
    if (g.flag & 0x7F87E000u)
        g.flag |= 0x80000000u;
    C(c)[31] = g.flag;
}
