/* The first-time setup's builder (uibuild.h): tools/art/ui_assets.py in C, picture for picture (tools/test/
 * ui_build_check.py: every file the same bytes as the Python's). */
#include "uibuild.h"

#pragma STDC FP_CONTRACT OFF /* (every float operation as Python does it: no fused multiply-adds) */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "discfs.h"
#include "tim.h"
#include "movie_picks.h"
#include "hd_picks.h"
#include "../runtime/mdec.h"

/* pow and exp as Python's: on Windows the C runtime Python uses (ucrtbase.dll), whose results differ from MinGW's own in
 * the last bit now and then (x**2: about 1 in 2000), enough to turn a choice between two nearby-equal patches */
#ifdef _WIN32
#include <windows.h>
static double (*ucrt_pow)(double, double), (*ucrt_exp)(double);
static void ucrt_load(void)
{
    static int done;
    if (done) return;
    done = 1;
    HMODULE m = LoadLibraryA("ucrtbase.dll");
    if (m) {
        ucrt_pow = (double (*)(double, double))(void *)GetProcAddress(m, "pow");
        ucrt_exp = (double (*)(double))(void *)GetProcAddress(m, "exp");
    }
}
static double py_pow(double x, double y) { ucrt_load(); return ucrt_pow ? ucrt_pow(x, y) : pow(x, y); }
static double py_exp(double x) { ucrt_load(); return ucrt_exp ? ucrt_exp(x) : exp(x); }
#else
static double py_pow(double x, double y) { return pow(x, y); }
static double py_exp(double x) { return exp(x); }
#endif

typedef struct { int w, h; uint8_t *p; } Img; /* RGBA, row-major */

/* Python's sum() of floats (3.12 on): Neumaier's compensated sum, the compensation added at the end (CPython's
 * builtin_sum_impl, step for step); a plain loop's result differs in the last bit now and then */
typedef struct { double s, c; } PySum;
static void ps_add(PySum *p, double x)
{
    double t = p->s + x;
    if (fabs(p->s) >= fabs(x)) p->c += (p->s - t) + x;
    else p->c += (x - t) + p->s;
    p->s = t;
}
static double ps_val(const PySum *p) { return p->c != 0.0 && isfinite(p->c) ? p->s + p->c : p->s; }

static char error_text[256];
const char *ui_build_error(void) { return error_text; }
static int fail(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vsnprintf(error_text, sizeof error_text, fmt, a);
    va_end(a);
    return 0;
}

static Img img_new(int w, int h)
{
    Img m = {w, h, calloc((size_t)(w > 0 ? w : 1) * (h > 0 ? h : 1), 4)};
    return m;
}
static void img_free(Img *m) { free(m->p); m->p = NULL; }
static uint8_t *at(const Img *m, int x, int y) { return m->p + ((size_t)y * m->w + x) * 4; }

/* ---- the sources: a pack's images, decoded once ---- */

typedef struct {
    DiscFs *fs;
    int pc;                 /* (the PC version: its .TPC colors are RGB) */
    int stand_in;           /* the PS1 disc in the PC version's place: its .TMS for .TPC, its screens doubled */
    char path[128];         /* the pack decoded last */
    uint8_t *data;
    Tim tims[1024];
    int n;
} Source;

static int pack(Source *s, const char *path)
{
    char twin[128];
    size_t len0 = strlen(path);
    if (s->stand_in && len0 > 4 && !strcmp(path + len0 - 4, ".TPC")) { /* (the PS1's pack of the same name) */
        snprintf(twin, sizeof twin, "%.*s.TMS", (int)len0 - 4, path);
        path = twin;
    } else if (s->stand_in && !strncmp(path, "/SHELLDB1/SHOWOPS", 17)) { /* (the PS1's line-ups: in SCREEN) */
        snprintf(twin, sizeof twin, "/SCREEN/%s", path + 10);
        path = twin;
    }
    if (!strcmp(s->path, path) && s->data) return s->n;
    for (int i = 0; i < s->n; i++) tim_free(&s->tims[i]);
    free(s->data);
    s->data = NULL;
    s->n = 0;
    s->path[0] = 0;
    size_t len;
    int i = discfs_find(s->fs, path);
    if (i < 0 || !(s->data = discfs_read(s->fs, i, &len))) return 0;
    size_t n = strlen(path);
    s->n = tim_scan(s->data, len, s->pc && n > 4 && !strcmp(path + n - 4, ".TPC"), s->tims, 1024);
    snprintf(s->path, sizeof s->path, "%s", path);
    return s->n;
}

/* image k of a pack (or a lone TIM, k 0), palette 0: RGBA (transparent where its color is 0); w 0 if missing */
static Img image(Source *s, const char *path, int k)
{
    Img m = {0, 0, NULL};
    if (k >= pack(s, path)) { fail("%s: no image %d", path, k); return m; }
    const Tim *t = &s->tims[k];
    m = img_new(t->w, t->h);
    if (m.p) tim_rgba(t, 0, m.p);
    if (s->stand_in && t->w >= 256 && t->h >= 240) { /* a PS1 screen: doubled to the PC's size (each pixel four) */
        Img big = img_new(t->w * 2, t->h * 2);
        for (int y = 0; y < big.h; y++)
            for (int x = 0; x < big.w; x++) memcpy(at(&big, x, y), at(&m, x / 2, y / 2), 4);
        img_free(&m);
        m = big;
    }
    return m;
}

static Img pcx(Source *s, const char *path)
{
    Img m = {0, 0, NULL};
    size_t len;
    int i = discfs_find(s->fs, path);
    uint8_t *d = i >= 0 ? discfs_read(s->fs, i, &len) : NULL;
    if (!d) { fail("%s: missing", path); return m; }
    m.p = pcx_rgba(d, len, &m.w, &m.h);
    free(d);
    if (!m.p) { m.w = m.h = 0; fail("%s: not a PCX", path); }
    return m;
}

/* ---- the output ---- */

static char out_dir[1024];
static const uint8_t KEY[3] = {255, 0, 255}; /* (transparent, in the keyed pictures) */

/* a 24-bit BMP, as ui_assets.py's save_bmp; keyed: transparent pixels magenta, else their color (black) */
static int save_bmp(const char *name, const Img *m, int keyed)
{
    if (!m->p || m->w <= 0) return fail("%s: nothing to write", name);
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", out_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return fail("cannot write %s", path);
    int stride = (m->w * 3 + 3) & ~3;
    uint32_t size = (uint32_t)stride * m->h;
    uint8_t h[54] = {'B', 'M'};
    uint32_t v[] = {54 + size, 0, 54, 40, (uint32_t)m->w, (uint32_t)m->h};
    memcpy(h + 2, &v[0], 4);
    memcpy(h + 6, &v[1], 4);
    memcpy(h + 10, &v[2], 4);
    memcpy(h + 14, &v[3], 4);
    memcpy(h + 18, &v[4], 4);
    memcpy(h + 22, &v[5], 4);
    h[26] = 1;
    h[28] = 24;
    memcpy(h + 34, &size, 4);
    uint32_t ppm = 2835;
    memcpy(h + 38, &ppm, 4);
    memcpy(h + 42, &ppm, 4);
    fwrite(h, 1, 54, f);
    uint8_t *row = calloc((size_t)stride, 1);
    for (int y = m->h - 1; y >= 0; y--) {
        for (int x = 0; x < m->w; x++) {
            const uint8_t *p = at(m, x, y);
            const uint8_t *c = keyed && !p[3] ? KEY : p;
            row[x * 3] = c[2];
            row[x * 3 + 1] = c[1];
            row[x * 3 + 2] = c[0];
        }
        fwrite(row, 1, (size_t)stride, f);
    }
    free(row);
    return fclose(f) == 0 ? 1 : fail("cannot write %s", path);
}
static int save_text(const char *name, const char *text)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", out_dir, name);
    FILE *f = fopen(path, "w"); /* (text: CRLF on Windows, as the Python's) */
    if (!f) return fail("cannot write %s", path);
    fputs(text, f);
    return fclose(f) == 0;
}

/* side by side, as many as given (each the first's height) */
static Img strip(const Img *parts, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) w += parts[i].w;
    Img m = img_new(w, parts[0].h);
    for (int i = 0, x0 = 0; i < n; x0 += parts[i].w, i++)
        for (int y = 0; y < m.h && y < parts[i].h; y++) memcpy(at(&m, x0, y), at(&parts[i], 0, y), (size_t)parts[i].w * 4);
    return m;
}

/* ---- the pictures ---- */

static const int HAIR_FIRST = 1, HAIR_N = 19; /* TITLE.TPC: Sweet Tooth's flaming hair, 19 frames */
static int hair(Source *pc)
{
    Img f[19];
    for (int i = 0; i < HAIR_N; i++) /* (the PS1's TITLE.TMS: frames 0-15, its menu text, frames 16-18) */
        f[i] = image(pc, "/SHELLDB1/TITLE.TPC", pc->stand_in ? i + (i >= 16) : HAIR_FIRST + i);
    Img m = strip(f, HAIR_N);
    int ok = save_bmp("hair.bmp", &m, 1);
    for (int i = 0; i < HAIR_N; i++) img_free(&f[i]);
    img_free(&m);
    return ok;
}

static const int PORTRAITS[14][2] = {{8, 14}, {5, 0}, {26, 9}, {3, 15}, {4, 16}, {10, 17}, {7, 18}, {6, 19}, {11, 20},
                                     {12, 21}, {13, 22}, {2, 23}, {27, 28}, {29, 30}}; /* CARSEL.TPC, car-select order */
static const int PORTRAITS_PS1[14][2] = {{37, 31}, {40, 44}, {15, 36}, {42, 30}, {41, 29}, {35, 28}, {38, 27}, {39, 26},
                                         {34, 25}, {33, 24}, {32, 23}, {43, 22}, {17, 16}, {19, 18}}; /* the PS1's CARSEL.TMS */
static const struct { const char *code; int x0; } INFO_ART[14] = {
    {"HH", 385}, {"OT", 348}, {"WH", 370}, {"MG", 400}, {"PV", 348}, {"TH", 370}, {"SP", 400},
    {"RK", 400}, {"IR", 370}, {"AX", 400}, {"FL", 380}, {"HS", 348}, {"ST", 380}, {"MN", 400}};
enum { INFO_W = 240 };

static int portraits(Source *pc)
{
    Img f[28];
    for (int i = 0; i < 28; i++) {
        f[i] = image(pc, "/SHELLDB1/CARSEL.TPC", (pc->stand_in ? PORTRAITS_PS1 : PORTRAITS)[i / 2][i % 2]);
        for (int k = 0; k < f[i].w * f[i].h; k++) f[i].p[k * 4 + 3] = 255; /* (as their RGB: no key) */
    }
    Img m = strip(f, 28);
    int ok = save_bmp("portraits.bmp", &m, 0);
    for (int i = 0; i < 28; i++) img_free(&f[i]);
    img_free(&m);
    Img hq = img_new(INFO_W * 14, 480);
    for (int i = 0; i < 14 && ok; i++) {
        char p[64];
        snprintf(p, sizeof p, "/BIOS/%sINFO.TIM", INFO_ART[i].code);
        Img info = image(pc, p, 0);
        if (!info.p) { ok = 0; break; }
        for (int y = 0; y < hq.h && y < info.h; y++) memcpy(at(&hq, i * INFO_W, y), at(&info, INFO_ART[i].x0, y), INFO_W * 4);
        char n[32];
        snprintf(n, sizeof n, "carinfo_%02d.bmp", i);
        ok &= save_bmp(n, &info, 0);
        img_free(&info);
    }
    ok = ok && save_bmp("portraits_hq.bmp", &hq, 0);
    img_free(&hq);
    return ok;
}

/* card scaled to back's height (nearest), centered on it */
static void card_on(Img *back, const Img *card)
{
    int w = card->w * back->h / card->h, x0 = (back->w - w) / 2;
    for (int y = 0; y < back->h; y++)
        for (int x = 0; x < w; x++) memcpy(at(back, x0 + x, y), at(card, x * card->w / w, y * card->h / back->h), 4);
}

static const char *const ARENA_CARDS[12] = {"NONE", "LA", "MOSC", "PARIS", "AMAZ", "NY", "ANT", "HOLL", "HONG", "SWAMP", "CYB", "OROOF"};
static const int ARENA_WIDE_OPENING[4] = {15, 8, 496, 248}, ARENA_CARD_BORDER = 10;

static int arenas(Source *pc)
{
    static const int tpc_of[9] = {0, 2, 3, 1, 5, 6, 4, 8, 7};
    static const int ps1_of_pc[9] = {8, 6, 5, 4, 3, 2, 1, 0, 7}; /* (the PS1 LEVELSEL.TMS index of each PC one) */
    int ok = 1;
    for (int level = 0; level < 9 && ok; level++) {
        Img px = image(pc, "/SHELLDB1/LEVELSEL.TPC", pc->stand_in ? ps1_of_pc[tpc_of[level]] : tpc_of[level]);
        if (level == 0) {
            Img none = pcx(pc, "/SHELLDB3/SC-NONE.PCX");
            if (none.p && px.p) card_on(&px, &none);
            img_free(&none);
        }
        char n[32];
        snprintf(n, sizeof n, "arena_%d.bmp", level);
        ok &= save_bmp(n, &px, 0);
        img_free(&px);
    }
    for (int i = 0; i < 12 && ok && !pc->stand_in; i++) { /* (the cards: the PC version's alone) */
        char p[64], n[32];
        snprintf(p, sizeof p, "/SHELLDB3/SC-%s.PCX", ARENA_CARDS[i]);
        Img card = pcx(pc, p);
        snprintf(n, sizeof n, "arena_card_%d.bmp", i);
        ok &= save_bmp(n, &card, 0);
        if (ok && i >= 9) { /* the secret ones' 2:1 pictures: the card's scene under its name, in the brick frame */
            Img frame = image(pc, "/SHELLDB1/LEVELSEL.TPC", 2);
            int fx0 = ARENA_WIDE_OPENING[0], fy0 = ARENA_WIDE_OPENING[1], fx1 = ARENA_WIDE_OPENING[2], fy1 = ARENA_WIDE_OPENING[3];
            int b = ARENA_CARD_BORDER, cx0 = b, cx1 = card.w - b, cy1 = card.h - b;
            int cy0 = cy1 - (cx1 - cx0) * (fy1 - fy0) / (fx1 - fx0);
            for (int y = fy0; frame.p && y < fy1; y++)
                for (int x = fx0; x < fx1; x++)
                    memcpy(at(&frame, x, y), at(&card, cx0 + (x - fx0) * (cx1 - cx0) / (fx1 - fx0), cy0 + (y - fy0) * (cy1 - cy0) / (fy1 - fy0)), 4);
            snprintf(n, sizeof n, "arena_%d.bmp", i);
            ok &= save_bmp(n, &frame, 0);
            img_free(&frame);
        }
        img_free(&card);
    }
    Img back = image(pc, "/SHELLDB1/LEVELSEL.TPC", pc->stand_in ? 8 : 0);
    ok = ok && save_bmp("arena_back.bmp", &back, 0);
    img_free(&back);
    return ok;
}

static int simple(Source *pc)
{
    int ok = 1;
    Img m = {0, 0, NULL};
    if (!pc->stand_in) { /* (PS1 only: the glass voted from its menu screens, in menus(); no car select titles) */
        m = image(pc, "/SHELLDB2/GLASS2.TIM", 0);
        ok &= save_bmp("glass.bmp", &m, 0);
        img_free(&m);
    }
    m = image(pc, "/SHELLDB1/TITLE.TIM", 0);
    ok &= ok && save_bmp("title.bmp", &m, 0);
    img_free(&m);
    if (!pc->stand_in) {
        m = image(pc, "/SHELLDB1/CARSEL.TPC", 1);
        ok &= ok && save_bmp("carsel_titles.bmp", &m, 1);
        img_free(&m);
    }
    m = image(pc, "/SHELLDB1/CARSEL.TPC", pc->stand_in ? 21 : 24);
    ok &= ok && save_bmp("emblem1.bmp", &m, 1);
    img_free(&m);
    m = image(pc, "/SHELLDB1/CARSEL.TPC", pc->stand_in ? 20 : 25); /* the 2 players' emblem (split car select: players 2-4) */
    ok &= ok && save_bmp("emblem2.bmp", &m, 1);
    img_free(&m);
    for (int level = 1; level <= 8 && ok; level++) {
        char p[64], n[32];
        snprintf(p, sizeof p, "/SHELLDB1/SHOWOPS%d.TIM", level);
        snprintf(n, sizeof n, "lineup_%d.bmp", level);
        m = image(pc, p, 0);
        ok &= save_bmp(n, &m, 0);
        img_free(&m);
    }
    return ok;
}

