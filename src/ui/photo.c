/* Photo mode (the pause menu's PHOTO MODE): the battle frozen, a free camera.
 *
 * The game is stopped in the pause menu; here each frame its world is drawn
 * again from a camera of our own, as rtMainLoop draws view 1 (decomp
 * FOUR_PLAYERS.md 4.1): the next frame record, its order table cleared,
 * viewProc(0), the effects, debris and renderScene, then its draw and
 * display environments and DrawOTag. Nothing is updated (no world step, no
 * animation step: renderFlipFrame is not called), so everything stays as
 * it was. The camera: view 1's camera object with its parent's pose zeroed,
 * so its own rotation and position are the world's. In split screen view 1
 * is made full-screen for the photo (view mode 1) and put back after. The
 * view keeps the zoom it had (view mode 1's H, put back after).
 *
 * Controls: left stick (or the D-pad's up/down) moves, right stick (or the
 * D-pad's left/right) turns, L1/R1 down/up, hold X to move
 * faster, Square saves a picture (screenshots/ by the game), Triangle hides
 * the legend, O goes back to the pause menu.
 *
 * Photo mode rewind (Vanilla Plus, vplus/killcam.c): a timeline over the
 * legend; L2/R2 move its mark back and forward through the last 30 seconds, held: faster the harder pressed (the
 * right stick turns), and when let go the battle is shown at that moment;
 * X plays it on from there, seen from this camera, up to the present (X
 * stops it there, O goes back to the present). Start does nothing here (the
 * pause menu's resume). L3 held: the camera faster. The camera stays where it
 * was across moments. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <psyz/video.h>
#include "runtime.h"
#include "rebase_table.h"
#include "draw.h"
unsigned char *wide_capture(int *w, int *h); /* vplus/wide.c: the frame as shown (widescreen: widened) */

#define VIEW0_FRAMES GA(0x8018118Cu)  /* view 1's frame record pair (0x80 each) */
#define SLOT_VIEW0 GA(0x80180EE4u)    /* the record view 1 draws into */
#define VIEW_MODE1_H (RB_viewModeTable + 0x60u + 0x08u)
#define POS_BUF 0x80DF6800u       /* a position for worldGroundHeightAt (high RAM) */
#define FAR_FROM_CAR 20000.0      /* how far the camera may go from the player's car */
#define ABOVE_GROUND_MIN 70.0     /* the camera stays over the floor under it (lower: the view's bottom edge in the ground) */
#define ABOVE_GROUND_MAX 2600.0   /* and this high at most */

/* the floor under (x, y), looked for from height z down (worldGroundHeightAt 800EF340: a bridge's
 * deck when over it, the road under it when below) */
static double ground_at(double x, double y, double z)
{
    mem_w32(&M.cpu, POS_BUF, (uint32_t)(int32_t)x);
    mem_w32(&M.cpu, POS_BUF + 4, (uint32_t)(int32_t)y);
    mem_w32(&M.cpu, POS_BUF + 8, (uint32_t)(int32_t)z);
    return (int32_t)guest_call(GA(0x800EF340u), POS_BUF, 0, 0, 0);
}

void input_player_axes(int player, int ax[6]); /* runtime/input.c: LX LY RX RY LT RT (-32768..32767) */
uint16_t input_player_dpad_buttons(int player); /* runtime/input.c: the real D-pad (no left stick) and the menu keys */
uint16_t input_player_pad_buttons(int player);
const char *ui_folder(void);
void photo_legend(int show);            /* options.c: the buttons' banner (an overlay), or none */
void photo_timeline(int on, float frac, float back_s, int playing); /* options.c: the rewind's timeline in that banner */
int rewind_range(uint64_t *oldest, uint64_t *now, uint64_t *at);   /* vplus/killcam.c */
double rewind_seconds(uint64_t frame);
void rewind_request(uint64_t frame);
uint16_t input_menu_buttons(int *menu_key);

/* Each frame is drawn with the machine's RAM, scratchpad and GTE kept and put back after: the view made full
 * screen, the camera and its parent placed, the drawing itself (packets, order table, the renderer's own state)
 * never reach the battle, which photo mode rewind replays exactly. Only the VBlank between frames (the game's
 * sounds) runs on the battle's machine, as in the pause menu. */
