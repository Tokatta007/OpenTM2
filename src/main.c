/* tm2pc: Twisted Metal 2 recompiled for PC.
 *
 *   tm2pc DISC.cue [--headless] [--frames N] [--tape words.txt] [--trace] [--frame-log FILE]
 *
 * Loads the boot executable from the disc into RAM as the BIOS would and
 * runs the recompiled code from its entry point. By default it opens a
 * window (PSY-Z renderer) and runs at the PS1's 59.83 Hz; --headless runs
 * with no window and no pacing (null GPU backend: tests, fast runs).
 * --frames stops after N frame boundaries (SetFrameStart calls) and prints
 * a summary. --tape gives the decoded pad word per frame boundary (one hex
 * word per line, player 1 in the low half), as the harness tapes do;
 * --tape-vblank gives one per VBlank instead (for screens like the pause
 * menu, which poll the pads without reaching a frame boundary).
 * --frame-log writes VBlanks and game ticks per frame boundary. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>
#include "runtime/runtime.h"
#include "runtime/gpu_backend.h"
#include "vplus/vplus.h"
#include "music/music.h"
#include "settings.h"
#include "runtime/update.h"
#include "runtime/setup.h"
#include "assets/discfs.h"
#include "profile/profile.h"
#include "profile/feats.h"
#include "ui/ui.h"
#include "runtime/rebase_table.h"
#include "vplus/cars.h"
#include "mods/mods.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
/* a crash: its code and place (as offsets in the exe: llvm-addr2line / tools/perf/symtop.py name them), the exe's
 * addresses on the stack, and the last VBlank, in log.txt before Windows ends the game */
static LONG WINAPI crash_report(EXCEPTION_POINTERS *e)
{
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL), at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    uintptr_t size = nt->OptionalHeader.SizeOfImage;
    fprintf(stderr, "crash: exception %08lX at %s+%llx (VBlank %llu)\n", (unsigned long)e->ExceptionRecord->ExceptionCode,
            at - base < size ? "exe" : "?", (unsigned long long)(at - base < size ? at - base : at), (unsigned long long)M.vblanks);
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2)
        fprintf(stderr, "crash: %s %llx\n", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long long)e->ExceptionRecord->ExceptionInformation[1]);
    const uintptr_t *sp = (const uintptr_t *)e->ContextRecord->Rsp;
    fprintf(stderr, "crash: stack");
    for (int i = 0, n = 0; i < 512 && n < 24; i++)
        if (sp[i] - base < size) fprintf(stderr, " %llx", (unsigned long long)(sp[i] - base)), n++;
    fprintf(stderr, "\n");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
static uint64_t boundary, max_boundaries = 0, max_vblanks = 0;
static uint32_t *tape;
static size_t tape_len;
static int tape_by_vblank; /* --tape-vblank: one word per VBlank instead of per frame boundary */
static double start_time;
static FILE *frame_log;
static uint64_t last_boundary_vblank;
#define MAX_SHOTS 1024
static uint64_t shots[MAX_SHOTS];
static int shot_by_vblank[MAX_SHOTS]; /* "1700v": at a VBlank count instead of a frame boundary */
static int shot_count;
static const char *shot_dir = ".";
static const char *settings_path;   /* default: tm2pc.ini next to the executable */
static const char *ui_shot_arg;     /* --ui-shot page,row,WxH,file.bmp: draw an Options page and exit */
static int preset_arg = -1;         /* --preset: overrides the settings file */

extern uint64_t gpu_commands;
extern int64_t gpu_culled_prims;

static double now_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static void summary(void)
{
    double t = now_seconds() - start_time;
    fprintf(stderr, "frames %llu, vblanks %llu, clock ticks %llu, GPU commands %llu (size-limited %lld), "
            "unexpected %u, %.2f s (%s)\n",
            (unsigned long long)boundary, (unsigned long long)M.vblanks, (unsigned long long)M.clock_ticks,
            (unsigned long long)gpu_commands, (long long)gpu_culled_prims, M.unexpected, t, gpu_backend->name);
}

#ifdef RC_TRACE_ENTRY
uint32_t rc_trace_ring[256];
#ifdef RC_MEM_WATCH
uint32_t rc_watch_lo, rc_watch_n;
void rc_watch_hit(uint32_t a, uint32_t v, int size)
{
    static int hits;
    static long long from = -1; /* TM2PC_WATCH_FROM=V: only from VBlank V on (past the loading) */
    if (from < 0) from = getenv("TM2PC_WATCH_FROM") ? atoll(getenv("TM2PC_WATCH_FROM")) : 0;
    if (M.vblanks < (uint64_t)from) return;
    if (hits++ >= (getenv("TM2PC_WATCH_MAX") ? atoi(getenv("TM2PC_WATCH_MAX")) : 200)) return;
    fprintf(stderr, "watch: vblank %llu: %08X <- %0*X (%d) at host %p, after", (unsigned long long)M.vblanks, a, size * 2, v, size,
            __builtin_return_address(0));
    for (unsigned i = 1; i <= 4; i++) fprintf(stderr, " %08X", gr(rc_trace_ring[(rc_trace_pos - i) & 255]));
    fprintf(stderr, " (retail)\n");
}
#endif
unsigned rc_trace_pos;

/* Hang watchdog (trace builds): if no VBlank happens for 2 s, the game is
 * stuck in a loop that never waits; print the functions it last entered. */
