/* The PC version's sharper battle textures (docs/FIRST_RUN_SETUP.md): ui/hd_arena.bin (the first-time setup's, from
 * the player's PC version: tools/art/hd_picks.txt) holds, for each chosen PS1 arena texture, its CRC-32 and the PC's
 * picture of it at twice the size. When the arena's pack is parsed (bitmapParseContainer(0x80019C40, 0)), each of its
 * TIMs found there is drawn with the PC picture's colors (Psyz_VideoSetTextureColorsHD, PSY-Z patch 19): the texel is
 * still the PS1's (see-through, semi-transparency, edges), only its color sharper.
 *
 * Nothing is taken on trust: a texture is replaced only once VRAM holds its pixels and its palette where its TIM says,
 * and when anything writes over either later (an animation, a scroll, another screen's pictures, a network state),
 * they are checked again: changed, the PS1's texture is drawn as it is. Settings: Video, Textures (settings_video.
 * textures: 0 the sharpest, 1 the PlayStation's only). Display only: the game is the same on every machine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psyz/video.h>
#include "runtime/runtime.h"
#include "runtime/gpu_backend.h"
#include "settings.h"

uint32_t hd_crc32(const uint8_t *p, size_t n); /* assets/uibuild.c */
const char *ui_folder(void);

enum { FIRST_SLOT = 16, SLOTS = 192, MAX_PICS = 512 };

typedef struct { uint32_t crc; int w, h; uint8_t *rgba; } Pic;
static Pic pics[MAX_PICS];
static int npics = -1; /* (-1: the file not read yet) */

typedef struct {
    int on;                      /* registered (and its VRAM as it was) */
    int x, y, w16, h;            /* its pixels in VRAM (16-bit units) */
    int cx, cy, cw, ch;          /* its palette (cw 0: none) */
    uint32_t pix_crc, clut_crc;  /* their VRAM contents */
    int page_x, page_y, clut, u0, v0, u1, v1;
    const Pic *pic;
} Slot;
static Slot slots[SLOTS];
static int nslots, shown; /* (shown: the renderer has them) */

static void load(void)
{
    npics = 0;
    char path[1200];
    snprintf(path, sizeof path, "%sui/hd_arena.bin", ui_folder());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint32_t head[3];
    if (fread(head, 4, 3, f) == 3 && head[0] == 0x48324D54u && head[1] == 1)
        for (uint32_t i = 0; i < head[2] && npics < MAX_PICS; i++) {
            Pic *p = &pics[npics];
            uint16_t wh[2];
            if (fread(&p->crc, 4, 1, f) != 1 || fread(wh, 2, 2, f) != 2) break;
            p->w = wh[0], p->h = wh[1];
            p->rgba = malloc((size_t)p->w * p->h * 4);
            if (!p->rgba || fread(p->rgba, 4, (size_t)p->w * p->h, f) != (size_t)p->w * p->h) { free(p->rgba); break; }
            npics++;
        }
    fclose(f);
    if (npics) fprintf(stderr, "hd textures: %d from the PC version\n", npics);
}

static uint32_t vram_crc(int x, int y, int w, int h)
{
    static uint16_t buf[256 * 256];
    if (!gpu_backend || !gpu_backend->download || w <= 0 || h <= 0 || w * h > 256 * 256) return 0;
    gpu_backend->download(x, y, w, h, buf);
    return hd_crc32((const uint8_t *)buf, (size_t)w * h * 2);
}

static void show(int on) /* the slots to the renderer, or away */
{
    for (int i = 0; i < nslots; i++) {
        const Slot *s = &slots[i];
        if (on && s->on)
            Psyz_VideoSetTextureColorsHD(FIRST_SLOT + i, s->page_x, s->page_y, s->clut, s->u0, s->v0, s->u1, s->v1,
                                         s->pic->rgba, s->pic->w, s->pic->h);
        else
            Psyz_VideoClearTextureHD(FIRST_SLOT + i);
    }
    shown = on;
}

static void clear_all(void)
{
    if (shown) show(0);
    nslots = 0;
}