static uint8_t *kept_ram, kept_scratch[1024];
static uint32_t kept_gte[64];
static void keep_machine(void)
{
    if (!kept_ram && !(kept_ram = malloc(GUEST_RAM_BYTES))) return;
    memcpy(kept_ram, M.ram, GUEST_RAM_BYTES);
    memcpy(kept_scratch, M.scratch, sizeof kept_scratch);
    memcpy(kept_gte, M.cpu.gte_data, 128);
    memcpy(kept_gte + 32, M.cpu.gte_ctrl, 128);
}
static void put_machine_back(void)
{
    if (!kept_ram) return;
    memcpy(M.ram, kept_ram, GUEST_RAM_BYTES);
    memcpy(M.scratch, kept_scratch, sizeof kept_scratch);
    memcpy(M.cpu.gte_data, kept_gte, 128);
    memcpy(M.cpu.gte_ctrl, kept_gte + 32, 128);
}
static double x, y, z, yaw, pitch, zoom; /* the camera, kept across a rewind's moments */
void photo_camera_xy(int32_t *px, int32_t *py) { *px = (int32_t)x; *py = (int32_t)y; } /* vplus/fixes.c: the sky follows it */
/* the sky (the view's follow object, drawn round its place) at the camera: flown far from the car, the camera left the
 * sky's dome and saw gray (the user's on Cyburbia). The machine is put back after the frame. */
static void sky_at_camera(void)
{
    uint32_t sky = guest_call(GA(0x801563A0u), 0, 0, 0, 0); /* viewGetFollowObj(0) */
    if (!sky) return;
    mem_w32(&M.cpu, sky + 0xA8, (uint32_t)(int32_t)x);
    mem_w32(&M.cpu, sky + 0xAC, (uint32_t)(int32_t)y);
    mem_w32(&M.cpu, sky + 0xB0, 0);
}
static void render_frame(void)
{
    CPU *c = &M.cpu;
    keep_machine();
    uint32_t cam = mem_r32(c, RB_viewCameraObjects), parent = mem_r32(c, RB_viewCameraObjectTable);
    if ((int)mem_r32(c, RB_viewModes) != 1) guest_call(GA(0x80155888u), VIEW0_FRAMES, 1, 0, 0); /* viewCreate: full screen */
    static const int16_t ident[9] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096}; /* the parent at the world's origin */
    for (int i = 0; i < 9; i++) mem_w16(c, parent + 0x80 + 2u * (uint32_t)i, (uint16_t)ident[i]);
    for (uint32_t i = 0; i < 12; i++) mem_w8(c, parent + 0x8C + i, 0);
    for (uint32_t i = 0; i < 8; i++) mem_w8(c, parent + 0xA0 + i, 0);
    for (uint32_t i = 0; i < 12; i++) mem_w8(c, parent + 0xA8 + i, 0);
    mem_w32(c, VIEW_MODE1_H, (uint32_t)(int)zoom);
    mem_w32(c, VIEW_MODE1_H + 4, (uint32_t)(int)zoom);
    mem_w16(c, cam + 0xA0, (uint16_t)(int16_t)(int)pitch);
    mem_w16(c, cam + 0xA2, 0);
    mem_w16(c, cam + 0xA4, (uint16_t)(int16_t)(int)yaw);
    mem_w32(c, cam + 0xA8, (uint32_t)(int32_t)x);
    mem_w32(c, cam + 0xAC, (uint32_t)(int32_t)y);
    mem_w32(c, cam + 0xB0, (uint32_t)(int32_t)z);
    uint32_t cur = mem_r32(c, SLOT_VIEW0), rec = cur == VIEW0_FRAMES ? VIEW0_FRAMES + 0x80 : VIEW0_FRAMES;
    mem_w32(c, rec + 8, mem_r32(c, rec + 4));          /* the packet cursor at the buffer's start */
    mem_w32(c, SLOT_VIEW0, rec);
    guest_call(GA(0x80141938u), mem_r32(c, rec), 4096, 0, 0); /* ClearOTagR */
    sky_at_camera();
    guest_call(GA(0x80155E80u), 0, 0, 0, 0);                   /* viewProc(0) */
    guest_call(GA(0x80110BE4u), rec, 0, 0, 0);                 /* fxSpritesDraw */
    guest_call(GA(0x8014B484u), rec, 0, 0, 0);                 /* debrisDraw */
    guest_call(GA(0x80152284u), rec, 0, 0, 0);                 /* renderScene */
    guest_call(GA(0x801414CCu), 0, 0, 0, 0);                   /* DrawSync */
    guest_call(GA(0x80141AA4u), rec + 0x10, 0, 0, 0);          /* PutDrawEnv */
    guest_call(GA(0x80141BA0u), cur + 0x6C, 0, 0, 0);          /* PutDispEnv: the other record's (it shows this one's buffer,
                                                                 * as interp.c's redraws: with rec's own, the battle's frame stayed) */
    guest_call(GA(0x80141A30u), mem_r32(c, rec) + 4095u * 4, 0, 0, 0); /* DrawOTag */
    static const char *ram_dump = (const char *)1; /* tests: TM2PC_PHOTO_RAM=path, the RAM as this camera's 30th frame
                                                    * drew it (its mesh queue and packets; decomp request 116) */
    static int ram_frames;
    if (ram_dump == (const char *)1) ram_dump = getenv("TM2PC_PHOTO_RAM");
    if (ram_dump && ++ram_frames == 30) {
        FILE *f = fopen(ram_dump, "wb");
        if (f) fwrite(M.ram, 1, GUEST_RAM_BYTES, f), fclose(f);
        void gpu_vram_read(int x, int y, int w, int h, uint16_t *p); /* (runtime/gpu_psyz.c) */
        static uint16_t vram[1024 * 512];                            /* and VRAM then, raw: path.vram (.ram swapped) */
        guest_call(GA(0x801414CCu), 0, 0, 0, 0);                     /* DrawSync */
        gpu_vram_read(0, 0, 1024, 512, vram);
        char vpath[1100];
        size_t n = strlen(ram_dump);
        snprintf(vpath, sizeof vpath, "%.*s.vram", (int)(n > 4 && !strcmp(ram_dump + n - 4, ".ram") ? n - 4 : n), ram_dump);
        f = fopen(vpath, "wb");
        if (f) fwrite(vram, 2, 1024 * 512, f), fclose(f);
        fprintf(stderr, "photo mode: RAM and VRAM written at VBlank %llu\n", (unsigned long long)M.vblanks);
    }
    void render_budget_carry(int phase); /* vplus/render_stats.c: Farthest's detail budget, by this camera's frames */
    render_budget_carry(0);
    put_machine_back();
    render_budget_carry(1);
    rt_vblank();
}