static DWORD WINAPI trace_watchdog(LPVOID arg)
{
    (void)arg;
    uint64_t last = (uint64_t)-1;
    for (int still = 0;; Sleep(500)) {
        uint64_t v = M.vblanks;
        still = v == last ? still + 1 : 0;
        last = v;
        if (still == 4) {
            unsigned pos = rc_trace_pos;
            fprintf(stderr, "watchdog: no VBlank for 2 s at vblank %llu; last functions entered:\n",
                    (unsigned long long)v);
            for (unsigned i = 64; i > 0; i--)
                fprintf(stderr, "%08X%s", rc_trace_ring[(pos - i) & 255], i % 8 == 1 ? "\n" : " ");
            fflush(stderr);
            _exit(3);
        }
    }
    return 0;
}
#endif

static void set_pads_from_tape(void)
{
    if (!tape || !tape_len)
        return;
    uint64_t at = tape_by_vblank ? M.vblanks : boundary;
    uint32_t w = tape[at < tape_len ? at : tape_len - 1];
    /* The decoded word (8014A31C's output, what tapes record) is the raw pad
     * halfword inverted and byte-swapped: bit 0 = L2 .. bit 7 = Square,
     * bit 8 = Select .. bit 15 = Left. Raw pad data is active-low. */
    for (int p = 0; p < 2; p++) {
        uint16_t d = (uint16_t)(p ? w >> 16 : w);
        uint16_t raw = (uint16_t)~d;
        M.pad_buttons[p] = (uint16_t)((raw >> 8) | (raw << 8));
    }
}

static void take_shot(const char *kind, uint64_t n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s-%05llu.bmp", shot_dir, kind, (unsigned long long)n);
    int ok = gpu_backend->capture(path, 0);
    if (!getenv("TM2PC_SHOT_NO_VRAM")) { /* the whole VRAM too, unless asked not (many shots) */
        snprintf(path, sizeof path, "%s/%s-%05llu-vram.bmp", shot_dir, kind, (unsigned long long)n);
        ok &= gpu_backend->capture(path, 1);
    }
    fprintf(stderr, "screenshot at %s %llu: %s\n", kind, (unsigned long long)n, ok ? "saved" : "failed");
}

/* Tests: TM2PC_LIVE_SHOT=N, every N seconds of real time the screen as presented, menus included, to
 * <shot dir>/live.bmp (the one before: live-prev.bmp). Two files, overwritten: a long run takes no more
 * room. For watching a run from outside, also while the PC is locked (the desktop is not drawn then). */
void rt_live_shot(void)
{
    static int every = -1, burst_n = -1, burst_i;
    static time_t next, burst_at;
    if (burst_n < 0) { /* TM2PC_LIVE_BURST=T,N: N frames in a row as presented, T seconds in (the 60 FPS frames between steps) */
        const char *b = getenv("TM2PC_LIVE_BURST");
        int t = 0;
        burst_n = b && sscanf(b, "%d,%d", &t, &burst_n) == 2 ? burst_n : 0;
        burst_at = time(NULL) + t;
    }
    if (burst_n > 0 && burst_i < burst_n && time(NULL) >= burst_at) {
        char path[512];
        snprintf(path, sizeof path, "%s/burst-%03d.bmp", shot_dir, burst_i++);
        gpu_backend->capture(path, 0);
    }
    if (every < 0) every = getenv("TM2PC_LIVE_SHOT") ? atoi(getenv("TM2PC_LIVE_SHOT")) : 0;
    if (every <= 0 || time(NULL) < next) return;
    next = time(NULL) + every;
    char path[512], prev[512];
    snprintf(path, sizeof path, "%s/live.bmp", shot_dir);
    snprintf(prev, sizeof prev, "%s/live-prev.bmp", shot_dir);
    remove(prev);
    rename(path, prev);
    gpu_backend->capture(path, 0);
}

static uint32_t rb_where(uint32_t a) /* a retail address in a moved array: where it is now */
{
    if (rc_addrmap_count) return ga(a); /* the decomp's port variant: its address map */
    for (int i = 0; i < RB_ARRAY_COUNT; i++)
        if (a >= rb_arrays[i].old && a < rb_arrays[i].old + rb_arrays[i].old_size) return a - rb_arrays[i].old + rb_arrays[i].addr;
    return a;
}

static uint32_t rb_back(uint32_t w) /* a pointer into a moved array: where it was in retail (other words as they are) */
{
    if (w < 0x80000000u || w >= 0x80000000u + GUEST_RAM_BYTES) return w;
    if (rc_addrmap_count) return gr(w);
    for (int i = 0; i < RB_ARRAY_COUNT; i++)
        if (w >= rb_arrays[i].addr && w < rb_arrays[i].addr + rb_arrays[i].old_size) return w - rb_arrays[i].addr + rb_arrays[i].old;
    return w;
}

/* Tests (TM2PC_PACKET_PEAK=1): the most packet bytes a frame wrote in one buffer. The views of a frame carry one
 * cursor on through a buffer (frame records: +4 start, +8 the cursor, +0xC the end): the frame records of views 1-2
 * and the mirror (renderView0Frames 8018118C, six of 0x80) and views 3-4's (0x80DD0100.., four). Logged as it grows. */