/* each TIM of the arena's pack (TXSP: magic, version, result, count, keys, then each entry's size and TIM) */
void hdtex_arena_parsed(uint32_t buf)
{
    if (npics < 0) load();
    clear_all();
    if (!npics || mem_r32(&M.cpu, buf) != 0x50535854u) return;
    uint32_t count = mem_r32(&M.cpu, buf + 12), at = buf + 16 + 4 * count;
    static uint8_t tim[0x30000];
    for (uint32_t k = 0; k < count && k < 4096; k++) {
        uint32_t size = mem_r32(&M.cpu, at), t = at + 4;
        at = t + (size & ~3u);
        if (mem_r32(&M.cpu, t) != 0x10) continue;
        uint32_t flags = mem_r32(&M.cpu, t + 4), p = t + 8, clut_len = 0;
        int bpp = (flags & 3) == 0 ? 4 : (flags & 3) == 1 ? 8 : 16, cx = 0, cy = 0, cw = 0, ch = 0;
        if (flags & 8) {
            clut_len = mem_r32(&M.cpu, p);
            cx = mem_r16(&M.cpu, p + 4), cy = mem_r16(&M.cpu, p + 6), cw = mem_r16(&M.cpu, p + 8), ch = mem_r16(&M.cpu, p + 10);
            p += clut_len;
        }
        uint32_t pix_len = mem_r32(&M.cpu, p);
        int x = mem_r16(&M.cpu, p + 4), y = mem_r16(&M.cpu, p + 6), w16 = mem_r16(&M.cpu, p + 8), h = mem_r16(&M.cpu, p + 10);
        uint32_t need = 12 + (uint32_t)w16 * (uint32_t)h * 2, bytes = 8 + clut_len + (pix_len > need ? pix_len : need);
        if (bytes > sizeof tim || w16 <= 0 || h <= 0) continue;
        for (uint32_t i = 0; i < bytes; i++) tim[i] = (uint8_t)mem_r8(&M.cpu, t + i);
        uint32_t crc = hd_crc32(tim, bytes);
        const Pic *pic = NULL;
        for (int i = 0; i < npics && !pic; i++) if (pics[i].crc == crc) pic = &pics[i];
        if (!pic || nslots == SLOTS || bpp == 16) continue;
        int tpw = 16 / bpp, page_x = x & ~63, page_y = y & ~255, u0 = (x - page_x) * tpw, v0 = y - page_y;
        if (u0 + w16 * tpw > 256 || v0 + h > 256 || pic->w != 2 * w16 * tpw || pic->h != 2 * h) continue;
        /* (VRAM as the TIM says: its pixels, its palette) */
        uint32_t pix_crc = hd_crc32(tim + 8 + clut_len + 12, (size_t)w16 * h * 2);
        uint32_t clut_crc = cw ? hd_crc32(tim + 8 + 12, (size_t)cw * ch * 2) : 0;
        if (vram_crc(x, y, w16, h) != pix_crc || (cw && vram_crc(cx, cy, cw, ch) != clut_crc)) {
            if (getenv("TM2PC_HD_DEBUG")) fprintf(stderr, "hd textures: a texture at %d,%d not in VRAM as its TIM: kept\n", x, y);
            continue;
        }
        Slot *s = &slots[nslots++];
        *s = (Slot){1, x, y, w16, h, cx, cy, cw, ch, pix_crc, clut_crc, page_x, page_y, cy * 64 + cx / 16, u0, v0,
                    u0 + w16 * tpw, v0 + h, pic};
    }
    if (nslots && settings_video.textures == 0) show(1);
    if (getenv("TM2PC_HD_DEBUG")) fprintf(stderr, "hd textures: %d of the arena's sharper\n", nslots);
}

static int overlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* (gpu.c, net: VRAM written) a texture changed there: the PS1's from now on */
void hdtex_vram_written(int x, int y, int w, int h)
{
    for (int i = 0; i < nslots; i++) {
        Slot *s = &slots[i];
        if (!s->on) continue;
        int hit = overlaps(x, y, w, h, s->x, s->y, s->w16, s->h), hitc = s->cw && overlaps(x, y, w, h, s->cx, s->cy, s->cw, s->ch);
        if (!hit && !hitc) continue;
        if ((hit && vram_crc(s->x, s->y, s->w16, s->h) != s->pix_crc) || (hitc && vram_crc(s->cx, s->cy, s->cw, s->ch) != s->clut_crc)) {
            s->on = 0;
            if (shown) Psyz_VideoClearTextureHD(FIRST_SLOT + i);
            if (getenv("TM2PC_HD_DEBUG")) fprintf(stderr, "hd textures: the one at %d,%d written over: the PS1's\n", s->x, s->y);
        }
    }
}

/* (Options, Video, Textures) */
void hdtex_setting_changed(void)
{
    show(settings_video.textures == 0);
}

int hdtex_available(void) /* the PC version's pictures there */
{
    if (npics < 0) load();
    return npics > 0;
}