static void save_picture(void)
{
    int w = 0, h = 0;
    unsigned char *px = wide_capture(&w, &h);
    if (!px) return;
    char dir[1200], path[1300];
    snprintf(dir, sizeof dir, "%sscreenshots", ui_folder());
    SDL_CreateDirectory(dir);
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    snprintf(path, sizeof path, "%s/photo_%04d%02d%02d_%02d%02d%02d.bmp", dir, lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
             lt->tm_hour, lt->tm_min, lt->tm_sec);
    Image im = {w, h, px};
    int ok = image_save_bmp(&im, path);
    fprintf(stderr, "photo mode: %s %s\n", ok ? "saved" : "could not save", path);
    free(px);
}

/* A picture taken (the user's): a shutter's click, and the shutter seen: black from the top and the bottom meeting in
 * the middle, then opening on the picture in a flash that fades. Drawn as an overlay over the frame just saved (not in
 * the picture), about a quarter of a second; the click is made here (the battle's loaded sounds have no shutter),
 * played on a stream of its own at the effects volume. */
void audio_get_volumes(int *music, int *effects); /* runtime/cd_audio.c */
static void shutter_sound(void)
{
    enum { RATE = 44100, N = RATE / 6 };
    static SDL_AudioStream *out;
    static int16_t pcm[N];
    static int made;
    if (!made) { /* two clicks (the blades closing, then opening): a sharp noise burst over a short low knock */
        uint32_t seed = 12345;
        for (int i = 0; i < N; i++) {
            double t = (double)i / RATE, s = 0;
            for (int k = 0; k < 2; k++) {
                double at = k ? 0.072 : 0, d = t - at, amp = k ? 0.6 : 1;
                if (d < 0) continue;
                seed = seed * 1664525u + 1013904223u;
                double noise = ((seed >> 9) & 0xFFFF) / 32768.0 - 1;
                s += amp * (noise * exp(-d / 0.0025) * 0.7 + sin(2 * M_PI * (k ? 260 : 180) * d) * exp(-d / 0.012) * 0.5);
            }
            s = s > 1 ? 1 : s < -1 ? -1 : s;
            pcm[i] = (int16_t)(s * 20000);
        }
        made = 1;
    }
    if (!out) {
        static const SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, RATE};
        out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (out) SDL_ResumeAudioStreamDevice(out);
    }
    if (!out) return;
    int music = 0, effects = 100;
    audio_get_volumes(&music, &effects);
    SDL_SetAudioStreamGain(out, (float)effects * (float)effects / 10000.f); /* (as the effects' own gain: by its square) */
    SDL_ClearAudioStream(out);
    SDL_PutAudioStreamData(out, pcm, (int)sizeof pcm);
}
static void shutter_show(void)
{
    PsyzSize size = Psyz_VideoGetDisplaySize();
    int fw = 0, fh = 0;
    unsigned char *frame = wide_capture(&fw, &fh);
    int w = size.w, h = size.h;
    unsigned char *px = w > 0 && h > 0 ? malloc((size_t)w * h * 3) : NULL;
    if (!px || !frame) { free(px); free(frame); return; }
    enum { CLOSE = 5, SHUT = 2, OPEN = 9 };
    for (int f = 0; f < CLOSE + SHUT + OPEN; f++) {
        double bar = f < CLOSE ? (double)(f + 1) / CLOSE : f < CLOSE + SHUT ? 1 : 1 - (double)(f - CLOSE - SHUT + 1) / OPEN;
        double flash = f < CLOSE + SHUT ? 0 : 0.75 * (1 - (double)(f - CLOSE - SHUT) / OPEN); /* (white, fading as it opens) */
        int edge = (int)(bar * h / 2 + 0.5);
        for (int y = 0; y < h; y++) {
            unsigned char *row = px + (size_t)y * w * 3;
            if (y < edge || y >= h - edge) { memset(row, 0, (size_t)w * 3); continue; }
            const unsigned char *src = frame + (size_t)(y * fh / h) * fw * 3;
            for (int x = 0; x < w; x++) {
                const unsigned char *p = src + (size_t)(x * fw / w) * 3;
                for (int k = 0; k < 3; k++) row[x * 3 + k] = (unsigned char)(p[k] + (255 - p[k]) * flash);
            }
        }
        int rect[4] = {0, 0, w, h};
        Psyz_VideoSetOverlay(px, w, h, rect, 1);
        const char *dump = getenv("TM2PC_SHUTTER_DUMP"); /* tests: a folder, some of the frames saved there */
        if (dump && f % 3 == 1) {
            char path[1100];
            snprintf(path, sizeof path, "%s/shutter_%02d.bmp", dump, f);
            Image im = {w, h, px};
            image_save_bmp(&im, path);
        }
        rt_vblank();
    }
    Psyz_VideoSetOverlay(NULL, 0, 0, NULL, 0);
    free(px);
    free(frame);
}