/* the HUD's weapon icons and two pickups (the first arena pack's: Suburbs), each scaled whole into a 64x64 cell */
static const int ICONS[10] = {302, 299, 305, 43, 306, 307, 304, 105, 33, 301};
static const int ICONS_PS1[10] = {76, 78, 74, 93, 73, 72, 75, 113, 95, 77}; /* (its TMS/BURB.TMS) */
enum { ICON_CELL = 64 };
static int icons(Source *pc)
{
    Img m = img_new(ICON_CELL * 10, ICON_CELL);
    for (int k = 0; k < m.w * m.h; k++) memcpy(m.p + k * 4, KEY, 3), m.p[k * 4 + 3] = 255;
    for (int i = 0; i < 10; i++) {
        Img img = pc->stand_in ? image(pc, "/TMS/BURB.TMS", ICONS_PS1[i]) : image(pc, "/LEVELDB/BURB.TPC", ICONS[i]);
        if (!img.p) { img_free(&m); return 0; }
        int k = ICON_CELL / img.w < ICON_CELL / img.h ? ICON_CELL / img.w : ICON_CELL / img.h;
        if (k < 1) k = 1;
        if (img.w * k > ICON_CELL || img.h * k > ICON_CELL) k = 1;
        int ox = i * ICON_CELL + (ICON_CELL - img.w * k) / 2, oy = (ICON_CELL - img.h * k) / 2;
        for (int y = 0; y < img.h * k && y < ICON_CELL; y++)
            for (int x = 0; x < img.w * k && x < ICON_CELL; x++) {
                const uint8_t *p = at(&img, x / k, y / k);
                if (p[3] && (p[0] || p[1] || p[2])) memcpy(at(&m, ox + x, oy + y), p, 3);
            }
        img_free(&img);
    }
    int ok = save_bmp("icons.bmp", &m, 0);
    img_free(&m);
    return ok;
}

/* ---- from the PS1 disc ---- */

static int plates(Source *ps1)
{
    Img m = img_new(128 * 14, 64);
    for (int i = 0; i < 14; i++) {
        char p[64];
        snprintf(p, sizeof p, "/SCREEN/%sPLATE.TIM", INFO_ART[i].code);
        Img t = image(ps1, p, 0);
        if (!t.p) { img_free(&m); return 0; }
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 128; x++) memcpy(at(&m, i * 128 + x, y), at(&t, x, y), 3), at(&m, i * 128 + x, y)[3] = 255;
        img_free(&t);
    }
    int ok = save_bmp("plates.bmp", &m, 0);
    img_free(&m);
    return ok;
}

enum { BAR_SHEET = 4, BAR_TOP = 43, BAR_H = 21 };
static int bar(Source *ps1)
{
    Img t = image(ps1, "/SHELLDB2/OPTIONS.TMS", BAR_SHEET);
    if (!t.p) return 0;
    Img m = img_new(t.w, BAR_H);
    for (int y = 0; y < BAR_H; y++) {
        memcpy(at(&m, 0, y), at(&t, 0, BAR_TOP + y), (size_t)t.w * 4);
        if (y >= 4 && y < 17) { /* the inside: the bar's own dark (the darker of its first two pixels) */
            const uint8_t *a = at(&t, 0, BAR_TOP + y), *b = at(&t, 1, BAR_TOP + y);
            const uint8_t *d = b[0] + b[1] + b[2] < a[0] + a[1] + a[2] ? b : a;
            for (int x = 0; x < t.w; x++) memcpy(at(&m, x, y), d, 4);
        }
        for (int x = 0; x < t.w; x++) at(&m, x, y)[3] = 255;
    }
    int ok = save_bmp("bar.bmp", &m, 0);
    img_free(&t);
    img_free(&m);
    return ok;
}

/* Scale2x (EPX), on a keyed picture (alpha 0: the key) */
static int same(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 4); }
static Img scale2x(const Img *s)
{
    Img o = img_new(s->w * 2, s->h * 2);
#define G(xx, yy) at(s, (xx) < 0 ? 0 : (xx) >= s->w ? s->w - 1 : (xx), (yy) < 0 ? 0 : (yy) >= s->h ? s->h - 1 : (yy))
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            const uint8_t *p = at(s, x, y), *a = G(x, y - 1), *b = G(x + 1, y), *c = G(x - 1, y), *d = G(x, y + 1);
            memcpy(at(&o, 2 * x, 2 * y), same(c, a) && !same(c, d) && !same(a, b) ? a : p, 4);
            memcpy(at(&o, 2 * x + 1, 2 * y), same(a, b) && !same(a, c) && !same(b, d) ? b : p, 4);
            memcpy(at(&o, 2 * x, 2 * y + 1), same(d, c) && !same(d, b) && !same(c, a) ? c : p, 4);
            memcpy(at(&o, 2 * x + 1, 2 * y + 1), same(b, d) && !same(b, a) && !same(d, c) ? d : p, 4);
        }
#undef G
    return o;
}

static void keyed(Img *m) /* transparent pixels the key color, opaque (as ui_assets.py's font sheet) */
{
    for (int k = 0; k < m->w * m->h; k++) {
        uint8_t *p = m->p + k * 4;
        if (!p[3]) memcpy(p, KEY, 3);
        p[3] = 255;
    }
}

static const struct { int font; uint32_t table; int spacing, shell; } FONTS[5] = {
    {0, 0x8016297C, -1, 0}, {1, 0x8016297C, -1, 0}, {5, 0x8016297C, 7, 0}, {6, 0x80162B4C, -1, 0}, {2, 0x80162D58, 0, 1}};

static int game_font(Source *ps1)
{
    Img ua = image(ps1, "/SCREEN/UAFONT1.TIM", 0), cp = image(ps1, "/SCREEN/CPFONT.TIM", 0);
    if (!ua.p || !cp.p) { img_free(&ua); img_free(&cp); return 0; }
    keyed(&ua);
    keyed(&cp);
    int w = ua.w > cp.w ? ua.w : cp.w;
    Img sheet = img_new(w, ua.h + cp.h);
    for (int y = 0; y < sheet.h; y++)
        for (int x = 0; x < w; x++) {
            const Img *s = y < ua.h ? &ua : &cp;
            int sy = y < ua.h ? y : y - ua.h;
            if (x < s->w) memcpy(at(&sheet, x, y), at(s, x, sy), 4);
            else memcpy(at(&sheet, x, y), KEY, 3), at(&sheet, x, y)[3] = 255;
        }
    Img s2 = scale2x(&sheet);
    Img big = img_new(s2.w * 2, s2.h * 2);
    for (int y = 0; y < big.h; y++)
        for (int x = 0; x < big.w; x++) memcpy(at(&big, x, y), at(&s2, x / 2, y / 2), 4);
    int ok = save_bmp("gamefont.bmp", &big, 0);
    int shell_v = ua.h;
    img_free(&ua); img_free(&cp); img_free(&sheet); img_free(&s2); img_free(&big);
    /* the glyph tables, from the executable */
    int i = discfs_find(ps1->fs, "/SCUS_943.06");
    size_t len = 0;
    uint8_t *exe = i >= 0 ? discfs_read(ps1->fs, i, &len) : NULL;
    if (!exe || len < 0x800) { free(exe); return fail("the PS1 executable is missing"); }
    uint32_t base = (uint32_t)exe[0x18] | exe[0x19] << 8 | exe[0x1A] << 16 | (uint32_t)exe[0x1B] << 24;
    size_t cap = 64 * 1024, n = 0;
    char *txt = malloc(cap);
    n += (size_t)snprintf(txt + n, cap - n, "# font spacing, then 77 glyphs: u v w h yoff (tools/art/ui_assets.py)\n");
    for (int f = 0; f < 5; f++) {
        size_t o = FONTS[f].table - base + 0x800;
        if (o + 77 * 6 > len) { free(exe); free(txt); return fail("the font tables are not where they should be"); }
        n += (size_t)snprintf(txt + n, cap - n, "font %d %d %d\n", FONTS[f].font, FONTS[f].spacing, FONTS[f].shell ? shell_v : 0);
        for (int g = 0; g < 77; g++) {
            const uint8_t *e = exe + o + (size_t)g * 6;
            n += (size_t)snprintf(txt + n, cap - n, "%d %d %d %d %d\n", e[0], e[1], e[2], e[3], (int16_t)(e[4] | e[5] << 8));
        }
    }
    ok = ok && save_text("gamefont.txt", txt);
    free(txt);
    free(exe);
    return ok;
}

/* ---- the menu screens without their baked-in text (tools/art/clean_menus.py) ---- */

/* the screens drawn on the shared glass (the PC's, 640x480): where enough of them agree on a pixel, it is bare glass */
enum { MENU_W = 640, MENU_H = 480, MIN_VOTES = 3, MIN_VOTES_DARK = 2 };
static const char *const GLASS_SCREENS[13] = {"/SHELLDB1/SINGLE.TIM", "/SHELLDB1/MULTI.TIM", "/SHELLDB2/OPTS.TIM", "/SHELLDB2/AUDIO.TIM",
                                              "/SHELLDB2/HISTORY.TIM", "/SHELLDB1/SHOWOPS1.TIM", "/SHELLDB1/SHOWOPS2.TIM",
                                              "/SHELLDB1/SHOWOPS3.TIM", "/SHELLDB1/SHOWOPS4.TIM", "/SHELLDB1/SHOWOPS5.TIM",
                                              "/SHELLDB1/SHOWOPS6.TIM", "/SHELLDB1/SHOWOPS7.TIM", "/SHELLDB1/SHOWOPS8.TIM"};
enum { NO_LEGEND_FIRST = 5 }; /* (the level previews: no button legend) */

typedef struct { Img px; uint8_t *votes; } Plate;

static Img screens[13];

/* the most common color of the given screens, each pixel; a tie: the first seen (Python's Counter.most_common) */
static Plate make_plate(int first, int n)
{
    Plate pl = {img_new(MENU_W, MENU_H), calloc(MENU_W * MENU_H, 1)};
    for (int y = 0; y < MENU_H; y++)
        for (int x = 0; x < MENU_W; x++) {
            int best = first, best_n = 0;
            for (int i = first; i < first + n; i++) {
                const uint8_t *c = at(&screens[i], x, y);
                int seen = 0, cnt = 0;
                for (int j = first; j < i && !seen; j++) seen = !memcmp(at(&screens[j], x, y), c, 3);
                if (seen) continue;
                for (int j = i; j < first + n; j++) cnt += !memcmp(at(&screens[j], x, y), c, 3);
                if (cnt > best_n) { best_n = cnt; best = i; }
            }
            memcpy(at(&pl.px, x, y), at(&screens[best], x, y), 3);
            at(&pl.px, x, y)[3] = 255;
            pl.votes[y * MENU_W + x] = (uint8_t)best_n;
        }
    return pl;
}

static int bright(const uint8_t *p) { return p[0] + p[1] + p[2] > 600; }
/* the button legend: bright text and border rows nearby the bottom (x0, y0, x1, y1), 0 if none */
static int legend_box(const Img *px, int box[4])
{
    int w = px->w, h = px->h, first = -1, last = -1;
    for (int y = h * 5 / 6; y < h; y++) {
        int n = 0;
        for (int x = 0; x < w; x++) n += bright(at(px, x, y));
        if (n > 30) { if (first < 0) first = y; last = y; }
    }
    if (first < 0) return 0;
    int y0 = first - 3, y1 = last + 4 < h ? last + 4 : h, x_min = w, x_max = -1;
    for (int x = 0; x < w; x++)
        for (int y = y0; y < y1; y++)
            if (bright(at(px, x, y))) { if (x < x_min) x_min = x; if (x > x_max) x_max = x; break; }
    box[0] = x_min - 3 > 0 ? x_min - 3 : 0;
    box[1] = y0;
    box[2] = x_max + 4 < w ? x_max + 4 : w;
    box[3] = y1;
    return 1;
}

/* a screen with its text areas (its legend, and the given box) filled from the plates */
static Img clean_screen(int s, const Plate *legend, const Plate *glass, const int *area)
{
    Img out = img_new(MENU_W, MENU_H);
    memcpy(out.p, screens[s].p, (size_t)MENU_W * MENU_H * 4);
    int boxes[2][4], kinds[2], nb = 0;
    if (legend_box(&screens[s], boxes[nb])) kinds[nb++] = 0;
    if (area) { memcpy(boxes[nb], area, sizeof boxes[0]); kinds[nb++] = 1; }
    for (int b = 0; b < nb; b++) {
        const Plate *pl = kinds[b] ? glass : legend;
        for (int y = boxes[b][1]; y < boxes[b][3]; y++)
            for (int x = boxes[b][0]; x < boxes[b][2]; x++) {
                int v = pl->votes[y * MENU_W + x];
                const uint8_t *q = at(&pl->px, x, y);
                if (v >= MIN_VOTES || (v >= MIN_VOTES_DARK && q[0] + q[1] + q[2] < 300)) memcpy(at(&out, x, y), q, 4);
            }
    }
    return out;
}

/* tone curves (one a channel) taking the plate's glass to GLASS2's (glass_levels) */
static void glass_levels(const Img *plate, const Img *glass, int curves[3][256])
{
    for (int k = 0; k < 3; k++) {
        static long long tot[256], cnt[256];
        memset(tot, 0, sizeof tot);
        memset(cnt, 0, sizeof cnt);
        for (int y = 0; y < plate->h && y < glass->h; y++)
            for (int x = 0; x < plate->w && x < glass->w; x++) {
                const uint8_t *p = at(plate, x, y);
                if (p[0] == 255 && p[1] == 0 && p[2] == 255) continue;
                tot[p[k]] += at(glass, x, y)[k];
                cnt[p[k]]++;
            }
        double last = 0.0;
        for (int v = 0; v < 256; v++) {
            int lo = v, hi = v;
            long long t = tot[v], n = cnt[v];
            while (n < 50 && (lo > 0 || hi < 255)) {
                lo = lo > 0 ? lo - 1 : 0;
                hi = hi < 255 ? hi + 1 : 255;
                t = n = 0;
                for (int j = lo; j <= hi; j++) t += tot[j], n += cnt[j];
            }
            double m = n ? (double)t / (double)n : last;
            if (m > last) last = m;
            curves[k][v] = (int)rint(last);
        }
    }
}

enum { ART = 90 };
/* the curves where px is the plate's glass; on the artwork only its black level taken down (apply_levels) */
static Img apply_levels(const Img *px, int curves[3][256], const Img *plate)
{
    int black[3];
    for (int k = 0; k < 3; k++) {
        black[k] = 0;
        for (int v = 0; v < 256; v++) if (curves[k][v] <= curves[k][0] + 1) black[k] = v;
    }
    Img out = img_new(px->w, px->h);
    for (int y = 0; y < px->h; y++)
        for (int x = 0; x < px->w; x++) {
            const uint8_t *p = at(px, x, y), *q = at(plate, x, y);
            int d = abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]);
            double w = q[0] == 255 && q[1] == 0 && q[2] == 255 ? 0.0 : 1.0 - (double)d / ART;
            if (w < 0.0) w = 0.0;
            uint8_t *o = at(&out, x, y);
            for (int k = 0; k < 3; k++) {
                double lifted = (double)((p[k] - black[k]) * 255) / (double)(255 - black[k]);
                if (lifted < 0) lifted = 0;
                o[k] = (uint8_t)rint(w * curves[k][p[k]] + (1.0 - w) * lifted);
            }
            o[3] = 255;
        }
    return out;
}

/* PS1 only: the thin light rule its screens draw above the button legend (and its dark edges), painted over: each
 * pixel of the band blended between the rows just outside it */
