/* The environment read once into a table (env_cache.h): getenv by a short hash probe, not a scan of every variable.
 * Names match without regard to case, as Windows' own. The table is built before main (a constructor), so threads
 * only read it. */
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#undef getenv
#ifdef _WIN32
#include <windows.h>
#else
extern char **environ;
#endif

enum { SLOTS = 1024 }; /* (a power of two, several times the variables an environment holds) */
static struct { char *name, *value; } table[SLOTS];
static int built;

static uint32_t hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)toupper((unsigned char)s[i])) * 16777619u;
    return h;
}
static int same(const char *a, const char *b, size_t n) /* (b: n characters, a: ends there) */
{
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i])) return 0;
    return a[n] == 0;
}
static void put(const char *entry)
{
    const char *eq = strchr(entry, '=');
    if (!eq || eq == entry) return; /* (Windows' "=C:=C:\..." entries: no name) */
    size_t n = (size_t)(eq - entry);
    uint32_t i = hash(entry, n) & (SLOTS - 1);
    for (int k = 0; k < SLOTS; k++, i = (i + 1) & (SLOTS - 1)) {
        if (!table[i].name) {
            char *name = malloc(n + 1);
            if (!name) return;
            memcpy(name, entry, n);
            name[n] = 0;
            table[i].name = name;
            table[i].value = strdup(eq + 1);
            return;
        }
        if (same(table[i].name, entry, n)) return; /* (the first of a name, as getenv) */
    }
}
static void build(void)
{
#ifdef _WIN32
    char *block = GetEnvironmentStringsA();
    if (block) {
        for (char *p = block; *p; p += strlen(p) + 1) put(p);
        FreeEnvironmentStringsA(block);
    }
#else
    for (char **e = environ; e && *e; e++) put(*e);
#endif
    built = 1;
}
__attribute__((constructor)) static void env_cache_init(void) { build(); }

char *tm2pc_getenv(const char *name)
{
    if (!built) return getenv(name); /* (a call before the table: the C library's own) */
    size_t n = strlen(name);
    uint32_t i = hash(name, n) & (SLOTS - 1);
    for (int k = 0; k < SLOTS && table[i].name; k++, i = (i + 1) & (SLOTS - 1))
        if (same(table[i].name, name, n)) return table[i].value;
    return NULL;
}
