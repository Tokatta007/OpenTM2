#include <stdlib.h>
/* Host versions of selected SDK and game functions (src/overrides.json):
 * VSync (where emulated time advances), and the libcd API on the disc image
 * (the movie player is in movie.c). */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "runtime.h"
#include <SDL3/SDL_timer.h>
#include "gpu_backend.h"
#include "../net/rb_state.h"
#include "rebase_table.h"


void rt_frame_boundary(void);   /* main.c */
void vp_disc_sector(uint32_t lba, uint8_t *sec); /* vplus/fixes.c */
void rt_pace_vblank(void);      /* main.c */

/* ---- time ---- */

void input_poll(void);                  /* input.c */

uint16_t (*rt_pad_filter)(int player, uint16_t buttons); /* ui: the pads as the game sees them (active-low) */
uint16_t rt_pad_tool[2]; /* tools (ui/options.c's turntable): buttons held on the game's pads (active-high bits) */

/* Trailer takes (TM2PC_PAD_SCRIPT_BATTLE): a scripted car that hardly moved for 45 VBlanks (against a wall or a fence:
 * the user's 2026-11-15 take, players 3 and 4 in a corner most of the match) backs out turning for 90 VBlanks, then
 * drives on turning the same way for 40 more (a three-point turn: well away from the wall, not straight back into it),
 * then its script again. One stuck spot keeps one turning side (the user's: alternating sides rocked a car in a corner
 * and kept it there); only after 4 tries there the other side, and a new stuck spot the other side from the last.
 * Its place: the human record's +0x6FC (x, y, z, as photo mode's). (In reverse, Left still turns the nose left: the
 * default controls, bindings.c steer_keep.) */
enum { UNSTICK_BACK = 90, UNSTICK_TURN = 40, UNSTICK_SAME_SPOT = 240, UNSTICK_TRIES = 4 };
static uint16_t pad_script_unstick(int p, uint16_t bits, uint64_t in_battle)
{
    static int32_t at[4][3];
    static uint64_t since[4], back_until[4], turn_until[4], seen[4];
    static int side[4], tries[4];
    static uint16_t last[4];
    p &= 3;
    if (seen[p] == M.vblanks) return last[p]; /* (once a VBlank: the same answer to every call) */
    int gap = M.vblanks - seen[p] > 12; /* (a new battle, a pause: start counting again; players 3-4 are asked once a game
                                         * step, 2-3 VBlanks at 4 players) */
    seen[p] = M.vblanks;
    if (gap) back_until[p] = turn_until[p] = 0;
    uint16_t steer = side[p] ? 0x0080 : 0x0020; /* (left, right) */
    if (M.vblanks < back_until[p]) /* backing out: reverse, the nose turned away, the gun on */
        return last[p] = (uint16_t)(0x0040 | 0x0200 | steer);
    if (M.vblanks < turn_until[p]) /* then forward, still turning the same way */
        return last[p] = (uint16_t)(0x4000 | 0x0010 | 0x0200 | steer);
    int netplay_first_local(void); /* net/game_tm2.c: online, pad 1 drives this machine's first player, not car 1 (the
                                    * user's Hong Kong final take 2026-11-16: the joiner watched the host's car, backing
                                    * out when the user slowed, never at its own wall) */
    int slot = p + netplay_first_local();
    if (slot >= 16) return last[p] = bits; /* (16 human records, vplus/cars.h) */
    uint32_t rec = RB_humanCars + (uint32_t)slot * 0x814u;
    int vp_player_out(int p); /* vplus/fixes.c */
    if (mem_r8(&M.cpu, rec + 0x9C) || vp_player_out(slot)) { /* (a wreck, a player out watching: nothing to back out of; its
                                                               * presses would only steer the spectating) */
        since[p] = 0;
        return last[p] = bits;
    }
    int32_t pos[3];
    for (int i = 0; i < 3; i++) pos[i] = (int32_t)mem_r32(&M.cpu, rec + 0x6FC + 4 * (uint32_t)i);
    if (gap || !since[p] || in_battle < 300) { /* (the first 5 s: the countdown, nobody moves yet) */
        memcpy(at[p], pos, sizeof pos);
        since[p] = M.vblanks;
    } else if (M.vblanks - since[p] >= 45) {
        double dx = pos[0] - at[p][0], dy = pos[1] - at[p][1];
        static int log = -1; /* tests: TM2PC_UNSTICK_LOG=1, each player's distance every 45 VBlanks */
        if (log < 0) log = getenv("TM2PC_UNSTICK_LOG") != NULL;
        if (log) fprintf(stderr, "unstick: p%d moved %.0f\n", p + 1, sqrt(dx * dx + dy * dy));
        if (dx * dx + dy * dy < 120.0 * 120.0 && (bits & 0x4000)) { /* (stuck while the script drives) */
            int same_spot = turn_until[p] && M.vblanks - turn_until[p] < UNSTICK_SAME_SPOT;
            tries[p] = same_spot ? tries[p] + 1 : 0;
            if (!same_spot || tries[p] % UNSTICK_TRIES == 0) side[p] = !side[p];
            steer = side[p] ? 0x0080 : 0x0020;
            back_until[p] = M.vblanks + UNSTICK_BACK;
            turn_until[p] = back_until[p] + UNSTICK_TURN;
            memcpy(at[p], pos, sizeof pos);
            since[p] = turn_until[p];
            fprintf(stderr, "trailer: player %d stuck (moved %.0f in 45 VBlanks), backs out to the %s (try %d)\n", p + 1,
                    sqrt(dx * dx + dy * dy), side[p] ? "left" : "right", tries[p] + 1, (unsigned long long)in_battle);
            return last[p] = (uint16_t)(0x0040 | 0x0200 | steer);
        }
        memcpy(at[p], pos, sizeof pos);
        since[p] = M.vblanks;
    }
    return last[p] = bits;
}