static unsigned char *held; /* the picture kept on the screen while the battle goes to a moment */
void photo_hold(int on)
{
    if (on) {
        int w = 0, h = 0;
        unsigned char *px = wide_capture(&w, &h);
        if (!px) return;
        Psyz_VideoSetDisplayRGB24Fill(1);
        Psyz_VideoSetDisplayRGB24(px, w, h);
        free(held);
        held = px;
        return;
    }
    Psyz_VideoSetDisplayRGB24Fill(0);
    Psyz_VideoSetDisplayRGB24(NULL, 0, 0);
    free(held);
    held = NULL;
}

static int pose_kept;
int photo_camera_put(uint32_t cam) /* vplus/killcam.c: the rewind playing, from this camera (its parent at the origin) */
{
    CPU *c = &M.cpu;
    mem_w32(c, VIEW_MODE1_H, (uint32_t)(int)zoom);
    mem_w32(c, VIEW_MODE1_H + 4, (uint32_t)(int)zoom);
    mem_w16(c, cam + 0xA0, (uint16_t)(int16_t)(int)pitch);
    mem_w16(c, cam + 0xA2, 0);
    mem_w16(c, cam + 0xA4, (uint16_t)(int16_t)(int)yaw);
    mem_w32(c, cam + 0xA8, (uint32_t)(int32_t)x);
    mem_w32(c, cam + 0xAC, (uint32_t)(int32_t)y);
    mem_w32(c, cam + 0xB0, (uint32_t)(int32_t)z);
    return 1;
}
static void timeline_show(uint64_t oldest, uint64_t now, uint64_t mark, int playing)
{
    float frac = now > oldest ? (float)(mark - oldest) / (float)(now - oldest) : 1;
    photo_timeline(1, frac, (float)rewind_seconds(mark), playing);
}
static int play_hidden; /* the rewind playing: Triangle hid the bar */
void photo_play_overlay(void) /* vplus/killcam.c: each frame the rewind plays: its timeline */
{
    uint64_t o, n, a;
    static int count, was_hidden;
    if (play_hidden) {
        if (!was_hidden) { photo_legend(0); photo_timeline(0, 0, 0, 0); was_hidden = 1; }
        return;
    }
    if (was_hidden) { was_hidden = 0; count = -1; }
    if (++count % 6 || !rewind_range(&o, &n, &a)) return;
    timeline_show(o, n, a, 1);
    photo_legend(1);
}