static void ps1_unline(Img *m)
{
    enum { GAP = 4, EDGE = 2 };
    int top = -1, bottom = -1, x0 = m->w, x1 = -1;
    for (int y = m->h * 5 / 6; y < m->h - GAP; y++) {
        int n = 0, lo = m->w, hi = -1;
        for (int x = 0; x < m->w; x++) {
            const uint8_t *p = at(m, x, y), *a = at(m, x, y - GAP), *b = at(m, x, y + GAP);
            int l = p[0] + p[1] + p[2];
            if (l > a[0] + a[1] + a[2] + 120 && l > b[0] + b[1] + b[2] + 120) {
                n++;
                if (x < lo) lo = x;
                hi = x;
            }
        }
        if (n < m->w / 3) continue;
        if (top < 0) top = y;
        bottom = y;
        if (lo < x0) x0 = lo;
        if (hi > x1) x1 = hi;
    }
    if (top < 0) return;
    int ya = top - EDGE - 1, yb = bottom + EDGE + 1;
    if (ya < 0 || yb >= m->h) return;
    x0 = x0 - 4 > 0 ? x0 - 4 : 0, x1 = x1 + 5 < m->w ? x1 + 5 : m->w;
    for (int y = ya + 1; y < yb; y++)
        for (int x = x0; x < x1; x++) {
            const uint8_t *a = at(m, x, ya), *b = at(m, x, yb);
            uint8_t *p = at(m, x, y);
            for (int k = 0; k < 3; k++) p[k] = (uint8_t)((a[k] * (yb - y) + b[k] * (y - ya)) / (yb - ya));
        }
}

static Plate glass_plate;
static int menus(Source *pc)
{
    for (int i = 0; i < 13; i++) {
        screens[i] = image(pc, GLASS_SCREENS[i], 0);
        if (!screens[i].p || screens[i].w != MENU_W || screens[i].h != MENU_H) return fail("%s: not a 640x480 screen", GLASS_SCREENS[i]);
    }
    Plate legend = make_plate(NO_LEGEND_FIRST, 13 - NO_LEGEND_FIRST);
    glass_plate = make_plate(0, 13);
    static const int TOP[4] = {0, 0, 330, 140};
    Img opts = clean_screen(2, &legend, &glass_plate, TOP), audio = clean_screen(3, &legend, &glass_plate, TOP);
    Img single = clean_screen(0, &legend, &glass_plate, pc->stand_in ? TOP : NULL); /* (the PS1's has its title) */
    /* the plate itself, its legend's place from the legend's plate (clean_menus.py's plate_glass.png) */
    int box[4];
    if (legend_box(&screens[0], box))
        for (int y = box[1]; y < box[3]; y++)
            for (int x = box[0]; x < box[2]; x++) {
                memcpy(at(&glass_plate.px, x, y), at(&legend.px, x, y), 4);
                glass_plate.votes[y * MENU_W + x] = legend.votes[y * MENU_W + x];
            }
    Img glass = {0, 0, NULL};
    static int curves[3][256];
    if (pc->stand_in) { /* PS1 only: no GLASS2 to match, the voted glass is the glass */
        for (int k = 0; k < 3; k++) for (int v = 0; v < 256; v++) curves[k][v] = v;
        ps1_unline(&glass_plate.px);
        if (!save_bmp("glass.bmp", &glass_plate.px, 0)) return 0;
    } else {
        glass = image(pc, "/SHELLDB2/GLASS2.TIM", 0);
        glass_levels(&glass_plate.px, &glass, curves);
    }
    Img o = apply_levels(&opts, curves, &glass_plate.px), a = apply_levels(&audio, curves, &glass_plate.px),
        s = apply_levels(&single, curves, &glass_plate.px);
    if (pc->stand_in) ps1_unline(&o), ps1_unline(&a), ps1_unline(&s);
    int ok = save_bmp("options.bmp", &o, 0) && save_bmp("audio.bmp", &a, 0) && save_bmp("single.bmp", &s, 0);
    img_free(&o); img_free(&a); img_free(&s); img_free(&opts); img_free(&audio); img_free(&single); img_free(&glass);
    img_free(&legend.px);
    free(legend.votes);
    return ok;
}

/* ---- the network background (tools/art/net_background.py, then ui_assets.py's network_on_glass) ---- */

typedef struct { double c[3]; } D3;
enum { NET_SMOOTH = 4, NET_SWEEPS = 400, BLOCK = 6, RING = 4, WHITE = 150, FEATHER = 14 };
static const int COVERED[7][4] = {{8, 6, 558, 42}, {13, 96, 282, 115}, {13, 112, 314, 286}, {13, 288, 186, 308},
                                  {13, 306, 314, 334}, {13, 338, 180, 358}, {13, 355, 314, 383}};
static const int FIT[3][4] = {{0, 386, 260, 480}, {0, 120, 12, 380}, {0, 44, 300, 94}};
static const int QUILT[2][4] = {{236, 306, 314, 334}, {236, 355, 314, 383}};
static const int SOURCE[4] = {236, 268, 470, 472};
static const double DETAIL_FADE = 10.0, GUIDE = 0.25;

/* the flame's texture rebuilt where the two thin boxes cross it (net_background.py's quilt): writes smooth */
static void quilt(const Img *net, const D3 *diff, D3 *smooth, const uint8_t *cov, int w, int h)
{
    int win = BLOCK + 2 * RING, sx0 = SOURCE[0], sy0 = SOURCE[1], sx1 = SOURCE[2], sy1 = SOURCE[3];
    int ncand = 0, *cands = malloc(sizeof(int) * 2 * (size_t)((sx1 - sx0) * (sy1 - sy0) / 4 + 1));
    for (int y = sy0; y < sy1 - win; y += 2)
        for (int x = sx0; x < sx1 - win; x += 2) {
            int ok = 1;
            for (int yy = y; yy < y + win && ok; yy++)
                for (int xx = x; xx < x + win && ok; xx++) {
                    const uint8_t *p = at(net, xx, yy);
                    int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
                    ok = !cov[yy * w + xx] && mn < WHITE;
                }
            if (ok) { cands[ncand * 2] = x; cands[ncand * 2 + 1] = y; ncand++; }
        }
    int span = BLOCK + 2 * RING;
    for (int qi = 0; qi < 2; qi++) {
        int qx0 = QUILT[qi][0], qy0 = QUILT[qi][1], qx1 = QUILT[qi][2], qy1 = QUILT[qi][3];
        /* acc: r, g, b, weight of the blocks copied in, by pixel (seen: set) */
        int aw = qx1 - qx0 + 2 * span, ah = qy1 - qy0 + 2 * span, ax0 = qx0 - span, ay0 = qy0 - span;
        double (*acc)[4] = calloc((size_t)aw * ah, sizeof *acc);
        uint8_t *has = calloc((size_t)aw * ah, 1);
#define ACC(x, y) acc[((y) - ay0) * aw + ((x) - ax0)]
#define HAS(x, y) has[((y) - ay0) * aw + ((x) - ax0)]
        static struct { int dx, dy; double t[3], wt; } ctx[(BLOCK + 2 * RING) * (BLOCK + 2 * RING)];
        for (int bx = qx1 - BLOCK; bx > qx0 - BLOCK; bx -= BLOCK)
            for (int by = qy0; by < qy1; by += BLOCK) {
                int nc = 0;
                for (int dy = -RING; dy < BLOCK + RING; dy++)
                    for (int dx = -RING; dx < BLOCK + RING; dx++) {
                        int x = bx + dx, y = by + dy;
                        if (x < 0 || x >= w || y < 0 || y >= h) continue;
                        if (!cov[y * w + x]) {
                            ctx[nc].dx = dx, ctx[nc].dy = dy, ctx[nc].wt = 1.0;
                            memcpy(ctx[nc].t, diff[y * w + x].c, sizeof ctx[nc].t);
                            nc++;
                        } else if (HAS(x, y)) {
                            const double *a = ACC(x, y);
                            ctx[nc].dx = dx, ctx[nc].dy = dy, ctx[nc].wt = 1.0;
                            ctx[nc].t[0] = a[0] / a[3], ctx[nc].t[1] = a[1] / a[3], ctx[nc].t[2] = a[2] / a[3];
                            nc++;
                        } else if (dx >= 0 && dx < BLOCK && dy >= 0 && dy < BLOCK) {
                            ctx[nc].dx = dx, ctx[nc].dy = dy, ctx[nc].wt = GUIDE;
                            memcpy(ctx[nc].t, smooth[y * w + x].c, sizeof ctx[nc].t);
                            nc++;
                        }
                    }
                double best = 0;
                int have = 0, px = 0, py = 0;
                for (int ci = 0; ci < ncand; ci++) {
                    int cx = cands[ci * 2], cy = cands[ci * 2 + 1];
                    double c = 0.0;
                    for (int k = 0; k < nc; k++) {
                        const double *q = diff[(cy + RING + ctx[k].dy) * w + cx + RING + ctx[k].dx].c;
                        double e0 = q[0] - ctx[k].t[0], e1 = q[1] - ctx[k].t[1], e2 = q[2] - ctx[k].t[2];
                        c += (e0 * e0 + e1 * e1 + e2 * e2) * ctx[k].wt;
                        if (have && c >= best) break;
                    }
                    if (!have || c < best) { best = c; have = 1; px = cx + RING; py = cy + RING; }
                }
                for (int dy = -RING; dy < BLOCK + RING; dy++)
                    for (int dx = -RING; dx < BLOCK + RING; dx++) {
                        int x = bx + dx, y = by + dy;
                        if (x < 0 || x >= w || y < 0 || y >= h || !cov[y * w + x]) continue;
                        int e1 = dx + RING, e2 = dy + RING, e3 = BLOCK + RING - 1 - dx, e4 = BLOCK + RING - 1 - dy;
                        int edge = e1 < e2 ? e1 : e2;
                        if (e3 < edge) edge = e3;
                        if (e4 < edge) edge = e4;
                        double wt = (double)(edge + 1) / (RING + 1);
                        if (wt > 1.0) wt = 1.0;
                        const double *q = diff[(py + dy) * w + px + dx].c;
                        double *a = ACC(x, y);
                        HAS(x, y) = 1;
                        a[0] += q[0] * wt; a[1] += q[1] * wt; a[2] += q[2] * wt; a[3] += wt;
                    }
            }
        /* q: the copied flame, by pixel (in acc) */
        D3 *q = calloc((size_t)aw * ah, sizeof *q);
#define Q(x, y) q[((y) - ay0) * aw + ((x) - ax0)]
        for (int y = ay0; y < ay0 + ah; y++)
            for (int x = ax0; x < ax0 + aw; x++)
                if (HAS(x, y)) for (int k = 0; k < 3; k++) Q(x, y).c[k] = ACC(x, y)[k] / ACC(x, y)[3];
        /* the smooth offset (harmonic) meeting the picture at the top, bottom and right edges */
        int nx = qx1 - qx0, ny = qy1 - qy0;
        D3 *top = calloc((size_t)nx, sizeof *top), *bot = calloc((size_t)nx, sizeof *bot), *right = calloc((size_t)ny, sizeof *right);
        for (int side = 0; side < 3; side++) {
            int n = side < 2 ? nx : ny;
            D3 *m = calloc((size_t)n, sizeof *m), *dst = side == 0 ? top : side == 1 ? bot : right;
            for (int i = 0; i < n; i++) {
                int ox = side < 2 ? qx0 + i : qx1, oy = side == 0 ? qy0 - 1 : side == 1 ? qy1 : qy0 + i;
                int ix = side < 2 ? qx0 + i : qx1 - 1, iy = side == 0 ? qy0 : side == 1 ? qy1 - 1 : qy0 + i;
                for (int k = 0; k < 3; k++) m[i].c[k] = diff[oy * w + ox].c[k] - Q(ix, iy).c[k];
            }
            for (int i = 0; i < n; i++) {
                int j0 = i - 3 > 0 ? i - 3 : 0, j1 = i + 4 < n ? i + 4 : n;
                for (int k = 0; k < 3; k++) {
                    PySum s = {0, 0};
                    for (int j = j0; j < j1; j++) ps_add(&s, m[j].c[k]);
                    dst[i].c[k] = ps_val(&s) / (j1 - j0);
                }
            }
            free(m);
        }
        /* f over the block and a ring of one around it */
        int fw = nx + 2, fh = ny + 2;
        double *f = malloc(sizeof(double) * (size_t)fw * fh);
#define F(x, y) f[((y) - (qy0 - 1)) * fw + ((x) - (qx0 - 1))]
        for (int k = 0; k < 3; k++) {
            for (int i = 0; i < fw * fh; i++) f[i] = 0.0;
            for (int i = 0; i < nx; i++) F(qx0 + i, qy0 - 1) = top[i].c[k], F(qx0 + i, qy1) = bot[i].c[k];
            for (int i = 0; i < ny; i++) F(qx1, qy0 + i) = right[i].c[k];
            F(qx1, qy0 - 1) = F(qx1, qy1) = 0.0;
            for (int s = 0; s < 300; s++)
                for (int y = qy0; y < qy1; y++)
                    for (int x = qx0; x < qx1; x++) {
                        double t = F(x - 1, y) + F(x + 1, y) + F(x, y - 1) + F(x, y + 1);
                        F(x, y) += 1.9 * (t / 4 - F(x, y));
                    }
            for (int y = qy0; y < qy1; y++)
                for (int x = qx0; x < qx1; x++) Q(x, y).c[k] += F(x, y);
        }
#undef F
        for (int y = qy0; y < qy1; y++)
            for (int x = qx0; x < qx1; x++) {
                if (!HAS(x, y)) continue;
                double t = (double)(x - qx0) / FEATHER;
                if (t > 1.0) t = 1.0;
                if (t < 0.0) t = 0.0;
                for (int k = 0; k < 3; k++) smooth[y * w + x].c[k] = smooth[y * w + x].c[k] * (1 - t) + Q(x, y).c[k] * t;
            }
#undef Q
#undef ACC
#undef HAS
        free(f); free(top); free(bot); free(right); free(q); free(acc); free(has);
    }
    free(cands);
}

