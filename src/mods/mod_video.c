/* A mod character's ending video (docs/MODDING.md 6: ending.mpg): MPEG-1 video with MP2 sound in an MPEG program
 * stream, the format modders make with ffmpeg (assets/mods/characters/_TEMPLATE/GUIDE.txt), decoded with pl_mpeg
 * (third_party/pl_mpeg, MIT). Its pictures go to the screen as the port's own pages do (an RGB picture over the
 * game's display), its sound into the SPU's CD input as a movie's XA does (cd_audio_xa_*), at the file's rate.
 *
 * The file comes from a mod (perhaps downloaded from a server), so it is checked before it is played
 * (mod_video_check: the same checks as tm2pc --check-mod): MPEG-1 video (not MPEG-2), at most 854 x 480, at most
 * 2 minutes, its sound (if any) MP2. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
#include <psyz/video.h>
#include "runtime/runtime.h"
#include "mods.h"

enum { VIDEO_W = 854, VIDEO_H = 480, VIDEO_SECONDS = 120 };

/* MPEG-2 video has a sequence extension (start code 000001B5) after its sequence header: MPEG-1 has none */
static int mpeg2_video(FILE *f)
{
    unsigned char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof buf, f);
    for (size_t i = 0; i + 4 <= n; i++)
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 && buf[i + 3] == 0xB5) return 1;
    return 0;
}

int mod_video_check(const char *path, char *why, int why_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, (size_t)why_size, "cannot be read"); return 0; }
    int mpeg2 = mpeg2_video(f);
    fclose(f);
    if (mpeg2) { snprintf(why, (size_t)why_size, "is MPEG-2 video: it must be MPEG-1 (GUIDE.txt, The ending)"); return 0; }
    plm_t *plm = plm_create_with_filename(path);
    if (!plm || !plm_has_headers(plm) || !plm_get_num_video_streams(plm)) {
        int sound = plm && plm_get_num_video_streams(plm) && plm_get_num_audio_streams(plm); /* (MP3 sound: no header) */
        snprintf(why, (size_t)why_size, sound ? "has sound that is not MP2 (GUIDE.txt, The ending)"
                                              : "is not an MPEG-1 video (GUIDE.txt, The ending)");
        if (plm) plm_destroy(plm);
        return 0;
    }
    int ok = 1, w = plm_get_width(plm), h = plm_get_height(plm);
    double seconds = plm_get_duration(plm);
    if (w <= 0 || h <= 0 || w > VIDEO_W || h > VIDEO_H) {
        snprintf(why, (size_t)why_size, "is %d x %d: %d x %d at most", w, h, VIDEO_W, VIDEO_H);
        ok = 0;
    } else if (seconds > VIDEO_SECONDS) {
        snprintf(why, (size_t)why_size, "is %.0f seconds: %d at most", seconds, VIDEO_SECONDS);
        ok = 0;
    } else if (!plm_decode_video(plm)) {
        snprintf(why, (size_t)why_size, "has no picture that decodes");
        ok = 0;
    } else if (plm_get_num_audio_streams(plm)) {
        plm_rewind(plm);
        if (!plm_decode_audio(plm)) {
            snprintf(why, (size_t)why_size, "has sound that is not MP2 (GUIDE.txt, The ending)");
            ok = 0;
        }
    }
    plm_destroy(plm);
    return ok;
}