/* Tests: TM2PC_PAD1_SCRIPT / TM2PC_PAD2_SCRIPT = "V:cross+up,V2:-,..." holds those buttons on the
 * game's pad from VBlank V until the next entry ("-" none): presses inside a battle. */
uint16_t pad_script_bits(int p) /* (players 3-4: vplus/humans.c) */
{
    static const char *scr[4];
    static int read;
    if (!read) {
        read = 1;
        scr[0] = getenv("TM2PC_PAD1_SCRIPT"); scr[1] = getenv("TM2PC_PAD2_SCRIPT");
        scr[2] = getenv("TM2PC_PAD3_SCRIPT"); scr[3] = getenv("TM2PC_PAD4_SCRIPT"); /* (tests: players 3-4, humans.c) */
    }
    const char *s = scr[p & 3];
    if (!s) return 0;
    /* (trailer takes: TM2PC_PAD_SCRIPT_BATTLE=1, the VBlanks counted from each battle's start, nothing held outside a
     * battle: the player's own menus take as long as they take) */
    static int per_battle = -1;
    static uint64_t battle_from;
    static int was_in;
    if (per_battle < 0) per_battle = getenv("TM2PC_PAD_SCRIPT_BATTLE") != NULL;
    uint64_t now = M.vblanks;
    if (per_battle) {
        int feats_in_battle(void);
        int in = feats_in_battle();
        if (in && !was_in) battle_from = M.vblanks;
        was_in = in;
        if (!in) return 0;
        now = M.vblanks - battle_from;
    }
    /* each script read once into (VBlank, buttons) pairs: read again every VBlank for four players, the trailer
     * takes' long scripts took a third of the frame (the user's 4-player take ran at 89% speed, 2026-11-15) */
    static struct { uint64_t v; uint16_t b; } *ent[4];
    static int n_ent[4] = {-1, -1, -1, -1};
    if (n_ent[p & 3] < 0) {
        static const struct { const char *n; uint16_t b; } names[] = {
            {"select", 0x1}, {"start", 0x8}, {"up", 0x10}, {"right", 0x20}, {"down", 0x40}, {"left", 0x80},
            {"l2", 0x100}, {"r2", 0x200}, {"l1", 0x400}, {"r1", 0x800}, {"triangle", 0x1000}, {"circle", 0x2000},
            {"cross", 0x4000}, {"square", 0x8000}};
        int n = 0, cap = 0;
        while (*s) {
            unsigned long long v = strtoull(s, (char **)&s, 10);
            if (*s != ':') break;
            s++;
            uint16_t b = 0;
            while (*s && *s != ',') {
                size_t len = strcspn(s, "+,");
                for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
                    if (strlen(names[i].n) == len && !strncmp(s, names[i].n, len)) b |= names[i].b;
                s += len;
                if (*s == '+') s++;
            }
            if (n == cap) ent[p & 3] = realloc(ent[p & 3], sizeof *ent[0] * (size_t)(cap = cap ? cap * 2 : 64));
            ent[p & 3][n].v = v, ent[p & 3][n].b = b, n++;
            if (*s == ',') s++;
        }
        n_ent[p & 3] = n;
    }
    uint16_t bits = 0;
    for (int i = 0; i < n_ent[p & 3]; i++) /* (the last entry at or before now, as the text was read) */
        if (ent[p & 3][i].v <= now) bits = ent[p & 3][i].b;
    if (per_battle) bits = pad_script_unstick(p, bits, now);
    return bits;
}