static Img net_clean(Source *pc)
{
    Img net = pcx(pc, "/SHELLDB3/NETMAIN.PCX"), glass = image(pc, "/SHELLDB2/GLASS2.TIM", 0), none = {0, 0, NULL};
    if (!net.p || !glass.p) { img_free(&net); img_free(&glass); return none; }
    int w = net.w, h = net.h;
    uint8_t *cov = calloc((size_t)w * h, 1);
    for (int i = 0; i < 7; i++)
        for (int y = COVERED[i][1]; y < COVERED[i][3]; y++)
            for (int x = COVERED[i][0]; x < COVERED[i][2]; x++) cov[y * w + x] = 1;
    double scale[3], offset[3];
    for (int k = 0; k < 3; k++) { /* NETMAIN ~ scale * GLASS2 + offset (least squares; integer sums, as the Python's) */
        long long n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int r = 0; r < 3; r++)
            for (int y = FIT[r][1]; y < FIT[r][3]; y++)
                for (int x = FIT[r][0]; x < FIT[r][2]; x++) {
                    long long g = at(&glass, x, y)[k], m = at(&net, x, y)[k];
                    n++; sx += g; sy += m; sxx += g * g; sxy += g * m;
                }
        scale[k] = (double)(n * sxy - sx * sy) / (double)(n * sxx - sx * sx);
        offset[k] = ((double)sy - scale[k] * (double)sx) / (double)n;
    }
    D3 *mapped = malloc(sizeof(D3) * (size_t)w * h), *diff = malloc(sizeof(D3) * (size_t)w * h);
    D3 *smooth = calloc((size_t)w * h, sizeof(D3)), *smooth_out;
    uint8_t *set = calloc((size_t)w * h, 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int k = 0; k < 3; k++) {
                mapped[y * w + x].c[k] = at(&glass, x, y)[k] * scale[k] + offset[k];
                diff[y * w + x].c[k] = at(&net, x, y)[k] - mapped[y * w + x].c[k];
            }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (cov[y * w + x]) continue;
            double a0 = 0, a1 = 0, a2 = 0;
            int n = 0;
            for (int yy = y - NET_SMOOTH > 0 ? y - NET_SMOOTH : 0; yy < (y + NET_SMOOTH + 1 < h ? y + NET_SMOOTH + 1 : h); yy++)
                for (int xx = x - NET_SMOOTH > 0 ? x - NET_SMOOTH : 0; xx < (x + NET_SMOOTH + 1 < w ? x + NET_SMOOTH + 1 : w); xx++)
                    if (!cov[yy * w + xx]) {
                        const double *d = diff[yy * w + xx].c;
                        a0 += d[0]; a1 += d[1]; a2 += d[2]; n++;
                    }
            smooth[y * w + x].c[0] = a0 / n, smooth[y * w + x].c[1] = a1 / n, smooth[y * w + x].c[2] = a2 / n;
            set[y * w + x] = 1;
        }
    smooth_out = malloc(sizeof(D3) * (size_t)w * h);
    memcpy(smooth_out, smooth, sizeof(D3) * (size_t)w * h);
    static const int dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int y = 0; y < h; y++) /* a first guess: the nearest uncovered smoothing each way, by 1 / d^2 */
        for (int x = 0; x < w; x++) {
            if (!cov[y * w + x]) continue;
            double a[3] = {0, 0, 0}, ws = 0.0;
            for (int dd = 0; dd < 4; dd++) {
                int xx = x, yy = y, d = 0;
                while (xx >= 0 && xx < w && yy >= 0 && yy < h && cov[yy * w + xx]) xx += dirs[dd][0], yy += dirs[dd][1], d++;
                if (xx >= 0 && xx < w && yy >= 0 && yy < h) {
                    double wt = 1.0 / (d * d);
                    const double *s = smooth[yy * w + xx].c;
                    a[0] += s[0] * wt; a[1] += s[1] * wt; a[2] += s[2] * wt;
                    ws += wt;
                }
            }
            for (int k = 0; k < 3; k++) smooth[y * w + x].c[k] = a[k] / ws;
            set[y * w + x] = 1;
        }
    double *f = malloc(sizeof(double) * (size_t)w * h);
    for (int k = 0; k < 3; k++) { /* the harmonic fill, over the covered pixels */
        for (int i = 0; i < w * h; i++) f[i] = set[i] ? smooth[i].c[k] : 0.0;
        for (int s = 0; s < NET_SWEEPS; s++)
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    if (!cov[y * w + x]) continue;
                    int n = 0;
                    double t = 0;
                    if (x > 0) t += f[y * w + x - 1], n++;
                    if (x < w - 1) t += f[y * w + x + 1], n++;
                    if (y > 0) t += f[(y - 1) * w + x], n++;
                    if (y < h - 1) t += f[(y + 1) * w + x], n++;
                    f[y * w + x] += 1.9 * (t / n - f[y * w + x]);
                }
        for (int i = 0; i < w * h; i++) if (cov[i]) smooth[i].c[k] = f[i];
    }
    free(f);
    for (int y = 0; y < h; y++) /* the texture, mirrored in across the nearest edges */
        for (int x = 0; x < w; x++) {
            if (!cov[y * w + x]) continue;
            double a[3] = {0, 0, 0}, ws = 0.0;
            int nearby = -1;
            for (int dd = 0; dd < 4; dd++) {
                int xx = x, yy = y, d = 0;
                while (xx >= 0 && xx < w && yy >= 0 && yy < h && cov[yy * w + xx]) xx += dirs[dd][0], yy += dirs[dd][1], d++;
                int sx = xx + dirs[dd][0] * (d - 1), sy = yy + dirs[dd][1] * (d - 1);
                if (sx < 0 || sx >= w || sy < 0 || sy >= h || cov[sy * w + sx]) continue;
                nearby = nearby < 0 || d < nearby ? d : nearby;
                double wt = 1.0 / (d * d);
                for (int k = 0; k < 3; k++) a[k] += (diff[sy * w + sx].c[k] - smooth_out[sy * w + sx].c[k]) * wt;
                ws += wt;
            }
            if (ws != 0.0) {
                double fade = py_exp(-(double)(nearby - 1) / DETAIL_FADE);
                for (int k = 0; k < 3; k++) smooth[y * w + x].c[k] = smooth[y * w + x].c[k] + a[k] / ws * fade;
            }
        }
    quilt(&net, diff, smooth, cov, w, h);
    Img out = img_new(w, h);
    memcpy(out.p, net.p, (size_t)w * h * 4);
    for (int i = 0; i < w * h; i++)
        if (cov[i])
            for (int k = 0; k < 3; k++) {
                double v = rint(mapped[i].c[k] + smooth[i].c[k]);
                out.p[i * 4 + k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
    free(cov); free(mapped); free(diff); free(smooth); free(smooth_out); free(set);
    img_free(&net);
    img_free(&glass);
    return out;
}

/* the artwork (flame, ring, cable) over GLASS2 itself, fading out at the left and right edges (network_on_glass) */
static int network(Source *pc)
{
    Img px = net_clean(pc), glass = image(pc, "/SHELLDB2/GLASS2.TIM", 0);
    if (!px.p || !glass.p) { img_free(&px); img_free(&glass); return fail("the network screen is missing"); }
    static int curves[3][256];
    glass_levels(&px, &glass, curves);
    const int fade = 48;
    Img out = img_new(px.w, px.h);
    for (int y = 0; y < px.h; y++)
        for (int x = 0; x < px.w; x++) {
            const uint8_t *p = at(&px, x, y), *g = at(&glass, x, y);
            int c[3] = {curves[0][p[0]], curves[1][p[1]], curves[2][p[2]]};
            int d = abs(c[0] - g[0]) + abs(c[1] - g[1]) + abs(c[2] - g[2]);
            double a = (double)d / ART, e = (double)x / fade, r = (double)(px.w - 1 - x) / fade;
            if (a > 1.0) a = 1.0;
            double m = 1.0 < e ? 1.0 : e;
            if (r < m) m = r;
            double art = a * m;
            uint8_t *o = at(&out, x, y);
            for (int k = 0; k < 3; k++) o[k] = (uint8_t)rint(art * (p[k] > g[k] ? p[k] : g[k]) + (1 - art) * g[k]);
            o[3] = 255;
        }
    int ok = save_bmp("network.bmp", &out, 0);
    img_free(&out); img_free(&px); img_free(&glass);
    return ok;
}

/* ---- Car Info with Vanilla Plus's ratings (tools/art/carinfo_stats.py) ---- */

static const int SLOTS_X[5] = {203, 231, 261, 289, 317}, ROWS_Y[3] = {373, 402, 431}; /* ARMOR, SPECIAL WEAPON, SPEED */
enum { DARK = 120 };
static const int VP_STATS[14][3] = { /* by car-select index; {0} the PC's own (Vanilla Plus.xlsx, Balancing) */
    {4, 4, 1}, {3, 3, 4}, {5, 3, 2}, {0}, {2, 3, 4}, {3, 5, 3}, {2, 4, 4}, {3, 4, 3}, {0}, {4, 3, 2}, {5, 5, 1}, {3, 4, 3}, {5, 3, 1}, {0}};

static int dark(const Img *m, int x, int y) { const uint8_t *p = at(m, x, y); return p[0] + p[1] + p[2] < DARK; }
static int row_y(const Img *m, int guess)
{
    long long sum = 0, n = 0;
    for (int y = guess - 12; y < guess + 13; y++)
        for (int x = SLOTS_X[0] - 8; x < SLOTS_X[0] + 9; x++)
            if (dark(m, x, y)) sum += y, n++;
    return n ? (int)rint((double)sum / (double)n) : guess;
}
static int has_ball(const Img *m, int cx, int cy)
{
    int n = 0;
    for (int y = cy - 9; y < cy + 10; y++)
        for (int x = cx - 7; x < cx + 8; x++) n += dark(m, x, y);
    return n >= 90;
}
/* src's pixels around (sx, sy) onto out around (dx, dy), through a soft ellipse (pow, as Python's ** does) */
static void paste(Img *out, const Img *src, int sx, int sy, int dx, int dy, int rx, int ry)
{
    for (int oy = -ry - 3; oy < ry + 4; oy++)
        for (int ox = -rx - 3; ox < rx + 4; ox++) {
            double r = py_pow(py_pow((double)ox / rx, 2.0) + py_pow((double)oy / ry, 2.0), 0.5);
            double a = r <= 0.85 ? 1.0 : r >= 1.2 ? 0.0 : (1.2 - r) / 0.35;
            if (a <= 0) continue;
            const uint8_t *s = at(src, sx + ox, sy + oy);
            uint8_t *d = at(out, dx + ox, dy + oy);
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)rint(d[c] + (s[c] - d[c]) * a);
        }
}
static Img restat(const Img *src, const int stats[3])
{
    Img out = img_new(src->w, src->h);
    memcpy(out.p, src->p, (size_t)src->w * src->h * 4);
    for (int r = 0; r < 3; r++) {
        int y = row_y(src, ROWS_Y[r]), has[5], last_empty = -1;
        for (int k = 0; k < 5; k++) { has[k] = has_ball(src, SLOTS_X[k], y); if (!has[k]) last_empty = SLOTS_X[k]; }
        for (int k = 0; k < 5; k++) {
            if (k < stats[r] && !has[k]) paste(&out, src, SLOTS_X[0], y, SLOTS_X[k], y, 9, 11);
            else if (k >= stats[r] && has[k] && last_empty >= 0) paste(&out, src, last_empty, y, SLOTS_X[k], y, 11, 12);
        }
    }
    return out;
}
static int carinfo_vp(Source *pc)
{
    int ok = 1;
    for (int i = 0; i < 14 && ok; i++) {
        if (!VP_STATS[i][0]) continue;
        char p[64], n[32];
        snprintf(p, sizeof p, "/BIOS/%sINFO.TIM", INFO_ART[i].code);
        Img info = image(pc, p, 0);
        if (!info.p) return 0;
        Img vp = restat(&info, VP_STATS[i]);
        snprintf(n, sizeof n, "carinfo_vp_%02d.bmp", i);
        ok = save_bmp(n, &vp, 0);
        img_free(&vp);
        img_free(&info);
    }
    return ok;
}

/* ---- the co-op duel's screen: COWIN's lettering, new lines (tools/art/duel_text.py --replace-from 5) ---- */

enum { DW = 640, DH = 480 };
static int dlum(const uint8_t *p) { return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000; }
static int warm(const uint8_t *p) { return p[0] > 120 && p[0] > p[2] + 60 && p[0] >= p[1]; }
static double hypot_py(double a, double b) /* (Python's math.hypot: correctly rounded) */
{
    long double x = a, y = b;
    return (double)sqrtl(x * x + y * y);
}
static int cmp_int(const void *a, const void *b) { return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b); }
static int floordiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

typedef struct { int n, minx, maxy, cap; int *xy; } Comp;
/* the 8-connected shapes of mask in rows y0..y1, at least 6 pixels, by their leftmost x (stable: found order) */
static int components(const uint8_t *mask, int y0, int y1, Comp **out)
{
    uint8_t *seen = calloc(DW * DH, 1);
    int nc = 0, cap = 64, *stack = malloc(sizeof(int) * DW * DH * 2);
    Comp *cs = malloc(sizeof(Comp) * (size_t)cap);
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < DW; x++) {
            if (!mask[y * DW + x] || seen[y * DW + x]) continue;
            Comp c = {0, x, y, 64, malloc(sizeof(int) * 2 * 64)};
            int sp = 0;
            stack[sp++] = x, stack[sp++] = y;
            seen[y * DW + x] = 1;
            while (sp) {
                int cy = stack[--sp], cx = stack[--sp];
                if (c.n == c.cap) c.cap *= 2, c.xy = realloc(c.xy, sizeof(int) * 2 * (size_t)c.cap);
                c.xy[c.n * 2] = cx, c.xy[c.n * 2 + 1] = cy, c.n++;
                if (cx < c.minx) c.minx = cx;
                if (cy > c.maxy) c.maxy = cy;
                for (int dx = -1; dx <= 1; dx++)
                    for (int dy = -1; dy <= 1; dy++) {
                        int nx = cx + dx, ny = cy + dy;
                        if (nx >= 0 && nx < DW && ny >= y0 && ny < y1 && mask[ny * DW + nx] && !seen[ny * DW + nx]) {
                            seen[ny * DW + nx] = 1;
                            stack[sp++] = nx, stack[sp++] = ny;
                        }
                    }
            }
            if (c.n >= 6) {
                if (nc == cap) cap *= 2, cs = realloc(cs, sizeof(Comp) * (size_t)cap);
                cs[nc++] = c;
            } else free(c.xy);
        }
    for (int i = 1; i < nc; i++) { /* (insertion sort: stable) */
        Comp c = cs[i];
        int j = i - 1;
        while (j >= 0 && cs[j].minx > c.minx) cs[j + 1] = cs[j], j--;
        cs[j + 1] = c;
    }
    free(seen);
    free(stack);
    *out = cs;
    return nc;
}
static void comps_free(Comp *c, int n) { for (int i = 0; i < n; i++) free(c[i].xy); free(c); }

/* the most common value (first seen of a tie) */
static int most_common_int(const int *v, int n)
{
    int best = v[0], bn = 0;
    for (int i = 0; i < n; i++) {
        int seen = 0, c = 0;
        for (int j = 0; j < i && !seen; j++) seen = v[j] == v[i];
        if (seen) continue;
        for (int j = i; j < n; j++) c += v[j] == v[i];
        if (c > bn) bn = c, best = v[i];
    }
    return best;
}

/* a letter's outline: per row (from the baseline, -64..63), leftmost and rightmost x (from x_ref); has: rows set */
typedef struct { int lo[128], hi[128]; uint8_t has[128]; } Prof;
static void profile(const Comp *c, int x_ref, int base, Prof *p)
{
    memset(p->has, 0, sizeof p->has);
    for (int i = 0; i < c->n; i++) {
        int r = c->xy[i * 2 + 1] - base + 64, v = c->xy[i * 2] - x_ref;
        if (r < 0 || r >= 128) continue;
        if (!p->has[r]) p->has[r] = 1, p->lo[r] = p->hi[r] = v;
        if (v < p->lo[r]) p->lo[r] = v;
        if (v > p->hi[r]) p->hi[r] = v;
    }
}
static int distance(const Prof *pa, int xa, const Prof *pb, int xb, int *d) /* closest horizontal distance; 0: no row shared */
{
    int found = 0;
    for (int r = 0; r < 128; r++)
        if (pa->has[r] && pb->has[r]) {
            int v = (xb + pb->lo[r]) - (xa + pa->hi[r]);
            if (!found || v < *d) *d = v;
            found = 1;
        }
    return found;
}
static int prof_right(const Prof *p) { int m = -100000; for (int r = 0; r < 128; r++) if (p->has[r] && p->hi[r] > m) m = p->hi[r]; return m; }

typedef struct { int dx, dy, kind; double f[3]; } GPx; /* kind 1 the letter, 0 its shadow */
typedef struct { int set, n; GPx *px; Prof prof; } Glyph;

static Img duel_co, duel_plate;
static uint8_t duel_text_mask[DW * DH];
static int (*duel_cell)[3];
static uint8_t *duel_cell_set;
static int duel_shift[3];
static double duel_s;
static int duel_keys[1024], duel_nkeys;
static double duel_model[1024][3];