static void packet_peak(void)
{
    static int on = -1;
    static uint32_t peak, peak_size;
    if (on < 0) on = getenv("TM2PC_PACKET_PEAK") != NULL;
    if (!on) return;
    uint32_t recs[10];
    for (uint32_t k = 0; k < 6; k++) recs[k] = ga(0x8018118Cu) + 0x80u * k; /* retail-address */
    for (uint32_t k = 0; k < 4; k++) recs[6 + k] = 0x80DD0100u + 0x80u * k;
    for (int k = 0; k < 10; k++) {
        uint32_t start = mem_r32(&M.cpu, recs[k] + 4), cur = mem_r32(&M.cpu, recs[k] + 8), end = mem_r32(&M.cpu, recs[k] + 0xC);
        if (start < 0x80000000u || cur < start || end <= start || cur > end + 80) continue;
        if (cur - start > peak) {
            int log = cur - start > peak + peak / 50 || !peak;
            peak = cur - start;
            peak_size = end - start;
            if (log) fprintf(stderr, "packets: peak %u bytes of the buffer's %u (vblank %llu)\n", peak, peak_size,
                             (unsigned long long)M.vblanks);
        }
    }
}

void port_config_sync(void); /* vplus/port_config.c */
void mod_cars_carsel_frame(void); /* vplus/mod_cars.c: car select's turning mod cars (decomp request 71, CAR_PROFILES.md) */
void rt_frame_boundary(void)
{
    packet_peak();
    port_config_sync();
    mod_cars_carsel_frame();
    { /* tests: TM2PC_WATCH8=hex address, each change of that byte (per frame boundary) */
        static uint32_t watch = 1;
        static int last = -1;
        if (watch == 1) watch = getenv("TM2PC_WATCH8") ? (uint32_t)strtoul(getenv("TM2PC_WATCH8"), NULL, 16) : 0;
        if (watch && mem_r8(&M.cpu, watch) != last) {
            last = mem_r8(&M.cpu, watch);
            fprintf(stderr, "watch %08x = %d at vblank %llu\n", watch, last, (unsigned long long)M.vblanks);
        }
    }
    if (frame_log) /* per boundary: VBlanks since the last one, ticks the game measured */
        fprintf(frame_log, "%llu %llu %u\n", (unsigned long long)boundary,
                (unsigned long long)(M.vblanks - last_boundary_vblank), mem_r16(&M.cpu, GA(0x8017EBF8u)));
    last_boundary_vblank = M.vblanks;
    static FILE *state_log; /* tests: TM2PC_STATE_LOG=file, a hash of the cars' records and the game's rand state per frame */
    static int state_log_read;
    if (!state_log_read) { /* (TM2PC_STATE_LOG_NOFRAMES: the file named for the network's logs only: lighter) */
        state_log_read = 1;
        if (getenv("TM2PC_STATE_LOG") && !getenv("TM2PC_STATE_LOG_NOFRAMES")) state_log = fopen(getenv("TM2PC_STATE_LOG"), "w");
    }
    if (state_log) {
        uint32_t h = 2166136261u;
        for (uint32_t a = 0x80187B10u; a < 0x8018D674u; a += 4) { /* by retail address, pointers back at retail's */ /* retail-address */
            uint32_t w = rb_back(mem_r32(&M.cpu, rb_where(a)));
            if (a < 0x80187B10u + 2 * HUMAN_REC_SIZE) w = human_rec_state_word((a - 0x80187B10u) % HUMAN_REC_SIZE, w); /* retail-address */
            h = (h ^ w) * 16777619u;
        }
        { /* humans 3.. (the variant's records past retail's two), the same way */
            uint32_t total = mem_r32(&M.cpu, GA(0x80181014u)); /* humanCarTotal */
            for (uint32_t i = 2; i < total && i < 16; i++)
                for (uint32_t o = 0; o < HUMAN_REC_SIZE; o += 4)
                    h = (h ^ human_rec_state_word(o, rb_back(mem_r32(&M.cpu, HUMAN_REC(i) + o)))) * 16777619u;
        }
        /* the car-pair tables (8018D674..8018F458) pair by pair, 15 x 15 slots: rows of rc_car_pair_stride in the build
         * (the 16-car variant's 64; the same values for the same pairs, decomp CAR_GROUPS.md 7.6) */
        static const struct { uint32_t at, size; } pairs[5] = {{0x8018D674u, 12}, {0x8018E100u, 4}, {0x8018E484u, 12},
                                                               {0x8018EF10u, 2}, {0x8018F0D4u, 4}}; /* retail-address */
        for (int t = 0; t < 5; t++)
            for (uint32_t i = 0; i < 15; i++)
                for (uint32_t j = 0; j < 15; j++) {
                    uint32_t e = rb_where(pairs[t].at) + (i * rc_car_pair_stride + j) * pairs[t].size;
                    for (uint32_t b = 0; b < pairs[t].size; b += 2) h = (h ^ mem_r16(&M.cpu, e + b)) * 16777619u;
                }
        static int full = -1; /* TM2PC_STATE_FULL=1: every byte of the 8 MB RAM but the one function the decomp's source
                               * build compiles differently (8015BD70..8015C2B0: the comparison with a port built from it) */
        if (full < 0) full = getenv("TM2PC_STATE_FULL") != NULL;
        if (full) {
            const uint8_t *r = M.ram;
            for (uint32_t o = 0; o < GUEST_RAM_BYTES; o += 4) {
                if (o >= 0x15BD70u && o < 0x15C2B0u) { o = 0x15C2ACu; continue; }
                /* what differs from run to run anyway: the sound's state (its instance pool, the libsnd voices: the
                 * audio's real time) and the stack's unused bottom (interrupts) */
                if (o >= 0x17F400u && o < 0x180300u) { o = 0x1802FCu; continue; }
                if (o >= 0x17E100u && o < 0x17E200u) { o = 0x17E1FCu; continue; } /* libspu's transfer state */
                if (o >= 0x1A5200u && o < 0x1A5B00u) { o = 0x1A5AFCu; continue; }
                if (o >= 0x1FE000u && o < 0x200000u) { o = 0x1FFFFCu; continue; }
                uint32_t w;
                memcpy(&w, r + o, 4);
                h = (h ^ w) * 16777619u;
            }
        }
        fprintf(state_log, "%llu %llu %08x", (unsigned long long)boundary, (unsigned long long)M.vblanks, h);
        if (getenv("TM2PC_STATE_BLOCKS")) /* tests: a hash per 64 KB of the 8 MB too (which parts differ between runs) */
            for (uint32_t blk = 0; blk < 128; blk++) {
                uint32_t bh = 2166136261u, w;
                for (uint32_t o = blk << 16; o < (blk + 1) << 16; o += 4) { memcpy(&w, M.ram + o, 4); bh = (bh ^ w) * 16777619u; }
                fprintf(state_log, " %08x", bh);
            }
        fputc('\n', state_log);
        if (getenv("TM2PC_STATE_WORDS") && boundary == strtoull(getenv("TM2PC_STATE_WORDS"), NULL, 10)) { /* tests: the hashed
             * words at that frame, by retail address (pointers back at retail's): two builds' files diff line by line */
            char path[1024];
            snprintf(path, sizeof path, "%s.words", getenv("TM2PC_STATE_LOG"));
            FILE *d = fopen(path, "w");
            for (uint32_t a = 0x80187B10u; d && a < 0x8018D674u; a += 4) /* retail-address */
                fprintf(d, "%08X %08X\n", a, rb_back(mem_r32(&M.cpu, rb_where(a))));
            for (int t = 0; d && t < 5; t++) /* the pair tables, pair by pair (above) */
                for (uint32_t i = 0; i < 15; i++)
                    for (uint32_t j = 0; j < 15; j++) {
                        uint32_t e = rb_where(pairs[t].at) + (i * rc_car_pair_stride + j) * pairs[t].size;
                        fprintf(d, "pair%d %2u %2u", t, i, j);
                        for (uint32_t b = 0; b < pairs[t].size; b += 2) fprintf(d, " %04X", mem_r16(&M.cpu, e + b));
                        fputc('\n', d);
                    }
            if (d) fclose(d);
        }
        if (getenv("TM2PC_STATE_DUMP") && boundary == strtoull(getenv("TM2PC_STATE_DUMP"), NULL, 10)) { /* tests: the RAM at that frame */
            char path[1024];
            snprintf(path, sizeof path, "%s.ram", getenv("TM2PC_STATE_LOG"));
            FILE *d = fopen(path, "wb");
            if (d) { fwrite(M.ram, 1, GUEST_RAM_BYTES, d); fclose(d); }
        }
    }
    for (int i = 0; i < shot_count; i++)
        if (!shot_by_vblank[i] && shots[i] == boundary)
            take_shot("frame", boundary);
    boundary++;
    set_pads_from_tape();
    if (M.trace && boundary % 60 == 0)
        fprintf(stderr, "frame %llu (vblank %llu)\n", (unsigned long long)boundary, (unsigned long long)M.vblanks);
    if (max_boundaries && boundary >= max_boundaries) {
        summary();
        exit(M.unexpected ? 2 : 0);
    }
}