uint32_t killcam_input(int kind, int player, uint32_t live);
static void update_pads(void)
{
    CPU *c = &M.cpu;
    if (M.live_input)
        input_poll(); /* keyboard/gamepads, presented as digital pads (all TM2 accepts) */
    if (rt_pad_filter)
        for (int p = 0; p < 2; p++) M.pad_buttons[p] = rt_pad_filter(p, M.pad_buttons[p]);
    for (int p = 0; p < 2; p++) M.pad_buttons[p] &= (uint16_t)~(pad_script_bits(p) | rt_pad_tool[p]); /* tests (active-low) */
    int humans_on(void), feats_in_battle(void), netplay_in_match(void);
    if (!humans_on() || !feats_in_battle()) /* (in a battle every human's input goes through vplus/humans.c) */
        for (int p = 0; p < 2; p++) M.pad_buttons[p] = (uint16_t)killcam_input(0, p, M.pad_buttons[p]); /* the kill cam's replay */
    else if (netplay_in_match())
        for (int p = 0; p < 2; p++) M.pad_buttons[p] |= 0x0008; /* (online: no pause from a pad; nobody pauses everyone) */
    for (int p = 0; p < 2; p++) {
        uint32_t b = M.pad_buf[p];
        if (!b) continue;
        if (M.pad_connected[p]) {
            mem_w8(c, b + 0, 0x00);                 /* status: OK */
            mem_w8(c, b + 1, 0x41);                 /* digital pad, 1 halfword */
            mem_w8(c, b + 2, M.pad_buttons[p] & 0xFF);
            mem_w8(c, b + 3, M.pad_buttons[p] >> 8);
        } else {
            mem_w8(c, b + 0, 0xFF);                 /* nothing connected */
        }
    }
}

static void cd_report(void);
void vp_vblank(void);   /* vplus/fixes.c: timed Vanilla Plus changes */
void ui_vblank(void);   /* ui/options.c: the Options pages (F1) */