static void duel_clean(int y, int x, uint8_t *o) /* the emblem without any text, in COWIN's colors */
{
    int ci = (y / 16) * (DW / 16) + x / 16;
    const int *sh = duel_cell_set[ci] ? duel_cell[ci] : duel_shift;
    const uint8_t *p = at(&duel_plate, x, y);
    for (int k = 0; k < 3; k++) { int v = p[k] + sh[k]; o[k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
}
static void duel_color(int x, int y, double out[3])
{
    double d = hypot_py(x - 320, (y - 240) * duel_s) / 4;
    int lo = -1, hi = -1;
    for (int i = 0; i < duel_nkeys; i++) {
        if (duel_keys[i] <= d && (lo < 0 || duel_keys[i] > duel_keys[lo])) lo = i;
        if (duel_keys[i] >= d && (hi < 0 || duel_keys[i] < duel_keys[hi])) hi = i;
    }
    if (lo < 0) lo = 0;
    if (hi < 0) hi = duel_nkeys - 1;
    double t = duel_keys[hi] == duel_keys[lo] ? 0 : (d - duel_keys[lo]) / (double)(duel_keys[hi] - duel_keys[lo]);
    for (int k = 0; k < 3; k++) out[k] = duel_model[lo][k] * (1 - t) + duel_model[hi][k] * t;
}

/* the credits' background without their text: the emblem, voted from the credits screens that share it (CREDIT01-07 but
 * 03, at least 2 agreeing, else the median by brightness); the PC's 640x480 or the PS1's 320x240 */
static Img credits_plate(Source *s)
{
    Img plate = {0, 0, NULL};
    static const int CRED[6] = {1, 2, 4, 5, 6, 7};
    Img cr[6];
    for (int i = 0; i < 6; i++) {
        char p[64];
        snprintf(p, sizeof p, "/SHELLDB2/CREDIT0%d.TIM", CRED[i]);
        cr[i] = image(s, p, 0);
        if (!cr[i].p) { for (int j = 0; j < i; j++) img_free(&cr[j]); return plate; }
    }
    const int W = cr[0].w, H = cr[0].h;
    const int text_lum = W >= 640 ? 175 : 120; /* (the PS1's smaller letters are dimmer, its red headings too) */
    /* the credits' text (and 2 pixels round it) out of the vote */
    uint8_t *masks = calloc((size_t)6 * W * H, 1);
    for (int i = 0; i < 6; i++)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (dlum(at(&cr[i], x, y)) >= text_lum || (W < 640 && at(&cr[i], x, y)[0] > at(&cr[i], x, y)[1] + 40))
                    for (int yy = y - 2 > 0 ? y - 2 : 0; yy < (y + 3 < H ? y + 3 : H); yy++)
                        for (int xx = x - 2 > 0 ? x - 2 : 0; xx < (x + 3 < W ? x + 3 : W); xx++) masks[(size_t)i * W * H + yy * W + xx] = 1;
    plate = img_new(W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *vals[6];
            int n = 0;
            for (int i = 0; i < 6; i++) if (!masks[(size_t)i * W * H + y * W + x]) vals[n++] = at(&cr[i], x, y);
            if (!n) for (int i = 0; i < 6; i++) vals[n++] = at(&cr[i], x, y);
            int best = 0, bn = 0;
            for (int i = 0; i < n; i++) {
                int seen = 0, c = 0;
                for (int j = 0; j < i && !seen; j++) seen = !memcmp(vals[j], vals[i], 3);
                if (seen) continue;
                for (int j = i; j < n; j++) c += !memcmp(vals[j], vals[i], 3);
                if (c > bn) bn = c, best = i;
            }
            const uint8_t *v = vals[best];
            if (bn < 2) { /* the median by brightness (a stable sort: ties as they came) */
                const uint8_t *srt[6];
                memcpy(srt, vals, sizeof(vals[0]) * (size_t)n);
                for (int i = 1; i < n; i++) {
                    const uint8_t *c = srt[i];
                    int j = i - 1;
                    while (j >= 0 && dlum(srt[j]) > dlum(c)) srt[j + 1] = srt[j], j--;
                    srt[j + 1] = c;
                }
                v = srt[n / 2];
            }
            memcpy(at(&plate, x, y), v, 3);
            at(&plate, x, y)[3] = 255;
        }
    free(masks);
    for (int i = 0; i < 6; i++) img_free(&cr[i]);
    return plate;
}

/* PS1 only: the co-op duel's screen from the PS1's COWIN (320x240: smaller letters, an outline, another layout than the
 * PC's). Its first five lines kept; its last three painted over from the credits' emblem (their colors shifted to
 * COWIN's); the two new lines set from pieces of its own lines, picked by hand (the disc is one fixed version): each
 * piece a run of the letter shapes (8-connected warm pixels) of a line, in order from the left. */
enum { PW = 320, PH = 240, PBANDS = 8, PSHAPES = 48 };
static int pwarm(const uint8_t *q) { return q[0] > 110 && q[0] > q[2] + 50; }
typedef struct { int n, x0, x1, *xy; } PShape;

/* a band's letter shapes, ordered by their left edge (a tie: as found, top to bottom, left to right) */
static int pshapes(const Img *m, int s, int e, uint8_t *seen, PShape *out)
{
    int n = 0, *stack = malloc(sizeof(int) * PW * PH);
    for (int y = s - 1; y <= e; y++)
        for (int x = 0; x < PW; x++) {
            if (seen[y * PW + x] || !pwarm(at(m, x, y)) || n == PSHAPES) continue;
            PShape *c = &out[n];
            c->n = 0, c->x0 = x, c->x1 = x, c->xy = malloc(sizeof(int) * PW * PH);
            int sp = 0;
            stack[sp++] = y * PW + x;
            seen[y * PW + x] = 1;
            while (sp) {
                int q = stack[--sp], a = q % PW, b = q / PW;
                c->xy[c->n++] = q;
                if (a < c->x0) c->x0 = a;
                if (a > c->x1) c->x1 = a;
                for (int db = -1; db <= 1; db++)
                    for (int da = -1; da <= 1; da++) {
                        int cc = a + da, dd = b + db;
                        if (cc < 0 || cc >= PW || dd < s - 2 || dd >= e + 2 || dd >= PH || seen[dd * PW + cc] || !pwarm(at(m, cc, dd))) continue;
                        seen[dd * PW + cc] = 1;
                        stack[sp++] = dd * PW + cc;
                    }
            }
            n++;
        }
    free(stack);
    for (int i = 1; i < n; i++) { /* (stable: insertion) */
        PShape t = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].x0 > t.x0) out[j + 1] = out[j], j--;
        out[j + 1] = t;
    }
    return n;
}

static int duel_ps1(Source *stand)
{
    static Source ps1; /* (the PS1's pictures at their own size) */
    memset(&ps1, 0, sizeof ps1);
    ps1.fs = stand->fs;
    Img co = image(&ps1, "/SHELLDB2/COWIN.TIM", 0), plate = credits_plate(&ps1);
    for (int i = 0; i < ps1.n; i++) tim_free(&ps1.tims[i]);
    free(ps1.data);
    if (!co.p || !plate.p || co.w != PW || co.h != PH || plate.w != PW) { img_free(&co); img_free(&plate); return fail("COWIN: not the PS1's"); }
    /* the lines: rows with warm pixels */
    int bands[PBANDS][2], nb = 0;
    for (int y = 0; y < PH;) {
        int any = 0;
        for (int x = 0; x < PW && !any; x++) any = pwarm(at(&co, x, y));
        if (!any) { y++; continue; }
        int s = y;
        for (; y < PH; y++) {
            any = 0;
            for (int x = 0; x < PW && !any; x++) any = pwarm(at(&co, x, y));
            if (!any) break;
        }
        if (nb < PBANDS) bands[nb][0] = s, bands[nb][1] = y;
        nb++;
    }
    if (nb != PBANDS) { img_free(&co); img_free(&plate); return fail("COWIN: %d lines, not 8", nb); }
    static PShape shapes[PBANDS][PSHAPES];
    int ns[PBANDS];
    uint8_t *seen = calloc(PW * PH, 1);
    for (int i = 0; i < PBANDS; i++) ns[i] = pshapes(&co, bands[i][0], bands[i][1], seen, shapes[i]);
    free(seen);
    /* the pieces: line, first shape, last shape; -1 a word space, -2 the end ("Be the last one standing",
     * "to earn your reward...") */
    static const int NEW[2][20][3] = {
        {{3, 0, 0}, {1, 4, 4}, {-1}, {1, 5, 7}, {-1}, {3, 13, 13}, {1, 2, 2}, {0, 8, 8}, {1, 5, 5}, {-1}, {3, 11, 12}, {1, 4, 4},
         {-1}, {0, 8, 8}, {1, 5, 5}, {3, 9, 10}, {6, 4, 4}, {5, 9, 11}, {-2}},
        {{6, 0, 0}, {-1}, {1, 4, 4}, {1, 2, 3}, {3, 10, 10}, {-1}, {6, 10, 12}, {-1}, {7, 6, 10}, {7, 16, 16}, {-2}}};
    enum { SPACE = 4 };
    Img out = img_new(PW, PH);
    memcpy(out.p, co.p, (size_t)PW * PH * 4);
    /* COWIN's colors against the plate's (above and below the text): the median shift */
    int shift[3];
    {
        static int dv[3][PW * PH / 4];
        int n = 0;
        for (int y = 0; y < PH; y += 2)
            for (int x = 0; x < PW; x += 2)
                if ((y < bands[0][0] - 4 || y > bands[PBANDS - 1][1] + 4) && n < PW * PH / 4) {
                    for (int k = 0; k < 3; k++) dv[k][n] = at(&co, x, y)[k] - at(&plate, x, y)[k];
                    n++;
                }
        for (int k = 0; k < 3; k++) { qsort(dv[k], (size_t)n, sizeof(int), cmp_int); shift[k] = n ? dv[k][n / 2] : 0; }
    }
    /* the last three lines away: near their letters (3 pixels), the plate */
    for (int i = 5; i < PBANDS; i++)
        for (int k = 0; k < ns[i]; k++)
            for (int j = 0; j < shapes[i][k].n; j++) {
                int q = shapes[i][k].xy[j], a = q % PW, b = q / PW;
                for (int dy = -3; dy <= 3; dy++)
                    for (int dx = -3; dx <= 3; dx++) {
                        int x = a + dx, y = b + dy;
                        if (x < 0 || y < 0 || x >= PW || y >= PH) continue;
                        for (int c = 0; c < 3; c++) {
                            int v = at(&plate, x, y)[c] + shift[c];
                            at(&out, x, y)[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
                        }
                    }
            }
    /* the new lines, centered, where lines 5 and 6 were */
    int ok = 1;
    for (int li = 0; li < 2 && ok; li++) {
        int width = 0;
        for (int k = 0; NEW[li][k][0] != -2; k++) {
            const int *pc = NEW[li][k];
            if (pc[0] < 0) { width += SPACE; continue; }
            if (pc[2] >= ns[pc[0]]) { ok = fail("COWIN: line %d has no letter shape %d", pc[0], pc[2]); break; }
            width += shapes[pc[0]][pc[2]].x1 - shapes[pc[0]][pc[1]].x0 + 1;
        }
        int x = (PW - width) / 2, ty = bands[5 + li][0];
        for (int k = 0; ok && NEW[li][k][0] != -2; k++) {
            const int *pc = NEW[li][k];
            if (pc[0] < 0) { x += SPACE; continue; }
            int a = shapes[pc[0]][pc[1]].x0, top = bands[pc[0]][0];
            for (int s = pc[1]; s <= pc[2]; s++) /* (each letter pixel and the outline round it: 1 pixel) */
                for (int j = 0; j < shapes[pc[0]][s].n; j++) {
                    int q = shapes[pc[0]][s].xy[j], sx = q % PW, sy = q / PW;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int px = sx + dx, py = sy + dy, tx = x + px - a, tyy = ty + py - top;
                            if (px < 0 || py < 0 || px >= PW || py >= PH || tx < 0 || tyy < 0 || tx >= PW || tyy >= PH) continue;
                            if ((dx || dy) && !pwarm(at(&co, px, py)) && pwarm(at(&out, tx, tyy))) continue; /* (outline: not over a letter) */
                            memcpy(at(&out, tx, tyy), at(&co, px, py), 3);
                        }
                }
            x += shapes[pc[0]][pc[2]].x1 - a + 1;
        }
    }
    for (int i = 0; i < PBANDS; i++) for (int k = 0; k < ns[i]; k++) free(shapes[i][k].xy);
    if (ok) {
        Img big = img_new(PW * 2, PH * 2); /* (at the PC's size, as the other pictures) */
        for (int y = 0; y < big.h; y++) for (int xx = 0; xx < big.w; xx++) memcpy(at(&big, xx, y), at(&out, xx / 2, y / 2), 4);
        ok = save_bmp("duel.bmp", &big, 0);
        img_free(&big);
    }
    img_free(&out); img_free(&co); img_free(&plate);
    return ok;
}