unsigned long long rt_vblanks_now(void) { return (unsigned long long)M.vblanks; } /* (tests: gpu_psyz.c's logs) */
int rt_fast_forward; /* vplus/killcam.c: photo mode rewind seeking: the frames as fast as they run */
void rt_pace_wait(void);
void rt_pace_vblank(void)
{
    if (tape_by_vblank)
        set_pads_from_tape();
    for (int i = 0; i < shot_count; i++)
        if (shot_by_vblank[i] && shots[i] == M.vblanks)
            take_shot("vblank", M.vblanks);
    if (max_vblanks && M.vblanks >= max_vblanks) {
        summary();
        exit(M.unexpected ? 2 : 0);
    }
    if (M.headless || rt_fast_forward)
        return;
    /* tests: TM2PC_BENCH=V0,V1, no pacing (as fast as the PC runs it) and the speed from VBlank V0 to V1 logged
     * ("bench: N VBlanks in S s = R per second"; 60 is real time): the performance pass's measure, 2026-11-15 */
    static int bench = -1, paced; /* (TM2PC_BENCH=V0,V1,p: paced as in play, the speed measured all the same) */
    static unsigned long long b0, b1;
    static double t0;
    if (bench < 0) {
        char p = 0;
        bench = getenv("TM2PC_BENCH") && sscanf(getenv("TM2PC_BENCH"), "%llu,%llu,%c", &b0, &b1, &p) >= 2;
        paced = p == 'p';
    }
    if (bench) {
        if (M.vblanks == b0) t0 = now_seconds();
        if (M.vblanks == b1 && t0 > 0) {
            double s = now_seconds() - t0;
            fprintf(stderr, "bench: %llu VBlanks in %.2f s = %.1f per second\n", b1 - b0, s, (double)(b1 - b0) / s);
        }
        if (!paced) return;
    }
    rt_pace_wait();
}
/* the wait to the next VBlank's time (also an extra frame's: the kill cam's slow shot, vplus/interp.c, so the VBlanks
 * after it are not hurried to catch up and shown in the same refresh) */