/* One VBlank of emulated time. */
void netplay_poll(void); /* net/game_tm2.c: a server's packets */
int menu_fiber_inside(void);  /* ui/menu_fiber.c: the online Options pages' coroutine */
void menu_fiber_yield(void);
void rt_vblank(void)
{
    if (menu_fiber_inside()) { menu_fiber_yield(); return; } /* (a page's wait for a VBlank: the game's own VBlank runs it) */
    M.vblanks++;
    netplay_poll();
    update_pads();
    M.clock_acc += CLOCK_TICKS_PER_VBLANK;
    while (M.clock_acc >= 1.0) {
        M.clock_acc -= 1.0;
        M.clock_ticks++;
        rt_raise_irq(IRQ_TMR2);
    }
    rt_raise_irq(IRQ_VBLANK);
    audio_volumes_vblank();
    cd_audio_skip_vblank();
    hw_wav_vblank();
    extern uint64_t gpu_dma_ns; /* runtime/gpu.c */
    uint64_t t0 = SDL_GetTicksNS();
    gpu_backend->vblank();
    uint64_t t1 = SDL_GetTicksNS();
    cd_report();
    vp_vblank();
    ui_vblank();
    uint64_t t2 = SDL_GetTicksNS();
    rt_pace_vblank();
    uint64_t t3 = SDL_GetTicksNS();
    /* a VBlank that came much later than the one before (tests: TM2PC_FRAME_TIMES=ms, each one; else 100 ms, logged in
     * every build at most 20 times a minute: the user's Amazonia slowdowns, not seen in a test, 2026-11-03; a loading
     * screen shows too, as one long VBlank) */
    static int frame_times = -1, in_minute;
    static uint64_t last_ns, minute_at;
    if (frame_times < 0) frame_times = getenv("TM2PC_FRAME_TIMES") ? atoi(getenv("TM2PC_FRAME_TIMES")) : 0;
    uint64_t now = SDL_GetTicksNS();
    if (last_ns && now - last_ns > (uint64_t)(frame_times > 0 ? frame_times : 100) * 1000000u) {
        if (now - minute_at > 60000000000ull) minute_at = now, in_minute = 0;
        if (frame_times > 0 || in_minute++ < 20)
            fprintf(stderr, "frame time: VBlank %llu took %.1f ms (the game %.1f of it drawing lists %.1f, the GPU's VBlank %.1f, the port's %.1f, pacing %.1f)\n",
                    (unsigned long long)M.vblanks, (now - last_ns) / 1e6, last_ns ? (t0 - last_ns) / 1e6 : 0.0, gpu_dma_ns / 1e6, (t1 - t0) / 1e6,
                    (t2 - t1) / 1e6, (t3 - t2) / 1e6);
    }
    last_ns = now;
    gpu_dma_ns = 0;
}

/* libetc VSync(mode): -1 VBlank count, 1 time since the last VBlank (no wait),
 * 0 wait for the next VBlank, n>1 wait until n VBlanks after the last call. */
void hle_VSync(CPU *c)
{
    int32_t mode = (int32_t)c->r[4];
    if (mode < 0) {
        c->r[2] = (uint32_t)(M.vblanks - M.vblank_offset);
        return;
    }
    if (mode == 1) {
        c->r[2] = 0;
        return;
    }
    void interp_vsync(void); /* vplus/interp.c: an in-between frame drawn for this VBlank */
    if (mode == 0) interp_vsync();
    uint64_t target = mode == 0 ? M.vblanks + 1 : M.last_vsync + (uint64_t)mode;
    if (target <= M.vblanks)
        target = M.vblanks + (mode == 0);
    while (M.vblanks < target)
        rt_vblank();
    M.last_vsync = M.vblanks;
    c->r[2] = 0;
}

/* Pad decoder hook (8014A31C). Some screens (pause) wait for a button change
 * in a loop that polls the pads without calling VSync. On hardware the BIOS
 * refreshes the pads every VBlank in the background; here time only moves in
 * VSync. So when the decoder runs again with no VBlank in between, the game
 * is busy-waiting: let one VBlank pass. Normal frames poll once per VSync. */
void F_8014A31C_orig(CPU *c);
void hle_PadDecode(CPU *c)
{
    static uint64_t last_vblank = (uint64_t)-1;
    if (M.vblanks == last_vblank)
        rt_vblank();
    last_vblank = M.vblanks;
    F_8014A31C_orig(c);
}

