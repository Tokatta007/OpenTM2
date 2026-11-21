/* The PC version's sharper battle textures against the PS1's (docs/FIRST_RUN_SETUP.md): for every image of the PS1's
 * arena and car packs, the PC image of its pack that is the same picture at twice the size, by content (the packs'
 * orders differ; also the same size: not sharper, but the same picture?). One line each: pack, PS1 index, size, the best PC index, its size, the error (the PC one averaged
 * down 2x2 against the PS1's, mean absolute difference per channel over the pixels both show; 999: no candidate) and
 * the difference of the see-through pixels (their share, %).
 *
 *     hd_audit <ps1.cue> <pc.iso | folder>        > report.tsv */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/assets/discfs.h"
#include "../../src/assets/tim.h"

typedef struct { int w, h; uint8_t *p; } Pic;

static int load(DiscFs *fs, const char *path, int pc, Pic *out, int cap)
{
    int i = discfs_find(fs, path);
    size_t len;
    uint8_t *d = i >= 0 ? discfs_read(fs, i, &len) : NULL;
    if (!d) return 0;
    static Tim t[1024];
    int n = tim_scan(d, len, pc, t, cap < 1024 ? cap : 1024);
    for (int k = 0; k < n; k++) {
        out[k].w = t[k].w, out[k].h = t[k].h;
        out[k].p = malloc((size_t)t[k].w * t[k].h * 4);
        tim_rgba(&t[k], 0, out[k].p);
        tim_free(&t[k]);
    }
    free(d);
    return n;
}

/* the PC picture (s times the PS1's size) against the PS1's: see-through disagreement (%) and the color error */
static double compare(const Pic *a, const Pic *b, double *alpha)
{
    int s = b->w / a->w, s2 = s * s;
    long long sum = 0, n = 0, mism = 0;
    for (int y = 0; y < a->h; y++)
        for (int x = 0; x < a->w; x++) {
            const uint8_t *p = a->p + ((size_t)y * a->w + x) * 4;
            int sum3[3] = {0, 0, 0}, op = 0;
            for (int dy = 0; dy < s; dy++)
                for (int dx = 0; dx < s; dx++) {
                    const uint8_t *q = b->p + ((size_t)(s * y + dy) * b->w + s * x + dx) * 4;
                    op += q[3] != 0;
                    for (int k = 0; k < 3; k++) sum3[k] += q[k];
                }
            int pa = p[3] != 0, pb = 2 * op >= s2;
            if (pa != pb) { mism++; continue; }
            if (!pa) continue;
            for (int k = 0; k < 3; k++) sum += abs(p[k] - sum3[k] / s2);
            n += 3;
        }
    *alpha = 100.0 * (double)mism / (a->w * a->h);
    return n ? (double)sum / (double)n : 0.0;
}

