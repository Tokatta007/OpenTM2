/* Movie player: the host version of TM2's moviecdPlayMovie (801384C4).
 *
 * moviecdPlayMovie(name, skip, loop, rgb24, end_frame, x0, y0, x1, y1, w, h,
 *                  DISPENV disp[2], int *shown)
 * streams an STR file with libpress and the MDEC into two VRAM buffers,
 * (x0,y0) and (x1,y1), flipping the display between them; it calls
 * skip(frame, end_frame, shown) after each frame and stops when that returns
 * nonzero (the return value) or at end_frame; with loop set it starts over.
 *
 * Here the file is read from the disc image: video sectors are joined into
 * frames and decoded by mdec.c, the frame is loaded into VRAM through the
 * GPU port and shown with the game's own PutDispEnv and SetDispMask; XA
 * audio sectors are decoded and streamed to the SPU's CD input (cd_audio.c).
 * Emulated time advances as the drive would deliver the sectors (2x speed,
 * 150 sectors a second, 15 frames a second for TM2's movies), so the game's
 * VBlank work (sound, pads for the skip check) runs as on hardware. */
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "mdec.h"

#define PUT_DISP_ENV GA(0x80141BA0u)
#define SET_DISP_MASK GA(0x80141430u)
#define SS_SET_SERIAL_ATTR GA(0x80145EF8u)
#define SS_SET_SERIAL_VOL GA(0x80145F8Cu)
#define SECTORS_PER_SECOND 150.0

void cd_audio_xa_begin(void);       /* cd_audio.c */
void cd_audio_xa_end(void);
void cd_audio_xa_push(const short *stereo, int frames);
int cd_audio_stop(void);

/* ---- XA ADPCM (4-bit, 18 sound groups of 128 bytes per sector) ---- */

static const int xa_k0[4] = { 0, 60, 115, 98 }, xa_k1[4] = { 0, 0, -52, -55 };

static void xa_decode_sector(const uint8_t *data, int stereo, int hist[2][2], short *out, int *frames)
{
    int n = 0;
    for (int g = 0; g < 18; g++) {
        const uint8_t *grp = data + g * 128;
        for (int unit = 0; unit < 8; unit++) {
            int ch = stereo ? unit & 1 : 0;
            uint8_t param = grp[4 + unit];
            int shift = param & 0x0F, filter = (param >> 4) & 3;
            if (shift > 12) shift = 9;
            short *o = out + (stereo ? (n + (unit >> 1) * 28) * 2 + ch : n + unit * 28);
            for (int s = 0; s < 28; s++) {
                int nib = (grp[16 + s * 4 + (unit >> 1)] >> ((unit & 1) * 4)) & 0x0F;
                int v = (int16_t)(nib << 12) >> shift;
                v += (hist[ch][0] * xa_k0[filter] + hist[ch][1] * xa_k1[filter] + 32) >> 6;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                hist[ch][1] = hist[ch][0];
                hist[ch][0] = v;
                o[stereo ? s * 2 : s] = (short)v;
            }
        }
        n += stereo ? 4 * 28 : 8 * 28;
    }
    *frames = n;
}

/* ---- video ---- */

static void upload(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *pix)
{
    gpu_write_gp0(0xA0000000u);
    gpu_write_gp0(y << 16 | x);
    gpu_write_gp0(h << 16 | w);
    for (uint32_t i = 0; i < w * h; i += 2)
        gpu_write_gp0((uint32_t)pix[i] | (i + 1 < w * h ? (uint32_t)pix[i + 1] << 16 : 0));
}

static void read_name(CPU *c, uint32_t a, char *out, int size)
{
    int i = 0;
    for (; i < size - 1 && (out[i] = (char)mem_r8(c, a + (uint32_t)i)); i++) ;
    out[i] = 0;
}