/* Frame boundary hook around the game's SetFrameStart. */
void F_80155214_orig(CPU *c);
void profile_apply_unlocks(CPU *c);   /* profile/profile_hooks.c */
void feats_frame(void);                /* profile/feat_hooks.c */
void ui_frame_start(void);             /* ui/options.c: back to the game's picture */
void killcam_frame(int battle); /* vplus/killcam.c: the final kill cam's snapshots and replays */
int feats_in_battle(void);
void interp_frame_start(void); /* vplus/interp.c */
void netplay_frame(void);      /* net/game_tm2.c: online, the step waits for every machine's inputs */
void hle_SetFrameStart(CPU *c)
{
    void rbs_pose_check(int resim); /* net/rb_state.c: tests (TM2PC_NET_POSECHK) */
    if (feats_in_battle()) rbs_pose_check(rbs_resimulating);
    if (rbs_resimulating) { /* a step run again (rollback): only its own work, its input already written */
        void killcam_rerun_frame(void); /* vplus/killcam.c: its frame's hash again */
        killcam_rerun_frame();
        feats_frame();
        F_80155214_orig(c);
        return;
    }
    rbs_live_step();
    interp_frame_start();
    netplay_frame();
    int netplay_on(void), netplay_rollback(void); /* net/game_tm2.c */
    void mirrors_rear_pair(void);                  /* vplus/mirrors.c: the mirror's record for the frame drawn next */
    if (netplay_rollback() && netplay_on()) mirrors_rear_pair();
    killcam_frame(feats_in_battle());
    void humans_frame(void); /* vplus/humans.c: every human's input this step */
    humans_frame();
    profile_apply_unlocks(c);
    feats_frame();
    ui_frame_start();
    rt_frame_boundary();
    F_80155214_orig(c);
}

/* ---- libcd ---- */

static uint32_t cd_pos;       /* current location (LBA, without the 150-sector lead-in) */
static uint32_t cd_mode;      /* Setmode value */
static uint32_t cd_status = 0x02;
void cd_audio_play(uint32_t lba);   /* cd_audio.c */
int cd_audio_stop(void);            /* returns 1 if music was playing */
void cd_audio_resume(void);
uint32_t cd_audio_position(void);   /* sector being played */
int cd_audio_playing(void);
static int cd_paused;               /* Pause stopped the music; Play resumes it */
void cd_audio_mute(int on);
static uint8_t cd_result[8];        /* last command's response, as CdSync returns it */
static int cd_result_len = 1;

