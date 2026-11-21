/* The renderer's capacity, measured (decomp RENDER_ANSWERS.md R2.4, portRenderStats, TM2_PORT_FAR_DISTANCE).
 *
 * The variant records, per view (rows 0..3) and for the rear-view mirror (row 4), the matrix slots used (200 a view,
 * 198 for transform nodes), the meshes queued against the mesh cap and the packet buffer used against its size, with
 * their highest values, the caps reached and how often. Read here only: reset at each round's start, a summary at its
 * end (TM2PC_RENDER_STATS=1: and every 10 seconds), and F3 in a battle a line on the HUD with view 1's (and the
 * mirror's) numbers now and at most. Nothing here changes what is drawn or played. */
#include "../runtime/runtime.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

enum { ROWS = 5, ROW_SIZE = 0x40, ROWS_AT = 0x10 };
/* the walker's transform-node limit: TM2_PORT_RENDER_SLOTS=400 in the port's build (tools/decomp/wc.py; request 47),
 * so 398 (retail 198) */
enum { NODE_LIMIT = 398 };
enum { F_FRAMES = 0x00, F_SLOTS = 0x04, F_SLOTS_MAX = 0x08, F_MESHES = 0x0C, F_MESHES_MAX = 0x10, F_MESH_CAP = 0x14,
       F_PACKET = 0x18, F_PACKET_MAX = 0x1C, F_PACKET_CAP = 0x20, F_SCENE = 0x24, F_SCENE_MAX = 0x28, F_CAPS = 0x2C,
       F_CAPS_SEEN = 0x30, F_CAPPED = 0x34, F_FIRST_CAPPED = 0x38 };

static uint32_t stats(void)
{
    static uint32_t at = 1;
    if (at == 1) at = port_sym("portRenderStats");
    return at;
}
static int field(int row, int f) { return (int)mem_r32(&M.cpu, stats() + ROWS_AT + ROW_SIZE * (uint32_t)row + (uint32_t)f); }

static void summary(const char *when)
{
    if (!stats()) return;
    static const char *const names[ROWS] = {"view 1", "view 2", "view 3", "view 4", "mirror"};
    for (int r = 0; r < ROWS; r++) {
        if (!field(r, F_FRAMES)) continue;
        int caps = field(r, F_CAPS_SEEN);
        fprintf(stderr, "render stats (%s) %s: %d frames; slots max %d of %d; meshes max %d of %d; packets max %d of %d "
                        "(this view's scene max %d)%s%s%s%s%s\n",
                when, names[r], field(r, F_FRAMES), field(r, F_SLOTS_MAX), NODE_LIMIT, field(r, F_MESHES_MAX), field(r, F_MESH_CAP),
                field(r, F_PACKET_MAX), field(r, F_PACKET_CAP), field(r, F_SCENE_MAX), caps ? "; CAPPED:" : "",
                caps & 1 ? " nodes" : "", caps & 2 ? " slots" : "", caps & 4 ? " meshes" : "", caps & 8 ? " packets" : "");
        if (caps)
            fprintf(stderr, "render stats (%s) %s: %d capped frames, the first at frame %d\n", when, names[r],
                    field(r, F_CAPPED), field(r, F_FIRST_CAPPED));
    }
}

void render_stats_round(void) /* each round's start: the last round's summary, then counting anew */
{
    if (!stats()) return;
    if (getenv("TM2PC_RENDER_STATS")) summary("round");
    mem_w32(&M.cpu, stats(), 1); /* reset: the next recorded view zeroes every row */
}

void render_stats_frame(void) /* each battle frame */
{
    static uint64_t next;
    if (!stats() || !getenv("TM2PC_RENDER_STATS")) return;
    if (M.vblanks >= next) {
        if (next) summary("so far");
        next = M.vblanks + 600;
    }
}

/* F3 in a battle: the readout on or off; the line for the HUD (view 1, and the mirror when it draws) */
static int shown;
int render_stats_line(char *out, size_t n, int mirror)
{
    static int key_was;
    const bool *keys = SDL_GetKeyboardState(NULL);
    int key = keys && keys[SDL_SCANCODE_F3];
    if (key && !key_was) shown = !shown;
    key_was = key;
    if (!shown || !stats()) return 0;
    int r = mirror ? 4 : 0;
    if (!field(r, F_FRAMES)) return 0;
    /* (the HUD's font has no '/': ':' between a count and its limit; packets in KB) */
    snprintf(out, n, "%s SLOT %d:%d MAX %d  MESH %d:%d MAX %d  PKT %dK:%dK MAX %dK%s", mirror ? "MIR" : "VIEW",
             field(r, F_SLOTS), NODE_LIMIT, field(r, F_SLOTS_MAX), field(r, F_MESHES), field(r, F_MESH_CAP), field(r, F_MESHES_MAX),
             field(r, F_PACKET) / 1024, field(r, F_PACKET_CAP) / 1024, field(r, F_PACKET_MAX) / 1024,
             field(r, F_CAPS_SEEN) ? "  CAPPED" : "");
    return 1;
}

int render_stats_now(char *out, size_t n) /* (tests: photo mode's TM2PC_PHOTO_STATS) view 1's numbers this frame */
{
    if (!stats() || !field(0, F_FRAMES)) { snprintf(out, n, "no render stats"); return 0; }
    int caps = field(0, F_CAPS);
    snprintf(out, n, "view 1: slots %d of %d, meshes %d of %d, packets %d of %d%s%s%s%s", field(0, F_SLOTS), NODE_LIMIT,
             field(0, F_MESHES), field(0, F_MESH_CAP), field(0, F_PACKET), field(0, F_PACKET_CAP), caps & 2 ? ", slots capped" : "",
             caps & 4 ? ", meshes capped" : "", caps & 8 ? ", packets capped" : "", caps & 1 ? ", nodes capped" : "");
    return 1;
}

/* A frame drawn on a machine that is put back after it (photo mode's frames, the kill cam's shots: ui/photo.c,
 * vplus/interp.c): Farthest's detail budget (request 98, portFarLodBudgetState) steps by the view's last frame here
 * (portRenderStats), and both went back to the paused battle's after every frame. So a camera that saw more than the
 * battle's (photo mode flown up over Holland's fields) filled the mesh queue and the packet buffer every frame, never
 * stepping down: what the walk reaches last (the ground near it, the sky) missing (the user's, 2026-11-03). Carried
 * over the put-back (drawing only): 0 after the drawing, 1 after the machine is back. */
void render_budget_carry(int phase)
{
    static uint32_t budget = 1;
    static uint8_t kept[0x10 + 0x150];
    static int have;
    if (budget == 1) budget = port_sym("portFarLodBudgetState");
    if (!budget || !stats()) return;
    if (phase == 0) {
        for (uint32_t i = 0; i < 0x10; i++) kept[i] = mem_r8(&M.cpu, budget + i);
        for (uint32_t i = 0; i < 0x150; i++) kept[0x10 + i] = mem_r8(&M.cpu, stats() + i);
        have = 1;
    } else if (have) {
        for (uint32_t i = 0; i < 0x10; i++) mem_w8(&M.cpu, budget + i, kept[i]);
        for (uint32_t i = 0; i < 0x150; i++) mem_w8(&M.cpu, stats() + i, kept[0x10 + i]);
        have = 0;
    }
}
