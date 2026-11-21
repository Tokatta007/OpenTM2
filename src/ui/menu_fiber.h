/* The Options pages as a coroutine beside a running online match (menu_fiber.c) */
#pragma once

int menu_fiber_start(void (*fn)(void)); /* fn runs as the coroutine (from the next resume); 0: not possible */
int menu_fiber_resume(void);            /* one of its frames; 1: it has just finished (and is gone) */
int menu_fiber_active(void);            /* started and not finished */
int menu_fiber_inside(void);            /* this code runs inside it now */
void menu_fiber_yield(void);            /* (inside) the thread back to the game until the next resume */