static int duel(Source *pc)
{
    if (pc->stand_in) return duel_ps1(pc);
    static const char *const LINES[8] = {"DARK TOOTH is dead.", "You are the victors in the", "11th TWISTED METAL\xE2\x84\xA2 contest.",
                                         "But, there can only be", "ONE winner of the ultimate prize.", "Try again in single player mode",
                                         "to find out what your", "Twisted reward will be..."};
    static const int USABLE[7] = {-1, -1, -1, -1, -1, 22, -1}; /* lines 0-6; -1: every letter splits cleanly */
    static const char *const TEXT[2] = {"Be the last one standing", "to earn your reward..."};
    const int replace_from = 5;
    duel_co = image(pc, "/SHELLDB2/COWIN.TIM", 0);
    duel_plate = credits_plate(pc);
    if (!duel_plate.p || !duel_co.p) return 0;
    for (int y = 0; y < DH; y++) for (int x = 0; x < DW; x++) duel_text_mask[y * DW + x] = (uint8_t)warm(at(&duel_co, x, y));
    /* COWIN's colors against the credits': the median shift */
    {
        int cap = DW * DH / 4 + 1, n = 0, *d[3];
        for (int k = 0; k < 3; k++) d[k] = malloc(sizeof(int) * (size_t)cap);
        for (int y = 0; y < DH; y += 2)
            for (int x = 0; x < DW; x += 2)
                if (!duel_text_mask[y * DW + x] && abs(dlum(at(&duel_co, x, y)) - dlum(at(&duel_plate, x, y))) < 16) {
                    for (int k = 0; k < 3; k++) d[k][n] = at(&duel_co, x, y)[k] - at(&duel_plate, x, y)[k];
                    n++;
                }
        for (int k = 0; k < 3; k++) { qsort(d[k], (size_t)n, sizeof(int), cmp_int); duel_shift[k] = d[k][n / 2]; free(d[k]); }
    }
    /* the lines (row bands) and their baselines */
    int bands[16][2], nb = 0;
    for (int y = 0; y < DH;) {
        int any = 0;
        for (int x = 0; x < DW && !any; x++) any = duel_text_mask[y * DW + x];
        if (any) {
            int y0 = y;
            for (;;) {
                int a2 = 0;
                if (y >= DH) break;
                for (int x = 0; x < DW && !a2; x++) a2 = duel_text_mask[y * DW + x];
                if (!a2) break;
                y++;
            }
            if (y - y0 > 8 && nb < 16) bands[nb][0] = y0, bands[nb][1] = y, nb++;
        }
        y++;
    }
    int baselines[16];
    for (int b = 0; b < nb; b++) {
        Comp *c;
        int n = components(duel_text_mask, bands[b][0], bands[b][1], &c), *bot = malloc(sizeof(int) * (size_t)(n ? n : 1));
        for (int i = 0; i < n; i++) bot[i] = c[i].maxy;
        baselines[b] = n ? most_common_int(bot, n) : bands[b][1];
        free(bot);
        comps_free(c, n);
    }
    /* nearby the letters of the lines replaced; any text */
    int first = replace_from < nb ? bands[replace_from][0] - 3 : DH;
    uint8_t *nearby = calloc(DW * DH, 1), *anytext = calloc(DW * DH, 1);
    for (int y = 0; y < DH; y++)
        for (int x = 0; x < DW; x++) {
            if (!duel_text_mask[y * DW + x]) continue;
            if (y >= first)
                for (int yy = y - 3 > 0 ? y - 3 : 0; yy < (y + 10 < DH ? y + 10 : DH); yy++)
                    for (int xx = x - 3 > 0 ? x - 3 : 0; xx < (x + 10 < DW ? x + 10 : DW); xx++) nearby[yy * DW + xx] = 1;
            for (int yy = y - 4 > 0 ? y - 4 : 0; yy < (y + 11 < DH ? y + 11 : DH); yy++)
                for (int xx = x - 4 > 0 ? x - 4 : 0; xx < (x + 11 < DW ? x + 11 : DW); xx++) anytext[yy * DW + xx] = 1;
        }
    /* the color shift on 16-pixel cells, from clean background */
    duel_cell = calloc((DH / 16) * (DW / 16), sizeof *duel_cell);
    duel_cell_set = calloc((DH / 16) * (DW / 16), 1);
    {
        int *d[3];
        for (int k = 0; k < 3; k++) d[k] = malloc(sizeof(int) * DW * DH);
        for (int cy = 0; cy < DH; cy += 16)
            for (int cx = 0; cx < DW; cx += 16) {
                static const int RS[4] = {24, 40, 64, 96};
                for (int ri = 0; ri < 4; ri++) {
                    int r = RS[ri], n = 0;
                    for (int y = cy - r > 0 ? cy - r : 0; y < (cy + 16 + r < DH ? cy + 16 + r : DH); y += 2)
                        for (int x = cx - r > 0 ? cx - r : 0; x < (cx + 16 + r < DW ? cx + 16 + r : DW); x += 2)
                            if (!anytext[y * DW + x]) {
                                for (int k = 0; k < 3; k++) d[k][n] = at(&duel_co, x, y)[k] - at(&duel_plate, x, y)[k];
                                n++;
                            }
                    if (n >= 40) {
                        int ci = (cy / 16) * (DW / 16) + cx / 16;
                        for (int k = 0; k < 3; k++) { qsort(d[k], (size_t)n, sizeof(int), cmp_int); duel_cell[ci][k] = d[k][n / 2]; }
                        duel_cell_set[ci] = 1;
                        break;
                    }
                }
            }
        for (int k = 0; k < 3; k++) free(d[k]);
    }
    Img bg = img_new(DW, DH);
    for (int y = 0; y < DH; y++)
        for (int x = 0; x < DW; x++) {
            if (nearby[y * DW + x]) duel_clean(y, x, at(&bg, x, y));
            else memcpy(at(&bg, x, y), at(&duel_co, x, y), 3);
            at(&bg, x, y)[3] = 255;
        }
    /* the color model: radial from the center, the vertical distance scaled by s (the s fitting best) */
    int ns = 0, *sx = malloc(sizeof(int) * DW * DH), *sy = malloc(sizeof(int) * DW * DH);
    for (int y = 0; y < DH; y++)
        for (int x = 0; x < DW; x++) if (duel_text_mask[y * DW + x]) sx[ns] = x - 320, sy[ns] = y - 240, ns++;
    int *bin = malloc(sizeof(int) * (size_t)ns), order[1024], norder;
    double best_fit = 0;
    for (int i = 6; i < 33; i++) {
        double s = i / 20.0;
        norder = 0;
        static long long bsum[1024];
        static int bcnt[1024];
        memset(bcnt, 0, sizeof bcnt);
        memset(bsum, 0, sizeof bsum);
        for (int k = 0; k < ns; k++) {
            int b = (int)hypot_py(sx[k], sy[k] * s) / 4;
            if (b >= 1024) b = 1023;
            bin[k] = b;
            if (!bcnt[b]) order[norder++] = b;
            bcnt[b]++;
            bsum[b] += at(&duel_co, sx[k] + 320, sy[k] + 240)[1];
        }
        PySum total = {0, 0};
        for (int o = 0; o < norder; o++) { /* (bins as first met; each its values in order) */
            int b = order[o];
            double mean = (double)bsum[b] / bcnt[b];
            PySum part = {0, 0};
            for (int k = 0; k < ns; k++)
                if (bin[k] == b) ps_add(&part, py_pow(at(&duel_co, sx[k] + 320, sy[k] + 240)[1] - mean, 2.0));
            ps_add(&total, ps_val(&part));
        }
        double fit = ps_val(&total) / ns;
        if (i == 6 || fit < best_fit) best_fit = fit, duel_s = s;
    }
    {
        static long long sum3[1024][3];
        static int cnt[1024];
        memset(sum3, 0, sizeof sum3);
        memset(cnt, 0, sizeof cnt);
        for (int k = 0; k < ns; k++) {
            int b = (int)hypot_py(sx[k], sy[k] * duel_s) / 4;
            if (b >= 1024) b = 1023;
            cnt[b]++;
            for (int c = 0; c < 3; c++) sum3[b][c] += at(&duel_co, sx[k] + 320, sy[k] + 240)[c];
        }
        duel_nkeys = 0;
        for (int b = 0; b < 1024; b++)
            if (cnt[b] >= 8) {
                duel_keys[duel_nkeys] = b;
                for (int c = 0; c < 3; c++) duel_model[duel_nkeys][c] = (double)sum3[b][c] / cnt[b];
                duel_nkeys++;
            }
    }
    free(bin); free(sx); free(sy);
    /* cut the letters */
    static Glyph glyphs[256];
    memset(glyphs, 0, sizeof glyphs);
    int gaps[512], ngaps = 0, spaces[512], nspaces = 0;
    for (int li = 0; li < nb && li < 7; li++) {
        char chars[64];
        int nchars = 0;
        for (const char *p = LINES[li]; *p; p++) if (*p != ' ') chars[nchars++] = *p;
        Comp *comps;
        int nc = components(duel_text_mask, bands[li][0], bands[li][1], &comps);
        int n = USABLE[li] >= 0 ? USABLE[li] : nchars;
        if (USABLE[li] < 0 && nc != nchars) { comps_free(comps, nc); continue; }
        int *bot = malloc(sizeof(int) * (size_t)(nc ? nc : 1));
        for (int i = 0; i < nc; i++) bot[i] = comps[i].maxy;
        int baseline = most_common_int(bot, nc);
        free(bot);
        uint8_t word_break[64] = {0};
        for (int i = 0, k = 0; LINES[li][i]; i++) {
            if (LINES[li][i] == ' ') word_break[k] = 1;
            else k++;
        }
        Prof prev_prof;
        int have_prev = 0, prev_left = 0;
        for (int i = 0; i < n && i < nchars && i < nc; i++) {
            const Comp *c = &comps[i];
            int left = c->minx;
            Prof prof;
            profile(c, left, baseline, &prof);
            int d;
            if (have_prev && distance(&prev_prof, prev_left, &prof, left, &d)) {
                if (word_break[i]) spaces[nspaces++] = d;
                else gaps[ngaps++] = d;
            }
            prev_prof = prof, prev_left = left, have_prev = 1;
            unsigned char ch = (unsigned char)chars[i];
            if (glyphs[ch].set) continue;
            uint8_t *cs = calloc(DW * DH, 1);
            int minx = DW, maxx = 0, miny = DH, maxy = 0;
            for (int q = 0; q < c->n; q++) {
                int x = c->xy[q * 2], y = c->xy[q * 2 + 1];
                cs[y * DW + x] = 1;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
            Glyph *g = &glyphs[ch];
            g->set = 1;
            g->prof = prof;
            g->px = malloc(sizeof(GPx) * (size_t)(maxx - minx + 11) * (maxy - miny + 11));
            for (int yy = miny - 1; yy < maxy + 9; yy++)
                for (int xx = minx - 1; xx < maxx + 9; xx++) {
                    if (xx < 0 || xx >= DW || yy < 0 || yy >= DH) continue;
                    const uint8_t *cp = at(&duel_co, xx, yy);
                    if (cs[yy * DW + xx]) {
                        double m[3];
                        duel_color(xx, yy, m);
                        GPx *gp = &g->px[g->n++];
                        gp->dx = xx - left, gp->dy = yy - baseline, gp->kind = 1;
                        for (int k = 0; k < 3; k++) gp->f[k] = cp[k] / (m[k] > 1 ? m[k] : 1.0);
                    } else if (!duel_text_mask[yy * DW + xx]) {
                        uint8_t cl[4];
                        duel_clean(yy, xx, cl);
                        if (dlum(cp) >= dlum(cl) - 20) continue;
                        int hit = 0;
                        for (int s0 = 0; s0 < 7 && !hit; s0++)
                            for (int s1 = 0; s1 < 7 && !hit; s1++) {
                                int ax = xx - s0, ay = yy - s1;
                                hit = ax >= 0 && ay >= 0 && cs[ay * DW + ax];
                            }
                        if (!hit) continue;
                        GPx *gp = &g->px[g->n++];
                        gp->dx = xx - left, gp->dy = yy - baseline, gp->kind = 0;
                        for (int k = 0; k < 3; k++) gp->f[k] = (double)cp[k] / (cl[k] > 1 ? cl[k] : 1);
                    }
                }
            free(cs);
        }
        comps_free(comps, nc);
    }
    qsort(gaps, (size_t)ngaps, sizeof(int), cmp_int);
    qsort(spaces, (size_t)nspaces, sizeof(int), cmp_int);
    int gap = gaps[ngaps / 2], space = spaces[nspaces / 2], pitches[16];
    for (int b = 0; b + 1 < nb; b++) pitches[b] = bands[b + 1][0] - bands[b][0];
    qsort(pitches, (size_t)(nb - 1), sizeof(int), cmp_int);
    int pitch = pitches[nb / 2 - 1];
    Img out = img_new(DW, DH);
    memcpy(out.p, bg.p, (size_t)DW * DH * 4);
    for (int li = 0; li < 2; li++) {
        const char *line = TEXT[li];
        int lx[64], lch[64], nl = 0, want = gap, have_prev = 0, prev_x = 0;
        const Prof *prev = NULL;
        for (const char *p = line; *p; p++) {
            if (*p == ' ') { want = space; continue; }
            Glyph *g = &glyphs[(unsigned char)*p];
            if (!g->set) { img_free(&out); img_free(&bg); return fail("the letter %c is not in COWIN", *p); }
            int x, d;
            if (!have_prev) x = 0;
            else x = distance(prev, prev_x, &g->prof, 0, &d) ? want - d : prev_x + prof_right(prev) + want;
            lx[nl] = x, lch[nl] = (unsigned char)*p, nl++;
            prev = &g->prof, prev_x = x, have_prev = 1, want = gap;
        }
        int right = -1000000;
        for (int i = 0; i < nl; i++) { int r = lx[i] + prof_right(&glyphs[lch[i]].prof); if (r > right) right = r; }
        int shift_x = 320 - floordiv(right, 2);
        int i2 = replace_from + li;
        int base = i2 < nb ? baselines[i2] : baselines[nb - 1] + (i2 - nb + 1) * pitch;
        for (int kind = 0; kind < 2; kind++) /* every shadow first, then the letters over them */
            for (int i = 0; i < nl; i++) {
                const Glyph *g = &glyphs[lch[i]];
                for (int k = 0; k < g->n; k++) {
                    const GPx *gp = &g->px[k];
                    if (gp->kind != kind) continue;
                    int xx = lx[i] + shift_x + gp->dx, yy = base + gp->dy;
                    if (xx < 0 || xx >= DW || yy < 0 || yy >= DH) continue;
                    double ref[3];
                    if (kind) duel_color(xx, yy, ref);
                    else for (int c = 0; c < 3; c++) ref[c] = at(&bg, xx, yy)[c];
                    for (int c = 0; c < 3; c++) {
                        double v = rint(ref[c] * gp->f[c]);
                        at(&out, xx, yy)[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
                    }
                }
            }
    }
    int ok = save_bmp("duel.bmp", &out, 0);
    for (int i = 0; i < 256; i++) free(glyphs[i].px);
    img_free(&out); img_free(&bg); img_free(&duel_co); img_free(&duel_plate);
    free(nearby); free(anytext); free(duel_cell); free(duel_cell_set);
    return ok;
}

/* ---- Python's random module (MT19937, seed, getrandbits, random, sample): carsel_road.py's choices, the same ---- */

static uint32_t mt[624];
static int mti;
static void mt_init(uint32_t s)
{
    mt[0] = s;
    for (mti = 1; mti < 624; mti++) mt[mti] = 1812433253u * (mt[mti - 1] ^ (mt[mti - 1] >> 30)) + (uint32_t)mti;
}
static void py_seed(uint32_t seed) /* (Python's random.seed(int) for a seed under 2^32: init_by_array([seed])) */
{
    mt_init(19650218u);
    int i = 1, j = 0;
    for (int k = 624; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525u)) + seed + (uint32_t)j;
        i++, j++;
        if (i >= 624) mt[0] = mt[623], i = 1;
        if (j >= 1) j = 0;
    }
    for (int k = 623; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941u)) - (uint32_t)i;
        i++;
        if (i >= 624) mt[0] = mt[623], i = 1;
    }
    mt[0] = 0x80000000u;
    mti = 624;
}
static uint32_t mt_next(void)
{
    if (mti >= 624) {
        for (int k = 0; k < 624; k++) {
            uint32_t y = (mt[k] & 0x80000000u) | (mt[(k + 1) % 624] & 0x7FFFFFFFu);
            mt[k] = mt[(k + 397) % 624] ^ (y >> 1) ^ (y & 1 ? 0x9908B0DFu : 0);
        }
        mti = 0;
    }
    uint32_t y = mt[mti++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9D2C5680u;
    y ^= (y << 15) & 0xEFC60000u;
    return y ^ (y >> 18);
}
static double py_random(void)
{
    uint32_t a = mt_next() >> 5, b = mt_next() >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}
static int py_randbelow(int n)
{
    int k = 0;
    while ((n >> k) && k < 31) k++;
    uint32_t r = mt_next() >> (32 - k);
    while ((int)r >= n) r = mt_next() >> (32 - k);
    return (int)r;
}
/* random.sample(range(n), k): the indices chosen, in order */
static void py_sample(int n, int k, int *out)
{
    int setsize = 21;
    if (k > 5) setsize += (int)py_pow(4.0, ceil(log(k * 3.0) / log(4.0)));
    if (n <= setsize) {
        int *pool = malloc(sizeof(int) * (size_t)n);
        for (int i = 0; i < n; i++) pool[i] = i;
        for (int i = 0; i < k; i++) {
            int j = py_randbelow(n - i);
            out[i] = pool[j];
            pool[j] = pool[n - i - 1];
        }
        free(pool);
    } else {
        uint8_t *sel = calloc((size_t)n, 1);
        for (int i = 0; i < k; i++) {
            int j = py_randbelow(n);
            while (sel[j]) j = py_randbelow(n);
            sel[j] = 1;
            out[i] = j;
        }
        free(sel);
    }
}

/* ---- car select without its button legend: the road continued under it (tools/art/carsel_road.py) ---- */

enum { ROAD_PATCH = 14, ROAD_STEP = 10, CANDIDATES = 400, ROAD_SEED = 7, DASH_END = 436 };
static double flum(const uint8_t *p) { return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000.0; }
static int yellowp(const uint8_t *p) { return p[0] > 120 && p[1] > 70 && p[2] < 90 && p[0] > p[2] + 60; }
typedef struct { int sx, sy; double m, s; } RoadSrc;
static double sq(double v) { return py_pow(v, 2.0); } /* (Python's ** 2) */

static int carsel(Source *pc)
{
    py_seed(ROAD_SEED);
    Img src = image(pc, "/SHELLDB1/CARSEL.TIM", 0);
    if (!src.p) return 0;
    int w = src.w, h = src.h, box[4];
    if (!legend_box(&src, box)) { img_free(&src); return fail("car select: no legend found"); }
    int x0 = box[0], y0 = box[1], x1 = box[2], y1 = box[3];
    Img out = img_new(w, h);
    memcpy(out.p, src.p, (size_t)w * h * 4);
#define KNOWN(x, y) (!((x) >= x0 && (x) < x1 && (y) >= y0 && (y) < y1))
    double *weight = malloc(sizeof(double) * (size_t)w * h), (*acc)[3] = calloc((size_t)w * h, sizeof *acc);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) weight[y * w + x] = KNOWN(x, y) ? 1.0 : 0.0;
    double (*above)[2] = malloc(sizeof *above * (size_t)w), (*below)[2] = malloc(sizeof *below * (size_t)w);
    for (int pass = 0; pass < 2; pass++) /* the road's brightness and grain contrast, above and below the box, per column */
        for (int x = 0; x < w; x++) {
            int ya = pass ? y1 + 1 : y0 - 9, yb = pass ? (h < y1 + 8 ? h : y1 + 8) : y0 - 2, n = 0;
            double vals[17 * 16];
            PySum s = {0, 0}, v = {0, 0};
            for (int y = ya; y < yb; y++)
                for (int xx = x - 8 > 0 ? x - 8 : 0; xx < (x + 9 < w ? x + 9 : w); xx++) vals[n++] = flum(at(&src, xx, y));
            for (int i = 0; i < n; i++) ps_add(&s, vals[i]);
            double m = ps_val(&s) / n;
            for (int i = 0; i < n; i++) ps_add(&v, sq(vals[i] - m));
            double (*dst)[2] = pass ? below : above;
            dst[x][0] = m, dst[x][1] = py_pow(ps_val(&v) / n, 0.5);
        }
#define TARGET(y, x, i) (above[x][i] * (1 - tt(y)) + below[x][i] * tt(y))
    double tt_den = y1 - y0;
    double tts[1024];
    for (int y = 0; y < h && y < 1024; y++) {
        double t = (y - y0 + 0.5) / tt_den;
        tts[y] = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    }
#define tt(y) tts[y]
    uint8_t *isy = malloc((size_t)w * h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) isy[y * w + x] = (uint8_t)yellowp(at(&src, x, y));
    int nsrc = 0;
    RoadSrc *sources = malloc(sizeof(RoadSrc) * (size_t)w * 64);
    for (int part = 0; part < 2; part++)
        for (int sy = part ? y1 + 1 : y0 - 32; sy < (part ? h - ROAD_PATCH + 1 : y0 - ROAD_PATCH - 1); sy++)
            for (int sx = 0; sx < w - ROAD_PATCH + 1; sx += 2) {
                int yel = 0;
                for (int y = sy; y < sy + ROAD_PATCH && !yel; y++)
                    for (int x = sx; x < sx + ROAD_PATCH && !yel; x++) yel = isy[y * w + x];
                if (yel) continue;
                double ls[ROAD_PATCH * ROAD_PATCH];
                PySum s = {0, 0}, v = {0, 0};
                int n = 0;
                for (int y = sy; y < sy + ROAD_PATCH; y++) for (int x = sx; x < sx + ROAD_PATCH; x++) ls[n++] = flum(at(&src, x, y));
                for (int i = 0; i < n; i++) ps_add(&s, ls[i]);
                double m = ps_val(&s) / n;
                for (int i = 0; i < n; i++) ps_add(&v, sq(ls[i] - m));
                sources[nsrc].sx = sx, sources[nsrc].sy = sy, sources[nsrc].m = m, sources[nsrc].s = py_pow(ps_val(&v) / n, 0.5);
                nsrc++;
            }
#define TAKE(sx, sy, flip, x, y, bx, by) at(&src, (sx) + ((flip) ? ROAD_PATCH - 1 - ((x) - (bx)) : (x) - (bx)), (sy) + (y) - (by))
    int *pool = malloc(sizeof(int) * (size_t)nsrc), *pick = malloc(sizeof(int) * CANDIDATES);
    double *keys = malloc(sizeof(double) * (size_t)nsrc);
    static int cx[ROAD_PATCH * ROAD_PATCH], cy[ROAD_PATCH * ROAD_PATCH];
    for (int by = y0 - 4; by < y1; by += ROAD_STEP)
        for (int bx = x0 - 4; bx < x1; bx += ROAD_STEP) {
            int nc = 0, need = 0;
            for (int y = by; y < by + ROAD_PATCH; y++)
                for (int x = bx; x < bx + ROAD_PATCH; x++)
                    if (x >= 0 && x < w && y >= 0 && y < h) cx[nc] = x, cy[nc] = y, nc++;
            for (int i = 0; i < nc && !need; i++) need = !KNOWN(cx[i], cy[i]) && weight[cy[i] * w + cx[i]] < 1;
            if (!need) continue;
            PySum stm = {0, 0}, sts = {0, 0};
            for (int i = 0; i < nc; i++) ps_add(&stm, TARGET(cy[i], cx[i], 0));
            for (int i = 0; i < nc; i++) ps_add(&sts, TARGET(cy[i], cx[i], 1));
            double tm = ps_val(&stm) / nc, ts = ps_val(&sts) / nc;
            int np = 0;
            for (int i = 0; i < nsrc; i++)
                if (0.8 * ts <= sources[i].s && sources[i].s <= 1.25 * ts && 0.75 * tm <= sources[i].m && sources[i].m <= 1.33 * tm) pool[np++] = i;
            if (np < 50) { /* the closest 200 (a stable sort by the key) */
                for (int i = 0; i < nsrc; i++)
                    keys[i] = fabs(sources[i].s - ts) / (ts > 1 ? ts : 1) + fabs(sources[i].m - tm) / (tm > 1 ? tm : 1);
                for (int i = 0; i < nsrc; i++) pool[i] = i;
                for (int i = 1; i < nsrc; i++) {
                    int c = pool[i], j = i - 1;
                    while (j >= 0 && keys[pool[j]] > keys[c]) pool[j + 1] = pool[j], j--;
                    pool[j + 1] = c;
                }
                np = nsrc < 200 ? nsrc : 200;
            }
            int k = np < CANDIDATES ? np : CANDIDATES;
            py_sample(np, k, pick);
            double best = 0, bcm[3] = {0}, blift = 0, bgain = 0;
            int have = 0, bsx = 0, bsy = 0, bflip = 0;
            for (int ci = 0; ci < k; ci++) {
                const RoadSrc *s = &sources[pool[pick[ci]]];
                int flip = py_random() < 0.5;
                double ls[ROAD_PATCH * ROAD_PATCH], csum[3] = {0, 0, 0};
                PySum lsum = {0, 0}, lvar = {0, 0};
                long long isum[3] = {0, 0, 0};
                for (int i = 0; i < nc; i++) {
                    const uint8_t *q = TAKE(s->sx, s->sy, flip, cx[i], cy[i], bx, by);
                    ls[i] = flum(q);
                    for (int c = 0; c < 3; c++) isum[c] += q[c];
                }
                for (int i = 0; i < nc; i++) ps_add(&lsum, ls[i]);
                double pm = ps_val(&lsum) / nc;
                for (int i = 0; i < nc; i++) ps_add(&lvar, sq(ls[i] - pm));
                double ps = py_pow(ps_val(&lvar) / nc, 0.5);
                if (ps < 1.0) ps = 1.0;
                for (int c = 0; c < 3; c++) csum[c] = (double)isum[c] / nc;
                double gain = ts / ps;
                if (gain < 0.6) gain = 0.6;
                if (gain > 1.6) gain = 1.6;
                double lift = tm / (pm > 1.0 ? pm : 1.0), err = 0.0;
                for (int i = 0; i < nc; i++) {
                    int x = cx[i], y = cy[i];
                    if (!(KNOWN(x, y) || weight[y * w + x] > 0)) continue;
                    const uint8_t *q = TAKE(s->sx, s->sy, flip, x, y, bx, by);
                    PySum e = {0, 0};
                    for (int c = 0; c < 3; c++) {
                        double a = csum[c] * lift + (q[c] - csum[c]) * gain;
                        double hv = KNOWN(x, y) ? at(&out, x, y)[c] : acc[y * w + x][c] / weight[y * w + x];
                        ps_add(&e, sq(a - hv));
                    }
                    err += ps_val(&e);
                }
                if (!have || err < best) {
                    best = err, have = 1, bsx = s->sx, bsy = s->sy, bflip = flip, blift = lift, bgain = gain;
                    memcpy(bcm, csum, sizeof bcm);
                }
            }
            for (int i = 0; i < nc; i++) {
                int x = cx[i], y = cy[i];
                if (KNOWN(x, y)) continue;
                int e1 = x - bx, e2 = bx + ROAD_PATCH - 1 - x, e3 = y - by, e4 = by + ROAD_PATCH - 1 - y;
                int e = e1 < e2 ? e1 : e2;
                if (e3 < e) e = e3;
                if (e4 < e) e = e4;
                e += 1;
                const uint8_t *q = TAKE(bsx, bsy, bflip, x, y, bx, by);
                double a[3];
                for (int c = 0; c < 3; c++) a[c] = bcm[c] * blift + (q[c] - bcm[c]) * bgain;
                double *dst = acc[y * w + x];
                if (e > 1 || weight[y * w + x] == 0) memcpy(dst, a, sizeof a);
                else for (int c = 0; c < 3; c++) { double hv = dst[c] / weight[y * w + x]; dst[c] = (hv + a[c]) / 2; }
                weight[y * w + x] = 1.0;
            }
        }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            for (int c = 0; c < 3; c++) {
                double wv = weight[y * w + x] > 1e-6 ? weight[y * w + x] : 1e-6, v = rint(acc[y * w + x][c] / wv);
                at(&out, x, y)[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
    /* the dash: its edges fitted over the rows above the box, extended, colored from its interior */
    int rows[64][3], nr = 0;
    for (int y = y0 - 22; y < y0; y++) {
        int mn = -1, mx = -1;
        for (int x = 280; x < 360; x++) if (yellowp(at(&src, x, y))) { if (mn < 0) mn = x; mx = x; }
        if (mn >= 0) rows[nr][0] = y, rows[nr][1] = mn, rows[nr][2] = mx, nr++;
    }
    nr -= 2; /* (the last two rows are darkened by the legend's border) */
    double fk[2], fmy[2], fmv[2];
    for (int side = 0; side < 2; side++) {
        double sy_ = 0, sv = 0;
        for (int i = 0; i < nr; i++) sy_ += rows[i][0], sv += rows[i][1 + side];
        double my = sy_ / nr, mv = sv / nr;
        PySum num = {0, 0}, den = {0, 0};
        for (int i = 0; i < nr; i++) ps_add(&num, (rows[i][0] - my) * (rows[i][1 + side] - mv));
        for (int i = 0; i < nr; i++) ps_add(&den, sq(rows[i][0] - my));
        fk[side] = ps_val(&num) / ps_val(&den), fmy[side] = my, fmv[side] = mv;
    }
#define FITV(side, y) (fmv[side] + fk[side] * ((y) - fmy[side]))
    int last_y = rows[nr - 1][0], last_l = rows[nr - 1][1], last_r = rows[nr - 1][2];
    double adjl = last_l - FITV(0, last_y), adjr = last_r - FITV(1, last_y);
    int ninter = 0;
    const uint8_t **inter = malloc(sizeof(uint8_t *) * 64 * 80);
    for (int i = 0; i < nr; i++)
        for (int x = rows[i][1] + 1; x < rows[i][2]; x++)
            if (yellowp(at(&src, x, rows[i][0]))) inter[ninter++] = at(&src, x, rows[i][0]);
    int start = last_y + 1;
    for (int y = start; y <= DASH_END; y++) {
        double l = FITV(0, y) + adjl, r = FITV(1, y) + adjr;
        int ri = nr - 1 - (y - start);
        if (ri < 0) ri = 0;
        int sy = rows[ri][0], sl = rows[ri][1], sr = rows[ri][2];
        for (int x = (int)l - 1; x < (int)r + 2; x++) {
            double a = x + 1 - l, b = r + 1 - x;
            if (a < 0.0) a = 0.0;
            if (b < 0.0) b = 0.0;
            double cover = 1.0 < a ? 1.0 : a;
            if (b < cover) cover = b;
            if (y == DASH_END) cover *= 0.5;
            if (cover <= 0) continue;
            double u = (x - l) / (r - l > 1.0 ? r - l : 1.0);
            int qx = (int)rint(sl + u * (sr - sl));
            if (qx < sl) qx = sl;
            if (qx > sr) qx = sr;
            const uint8_t *q = at(&src, qx, sy);
            if (!yellowp(q)) q = inter[(((x * 31 + y * 17) % ninter) + ninter) % ninter];
            uint8_t base[3];
            if (y >= y0) memcpy(base, at(&out, x, y), 3);
            else memcpy(base, !yellowp(at(&src, x, y - 3)) ? at(&src, x, y - 3) : at(&out, x, y), 3);
            for (int c = 0; c < 3; c++) at(&out, x, y)[c] = (uint8_t)rint(q[c] * cover + base[c] * (1 - cover));
        }
    }
    if (pc->stand_in) ps1_unline(&out);
    int ok = save_bmp("carsel.bmp", &out, 0);
    free(inter); free(pool); free(pick); free(keys); free(sources); free(isy); free(above); free(below); free(weight); free(acc);
    img_free(&out);
    img_free(&src);
#undef KNOWN
#undef TARGET
#undef tt
#undef TAKE
#undef FITV
    return ok;
}

/* ---- the Antarctica Santa (tools/art/santa.py; the decomp's docs/SANTA.md, way A): santa.tms and santa.dmd ---- */

static uint32_t rd32(const uint8_t *b, size_t o) { return (uint32_t)b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; }
static void wr32(uint8_t *b, size_t o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); b[o + 2] = (uint8_t)(v >> 16); b[o + 3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *b, size_t o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }

static int santa(Source *pc)
{
    enum { BASE = 0x80019C40 }; /* (the address the model's pointers are built for: relocated at install) */
    size_t dlen = 0, tlen = 0;
    int di = discfs_find(pc->fs, "/LEVELDB/GLACIER.DPC"), ti = discfs_find(pc->fs, "/LEVELDB/GLACIER.TPC");
    uint8_t *dpc = di >= 0 ? discfs_read(pc->fs, di, &dlen) : NULL, *tpc = ti >= 0 ? discfs_read(pc->fs, ti, &tlen) : NULL;
    if (!dpc || !tpc || dlen < 0x03C430) { free(dpc); free(tpc); return fail("Antarctica's PC files are missing"); }
    /* the texture container: "TXSP", version, stamp, count, a key per image, then each image's size and TIM */
    const uint32_t stamp = 0x53414E54;
    size_t cap = 3 * (16 + 12 + 512 + 12 + 64 * 64) + 64, n = 0;
    uint8_t *tms = calloc(cap, 1);
    wr32(tms, 0, 0x50535854), wr32(tms, 4, 0x43), wr32(tms, 8, stamp), wr32(tms, 12, 3);
    wr32(tms, 16, 0x5A71), wr32(tms, 20, 0x5A72), wr32(tms, 24, 0x5A73);
    n = 28;
    static const int TEX[3] = {296, 297, 298};
    for (int k = 0; k < 3; k++) {
        size_t o = 0x10;
        for (int i = 0; i < TEX[k] && o + 4 <= tlen; i++) o += 4 + rd32(tpc, o);
        if (o + 4 > tlen) { free(dpc); free(tpc); free(tms); return fail("GLACIER.TPC is short"); }
        const uint8_t *t = tpc + o + 4;
        uint32_t clut_len = rd32(t, 8);
        size_t p = 8 + clut_len;
        int w16 = t[p + 8] | t[p + 9] << 8, h = t[p + 10] | t[p + 11] << 8, w = w16 * 2;
        size_t pix = (size_t)w16 * 2 * h, size = 8 + 12 + 512 + 12 + pix;
        wr32(tms, n, (uint32_t)size);
        uint8_t *q = tms + n + 4;
        wr32(q, 0, 0x10), wr32(q, 4, 0x09);
        wr32(q, 8, 12 + 512), wr16(q, 12, 0), wr16(q, 14, 0), wr16(q, 16, 256), wr16(q, 18, 1);
        for (int c = 0; c < 256; c++) { /* the PC's 0RRRRRGGGGGBBBBB to the PS1's SBBBBBGGGGGRRRRR */
            uint32_t v = t[8 + 12 + 2 * c] | t[8 + 12 + 2 * c + 1] << 8;
            wr16(q, 20 + 2 * (size_t)c, (v & 0x8000) | ((v & 0x1F) << 10) | (v & 0x3E0) | ((v >> 10) & 0x1F));
        }
        wr32(q, 532, (uint32_t)(12 + pix)), wr16(q, 536, 0), wr16(q, 538, 0), wr16(q, 540, (uint32_t)(w / 2)), wr16(q, 542, (uint32_t)h);
        memcpy(q + 544, t + p + 12, pix);
        n += 4 + size;
    }
    /* the model image (SANTA.md 1.6): nodes copied from the PC file, pointers rebuilt */
    uint8_t img[0x250] = {0};
#define P(off) (BASE + (uint32_t)(off))
    const uint32_t head[8] = {0x50535844, 0x43, stamp, BASE, 3, P(0x18), 1, P(0x20)};
    for (int i = 0; i < 8; i++) wr32(img, (size_t)i * 4, head[i]);
    memcpy(img + 0x20, dpc + 0x03C41C, 0x10); /* select 516 #2 */
    wr32(img, 0x2C, 0);
    wr32(img, 0x30, P(0x3C)), wr32(img, 0x34, P(0x54)), wr32(img, 0x38, P(0x7C));
    img[0x3C] = 1; /* step 0: an empty group */
    memcpy(img + 0x54, dpc + 0x03C2B4, 0x20);
    wr32(img, 0x54 + 0x1C, 0);
    wr32(img, 0x74, P(0xA8)), wr32(img, 0x78, P(0xD4));
    memcpy(img + 0x7C, dpc + 0x03C3F0, 0x20);
    wr32(img, 0x7C + 0x1C, P(0x20)); /* its end: the select node, step 0 */
    wr32(img, 0x9C, P(0x100)), wr32(img, 0xA0, P(0x12C)), wr32(img, 0xA4, P(0x158));
    static const uint32_t MESHES[5] = {0x03C1FC, 0x03C258, 0x03C2DC, 0x03C338, 0x03C394};
    static const int VERTS[5] = {0, 0, 0, 1, 2}, IMAGES[5] = {0, 1, 2, 2, 2};
    for (int k = 0; k < 5; k++) {
        size_t o = 0xA8 + 44 * (size_t)k, q = 0x184 + 36 * (size_t)k;
        memcpy(img + o, dpc + MESHES[k], 44);
        img[o + 1] = 0xFF;
        wr16(img, o + 2, 0);
        wr32(img, o + 4, P(0x238)), wr32(img, o + 8, P(0x238)), wr32(img, o + 12, P(q)), wr32(img, o + 16, 1);
        uint32_t prim = rd32(dpc, MESHES[k] + 0x0C) - BASE;
        if (prim + 12 > dlen) { free(dpc); free(tpc); free(tms); return fail("GLACIER.DPC: a primitive out of the file"); }
        img[q] = 4, img[q + 1] = 1, img[q + 2] = 9, img[q + 3] = 9;
        wr16(img, q + 4, (uint32_t)VERTS[k]);
        memcpy(img + q + 6, dpc + prim + 6, 6); /* half width, half length, extra (s16 each) */
        const uint32_t words[6] = {0x95810000, 0x2C7F7F7F, 0, 0x0080003Fu | (uint32_t)IMAGES[k] << 16, 0x00003F00, 0x00003F3F};
        for (int i = 0; i < 6; i++) wr32(img, q + 12 + 4 * (size_t)i, words[i]);
    }
    static const int Z[3] = {34, 53, 72};
    for (int i = 0; i < 3; i++) wr16(img, 0x238 + 8 * (size_t)i + 4, (uint32_t)Z[i]);
#undef P
    char path[1200];
    int ok = 1;
    for (int f = 0; f < 2 && ok; f++) {
        snprintf(path, sizeof path, "%s/%s", out_dir, f ? "santa.dmd" : "santa.tms");
        FILE *o = fopen(path, "wb");
        ok = o && fwrite(f ? img : tms, 1, f ? sizeof img : n, o) == (f ? sizeof img : n);
        if (o && fclose(o)) ok = 0;
        if (!ok) fail("cannot write %s", path);
    }
    free(dpc); free(tpc); free(tms);
    return ok;
}

/* ---- the Movies menu's Credits picture: the credits' own background (its emblem, text voted away), 1:2 ---- */

enum { CREDITS_W = 240, CREDITS_H = 480, CREDITS_X = 200 }; /* (x: the 640-wide screen's slice, the emblem's middle) */

/* 240x480: the PC's slice as it is, the PS1's (320x240) doubled */
static Img credits_picture(Source *s)
{
    Img plate = credits_plate(s), m = {0, 0, NULL};
    if (!plate.p) return m;
    int f = CREDITS_H / plate.h;
    if (f < 1) f = 1;
    m = img_new(CREDITS_W, CREDITS_H);
    for (int y = 0; y < CREDITS_H; y++)
        for (int x = 0; x < CREDITS_W; x++) {
            int sx = (CREDITS_X + x) / f, sy = y / f;
            if (sx >= plate.w) sx = plate.w - 1;
            if (sy >= plate.h) sy = plate.h - 1;
            memcpy(at(&m, x, y), at(&plate, sx, sy), 4);
        }
    /* the red title's last specks (the vote kept a few): the average of the gray round them */
#define REDDISH(p) ((p)[0] > (p)[1] + 40 && (p)[0] > (p)[2] + 40)
    for (int y = 0; y < CREDITS_H; y++)
        for (int x = 0; x < CREDITS_W; x++) {
            if (!REDDISH(at(&m, x, y))) continue;
            int sum[3] = {0, 0, 0}, n = 0;
            for (int yy = y - 4; yy <= y + 4; yy++)
                for (int xx = x - 4; xx <= x + 4; xx++) {
                    if (xx < 0 || yy < 0 || xx >= CREDITS_W || yy >= CREDITS_H || REDDISH(at(&m, xx, yy))) continue;
                    for (int c = 0; c < 3; c++) sum[c] += at(&m, xx, yy)[c];
                    n++;
                }
            for (int c = 0; c < 3 && n; c++) at(&m, x, y)[c] = (uint8_t)(sum[c] / n);
        }
#undef REDDISH
    img_free(&plate);
    return m;
}

static int credits(Source *s)
{
    Img m = credits_picture(s);
    if (!m.p) return 0;
    int ok = save_bmp("credits_portrait.bmp", &m, 0);
    img_free(&m);
    return ok;
}

/* ---- the profile portraits from the movies (tools/art/movie_portraits.py): stills cropped 120x240 ---- */

enum { MP_W = 120, MP_H = 240 };

/* each movie's frames in order, decoded as the game's player does (src/runtime/movie.c), the picks' crops taken */
static int movie_crops(Source *ps1, const char *movie, Img *crops, int npicks)
{
    char path[64];
    snprintf(path, sizeof path, "/MOVIES/%s.STR", movie);
    int fi = discfs_find(ps1->fs, path), last = -1;
    for (int k = 0; k < npicks; k++)
        if (!strcmp(MOVIE_PICKS[k].movie, movie) && MOVIE_PICKS[k].frame > last) last = MOVIE_PICKS[k].frame;
    if (fi < 0) return fail("%s: missing", path);
    uint32_t sectors = (discfs_size(ps1->fs, fi) + 2047) / 2048, cur = 0xFFFFFFFFu, have = 0, need = 0;
    uint8_t raw[2352], *frame = malloc(2016 * 64), *rgb = malloc(640 * 480 * 3);
    int index = 0, ok = 1;
    for (uint32_t n = 0; n < sectors && index <= last && ok; n++) {
        if (!discfs_raw(ps1->fs, fi, n, raw)) { ok = fail("%s: unreadable (a raw .bin is needed)", path); break; }
        const uint8_t *sub = raw + 16, *data = raw + 24;
        if (sub[2] & 0x04) continue; /* (audio) */
        if (data[0] != 0x60 || data[1] != 0x01) continue;
        uint32_t secno = data[4] | data[5] << 8, count = data[6] | data[7] << 8;
        uint32_t fno = data[8] | data[9] << 8 | data[10] << 16 | (uint32_t)data[11] << 24;
        uint32_t w = data[16] | data[17] << 8, h = data[18] | data[19] << 8;
        if (fno != cur) { cur = fno; have = 0; need = count; }
        if (secno >= 64 || need > 64) continue;
        memcpy(frame + secno * 2016, data + 32, 2016);
        if (++have < need) continue;
        int k0 = index++;
        int wanted = 0;
        for (int k = 0; k < npicks && !wanted; k++) wanted = !strcmp(MOVIE_PICKS[k].movie, movie) && MOVIE_PICKS[k].frame == k0;
        if (!wanted) continue;
        if (w > 640 || h > 480 || w < MP_W || h < MP_H || !mdec_decode_frame(frame, need * 2016, (int)w, (int)h, rgb)) {
            ok = fail("%s: frame %d does not decode", path, k0);
            break;
        }
        for (int k = 0; k < npicks; k++) {
            if (strcmp(MOVIE_PICKS[k].movie, movie) || MOVIE_PICKS[k].frame != k0) continue;
            int x0 = MOVIE_PICKS[k].x;
            if (x0 > (int)w - MP_W) x0 = (int)w - MP_W;
            if (x0 < 0) x0 = 0;
            double g = MOVIE_PICKS[k].gain; /* (brighter, for the darkest shots) */
            crops[k] = img_new(MP_W, MP_H);
            for (int y = 0; y < MP_H; y++)
                for (int x = 0; x < MP_W; x++) {
                    const uint8_t *q = rgb + ((size_t)y * w + x0 + x) * 3;
                    uint8_t *o = at(&crops[k], x, y);
                    for (int c = 0; c < 3; c++) {
                        int v = (int)(q[c] * g + 0.5);
                        o[c] = (uint8_t)(v > 255 ? 255 : v);
                    }
                    o[3] = 255;
                }
        }
    }
    free(frame);
    free(rgb);
    return ok;
}

static Source *credits_source; /* (the PC's, else the PS1's: the image picks' picture) */

static int movie_portraits(Source *ps1)
{
    enum { N = (int)(sizeof MOVIE_PICKS / sizeof MOVIE_PICKS[0]) };
    static Img crops[N];
    int ok = 1;
    memset(crops, 0, sizeof crops);
    for (int k = 0; k < N && ok; k++) {
        if (MOVIE_PICKS[k].frame >= 0) {
            int first = 1; /* (each movie once, at its first pick) */
            for (int j = 0; j < k && first; j++) first = MOVIE_PICKS[j].frame < 0 || strcmp(MOVIE_PICKS[j].movie, MOVIE_PICKS[k].movie);
            if (first) ok = movie_crops(ps1, MOVIE_PICKS[k].movie, crops, N);
        } else { /* not a still: the Credits picture, each pixel the average of four */
            Img big = credits_picture(credits_source);
            crops[k] = img_new(MP_W, MP_H);
            for (int y = 0; y < MP_H && big.p; y++)
                for (int x = 0; x < MP_W; x++) {
                    uint8_t *o = at(&crops[k], x, y);
                    for (int c = 0; c < 3; c++)
                        o[c] = (uint8_t)((at(&big, 2 * x, 2 * y)[c] + at(&big, 2 * x + 1, 2 * y)[c] + at(&big, 2 * x, 2 * y + 1)[c] +
                                          at(&big, 2 * x + 1, 2 * y + 1)[c]) / 4);
                    o[3] = 255;
                }
            img_free(&big);
        }
    }
    for (int k = 0; k < N && ok; k++)
        if (!crops[k].p) ok = fail("movie portrait %s: frame %d not found", MOVIE_PICKS[k].id, MOVIE_PICKS[k].frame);
    if (ok) {
        Img m = strip(crops, N);
        ok = save_bmp("movie_portraits.bmp", &m, 0);
        img_free(&m);
    }
    if (ok) {
        char *text = malloc((size_t)N * 96 + 64);
        size_t at_ = (size_t)sprintf(text, "# made by the first-time setup\n");
        for (int k = 0; k < N; k++) at_ += (size_t)sprintf(text + at_, "%s %s %s\n", MOVIE_PICKS[k].id, MOVIE_PICKS[k].who, MOVIE_PICKS[k].movie);
        ok = save_text("movie_portraits.txt", text);
        free(text);
    }
    for (int k = 0; k < N; k++) img_free(&crops[k]);
    return ok;
}

/* ---- the PC version's sharper battle textures (docs/FIRST_RUN_SETUP.md; tools/art/hd_picks.txt): hd_arena.bin ---- */

uint32_t hd_crc32(const uint8_t *p, size_t n) /* (vplus/hd_textures.c finds the PS1 textures by it) */
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static Source *hd_ps1; /* (the PS1 disc, beside the PC version's) */

/* "TM2H", version, count; each: the PS1 texture's CRC-32 (its TIM's bytes), the PC picture's width and height
 * (16 bits each), its RGBA; a see-through pixel's color taken from its neighbors (the filter's edges not dark) */
static int hd_textures(Source *pc)
{
    if (pc->stand_in) return 1; /* (PS1 only: the PS1's textures, as they are) */
    enum { N = (int)(sizeof HD_PICKS / sizeof HD_PICKS[0]) };
    char path[1200];
    snprintf(path, sizeof path, "%s/hd_arena.bin", out_dir);
    FILE *f = fopen(path, "wb");
    if (!f) return fail("cannot write %s", path);
    uint32_t head[3] = {0x48324D54u, 1, 0}; /* "TM2H" */
    fwrite(head, 4, 3, f);
    int n = 0, ok = 1;
    for (int k = 0; k < N && ok; k++) {
        if (HD_PICKS[k].i >= pack(hd_ps1, HD_PICKS[k].ps1)) { ok = fail("%s: no image %d", HD_PICKS[k].ps1, HD_PICKS[k].i); break; }
        const Tim *t = &hd_ps1->tims[HD_PICKS[k].i];
        uint32_t crc = hd_crc32(hd_ps1->data + t->off, t->bytes);
        Img m = image(pc, HD_PICKS[k].pc, HD_PICKS[k].j);
        if (!m.p || m.w != 2 * t->w || m.h != 2 * t->h || m.w > 256 || m.h > 256) {
            img_free(&m);
            ok = fail("%s %d: not the PS1's twice", HD_PICKS[k].pc, HD_PICKS[k].j);
            break;
        }
        for (int pass = 0; pass < 4; pass++) { /* (see-through pixels: their seen neighbors' average color) */
            Img next = img_new(m.w, m.h);
            memcpy(next.p, m.p, (size_t)m.w * m.h * 4);
            for (int y = 0; y < m.h; y++)
                for (int x = 0; x < m.w; x++) {
                    if (at(&m, x, y)[3]) continue;
                    int s[3] = {0, 0, 0}, c = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= m.w || yy >= m.h || !at(&m, xx, yy)[3]) continue;
                            for (int q = 0; q < 3; q++) s[q] += at(&m, xx, yy)[q];
                            c++;
                        }
                    if (c) {
                        for (int q = 0; q < 3; q++) at(&next, x, y)[q] = (uint8_t)(s[q] / c);
                        at(&next, x, y)[3] = 1; /* (filled: for the next pass; 0 again below) */
                    }
                }
            img_free(&m);
            m = next;
        }
        for (int i = 0; i < m.w * m.h; i++) if (m.p[i * 4 + 3] == 1) m.p[i * 4 + 3] = 0;
        uint16_t wh[2] = {(uint16_t)m.w, (uint16_t)m.h};
        ok = fwrite(&crc, 4, 1, f) == 1 && fwrite(wh, 2, 2, f) == 2 && fwrite(m.p, 4, (size_t)m.w * m.h, f) == (size_t)m.w * m.h;
        img_free(&m);
        n++;
    }
    head[2] = (uint32_t)n;
    if (ok) { fseek(f, 0, SEEK_SET); fwrite(head, 4, 3, f); }
    if (fclose(f)) ok = 0;
    return ok ? 1 : (error_text[0] ? 0 : fail("cannot write %s", path));
}