static double last_floor; /* the floor under the camera last (session) */
/* one step of the free camera (a frame: photo mode's loop, a VBlank: the spectators' freecam): the sticks and the D-pad
 * move and turn it (dpad_turn: Left/Right turn), R1/L1 up and down, R2/L2 zoom, fast with `fast`; kept near the
 * player's car (car), over the floor, not too high */
static double cam_far; /* the spectators' freecam: farther (the whole arena; the user's on Cyburbia), 0 photo mode's */
static void cam_step(uint16_t b, const int ax[6], int dpad_turn, int fast, int zooms, const double car[3])
{
    int timeline = !dpad_turn;
    double speed = fast ? 60 : 20;
    double fwd = -ax[1] / 32768.0, side = ax[0] / 32768.0, turn = ax[2] / 32768.0, look = ax[3] / 32768.0; /* (up: look up) */
    int stick = fabs(side) >= 0.2 || fabs(fwd) >= 0.2; /* the left stick also comes as the D-pad: it moves, not turns */
    if ((b & 0x0010) && !stick) fwd = 1;
    if ((b & 0x0040) && !stick) fwd = -1;
    if ((b & 0x0080) && !stick && !timeline) turn = -1; /* (with the timeline: the D-pad moves its mark) */
    if ((b & 0x0020) && !stick && !timeline) turn = 1;
    if (fabs(fwd) < 0.2) fwd = 0;
    if (fabs(side) < 0.2) side = 0;
    if (fabs(turn) < 0.2) turn = 0;
    if (fabs(look) < 0.2) look = 0;
    yaw += turn * 32;
    pitch += look * 24;
    if (pitch > 900) pitch = 900;
    if (pitch < -900) pitch = -900;
    double a = yaw * 2 * M_PI / 4096;
    x += (sin(a) * fwd + cos(a) * side) * speed;
    y += (cos(a) * fwd - sin(a) * side) * speed;
    if (b & 0x0800) z += speed;     /* R1 */
    if (b & 0x0400) z -= speed;     /* L1 */
    /* in bounds: near the player's car, over the floor, not too high */
    double dx = x - car[0], dy = y - car[1], d = sqrt(dx * dx + dy * dy);
    double far = cam_far > 0 ? cam_far : FAR_FROM_CAR;
    if (d > far) { x = car[0] + dx * far / d; y = car[1] + dy * far / d; }
    keep_machine(); /* (the ground's lookup: on a copy too) */
    double floor_z = ground_at(x, y, z + 200);
    put_machine_back();
    /* past the arena's edge (nothing under it but the map's bottom): as if the last floor went on */
    if (last_floor > -1e8 && floor_z < last_floor - 2000) floor_z = last_floor; /* (a real drop, a roof to the street, is less) */
    else last_floor = floor_z;
    if (z < floor_z + ABOVE_GROUND_MIN) z = floor_z + ABOVE_GROUND_MIN;
    if (z > floor_z + ABOVE_GROUND_MAX) z = floor_z + ABOVE_GROUND_MAX;
    if (zooms && (b & 0x0200 || ax[5] > 8000)) zoom *= 1.02; /* R2: zoom in (photo mode only) */
    if (zooms && (b & 0x0100 || ax[4] > 8000)) zoom /= 1.02; /* L2 */
    if (zoom < 100) zoom = 100;
    if (zoom > 1200) zoom = 1200;
}

