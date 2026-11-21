/* The first-time setup (setup.h): a small window before the game's, as the updater's (update_ui.c). */
#include "setup.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../assets/uibuild.h"
#include "../settings.h"
#include "../ui/draw.h"

static char ps1[1024], pc[1024], ui_dir[1100], message[320];
static SDL_Mutex *picks;                 /* (the file dialogs answer on their own threads) */
static SDL_AtomicInt build_pct, build_state; /* 0 not started, 1 building, 2 done, 3 failed */
static void *build_what; /* (SDL_SetAtomicPointer's) */
static char build_error[300];

enum { FORM, BUILDING };

static int is_dir(const char *p)
{
    struct stat st;
    return !stat(p, &st) && (st.st_mode & S_IFMT) == S_IFDIR;
}
static int is_file(const char *p)
{
    struct stat st;
    return !stat(p, &st) && (st.st_mode & S_IFMT) == S_IFREG;
}
static int ends_with(const char *s, const char *e)
{
    size_t n = strlen(s), m = strlen(e);
    return n >= m && !SDL_strcasecmp(s + n - m, e);
}

/* ui/'s maker: this builder's version (0: none); with_pc: made with the PC version */
static int made_version(int *with_pc)
{
    char p[1200], line[64];
    snprintf(p, sizeof p, "%s/setup.txt", ui_dir);
    FILE *f = fopen(p, "r");
    int v = 0;
    *with_pc = 0;
    while (f && fgets(line, sizeof line, f)) {
        sscanf(line, "version %d", &v);
        sscanf(line, "pc %d", with_pc);
    }
    if (f) fclose(f);
    return v;
}

/* a .bin chosen: its .cue beside it (the game reads the .cue) */
static void take_ps1(const char *path)
{
    snprintf(ps1, sizeof ps1, "%s", path);
    if (ends_with(ps1, ".bin")) {
        char cue[1024];
        snprintf(cue, sizeof cue, "%.*s.cue", (int)strlen(ps1) - 4, ps1);
        if (is_file(cue)) snprintf(ps1, sizeof ps1, "%s", cue);
    }
}

/* the .cue's track files (FILE "..."): the first one not beside it, in missing (1), or 0 when all are there */
static int cue_missing(const char *cue, char *missing, size_t cap)
{
    FILE *f = fopen(cue, "r");
    if (!f) return 0;
    const char *slash = strrchr(cue, '/'), *back = strrchr(cue, '\\');
    if (back > slash) slash = back;
    int dir = slash ? (int)(slash - cue + 1) : 0, gone = 0;
    char line[1024];
    while (!gone && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (SDL_strncasecmp(p, "FILE", 4)) continue;
        char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!q1 || !q2) continue;
        *q2 = 0;
        char path[1200];
        snprintf(path, sizeof path, "%.*s%s", dir, cue, q1 + 1);
        if (!is_file(path)) {
            snprintf(missing, cap, "%s", q1 + 1);
            gone = 1;
        }
    }
    fclose(f);
    return gone;
}

static int is_archive(const char *p)
{
    return ends_with(p, ".zip") || ends_with(p, ".7z") || ends_with(p, ".rar");
}

static void SDLCALL picked(void *which, const char *const *list, int filter)
{
    (void)filter;
    if (!list || !list[0]) return;
    SDL_LockMutex(picks);
    if (which == ps1) take_ps1(list[0]);
    else snprintf(pc, sizeof pc, "%s", list[0]);
    message[0] = 0;
    SDL_UnlockMutex(picks);
}

static void dropped(const char *path)
{
    SDL_LockMutex(picks);
    if (ends_with(path, ".cue") || ends_with(path, ".bin")) take_ps1(path);
    else if (ends_with(path, ".iso") || is_dir(path)) snprintf(pc, sizeof pc, "%s", path);
    else if (is_archive(path)) snprintf(message, sizeof message, "Extract the archive first, then drop the .cue file from it.");
    else snprintf(message, sizeof message, "Drop the PlayStation disc's .cue file, or the PC version's .iso or folder.");
    SDL_UnlockMutex(picks);
}

static void progress(int pct, const char *what, void *ctx)
{
    (void)ctx;
    SDL_SetAtomicInt(&build_pct, pct);
    SDL_SetAtomicPointer(&build_what, (void *)what);
}

static int SDLCALL build_thread(void *u)
{
    (void)u;
    int ok = ui_build(ps1, *pc ? pc : NULL, ui_dir, progress, NULL);
    if (!ok) snprintf(build_error, sizeof build_error, "%s", ui_build_error());
    SDL_SetAtomicInt(&build_state, ok ? 2 : 3);
    return ok;
}