void hle_moviecdPlayMovie(CPU *c)
{
    uint32_t sp = c->r[29];
    uint32_t skip_cb = c->r[5], loop = c->r[6] & 0xFF, rgb24 = c->r[7] & 0xFF;
    uint32_t end_frame = mem_r32(c, sp + 16);
    uint32_t bx[2] = { mem_r32(c, sp + 20), mem_r32(c, sp + 28) };
    uint32_t by[2] = { mem_r32(c, sp + 24), mem_r32(c, sp + 32) };
    uint32_t disp = mem_r32(c, sp + 44), shown_ptr = mem_r32(c, sp + 48);
    char name[64];
    read_name(c, c->r[4], name, sizeof name);

    uint32_t lba, size;
    if (!cd_find_file(name, &lba, &size)) {
        if (M.trace) fprintf(stderr, "movie %s: not found\n", name);
        c->r[2] = 0;
        return;
    }
    uint32_t sectors = (size + 2047) / 2048;
    void ui_movie_start(void); /* ui/options.c: a port page's held picture let go (the movie is the picture now) */
    ui_movie_start();
    if (M.trace) fprintf(stderr, "movie %s: %u sectors, to frame %u%s\n", name, sectors, end_frame, loop ? ", looping" : "");

    void cd_audio_fade_stop(void); /* cd_audio.c */
    cd_audio_fade_stop(); /* (faded: a sudden stop popped) */
    cd_audio_xa_begin();
    guest_call(SS_SET_SERIAL_ATTR, 0, 0, 1, 0);
    guest_call(SS_SET_SERIAL_VOL, 0, 0x7F, 0x7F, 0);

    uint8_t *frame = malloc(64 * 2016), *rgb = malloc(640 * 480 * 3);
    uint16_t *pix = malloc(640 * 480 * 2);
    short *pcm = malloc(18 * 8 * 28 * 2 * sizeof(short)); /* room for mono expanded to stereo */
    uint8_t raw[2352];
    uint32_t result = 0, buffer = 0;
    int done = 0;

    while (!done) {
        int hist[2][2] = { { 0, 0 }, { 0, 0 } };
        uint32_t cur = (uint32_t)-1, have = 0, need = 0, last_frame = 0;
        uint64_t start = M.vblanks;
        int finished = 0;
        for (uint32_t s = 0; s < sectors && !finished && !done; s++) {
            if (!cd_read_raw(lba + s, raw))
                break;
            const uint8_t *sub = raw + 16, *data = raw + 24;
            if (sub[2] & 0x04) { /* audio */
                int frames;
                int stereo = sub[3] & 1;
                xa_decode_sector(data, stereo, hist, pcm, &frames);
                if (!stereo) /* mono: both sides */
                    for (int i = frames - 1; i >= 0; i--)
                        pcm[2 * i] = pcm[2 * i + 1] = pcm[i];
                cd_audio_xa_push(pcm, frames);
                continue;
            }
            if (data[0] != 0x60 || data[1] != 0x01)
                continue;
            uint32_t secno = data[4] | data[5] << 8, count = data[6] | data[7] << 8;
            uint32_t fno = data[8] | data[9] << 8 | data[10] << 16 | (uint32_t)data[11] << 24;
            uint32_t w = data[16] | data[17] << 8, h = data[18] | data[19] << 8;
            if (fno != cur) { cur = fno; have = 0; need = count; }
            if (secno >= 64 || count > 64)
                continue;
            memcpy(frame + secno * 2016, data + 32, 2016);
            if (++have < need)
                continue;

            /* a whole frame: decode, wait for its time, show it */
            if (w > 640 || h > 480 || !mdec_decode_frame(frame, need * 2016, (int)w, (int)h, rgb)) {
                if (M.trace) fprintf(stderr, "movie %s: frame %u does not decode\n", name, fno);
                continue;
            }
            uint32_t vw = rgb24 ? w * 3 / 2 : w;
            if (rgb24) {
                memcpy(pix, rgb, (size_t)w * h * 3);
            } else {
                for (uint32_t i = 0; i < w * h; i++)
                    pix[i] = (uint16_t)(rgb[i * 3] >> 3 | (rgb[i * 3 + 1] >> 3) << 5 | (rgb[i * 3 + 2] >> 3) << 10);
            }
            upload(bx[buffer], by[buffer], vw, h, pix);
            uint64_t due = start + (uint64_t)((s + 1) * NTSC_VBLANK_HZ / SECTORS_PER_SECOND);
            do rt_vblank(); while (M.vblanks < due);
            mem_w32(c, shown_ptr, buffer);
            uint32_t env = disp + buffer * 0x14;
            if (rgb24)
                mem_w8(c, env + 0x11, 1);
            guest_call(PUT_DISP_ENV, env, 0, 0, 0);
            guest_call(SET_DISP_MASK, 1, 0, 0, 0);
            last_frame = fno;
            if (fno >= end_frame) {
                finished = 1;
            } else if (skip_cb) {
                result = guest_call(skip_cb, fno, end_frame, buffer, 0);
                if (result) done = 1;
            }
            buffer ^= 1;
        }
        if (M.trace) fprintf(stderr, "movie %s: %s at frame %u\n", name, done ? "skipped" : "ended", last_frame);
        if (!loop)
            done = 1;
    }

    free(frame); free(rgb); free(pix); free(pcm);
    guest_call(SS_SET_SERIAL_VOL, 0, 0, 0, 0);
    cd_audio_xa_end();
    c->r[2] = result;
}
