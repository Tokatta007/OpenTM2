/* The update before the game (update.h: update_pregame): the question, then a small window with the download's bar. */
#include "update.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ui/draw.h"

static int ask(void) /* "Yes" (1) or "No" */
{
    const char *answer = getenv("TM2PC_UPDATE_ANSWER"); /* tests: yes / no */
    if (answer) return !SDL_strcasecmp(answer, "yes");
    char msg[256];
    snprintf(msg, sizeof msg, "New update %s is available (you have %s).\n\nDo you want to update OpenTM2 before starting?",
             update_version(), update_current());
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, NULL, "OpenTM2 update", msg, 2, buttons, NULL};
    int pressed = 0;
    if (!SDL_ShowMessageBox(&box, &pressed)) return 0;
    return pressed == 1;
}

static void draw(Image *im, float scale, Uint64 t0)
{
    static const uint8_t back[3] = {14, 14, 18}, line[3] = {188, 188, 196}, trough[3] = {40, 40, 48}, fill[3] = {230, 140, 40};
    const TextColor white = {{240, 240, 244}, {200, 200, 208}}, pale = {{170, 170, 180}, {150, 150, 160}};
    draw_fill(im, 0, 0, im->w, im->h, back, 255);
    int s = (int)(16 * scale), x0 = (int)(24 * scale), w = im->w - 2 * x0;
    char t[128];
    UpdateState st = update_state();
    snprintf(t, sizeof t, st == UPDATE_READY ? "%s is ready: starting it..." : "Updating OpenTM2 to %s", update_version());
    if (text_ready()) text_draw(im, t, x0, (int)(18 * scale), (int)(20 * scale), white);
    int by = (int)(64 * scale), bh = (int)(22 * scale), got = 0, total = 0;
    update_bytes(&got, &total);
    int pc = total > 0 ? (int)((long long)got * 100 / total) : 0;
    draw_fill(im, x0 - 1, by - 1, w + 2, bh + 2, line, 255);
    draw_fill(im, x0, by, w, bh, trough, 255);
    draw_fill(im, x0, by, st == UPDATE_READY ? w : (int)((long long)w * pc / 100), bh, fill, 255);
    double secs = (double)(SDL_GetTicks() - t0) / 1000.0, rate = secs > 0.5 ? got / secs : 0;
    if (st == UPDATE_READY) snprintf(t, sizeof t, "Done.");
    else if (total > 0 && rate > 0)
        snprintf(t, sizeof t, "%.1f of %.1f MB, %d%%, about %d s left", got / 1048576.0, total / 1048576.0, pc,
                 (int)((total - got) / rate + 0.99));
    else if (total > 0) snprintf(t, sizeof t, "%.1f of %.1f MB, %d%%", got / 1048576.0, total / 1048576.0, pc);
    else snprintf(t, sizeof t, "Connecting...");
    if (text_ready()) text_draw(im, t, x0, by + bh + (int)(14 * scale), s, pale);
    if (text_ready() && st != UPDATE_READY) text_draw(im, "Close this window to start the game without the update.", x0, im->h - (int)(30 * scale), (int)(13 * scale), pale);
}

void update_pregame(const char *base)
{
    /* online, this build's id is a hash of the executable's file: taken now, before an update can replace the file.
     * Updated from Options and not restarted, the game read the new file later and joined servers of the new version
     * while still running the old one (Skull Servant's v1.0.11 log: "this is v1.0.10", "v1.0.11 in place", then online) */
    uint32_t lobby_build_id(void); /* net/lobby.c */
    lobby_build_id();
    update_start(base);
    Uint64 t0 = SDL_GetTicks();
    while (update_state() == UPDATE_CHECKING && SDL_GetTicks() - t0 < 4000) SDL_Delay(20); /* (a slow answer: the game
                                                                                           * starts; Options shows it) */
    if (update_state() != UPDATE_AVAILABLE || !ask()) return;
    char font[1100];
    snprintf(font, sizeof font, "%sfonts/AveriaSerifLibre-Bold.ttf", base);
    if (!text_ready()) text_init(font);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { update_install(); while (update_state() == UPDATE_DOWNLOADING) SDL_Delay(50); }
    else {
        float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        if (scale < 1) scale = 1;
        int w = (int)(520 * scale), h = (int)(170 * scale);
        SDL_Window *win = SDL_CreateWindow("OpenTM2 update", w, h, 0);
        SDL_Renderer *r = win ? SDL_CreateRenderer(win, NULL) : NULL;
        SDL_Texture *tex = r ? SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h) : NULL;
        Image im = {0};
        image_resize(&im, w, h);
        update_install();
        Uint64 shown = 0, start = SDL_GetTicks();
        int closed = 0;
        while (update_state() == UPDATE_DOWNLOADING || (update_state() == UPDATE_READY && (!shown || SDL_GetTicks() - shown < 600))) {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) closed = 1;
            if (closed && update_state() == UPDATE_DOWNLOADING) update_cancel(); /* (the old files stay; the game starts) */
            if (update_state() == UPDATE_READY && !shown) shown = SDL_GetTicks();
            if (tex && im.px) {
                draw(&im, scale, start);
                SDL_UpdateTexture(tex, NULL, im.px, im.w * 3);
                SDL_RenderClear(r);
                SDL_RenderTexture(r, tex, NULL, NULL);
                SDL_RenderPresent(r);
            }
            SDL_Delay(30);
        }
        if (getenv("TM2PC_UPDATE_SHOT") && im.px) image_save_bmp(&im, getenv("TM2PC_UPDATE_SHOT")); /* tests: the window */
        image_free(&im);
        if (tex) SDL_DestroyTexture(tex);
        if (r) SDL_DestroyRenderer(r);
        if (win) SDL_DestroyWindow(win);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        if (closed && update_state() != UPDATE_READY) return;
    }
    if (update_state() == UPDATE_READY) update_restart(); /* (this one ends; the new one goes into the game) */
    if (update_state() == UPDATE_FAILED) { /* (told; the game starts as it is) */
        char msg[256];
        snprintf(msg, sizeof msg, "The update could not be finished (%s).\nOpenTM2 will start with the version you have.",
                 update_error());
        if (!getenv("TM2PC_UPDATE_ANSWER")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "OpenTM2 update", msg, NULL);
    }
}