/* ---- the window's look ---- */

typedef struct { int x, y, w, h; } Rect;
static float sc = 1;
static int L(float v) { return (int)(v * sc + 0.5f); }
static int inside(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static const uint8_t BACK[3] = {14, 14, 18}, LINE[3] = {188, 188, 196}, FIELD[3] = {30, 30, 36}, BUTTON[3] = {52, 52, 60},
                     HOVER[3] = {84, 70, 52}, COPPER[3] = {230, 140, 40}, TROUGH[3] = {40, 40, 48};
static const TextColor WHITE = {{240, 240, 244}, {200, 200, 208}}, PALE = {{170, 170, 180}, {150, 150, 160}},
                       WARM = {{250, 190, 110}, {230, 150, 70}}, RED = {{255, 120, 110}, {220, 80, 70}};

static void say(Image *im, const char *s, int x, int y, int size, TextColor c)
{
    if (text_ready()) text_draw(im, s, x, y, size, c);
}

/* the path's end, "..." before it, as much as fits */
static void path_box(Image *im, Rect r, const char *path, const char *empty)
{
    draw_fill(im, r.x - 1, r.y - 1, r.w + 2, r.h + 2, LINE, 255);
    draw_fill(im, r.x, r.y, r.w, r.h, FIELD, 255);
    int size = L(14), room = r.w - L(16);
    if (!*path) { say(im, empty, r.x + L(8), r.y + (r.h - size) / 2, size, PALE); return; }
    const char *s = path;
    char buf[1100];
    snprintf(buf, sizeof buf, "%s", s);
    while (text_ready() && *s && text_width(buf, size) > room) snprintf(buf, sizeof buf, "...%s", ++s);
    say(im, buf, r.x + L(8), r.y + (r.h - size) / 2, size, WHITE);
}

static void button(Image *im, Rect r, const char *label, int hover, int main_one)
{
    draw_fill(im, r.x - 1, r.y - 1, r.w + 2, r.h + 2, main_one ? COPPER : LINE, 255);
    draw_fill(im, r.x, r.y, r.w, r.h, hover ? HOVER : BUTTON, 255);
    int size = L(15), w = text_ready() ? text_width(label, size) : 0;
    say(im, label, r.x + (r.w - w) / 2, r.y + (r.h - size) / 2, size, WHITE);
}

typedef struct { Rect ps1_box, ps1_pick, pc_box, pc_iso, pc_dir, quit, build; } Layout;

static Layout layout(const Image *im)
{
    Layout l;
    int x0 = L(24), right = im->w - L(24), bw = L(88), bh = L(28);
    l.ps1_pick = (Rect){right - bw, L(122), bw, bh};
    l.ps1_box = (Rect){x0, L(122), l.ps1_pick.x - L(10) - x0, bh};
    l.pc_dir = (Rect){right - bw, L(204), bw, bh};
    l.pc_iso = (Rect){l.pc_dir.x - L(8) - bw, L(204), bw, bh};
    l.pc_box = (Rect){x0, L(204), l.pc_iso.x - L(10) - x0, bh};
    l.build = (Rect){right - L(120), im->h - L(52), L(120), L(32)};
    l.quit = (Rect){l.build.x - L(12) - L(100), im->h - L(52), L(100), L(32)};
    return l;
}

static void draw(Image *im, int mode, int mx, int my, int forced)
{
    draw_fill(im, 0, 0, im->w, im->h, BACK, 255);
    Layout l = layout(im);
    int x0 = L(24);
    say(im, forced ? "OpenTM2: the game discs" : "OpenTM2: first-time setup", x0, L(16), L(24), WHITE);
    say(im, "OpenTM2 makes its menu pictures from your own copies of Twisted Metal 2.", x0, L(52), L(14), PALE);
    say(im, "Nothing made from them leaves this PC.", x0, L(70), L(14), PALE);
    SDL_LockMutex(picks);
    say(im, "The PlayStation disc (USA): the .cue file (needed)", x0, L(100), L(15), WHITE);
    path_box(im, l.ps1_box, ps1, "Browse, or drop the .cue on this window");
    say(im, "Extract the .cue and all its .bin files into one folder first, then choose the .cue.", x0, L(156), L(13), PALE);
    say(im, "The 1997 PC version: its .iso, or its installed folder (either one)", x0, L(182), L(15), WHITE);
    path_box(im, l.pc_box, pc, "None: the pictures from the PlayStation disc alone");
    int have_pc = *pc != 0;
    SDL_UnlockMutex(picks);
    if (mode == FORM) {
        button(im, l.ps1_pick, "Browse...", inside(l.ps1_pick, mx, my), 0);
        button(im, l.pc_iso, ".iso...", inside(l.pc_iso, mx, my), 0);
        button(im, l.pc_dir, "Folder...", inside(l.pc_dir, mx, my), 0);
        say(im, have_pc ? "PlayStation + PC (recommended): the sharpest pictures."
                        : "Recommended: PlayStation + PC, for pictures twice as sharp. You can add the PC version later (Options).",
            x0, L(244), L(13), have_pc ? PALE : WARM);
        if (*message) say(im, message, x0, L(264), L(13), RED);
        button(im, l.quit, "Quit", inside(l.quit, mx, my), 0);
        button(im, l.build, "Build", inside(l.build, mx, my), 1);
    } else {
        int by = L(250), bh = L(20), w = im->w - 2 * x0, pct = SDL_GetAtomicInt(&build_pct);
        const char *what = SDL_GetAtomicPointer(&build_what);
        char t[160];
        snprintf(t, sizeof t, "Making the pictures: %s (%d%%)", what ? what : "", pct);
        say(im, t, x0, by - L(26), L(14), WHITE);
        draw_fill(im, x0 - 1, by - 1, w + 2, bh + 2, LINE, 255);
        draw_fill(im, x0, by, w, bh, TROUGH, 255);
        draw_fill(im, x0, by, w * pct / 100, bh, COPPER, 255);
    }
}

/* PS1 only: asked once, the recommendation said again */
static int confirm_ps1_only(SDL_Window *win)
{
    if (getenv("TM2PC_SETUP_AUTO")) return 1;
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Go back"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "PlayStation only"},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, win, "OpenTM2 setup",
                                    "Make the pictures without the PC version?\n\n"
                                    "They will come from the PlayStation disc alone, at half the sharpness. We recommend "
                                    "both discs; you can add the PC version later (Options, Game discs).",
                                    2, buttons, NULL};
    int pressed = 0;
    return SDL_ShowMessageBox(&box, &pressed) && pressed == 1;
}