/* ---- the spectators' freecam: photo mode's camera while the battle goes on (vplus/fixes.c: one player out, Square
 * from the cinematic camera); placed each frame through the kill cam's camera hook (vplus/killcam.c) ---- */
static uint64_t free_last;
void photo_freecam_begin(double cx, double cy, double cz, int heading) /* behind and above that car, looking at it */
{
    CPU *c = &M.cpu;
    uint32_t h_saved = mem_r32(c, VIEW_MODE1_H);
    yaw = heading & 4095, pitch = 233, zoom = (int32_t)h_saved > 0 ? (int32_t)h_saved : 228;
    double a0 = yaw * 2 * M_PI / 4096;
    x = cx - sin(a0) * 400, y = cy - cos(a0) * 400, z = cz + 160;
    last_floor = -1e9;
    free_last = M.vblanks;
    pose_kept = 0; /* (photo mode, opened later, starts from the player's car again) */
}
int photo_freecam_put(uint32_t cam) /* each frame drawn: moved by the VBlanks since the last, then placed */
{
    uint64_t n = M.vblanks - free_last;
    free_last = M.vblanks;
    if (n > 4) n = 4;
    uint16_t b = input_player_pad_buttons(0);
    int ax[6];
    input_player_axes(0, ax);
    double car[3];
    for (uint32_t i = 0; i < 3; i++) car[i] = (int32_t)mem_r32(&M.cpu, RB_humanCars + 0x6FC + 4 * i); /* (the player's wreck) */
    cam_far = 80000;
    for (uint64_t i = 0; i < n; i++) cam_step(b, ax, 1, (b & 0x0002) != 0, 0, car); /* (X: back to the chase camera; L3: faster) */
    cam_far = 0;
    return photo_camera_put(cam);
}
/* the rewind playing: photo mode's camera flown as in photo mode (sticks, D-pad, L1/R1, L3 faster) and Triangle hiding
 * the bar, each frame drawn (the user's: the camera stood still while it played) */
