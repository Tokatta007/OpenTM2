/* The Options pages beside a running online match (ui/options.c). Online nobody pauses everyone: the game goes on, so
 * the pages (their own loop, as everywhere else) run as a coroutine. Each frame the game's VBlank resumes them for one
 * of their frames; where they would present a frame and wait (ui_vsync, rt_vblank) they hand the thread back instead.
 * Windows fibers: the same thread, so the window, the GPU and the sound are used as ever. Elsewhere: not available
 * (menu_fiber_start returns 0 and the caller keeps the pages out of the online menu). */
#include "menu_fiber.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void *main_fiber, *fiber;
static void (*body)(void);
static int inside, done;

static void WINAPI entry(void *arg)
{
    (void)arg;
    body();
    done = 1;
    for (;;) SwitchToFiber(main_fiber); /* (never resumed again: deleted once it is done) */
}

int menu_fiber_start(void (*fn)(void))
{
    if (fiber || !fn) return 0;
    if (!main_fiber) {
        main_fiber = ConvertThreadToFiber(NULL);
        if (!main_fiber) main_fiber = GetCurrentFiber(); /* (already one) */
        if (!main_fiber) return 0;
    }
    body = fn;
    done = 0;
    fiber = CreateFiberEx(256 * 1024, 16 * 1024 * 1024, 0, entry, NULL); /* (the pages' loop: a deep stack reserved) */
    return fiber != NULL;
}

int menu_fiber_resume(void)
{
    if (!fiber || inside) return 0;
    inside = 1;
    SwitchToFiber(fiber);
    inside = 0;
    if (done) {
        DeleteFiber(fiber);
        fiber = NULL;
        return 1;
    }
    return 0;
}

int menu_fiber_active(void) { return fiber != NULL; }
int menu_fiber_inside(void) { return inside; }
void menu_fiber_yield(void) { if (inside) SwitchToFiber(main_fiber); }
#else
int menu_fiber_start(void (*fn)(void)) { (void)fn; return 0; }
int menu_fiber_resume(void) { return 0; }
int menu_fiber_active(void) { return 0; }
int menu_fiber_inside(void) { return 0; }
void menu_fiber_yield(void) {}
#endif