int setup_pregame(const char *base, int forced, char *disc, size_t cap)
{
    snprintf(ui_dir, sizeof ui_dir, "%sui", base);
    int with_pc = 0, version = made_version(&with_pc);
    const char *env_ps1 = getenv("TM2PC_SETUP_PS1"), *env_pc = getenv("TM2PC_SETUP_PC");
    char probe[1200];
    snprintf(probe, sizeof probe, "%s/title.bmp", ui_dir);
    int tools_made = !version && is_file(probe); /* (made by tools/art/ui_assets.py: kept) */
    if (!forced && !env_ps1 && (version >= UI_BUILD_VERSION || tools_made)) return 1;
    snprintf(ps1, sizeof ps1, "%s", env_ps1 ? env_ps1 : disc);
    snprintf(pc, sizeof pc, "%s", env_pc ? (strcmp(env_pc, "-") ? env_pc : "") : settings_pc_version());
    /* a newer builder, the same discs: made again without a question */
    int automatic = getenv("TM2PC_SETUP_AUTO") || (!forced && version > 0 && is_file(ps1) && (!*pc || with_pc));
    picks = SDL_CreateMutex();
    SDL_SetAtomicInt(&build_state, 0);
    SDL_SetAtomicInt(&build_pct, 0);
    SDL_SetAtomicPointer(&build_what, (void *)"");
    message[0] = 0;
    char font[1100];
    snprintf(font, sizeof font, "%sfonts/AveriaSerifLibre-Bold.ttf", base);
    if (!text_ready()) text_init(font);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "setup: no window (%s)\n", SDL_GetError());
        return version > 0 || tools_made;
    }
    sc = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (sc < 1) sc = 1;
    int w = L(640), h = L(330), mode = FORM, done = 0, quit = 0, mx = -1, my = -1;
    SDL_Window *win = SDL_CreateWindow(forced ? "OpenTM2: game discs" : "OpenTM2: first-time setup", w, h, 0);
    SDL_Renderer *r = win ? SDL_CreateRenderer(win, NULL) : NULL;
    SDL_Texture *tex = r ? SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h) : NULL;
    Image im = {0};
    image_resize(&im, w, h);
    SDL_Thread *thread = NULL;
    static const SDL_DialogFileFilter CUE[] = {{"PlayStation disc image (.cue)", "cue;bin"}};
    static const SDL_DialogFileFilter ISO[] = {{"Disc image (.iso)", "iso"}};
    const char *shot = getenv("TM2PC_SETUP_SHOT"); /* tests: the window, as a picture */
    int frames = 0, start_build = automatic;
    while (!done && !quit) {
        SDL_Event e;
        int click = 0;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = mode == FORM;
            else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                int ww = w, wh = h;
                SDL_GetWindowSize(win, &ww, &wh);
                float fx = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x, fy = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y;
                mx = (int)(fx * w / (ww > 0 ? ww : w)), my = (int)(fy * h / (wh > 0 ? wh : h));
                click = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT;
            } else if (e.type == SDL_EVENT_DROP_FILE && mode == FORM && e.drop.data) dropped(e.drop.data);
            else if (e.type == SDL_EVENT_KEY_DOWN && mode == FORM && e.key.key == SDLK_ESCAPE) quit = 1;
            if (click && mode == FORM) {
                Layout l = layout(&im);
                if (inside(l.ps1_pick, mx, my)) SDL_ShowOpenFileDialog(picked, ps1, win, CUE, 1, *ps1 ? ps1 : NULL, false);
                else if (inside(l.pc_iso, mx, my)) SDL_ShowOpenFileDialog(picked, pc, win, ISO, 1, *pc ? pc : NULL, false);
                else if (inside(l.pc_dir, mx, my)) SDL_ShowOpenFolderDialog(picked, pc, win, *pc ? pc : NULL, false);
                else if (inside(l.quit, mx, my)) quit = 1;
                else if (inside(l.build, mx, my)) start_build = 1;
                click = 0;
            }
        }
        if (start_build && mode == FORM) {
            start_build = 0;
            SDL_LockMutex(picks);
            int ok_ps1 = is_file(ps1) && (ends_with(ps1, ".cue")), ok_pc = !*pc || is_dir(pc) || is_file(pc);
            char gone[256] = "";
            int bin_gone = ok_ps1 && cue_missing(ps1, gone, sizeof gone);
            SDL_UnlockMutex(picks);
            if (bin_gone) snprintf(message, sizeof message, "%.120s is missing: extract it beside the .cue.", gone);
            else if (*ps1 && is_archive(ps1)) snprintf(message, sizeof message, "Extract the archive first, then choose the .cue file from it.");
            else if (*ps1 && ends_with(ps1, ".bin")) snprintf(message, sizeof message, "That .bin has no .cue beside it: extract the .cue too, then choose the .cue.");
            else if (!ok_ps1) snprintf(message, sizeof message, "Choose the PlayStation disc's .cue file (its .bin files beside it).");
            else if (!ok_pc) snprintf(message, sizeof message, "The PC version's .iso or folder cannot be found.");
            else if (*pc || automatic || confirm_ps1_only(win)) {
                message[0] = 0;
                mode = BUILDING;
                SDL_SetAtomicInt(&build_state, 1);
                thread = SDL_CreateThread(build_thread, "ui_build", NULL);
                if (!thread) build_thread(NULL);
            }
        }
        if (mode == BUILDING && SDL_GetAtomicInt(&build_state) >= 2) {
            SDL_WaitThread(thread, NULL);
            thread = NULL;
            if (SDL_GetAtomicInt(&build_state) == 2) done = 1;
            else {
                mode = FORM;
                automatic = 0;
                snprintf(message, sizeof message, "%s", build_error);
            }
        }
        if (tex && im.px) {
            draw(&im, mode, mx, my, forced);
            if (shot && frames == 1) image_save_bmp(&im, shot);
            SDL_UpdateTexture(tex, NULL, im.px, im.w * 3);
            SDL_RenderClear(r);
            SDL_RenderTexture(r, tex, NULL, NULL);
            SDL_RenderPresent(r);
        }
        frames++;
        if (getenv("TM2PC_SETUP_AUTO") && *message && mode == FORM && frames > 2) { fprintf(stderr, "setup: %s\n", message); quit = 1; }
        SDL_Delay(30);
    }
    image_free(&im);
    if (tex) SDL_DestroyTexture(tex);
    if (r) SDL_DestroyRenderer(r);
    if (win) SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_DestroyMutex(picks);
    picks = NULL;
    if (done) {
        settings_set_game_discs(ps1, pc);
        snprintf(disc, cap, "%s", ps1);
        fprintf(stderr, "setup: the pictures made (%s)\n", *pc ? "PlayStation and PC" : "PlayStation only");
        return 1;
    }
    return version > 0 || tools_made; /* (closed: the pictures there are kept; none: the game cannot start) */
}

int setup_restart(void)
{
#ifdef _WIN32
    char exe[1024], cmd[1100];
    DWORD n = GetModuleFileNameA(0, exe, sizeof exe);
    if (!n || n >= sizeof exe) return 0;
    snprintf(cmd, sizeof cmd, "\"%s\" --setup", exe);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    exit(0);
#else
    return 0;
#endif
}