int photo_play_camera_put(uint32_t cam)
{
    static uint64_t last;
    static uint16_t prev = 0xFFFF;
    uint64_t n = M.vblanks - last;
    last = M.vblanks;
    if (n > 4) n = 4;
    uint16_t b = input_player_dpad_buttons(0);
    if ((b & 0x1000) && !(prev & 0x1000)) play_hidden = !play_hidden;
    prev = b;
    int ax[6];
    input_player_axes(0, ax);
    double car[3];
    for (uint32_t i = 0; i < 3; i++) car[i] = (int32_t)mem_r32(&M.cpu, RB_humanCars + 0x6FC + 4 * i);
    for (uint64_t i = 0; i < n; i++) cam_step(b, ax, 1, (b & 0x0002) != 0, 0, car);
    return photo_camera_put(cam);
}
void photo_play_reset(void) { play_hidden = 0; } /* a new playback: the bar shown */
/* rewound: at a moment of photo mode rewind (0 back, 1 a moment asked for, 2 play); from the pause: 0 back, 1 a moment */
static int session(int rewound)
{
    last_floor = -1e9;
    CPU *c = &M.cpu;
    uint32_t cam = mem_r32(c, RB_viewCameraObjects), parent = mem_r32(c, RB_viewCameraObjectTable);
    if (!cam || !parent) return 0;
    uint32_t h_saved = mem_r32(c, VIEW_MODE1_H);
    /* the camera: behind the player's car and a little above it, as the chase camera, looking at it */
    double car[3];
    for (uint32_t i = 0; i < 3; i++) car[i] = (int32_t)mem_r32(c, RB_humanCars + 0x6FC + 4 * i);
    if (!rewound || !pose_kept) {
        yaw = (int32_t)mem_r32(c, RB_humanCars + 0x710) & 4095, pitch = 233, zoom = (int32_t)h_saved > 0 ? (int32_t)h_saved : 228;
        double a0 = yaw * 2 * M_PI / 4096;
        x = car[0] - sin(a0) * 400, y = car[1] - cos(a0) * 400, z = car[2] + 160; /* (pitch 233: down at it) */
        pose_kept = 1;
    }
    if (getenv("TM2PC_PHOTO_AT")) sscanf(getenv("TM2PC_PHOTO_AT"), "%lf,%lf,%lf,%lf,%lf", &x, &y, &z, &yaw, &pitch); /* tests: a place
                                                                                     * to look from (x, y, z, yaw, and pitch) */
    if (getenv("TM2PC_PHOTO_AT") && !strcmp(getenv("TM2PC_PHOTO_AT"), "santa")) /* tests: in front of pedestrian 516 #2 */
        for (uint32_t r = GA(0x80190E54u); r < GA(0x80190E54u) + 6 * 60; r += 60)
            if ((int16_t)mem_r16(c, r + 4) == 516 && mem_r16(c, r + 6) == 2 && mem_r32(c, r + 56)) {
                uint32_t o = mem_r32(c, r + 56);
                x = (int32_t)mem_r32(c, o + 0xA8); y = (int32_t)mem_r32(c, o + 0xAC) - 220; z = (int32_t)mem_r32(c, o + 0xB0) + 50; yaw = 0;
                fprintf(stderr, "photo mode: santa at %d %d %d\n", (int32_t)mem_r32(c, o + 0xA8), (int32_t)mem_r32(c, o + 0xAC), (int32_t)mem_r32(c, o + 0xB0));
            }
    photo_hold(0);
    int hidden = 0, legend_shown = -1, result = 0;
    /* the rewind's timeline: its mark (sel) moved with the D-pad, the moment asked for when let go */
    uint64_t r_old = 0, r_now = 0, r_at = 0, sel = 0;
    int timeline = rewind_range(&r_old, &r_now, &r_at), held_for = 0, step = 1;
    const char *rt = getenv("TM2PC_REWIND_TEST"); /* tests: back N frames, a picture, play to the present, back */
    if (timeline) {
        sel = r_at;
        double s = rewind_seconds(r_old);
        step = s > 0 ? (int)((double)(r_now - r_old) / s / 4 + 0.5) : 15; /* a quarter second */
        if (step < 1) step = 1;
        timeline_show(r_old, r_now, sel, 0);
    } else photo_timeline(0, 0, 0, 0);
    uint16_t prev = 0xFFFF;
    int test = getenv("TM2PC_PHOTO_TEST") ? atoi(getenv("TM2PC_PHOTO_TEST")) : 0, frame = 0; /* tests: N frames forward, a picture, out */
    int still = getenv("TM2PC_PHOTO_STILL") ? atoi(getenv("TM2PC_PHOTO_STILL")) : 0; /* tests: N frames still, a picture, out */
    fprintf(stderr, "photo mode: on\n");
    for (;;) {
        uint16_t b = input_player_pad_buttons(0);
        int menu_key = 0;
        b |= input_menu_buttons(&menu_key);
        if (test) b = ++frame >= test ? 0x2000 : frame == test - 1 ? 0x8000 : frame < test / 3 ? 0x0810 : frame < 2 * test / 3 ? 0x0020 : 0x0010;
        if (still) b = ++frame > still ? 0x2000 : frame == still ? 0x8000 : 0;
        uint16_t pr = (uint16_t)(b & ~prev);
        prev = b;
        if (rt && timeline) { /* tests */
            if (!test) frame++;
            int back = atoi(rt);
            b = 0; pr = 0;
            if (!rewound && frame == 5) sel = r_now > r_old + (uint64_t)back ? r_now - (uint64_t)back : r_old;
            else if (!rewound && frame == 6) pr = 0x4000;
            else if (rewound && r_at < r_now && frame == 5) pr = 0x8000;
            else if (rewound && r_at < r_now && frame == 8) pr = 0x4000;
            else if (frame >= 8) pr = 0x2000;
        }
        if (pr & 0x2000) break; /* O: back to the pause menu */
        if (timeline) {
            /* L2 back, R2 forward (the user's, 2026-10-17: the D-pad and the sticks move the camera). A press: a quarter
             * second; held: on, faster the harder the trigger is pressed and the longer it is held */
            int tax[6];
            input_player_axes(0, tax);
            int dir = (b & 0x0100) ? -1 : (b & 0x0200) ? 1 : 0;
            if (dir) {
                held_for++;
                if (held_for == 1 || (held_for > 15 && held_for % 2 == 0)) {
                    double press = (dir < 0 ? tax[4] : tax[5]) / 32767.0; /* (0: a digital trigger or a key) */
                    int mult = (held_for > 75 ? 4 : 1) * (held_for > 1 && press > 0.9 ? 3 : held_for > 1 && press > 0.6 ? 2 : 1);
                    int64_t d = (int64_t)step * mult * dir, v = (int64_t)sel + d;
                    if (v < (int64_t)r_old) v = (int64_t)r_old;
                    if (v > (int64_t)r_now) v = (int64_t)r_now;
                    sel = (uint64_t)v;
                    timeline_show(r_old, r_now, sel, sel != r_at ? 2 : 0);
                    legend_shown = -1;
                }
            } else held_for = 0;
            /* X: go to the marked moment (moving the mark alone goes nowhere: a tap of L2 let go jumped back in time, and
             * photo mode came again there, the user's "Resume opened photo mode again"); at the moment shown, play */
            if ((pr & 0x4000) && sel != r_at) { rewind_request(sel); result = 1; break; }
            if ((pr & 0x4000) && rewound && r_at < r_now) { result = 2; break; } /* X: play (the user's: not Start) */
            if ((pr & 0x4000) && !rewound && sel == r_at) { result = 3; break; } /* X at the present: play on, as its legend
                                                                                  * says (it did nothing, and the user's way
                                                                                  * out then reopened photo mode) */
        }
        if (pr & 0x1000) hidden = !hidden;
        int ax[6];
        input_player_axes(0, ax);
        uint16_t held = test || still || rt ? b : input_player_dpad_buttons(0); /* (the real D-pad: the sticks are the sticks) */
        cam_step(held, ax, 1, (held & 0x0002) != 0, 0, car); /* (L3: faster; no zoom: moving closer does it) */
        extern int64_t gpu_culled_prims;
        int64_t culled_before = gpu_culled_prims;
        render_frame();
        if ((pr & 0x8000) && getenv("TM2PC_PHOTO_STATS")) { /* tests: what the picture's frame drew (decomp HIDDEN_AREAS.md 7) */
            int render_stats_now(char *out, size_t n);
            char line[200] = "";
            render_stats_now(line, sizeof line);
            fprintf(stderr, "photo stats: at %.0f %.0f %.0f yaw %.0f pitch %.0f; polygons over the size limit dropped %lld; %s\n",
                    x, y, z, yaw, pitch, (long long)(gpu_culled_prims - culled_before), line);
        }
        if (pr & 0x8000) { /* Square: the picture, without the legend */
            if (!hidden) { photo_legend(0); render_frame(); }
            save_picture();
            if (!rt) { shutter_sound(); shutter_show(); render_frame(); } /* (the rewind's baseline: its VBlanks as before) */
            legend_shown = -1;
        }
        if (legend_shown != hidden) { photo_legend(!hidden); legend_shown = hidden; } /* the legend */
        if (getenv("TM2PC_PHOTO_TRACE")) fprintf(stderr, "photo mode: at %.0f %.0f %.0f yaw %.0f pitch %.0f zoom %.0f\n", x, y, z, yaw, pitch, zoom);
    }
    if (result == 1 || (rewound && result == 0)) photo_hold(1); /* the picture stays while the battle goes there */
    photo_legend(result == 1 && !hidden); /* (the timeline stays while it goes there) */
    photo_timeline(0, 0, 0, 0);
    fprintf(stderr, "photo mode: off\n");
    return result;
}
/* while photo mode is up the in-between frames stay off (vplus/interp.c): uncapped, they redrew the battle's own camera
 * over photo mode's frames, so its camera seemed never to move (the user's, at 60 and uncapped) */
static int photo_up;
int photo_open(void) { return photo_up; }
int photo_mode_run(void) { pose_kept = 0; photo_up = 1; int r = session(0); photo_up = 0; return r; } /* the pause menu's: 1 a rewind's moment asked for */
int photo_rewind_session(void) { photo_up = 1; int r = session(1); photo_up = 0; return r; } /* vplus/killcam.c */