/* the shapes alike: the correlation of the two's brightness (the PC one averaged down), over the pixels both show */
static double correlation(const Pic *a, const Pic *b)
{
    int s = b->w / a->w;
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = 0;
    for (int y = 0; y < a->h; y++)
        for (int x = 0; x < a->w; x++) {
            const uint8_t *p = a->p + ((size_t)y * a->w + x) * 4;
            double lb = 0;
            int op = 0;
            for (int dy = 0; dy < s; dy++)
                for (int dx = 0; dx < s; dx++) {
                    const uint8_t *q = b->p + ((size_t)(s * y + dy) * b->w + s * x + dx) * 4;
                    op += q[3] != 0;
                    lb += q[0] * 0.299 + q[1] * 0.587 + q[2] * 0.114;
                }
            if (!p[3] || 2 * op < s * s) continue;
            lb /= s * s;
            double la = p[0] * 0.299 + p[1] * 0.587 + p[2] * 0.114;
            sa += la, sb += lb, saa += la * la, sbb += lb * lb, sab += la * lb, n++;
        }
    if (n < 4) return 0;
    double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    return va > 1e-6 && vb > 1e-6 ? (sab - sa * sb / n) / sqrt(va * vb) : 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "hd_audit <ps1.cue> <pc.iso | folder>\n"); return 2; }
    DiscFs *ps1 = discfs_open(argv[1]), *pc = discfs_open(argv[2]);
    if (!ps1 || !pc) { fprintf(stderr, "cannot read the discs\n"); return 1; }
    /* the PS1 pack, the PC pack */
    static const char *const PACKS[][2] = {
        {"/TMS/BURB.TMS", "/LEVELDB/BURB.TPC"}, {"/TMS/DEN.TMS", "/LEVELDB/DEN.TPC"}, {"/TMS/DISH.TMS", "/LEVELDB/DISH.TPC"},
        {"/TMS/FREEWAY.TMS", "/LEVELDB/FREEWAY.TPC"}, {"/TMS/GLACIER.TMS", "/LEVELDB/GLACIER.TPC"},
        {"/TMS/GLACIER2.TMS", "/LEVELDB/GLACIER.TPC"}, {"/TMS/HKONG.TMS", "/LEVELDB/HKONG.TPC"},
        {"/TMS/ISLANDS.TMS", "/LEVELDB/ISLANDS.TPC"}, {"/TMS/PARIS.TMS", "/LEVELDB/PARIS.TPC"},
        {"/TMS/PARIS2.TMS", "/LEVELDB/PARIS.TPC"}, {"/TMS/ROOF.TMS", "/LEVELDB/ROOF.TPC"}, {"/TMS/ROOF2.TMS", "/LEVELDB/ROOF.TPC"},
        {"/TMS/SROOF.TMS", "/LEVELDB/SROOF.TPC"}, {"/TMS/SWAMP.TMS", "/LEVELDB/SWAMP.TPC"},
        {"/TMS/AX.TMS", "/CARSDB/AX.TPC"}, {"/TMS/BB.TMS", "/CARSDB/BB.TPC"}, {"/TMS/FL.TMS", "/CARSDB/FL.TPC"},
        {"/TMS/HH.TMS", "/CARSDB/HH.TPC"}, {"/TMS/HS.TMS", "/CARSDB/HS.TPC"}, {"/TMS/IR.TMS", "/CARSDB/IR.TPC"},
        {"/TMS/MG.TMS", "/CARSDB/MG.TPC"}, {"/TMS/MN.TMS", "/CARSDB/MN.TPC"}, {"/TMS/OT.TMS", "/CARSDB/OT.TPC"},
        {"/TMS/PV.TMS", "/CARSDB/PV.TPC"}, {"/TMS/RK.TMS", "/CARSDB/RK.TPC"}, {"/TMS/SP.TMS", "/CARSDB/SP.TPC"},
        {"/TMS/ST.TMS", "/CARSDB/ST.TPC"}, {"/TMS/TH.TMS", "/CARSDB/TH.TPC"}, {"/TMS/WH.TMS", "/CARSDB/WH.TPC"},
    };
    static Pic a[1024], b[1024];
    for (size_t k = 0; k < sizeof PACKS / sizeof PACKS[0]; k++) {
        int na = load(ps1, PACKS[k][0], 0, a, 1024), nb = load(pc, PACKS[k][1], 1, b, 1024);
        for (int i = 0; i < na; i++) {
            int best = -1;
            double be = 999, ba = 100;
            for (int j = 0; j < nb; j++) {
                if (!((b[j].w == 2 * a[i].w && b[j].h == 2 * a[i].h) || (b[j].w == a[i].w && b[j].h == a[i].h))) continue;
                double al, e = compare(&a[i], &b[j], &al);
                if (al + e < ba + be) best = j, be = e, ba = al;
            }
            /* the twice-as-big one most alike in shape: its index, correlation, color error and see-through difference */
            int best2 = -1;
            double bc = -2, e2 = 999, a2 = 100;
            for (int j = 0; j < nb; j++) {
                if (b[j].w != 2 * a[i].w || b[j].h != 2 * a[i].h) continue;
                double c = correlation(&a[i], &b[j]);
                if (c > bc) bc = c, best2 = j;
            }
            if (best2 >= 0) e2 = compare(&a[i], &b[best2], &a2);
            printf("%s\t%d\t%dx%d\t%s\t%d\t%dx%d\t%.1f\t%.1f\t%d\t%.3f\t%.1f\t%.1f\n", PACKS[k][0], i, a[i].w, a[i].h, PACKS[k][1],
                   best, best >= 0 ? b[best].w : 0, best >= 0 ? b[best].h : 0, be, ba, best2, bc, e2, a2);
        }
        for (int i = 0; i < na; i++) free(a[i].p);
        for (int j = 0; j < nb; j++) free(b[j].p);
    }
    discfs_close(ps1);
    discfs_close(pc);
    return 0;
}
