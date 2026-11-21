/* getenv from a table read once (env_cache.c). Force-included into every source of the game (CMakeLists.txt): the
 * port reads its TM2PC_ switches in per-frame code in hundreds of places, and the C library's getenv scans the whole
 * environment each time, the trailer takes' long scripts in it too: a third of a 4-player match's main thread (the
 * user's 2026-11-15 profile). The port never changes its environment while it runs. */
#pragma once
#ifndef _CRT_RAND_S
#define _CRT_RAND_S /* (profile.c's rand_s: stdlib.h is included here first now) */
#endif
#include <stdlib.h>
char *tm2pc_getenv(const char *name);
#define getenv(name) tm2pc_getenv(name)