void rt_pace_wait(void)
{
    static double next;
    static int slow = -1; /* tests (TM2PC_TEST_SLOW_MS=N): N ms more work each VBlank, a slower PC (over 16: under 60 FPS) */
    if (slow < 0) slow = getenv("TM2PC_TEST_SLOW_MS") ? atoi(getenv("TM2PC_TEST_SLOW_MS")) : 0;
#ifdef _WIN32
    if (slow) Sleep((DWORD)slow);
#endif
    double t = now_seconds();
    if (next == 0 || t - next > 0.25)
        next = t;
    next += 1.0 / NTSC_VBLANK_HZ;
    int interp_extra(double left); /* vplus/interp.c: uncapped, an in-between frame drawn and shown now */
    while ((t = now_seconds()) < next) {
        if (interp_extra((next - t) * NTSC_VBLANK_HZ)) continue;
#ifdef _WIN32
        if (next - t > 0.002) Sleep(1);
#endif
    }
}

static int load_tape(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t cap = 1024;
    tape = malloc(cap * sizeof *tape);
    char line[64];
    while (fgets(line, sizeof line, f)) {
        if (tape_len == cap) tape = realloc(tape, (cap *= 2) * sizeof *tape);
        tape[tape_len++] = (uint32_t)strtoul(line, 0, 16);
    }
    fclose(f);
    for (size_t i = 0; i < tape_len; i++) /* player 2 is plugged in if the tape uses it */
        if (tape[i] >> 16) M.pad_connected[1] = 1;
    return 1;
}

/* Errors a player needs to see when there is no console window. */
static void error_box(const char *msg)
{
#ifdef _WIN32
    if (!M.headless)
        MessageBoxA(NULL, msg, "OpenTM2", MB_OK | MB_ICONERROR);
#else
    (void)msg;
#endif
}

/* The disc image before the game starts, with what is wrong in words a player can act on (0: it is the retail USA
 * disc): not there, a .bin its .cue lists missing, not a disc image, or another disc (a prototype, another region,
 * another game). The retail SCUS_943.06 is 737,280 bytes; the port is built from that executable and no other. */
#define RETAIL_EXE_SIZE 737280u
int cue_bin_fallback(const char *cue, char *out, size_t cap); /* runtime/cd.c */
static int disc_problem(const char *disc, char *msg, size_t cap)
{
    FILE *f = fopen(disc, "rb");
    if (!f) {
        snprintf(msg, cap, "The disc image isn't there:\n%s\n\nThe setup opens next: choose your Twisted Metal 2 (USA) "
                 ".cue file there.", disc);
        return 1;
    }
    char line[1024];
    const char *ext = strrchr(disc, '.');
    int is_cue = ext && !SDL_strcasecmp(ext, ".cue");
    while (is_cue && fgets(line, sizeof line, f)) { /* every .bin the .cue lists, beside it */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *q1 = SDL_strncasecmp(p, "FILE", 4) ? NULL : strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!q1 || !q2) continue;
        *q2 = 0;
        const char *slash = strrchr(disc, '/'), *back = strrchr(disc, '\\');
        if (back > slash) slash = back;
        char path[1200], alt[1100];
        snprintf(path, sizeof path, "%.*s%s", slash ? (int)(slash - disc + 1) : 0, disc, q1 + 1);
        FILE *b = fopen(path, "rb");
        if (b) { fclose(b); continue; }
        if (cue_bin_fallback(disc, alt, sizeof alt)) continue;
        fclose(f);
        snprintf(msg, cap, "A file of the disc image is missing:\n%s\n\nExtract the .cue and all its .bin files into one "
                 "folder, then choose the .cue again.", q1 + 1);
        return 1;
    }
    fclose(f);
    DiscFs *fs = discfs_open(disc);
    if (!fs) {
        snprintf(msg, cap, "This file can't be read as a PlayStation disc image:\n%s\n\nOpenTM2 needs the Twisted Metal 2 "
                 "(USA) disc as a .cue with its .bin files.", disc);
        return 1;
    }
    int i = discfs_find(fs, "/SCUS_943.06");
    uint32_t size = i >= 0 ? discfs_size(fs, i) : 0;
    discfs_close(fs);
    if (size != RETAIL_EXE_SIZE) {
        snprintf(msg, cap, "This isn't the retail Twisted Metal 2 (USA) disc (SCUS-94306):\n%s\n\nOpenTM2 only works with "
                 "that version: a prototype, a demo, another region or another game won't run. The setup opens next: choose "
                 "the USA disc's .cue there.", disc);
        return 1;
    }
    return 0;
}