static uint8_t bcd(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static uint32_t unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
static uint32_t msf_to_lba(uint8_t m, uint8_t s, uint8_t f) { return (unbcd(m) * 60 + unbcd(s)) * 75 + unbcd(f) - 150; }
static void lba_to_msf(uint32_t lba, uint8_t *out)
{
    lba += 150;
    out[0] = bcd(lba / 4500); out[1] = bcd((lba / 75) % 60); out[2] = bcd(lba % 75);
}

static void put_result(CPU *c, uint32_t result, uint8_t status)
{
    if (result) mem_w8(c, result, status);
}

static void put_last_result(CPU *c, uint32_t result)
{
    for (int k = 0; result && k < cd_result_len; k++)
        mem_w8(c, result + (uint32_t)k, cd_result[k]);
}

void hle_CdInit(CPU *c) { cd_status = 0x02; c->r[2] = 1; }

void hle_CdSearchFile(CPU *c)
{
    uint32_t fp = c->r[4], name = c->r[5], lba, size;
    char path[64];
    int i = 0;
    for (; i < 63; i++)
        if (!(path[i] = (char)mem_r8(c, name + i))) break;
    path[i] = 0;
    if (!cd_find_file(path, &lba, &size)) {
        if (M.trace) fprintf(stderr, "CdSearchFile %s: not found\n", path);
        c->r[2] = 0;
        return;
    }
    uint8_t msf[3];
    lba_to_msf(lba, msf);
    mem_w8(c, fp + 0, msf[0]); mem_w8(c, fp + 1, msf[1]); mem_w8(c, fp + 2, msf[2]); mem_w8(c, fp + 3, 0);
    mem_w32(c, fp + 4, size);
    for (int k = 0; k < 16; k++)
        mem_w8(c, fp + 8 + k, k < i ? (uint8_t)path[k] : 0);
    if (M.trace) fprintf(stderr, "CdSearchFile %s: lba %u size %u\n", path, lba, size);
    c->r[2] = fp;
}

static void cd_command(CPU *c, uint32_t com, uint32_t param, uint32_t result)
{
    if (M.trace && com != 0x01)
        fprintf(stderr, "CdControl %02X param %08X [%02X %02X %02X %02X] (ra %08X)\n", com, param,
                param ? mem_r8(c, param) : 0, param ? mem_r8(c, param + 1) : 0,
                param ? mem_r8(c, param + 2) : 0, param ? mem_r8(c, param + 3) : 0, c->r[31]);
    cd_result_len = 1;
    switch (com) {
    case 0x02: /* Setloc */
        cd_pos = msf_to_lba(mem_r8(c, param), mem_r8(c, param + 1), mem_r8(c, param + 2));
        cd_paused = 0;
        break;
    case 0x03: /* Play (CD audio) from the given position; without one, from
                * Setloc's, or after Pause where the drive stopped */
        if (param) {
            cd_pos = msf_to_lba(mem_r8(c, param), mem_r8(c, param + 1), mem_r8(c, param + 2));
            cd_audio_play(cd_pos);
        } else if (cd_paused) {
            cd_audio_resume();
        } else {
            cd_audio_play(cd_pos);
        }
        cd_paused = 0;
        cd_status = 0x82;
        break;
    case 0x06: case 0x1B: cd_audio_stop(); cd_paused = 0; cd_status = 0x22; break; /* ReadN, ReadS */
    case 0x08: cd_audio_stop(); cd_paused = 0; cd_status = 0x02; break;           /* Stop */
    case 0x09: cd_paused = cd_audio_stop(); cd_status = 0x02; break;              /* Pause: keeps the position */
    case 0x0B: cd_audio_mute(1); break;                              /* Mute */
    case 0x0C: cd_audio_mute(0); break;                              /* Demute */
    case 0x0E: if (param) cd_mode = mem_r8(c, param); break; /* Setmode */
    case 0x11: { /* GetlocP: track, index, position in the track, absolute position.
                  * TM2 polls this while music plays and saves it on pause; unpause
                  * plays from the saved position. */
        uint32_t lba = cd_audio_position();
        int t = 0;
        while (t + 1 < cd_track_count && lba >= cd_tracks[t + 1].start) t++;
        uint8_t rel[3], abs[3];
        lba_to_msf(lba, abs);
        lba_to_msf(lba - cd_tracks[t].start - 150, rel); /* relative: no lead-in */
        cd_result[0] = bcd((uint32_t)t + 1); cd_result[1] = 1;
        memcpy(cd_result + 2, rel, 3); memcpy(cd_result + 5, abs, 3);
        cd_result_len = 8;
        put_last_result(c, result);
        return;
    }
    case 0x13: /* GetTN: first, last track */
        if (result) { mem_w8(c, result + 1, bcd(1)); mem_w8(c, result + 2, bcd((uint32_t)cd_track_count)); }
        break;
    case 0x14: { /* GetTD: start of a track (0 = end of disc) */
        uint32_t t = param ? unbcd(mem_r8(c, param)) : 0, lba;
        Track *last = &cd_tracks[cd_track_count - 1];
        lba = (t == 0 || t > (uint32_t)cd_track_count) ? last->start + last->sectors : cd_tracks[t - 1].start;
        uint8_t msf[3];
        lba_to_msf(lba, msf);
        if (result) { mem_w8(c, result + 1, msf[0]); mem_w8(c, result + 2, msf[1]); }
        break;
    }
    default:
        break; /* Nop, Init, Mute, Demute, Setfilter, Seek*, MotorOn, Standby: status only */
    }
    cd_result[0] = (uint8_t)cd_status;
    put_result(c, result, (uint8_t)cd_status);
}

void hle_CdControl(CPU *c) { cd_command(c, c->r[4] & 0xFF, c->r[5], c->r[6]); c->r[2] = 1; }

/* CD-DA reports. With Setmode Report (0x04) the drive interrupts with
 * DataReady about every 10 sectors while playing: stat, track, index, then
 * mm:ss:ff, absolute (ss bit 7 clear) or within the track. libcd passes them
 * to the CdReadyCallback (its pointer at 0x80176B9C). TM2 keeps the absolute
 * position from them: to loop the song and to resume it after pause. */
#define CD_READY_CALLBACK GA(0x80176B9Cu)
static void cd_report(void)
{
    if (!(cd_mode & 0x04) || M.in_interrupt || M.vblanks % 8 || !cd_audio_playing())
        return;
    CPU *c = &M.cpu;
    uint32_t cb = mem_r32(c, CD_READY_CALLBACK);
    if (!cb)
        return;
    uint32_t lba = cd_audio_position();
    int t = 0;
    while (t + 1 < cd_track_count && lba >= cd_tracks[t + 1].start) t++;
    uint8_t msf[3];
    lba_to_msf(lba, msf);
    uint32_t buf = (c->r[29] - 0x40) & ~7u; /* in the gap guest_call leaves below the stack */
    uint8_t rep[8] = { (uint8_t)cd_status, bcd((uint32_t)t + 1), 1, msf[0], msf[1], msf[2], 0, 0 };
    for (int k = 0; k < 8; k++)
        mem_w8(c, buf + (uint32_t)k, rep[k]);
    guest_call(cb, 1 /* CdlDataReady */, buf, 0, 0);
}

/* CdRead(sectors, buf, mode): reads now; CdReadSync then reports completion. */
void hle_CdRead(CPU *c)
{
    uint32_t n = c->r[4], buf = c->r[5], mode = c->r[6];
    int size = (mode & 0x20) ? 2340 : 2048;
    uint8_t sec[2048];
    for (uint32_t i = 0; i < n; i++, cd_pos++) {
        if (!cd_read_sector(cd_pos, sec)) {
            rt_unexpected("CdRead beyond data track", cd_pos, n);
            break;
        }
        if (size == 2048) {
            vp_disc_sector(cd_pos, sec); /* vplus/fixes.c: data fixed as it is read */
            for (int k = 0; k < 2048; k++) mem_w8(c, buf + k, sec[k]);
        } else {
            uint8_t raw[2352];
            cd_read_raw(cd_pos, raw);
            for (int k = 0; k < 2340; k++) mem_w8(c, buf + k, raw[12 + k]);
        }
        buf += (uint32_t)size;
    }
    cd_status = 0x22; /* motor on, reading: what the drive reports after a read */
    if (M.trace) fprintf(stderr, "CdRead %u sectors -> %08X\n", n, c->r[5]);
    c->r[2] = 1;
}

void hle_CdReadSync(CPU *c) { put_result(c, c->r[5], (uint8_t)cd_status); c->r[2] = 0; }
void hle_CdSync(CPU *c) { put_last_result(c, c->r[5]); c->r[2] = 2; /* CdlComplete */ }

/* CdGetToc(CdlLOC *loc): track start positions; returns the track count.
 * libcd builds it with GetTD, which gives minutes and seconds only, so the
 * frame is 0 (as retail shows: TM2's music range starts on a whole second,
 * and its loop check rejects positions before that). */
void hle_CdGetToc(CPU *c)
{
    uint32_t loc = c->r[4];
    for (int t = 0; t < cd_track_count; t++) {
        uint8_t msf[3];
        lba_to_msf(cd_tracks[t].start, msf);
        mem_w8(c, loc + 4 * (t + 1), msf[0]); mem_w8(c, loc + 4 * (t + 1) + 1, msf[1]);
        mem_w8(c, loc + 4 * (t + 1) + 2, 0); mem_w8(c, loc + 4 * (t + 1) + 3, bcd((uint32_t)t + 1));
    }
    c->r[2] = (uint32_t)cd_track_count;
}