/* ---- the whole ---- */

static void make_dir(const char *p)
{
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
}

int ui_build(const char *ps1_path, const char *pc_path, const char *out, UiBuildProgress progress, void *ctx)
{
    error_text[0] = 0;
    snprintf(out_dir, sizeof out_dir, "%s", out);
    make_dir(out_dir);
    static Source ps1, pc;
    memset(&ps1, 0, sizeof ps1);
    memset(&pc, 0, sizeof pc);
    if (!(ps1.fs = discfs_open(ps1_path))) return fail("cannot read the PS1 disc image %s", ps1_path);
    if (discfs_find(ps1.fs, "/SCUS_943.06") < 0) { discfs_close(ps1.fs); return fail("%s is not the Twisted Metal 2 PlayStation disc (USA)", ps1_path); }
    if (pc_path && !(pc.fs = discfs_open(pc_path))) { discfs_close(ps1.fs); return fail("cannot read the PC version at %s", pc_path); }
    if (pc.fs && (discfs_find(pc.fs, "/SHELLDB1/TITLE.TPC") < 0 || discfs_find(pc.fs, "/LEVELDB/GLACIER.TPC") < 0)) {
        discfs_close(ps1.fs);
        discfs_close(pc.fs);
        return fail("%s is not the Twisted Metal 2 PC version (its disc or installed folder)", pc_path);
    }
    pc.pc = 1;
    static Source stand;
    memset(&stand, 0, sizeof stand);
    stand.fs = ps1.fs;
    stand.stand_in = 1;
    credits_source = pc.fs ? &pc : &ps1;
    hd_ps1 = &ps1;
    struct { const char *what; int (*fn)(Source *); Source *src; } steps[] = {
        {"menus", simple, &pc}, {"the menus' backgrounds", menus, &pc}, {"the network screen", network, &pc}, {"Vanilla Plus car info", carinfo_vp, &pc}, {"the co-op duel screen", duel, &pc}, {"car select", carsel, &pc}, {"the Antarctica Santa", santa, &pc}, {"Sweet Tooth's hair", hair, &pc}, {"portraits and car info", portraits, &pc},
        {"arenas", arenas, &pc}, {"weapon icons", icons, &pc},
        {"the sharper battle textures", hd_textures, &pc}, {"the Credits picture", credits, credits_source}, {"the movie portraits", movie_portraits, &ps1},
        {"opponent plates", plates, &ps1}, {"the highlight bar", bar, &ps1}, {"the game's font", game_font, &ps1},
    };
    int n = (int)(sizeof steps / sizeof steps[0]), ok = 1;
    for (int i = 0; i < n && ok; i++) {
        Source *src = steps[i].src == &pc && !pc.fs ? &stand : steps[i].src;
        if (progress) progress(i * 100 / n, steps[i].what, ctx);
        ok = steps[i].fn(src);
        if (!ok && src == &stand) { /* (PS1 only: the pictures it has no twin for are left out) */
            fprintf(stderr, "ui_build: PS1 only, %s left out (%s)\n", steps[i].what, error_text);
            ok = 1;
        }
    }
    for (int i = 0; i < ps1.n; i++) tim_free(&ps1.tims[i]);
    for (int i = 0; i < pc.n; i++) tim_free(&pc.tims[i]);
    for (int i = 0; i < stand.n; i++) tim_free(&stand.tims[i]);
    free(stand.data);
    free(ps1.data);
    free(pc.data);
    discfs_close(ps1.fs);
    discfs_close(pc.fs);
    if (ok) { /* what made them: a newer builder makes them again (uibuild.h) */
        char stamp[64];
        snprintf(stamp, sizeof stamp, "version %d\npc %d\n", UI_BUILD_VERSION, pc_path != NULL);
        ok = save_text("setup.txt", stamp);
    }
    if (ok && progress) progress(100, "done", ctx);
    return ok;
}