extern const unsigned rc_exe_image_size, rc_port_section_count; /* gen/exe_image.c */
extern const uint8_t rc_exe_image[];
extern const struct { uint32_t addr, size, has_data; const uint8_t *data; } rc_port_sections[];
static int load_boot_exe(void)
{
    uint32_t lba, size;
    if (!cd_find_file("\\SCUS_943.06;1", &lba, &size)) {
        fprintf(stderr, "boot file SCUS_943.06 not found on the disc\n");
        return 0;
    }
    uint8_t *exe = malloc((rc_exe_image_size > size ? rc_exe_image_size : size) + 2048);
    for (uint32_t s = 0; s * 2048 < size; s++)
        cd_read_sector(lba + s, exe + s * 2048);
    if (rc_exe_image_size) { /* a build at a new layout (the decomp's port variant): its own image, not the disc's */
        memcpy(exe, rc_exe_image, rc_exe_image_size);
        fprintf(stderr, "boot: the build's own executable image (%u bytes)\n", rc_exe_image_size);
    }
    if (memcmp(exe, "PS-X EXE", 8)) {
        fprintf(stderr, "boot file is not a PS-X EXE\n");
        return 0;
    }
    uint32_t pc0, gp0, t_addr, t_size, sp0;
    memcpy(&pc0, exe + 0x10, 4); memcpy(&gp0, exe + 0x14, 4);
    memcpy(&t_addr, exe + 0x18, 4); memcpy(&t_size, exe + 0x1C, 4);
    memcpy(&sp0, exe + 0x30, 4);
    memcpy(M.ram + (t_addr & 0x1FFFFF), exe + 0x800, t_size);
    for (unsigned i = 0; i < rc_port_section_count; i++) { /* its port sections: data, or zeroed */
        uint32_t a = rc_port_sections[i].addr & (GUEST_RAM_BYTES - 1), n = rc_port_sections[i].size;
        if (rc_port_sections[i].has_data) memcpy(M.ram + a, rc_port_sections[i].data, n);
        else memset(M.ram + a, 0, n);
    }
    M.cpu.pc = pc0;
    M.cpu.r[28] = gp0;
    M.cpu.r[29] = sp0 ? sp0 : 0x801FFFF0u;
    M.cpu.r[30] = M.cpu.r[29];
    free(exe);
    return 1;
}

/* The game is a windowed program, with no console window of its own.
 * Started from a terminal it prints there; with its output redirected
 * (tests) it writes to that; started from Explorer or a shortcut, its
 * messages go to tm2pc.log next to the executable. */