typedef struct { unsigned char *rgb; int w, h, shown; uint64_t frames; } Picture;
void cd_audio_xa_begin(void), cd_audio_xa_end(void), cd_audio_xa_rate(int rate); /* runtime/cd_audio.c */
void cd_audio_xa_push(const short *stereo, int frames);
static void on_video(plm_t *plm, plm_frame_t *frame, void *user)
{
    (void)plm;
    Picture *p = user;
    plm_frame_to_rgb(frame, p->rgb, p->w * 3);
    p->shown = 1;
    p->frames++;
}
static void on_audio(plm_t *plm, plm_samples_t *s, void *user)
{
    (void)plm;
    (void)user;
    short pcm[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
    for (unsigned i = 0; i < s->count * 2; i++) {
        float v = s->interleaved[i] * 32767.0f;
        pcm[i] = (short)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    cd_audio_xa_push(pcm, (int)s->count);
}

/* Plays the video, a frame each VBlank as its time comes; skip(): 1 to stop (asked each VBlank). Tests:
 * TM2PC_VIDEO_SHOT=<frame>,<file.bmp> saves that frame's picture. Returns 1 if it played. */
int mod_video_play(const char *path, int (*skip)(void))
{
    char why[120];
    if (!mod_video_check(path, why, sizeof why)) {
        fprintf(stderr, "ending video %s %s: not played\n", path, why);
        return 0;
    }
    plm_t *plm = plm_create_with_filename(path);
    if (!plm) return 0;
    Picture p = {NULL, plm_get_width(plm), plm_get_height(plm), 0, 0};
    p.rgb = calloc((size_t)p.w * p.h, 3);
    if (!p.rgb) { plm_destroy(plm); return 0; }
    int sound = plm_get_num_audio_streams(plm) > 0;
    plm_set_loop(plm, 0);
    plm_set_audio_enabled(plm, sound);
    plm_set_video_decode_callback(plm, on_video, &p);
    if (sound) {
        plm_set_audio_decode_callback(plm, on_audio, NULL);
        plm_set_audio_lead_time(plm, 0.25); /* (the sound a little ahead: the ring is never empty) */
        cd_audio_xa_begin();
        cd_audio_xa_rate(plm_get_samplerate(plm));
    }
    fprintf(stderr, "ending video %s: %d x %d, %.1f s, %.2f frames a second%s\n", path, p.w, p.h, plm_get_duration(plm),
            plm_get_framerate(plm), sound ? "" : ", no sound");
    long shot_frame = -1;
    char shot_path[600] = "";
    const char *t = getenv("TM2PC_VIDEO_SHOT");
    if (t && sscanf(t, "%ld,%599s", &shot_frame, shot_path) != 2) shot_frame = -1;
    Psyz_VideoSetDisplayRGB24Fill(1);
    while (!plm_has_ended(plm)) {
        plm_decode(plm, 1.0 / NTSC_VBLANK_HZ);
        if (p.shown) Psyz_VideoSetDisplayRGB24(p.rgb, p.w, p.h);
        if (shot_frame >= 0 && p.frames == (uint64_t)shot_frame + 1) {
            FILE *f = fopen(shot_path, "wb"); /* (a 24-bit BMP, bottom-up) */
            if (f) {
                int row = (p.w * 3 + 3) & ~3;
                unsigned size = 54u + (unsigned)(row * p.h);
                unsigned char h[54] = {'B', 'M'};
                memcpy(h + 2, &size, 4);
                h[10] = 54; h[14] = 40; h[26] = 1; h[28] = 24;
                memcpy(h + 18, &p.w, 4);
                memcpy(h + 22, &p.h, 4);
                fwrite(h, 1, 54, f);
                unsigned char line[VIDEO_W * 3 + 4] = {0};
                for (int y = p.h - 1; y >= 0; y--) {
                    for (int x = 0; x < p.w; x++) {
                        const unsigned char *s = p.rgb + ((size_t)y * p.w + x) * 3;
                        line[x * 3] = s[2]; line[x * 3 + 1] = s[1]; line[x * 3 + 2] = s[0];
                    }
                    fwrite(line, 1, (size_t)row, f);
                }
                fclose(f);
            }
            shot_frame = -1;
        }
        rt_vblank();
        if (skip && skip()) break;
    }
    fprintf(stderr, "ending video: %llu frames shown\n", (unsigned long long)p.frames);
    if (sound) cd_audio_xa_end();
    plm_destroy(plm);
    free(p.rgb);
    return 1;
}