static void setup_output(int prepare)
{
#ifdef _WIN32
    /* Started by RecompLauncher (RECOMPLAUNCHER set): the log still goes to tm2pc.log (players send it with a bug);
     * stdout stays the launcher's when it reads it (--prepare's progress lines). --prepare logs to tm2pc-setup.log. */
    int launcher = getenv("RECOMPLAUNCHER") != NULL;
    if (!launcher && GetFileType(GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_UNKNOWN)
        return;
    if (!launcher && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        return;
    }
    char path[1100];
    DWORD n = GetModuleFileNameA(0, path, 1024);
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    strcpy(path + n, prepare ? "tm2pc-setup.log" : "tm2pc.log");
    if (!prepare) { /* the session before kept as tm2pc-prev.log (a freeze or a crash: its log survives the restart) */
        char prev[1100];
        snprintf(prev, sizeof prev, "%.*stm2pc-prev.log", (int)n, path);
        MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    }
    int keep_stdout = launcher && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN;
    if (freopen(path, "w", stderr)) {
        setvbuf(stderr, NULL, _IONBF, 0);
        if (!keep_stdout) freopen(path, "a", stdout);
    }
#else
    (void)prepare;
#endif
}

/* The freeze watchdog: no frame shown for 10 seconds (the game's or a menu's) and the log says so, from the first frame on, with the guest's
 * registers as they were last written (recompiled code: r31 the last call's return, a0-a3 its arguments), once a freeze
 * (the user's Gun Game froze at a respawn, 2026-10-29, the music playing on). */
volatile unsigned rt_heartbeat;
static int SDLCALL watchdog(void *unused)
{
    (void)unused;
    unsigned seen = rt_heartbeat;
    int still = 0;
    for (;;) {
        SDL_Delay(1000);
        unsigned now = rt_heartbeat;
        if (!now) continue; /* (no frame yet: the setup window, the first-time pictures; their minutes no freeze) */
        if (now != seen) { seen = now; if (still >= 10) fprintf(stderr, "watchdog: frames again after %d s\n", still); still = 0; continue; }
        extern volatile uint32_t gpu_walking; /* runtime/gpu.c */
        if (++still == 10)
            fprintf(stderr, "watchdog: FROZEN, no frame for 10 s (vblank %llu; ra %08x a0 %08x a1 %08x a2 %08x a3 %08x sp %08x; "
                            "walking order table %08x)\n", (unsigned long long)M.vblanks, M.cpu.r[31], M.cpu.r[4], M.cpu.r[5],
                    M.cpu.r[6], M.cpu.r[7], M.cpu.r[29], gpu_walking);
    }
    return 0;
}

/* SDL's and PSY-Z's warnings and errors (as "no swapchain") into the log too: SDL writes them only to a console or a
 * debugger, so a player's tm2pc.log had none. At most 50 (a warning each frame stays small). */
static SDL_LogOutputFunction sdl_log_was;
static void *sdl_log_was_data;
static void SDLCALL sdl_log_too(void *data, int category, SDL_LogPriority priority, const char *message)
{
    static int logged;
    if (priority >= SDL_LOG_PRIORITY_WARN && logged < 50) {
        fprintf(stderr, "sdl: %s%s\n", message, ++logged == 50 ? " (no more SDL messages logged)" : "");
    }
    if (sdl_log_was) sdl_log_was(sdl_log_was_data, category, priority, message);
}

int main(int argc, char **argv)
{
    int prepare = 0; /* --prepare: the first-time setup without a window (RecompLauncher's setup step), then exit */
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--prepare")) prepare = 1;
    setup_output(prepare);
    SDL_GetLogOutputFunction(&sdl_log_was, &sdl_log_was_data);
    SDL_SetLogOutputFunction(sdl_log_too, NULL);
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_report);
#endif
    { SDL_Thread *w = SDL_CreateThread(watchdog, "watchdog", NULL); if (w) SDL_DetachThread(w); }
    const char *disc = 0, *pc_arg = 0;
    int setup_forced = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--prepare")) continue;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) disc = argv[++i]; /* (the disc, as the first argument) */
        else if (!strncmp(argv[i], "--game=", 7)) disc = argv[i] + 7;
        else if (!strcmp(argv[i], "--pc") && i + 1 < argc) pc_arg = argv[++i]; /* (with --prepare: the PC version) */
        else if (!strncmp(argv[i], "--pc=", 5)) pc_arg = argv[i] + 5;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_boundaries = strtoull(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--vblanks") && i + 1 < argc) max_vblanks = strtoull(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--tape") && i + 1 < argc) {
            if (!load_tape(argv[++i])) { fprintf(stderr, "cannot read tape %s\n", argv[i]); return 1; }
        } else if (!strcmp(argv[i], "--tape-vblank") && i + 1 < argc) {
            if (!load_tape(argv[++i])) { fprintf(stderr, "cannot read tape %s\n", argv[i]); return 1; }
            tape_by_vblank = 1;
        } else if (!strcmp(argv[i], "--trace")) M.trace = 1;
        else if (!strcmp(argv[i], "--frame-log") && i + 1 < argc) frame_log = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--headless")) M.headless = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            if (!hw_wav_open(argv[++i])) { fprintf(stderr, "cannot write %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--check-mod") && i + 1 < argc) /* (modders: one mod folder's checks, docs/MODDING.md 9) */
            return mods_check_one(argv[i + 1]) ? 0 : 1;
        else if (!strcmp(argv[i], "--new-mod") && i + 2 < argc) /* (modders: a new mod folder with a fresh uid) */
            return mods_new(argv[i + 1], argv[i + 2]) ? 0 : 1;
        else if (!strcmp(argv[i], "--test-mod-sound") && i + 2 < argc) { /* tests: a character's sounds through the SPU */
            int mod_sound_selftest(const char *folder, const char *out);
            return mod_sound_selftest(argv[i + 1], argv[i + 2]);
        }
        else if (!strcmp(argv[i], "--test-mod-download") && i + 2 < argc) { /* tests: a mod received as from a server */
            int mod_files_selftest(const char *mod_dir, const char *game_folder);
            return mod_files_selftest(argv[i + 1], argv[i + 2]);
        }
        else if (!strcmp(argv[i], "--list-controllers")) {
            void input_list(void);
            input_list();
            return 0;
        }
        else if (!strcmp(argv[i], "--shot-dir") && i + 1 < argc) shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--ui-shot") && i + 1 < argc) ui_shot_arg = argv[++i];
        else if (!strcmp(argv[i], "--settings") && i + 1 < argc) settings_path = argv[++i];
        else if (!strcmp(argv[i], "--setup")) setup_forced = 1; /* (the Options row: the setup window again) */
        else if (!strcmp(argv[i], "--preset") && i + 1 < argc) {
            const char *p = argv[++i];
            if (!strcmp(p, "original")) preset_arg = 0;
            else if (!strcmp(p, "vanilla_plus")) preset_arg = 1;
            else { fprintf(stderr, "--preset: original or vanilla_plus\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) {
            for (char *p = argv[++i]; *p && shot_count < MAX_SHOTS;) {
                shots[shot_count] = strtoull(p, &p, 10);
                shot_by_vblank[shot_count++] = *p == 'v';
                if (*p == 'v') p++;
                if (*p == ',') p++;
            }
        }
        else disc = argv[i];
    }
    /* tm2pc.ini and the music and profiles folders are next to the executable */
    char base[1024] = "", path[1100], disc_full[1024];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(0, base, sizeof base);
    while (n > 0 && base[n - 1] != '\\' && base[n - 1] != '/') n--;
    base[n] = 0;
    if (disc && _fullpath(disc_full, disc, sizeof disc_full))
        disc = disc_full;
    static char pc_full[1024];
    if (pc_arg && *pc_arg && _fullpath(pc_full, pc_arg, sizeof pc_full))
        pc_arg = pc_full;
#endif
    snprintf(path, sizeof path, "%smusic", base);
    music_set_folder(path);
    snprintf(path, sizeof path, "%sprofiles", base);
    profile_set_folder(path);
    ui_set_folder(base);
    mods_scan(base); /* (mods/: listed and checked; docs/MODDING.md) */
    /* The settings file is read (and created) in windowed play. Headless
     * runs (tests) use the defaults unless --settings names a file. It
     * remembers the disc, so the game starts without arguments. */
    snprintf(path, sizeof path, "%stm2pc.ini", base);
    const char *ini = settings_path ? settings_path : path;
    if (settings_path || !M.headless) {
        if (disc) settings_set_disc(disc);
        settings_load(ini, !settings_path);
        if (!disc) disc = settings_disc();
        else settings_remember_disc(ini);
        if (prepare) /* (RecompLauncher's setup step: the menu pictures from the discs, no window; exit 0 when made) */
            return setup_prepare(base, disc, pc_arg) ? 0 : 1;
        if (!settings_path || getenv("TM2PC_UPDATE_URL")) update_pregame(base); /* (play: a newer version, before the game? tests: theirs only) */
        static char chosen[1024];
        snprintf(chosen, sizeof chosen, "%s", disc ? disc : "");
        if ((!settings_path || getenv("TM2PC_SETUP_PS1")) && !ui_shot_arg) { /* (play: the menu pictures from the discs) */
            if (!setup_pregame(base, setup_forced, chosen, sizeof chosen)) return 0;
            disc = chosen;
        }
        snprintf(path, sizeof path, "%sui/movie_portraits.txt", base);
        profile_load_movie_portraits(path); /* before the profile: its portrait may be one */
        profile_load();
        atexit(feats_flush); /* the feats of a match still running when the window closes */
    }
    if (preset_arg >= 0)
        vp_preset(preset_arg);
    if (ui_shot_arg) {
        int page = 0, sel = 0, w = 0, h = 0, n = 0;
        if (sscanf(ui_shot_arg, "%d,%d,%dx%d,%n", &page, &sel, &w, &h, &n) < 4 || !n) {
            fprintf(stderr, "--ui-shot page,row,WxH,file.bmp\n");
            return 1;
        }
        int ok = ui_shot(page, sel, w, h, ui_shot_arg + n);
        fprintf(stderr, "ui shot %s: %s\n", ui_shot_arg + n, ok ? "saved" : "failed");
        return ok ? 0 : 1;
    }
    if (!disc || !*disc) {
        const char *msg = "No disc image: start tm2pc with the Twisted Metal 2 .cue file once\n"
                          "(or set disc = in the [game] section of tm2pc.ini).";
        fprintf(stderr, "%s\nusage: tm2pc DISC.cue [--headless] [--frames N] [--tape words.txt] [--trace] [--frame-log FILE]\n"
                        "       [--preset original|vanilla_plus] [--settings FILE]\n", msg);
        error_box(msg);
        return 1;
    }
    M.ram = calloc(1, GUEST_RAM_BYTES); /* 16 MB, not mirrored (runtime/psx_cpu.h) */
    M.scratch = calloc(1, 0x400);
    M.cpu.ram = M.ram;
    M.cpu.scratch = M.scratch;
    M.pad_buttons[0] = M.pad_buttons[1] = 0xFFFF;
    M.pad_connected[0] = 1;
    for (int i = 0; i < 4; i++) M.rcnt_autoclear[i] = 1; /* BIOS default */
    char msg_disc[1600];
    if (disc_problem(disc, msg_disc, sizeof msg_disc)) { /* (what is wrong with it, in words a player can act on) */
        fprintf(stderr, "%s\n", msg_disc);
        error_box(msg_disc);
        if (!M.headless && !settings_path && !setup_forced) setup_restart(); /* (play: the setup window, to choose
                                                                               * another; not again from it: no loop) */
        return 1;
    }
    if (!cd_open(disc)) {
        char msg[1200];
        snprintf(msg, sizeof msg, "Cannot open the disc image\n%s", disc);
        fprintf(stderr, "%s\n", msg);
        error_box(msg);
        return 1;
    }
    if (!gpu_select_backend(M.headless ? &gpu_null_backend : &gpu_psyz_backend)) {
        fprintf(stderr, "cannot start the %s renderer\n", gpu_backend->name);
        return 1;
    }
    if (!hw_audio_init(!M.headless))
        fprintf(stderr, "no audio output\n");
    if (!M.headless && !tape) {
        void input_init(void);
        input_init();
        M.live_input = 1;
    }
#ifdef RC_MEM_WATCH
    if (getenv("TM2PC_WATCH")) { /* addr,len (hex address) */
        unsigned lo = 0, n = 4;
        sscanf(getenv("TM2PC_WATCH"), "%x,%u", &lo, &n);
        rc_watch_lo = lo & 0x1FFFFFFFu;
        rc_watch_n = n;
        fprintf(stderr, "watch: %08X, %u bytes\n", lo, n);
    }
#endif
    if (!load_boot_exe()) return 1;
    for (int i = 0; i < RB_ARRAY_COUNT; i++) /* the moved arrays (tools/recomp/rebase.py): their retail contents */
        for (uint32_t k = 0; k < rb_arrays[i].size; k++)
            mem_w8(&M.cpu, rb_arrays[i].addr + k, k < rb_arrays[i].old_size ? mem_r8(&M.cpu, rb_arrays[i].old + k) : 0);
    set_pads_from_tape();
#ifdef RC_TRACE_ENTRY
    if (!getenv("TM2PC_WATCH")) CreateThread(0, 0, trace_watchdog, 0, 0, 0); /* (a watch run: menus hold VBlanks) */
#endif
    start_time = now_seconds();
    rc_call(&M.cpu, M.cpu.pc);
    fprintf(stderr, "the game returned from its entry point\n");
    summary();
    return 0;
}
