/* The first-time setup's readers (src/assets) against tools/disc's Python: every image of a disc image or a game folder,
 * one line each: its path, index, size and the CRC-32 of its RGBA (palette 0). tools/test/asset_check.py runs both and
 * compares the lists.
 *
 *     asset_check <disc.cue | disc.iso | folder> [pc]      (pc: .TPC packs' colors are RGB) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/assets/discfs.h"
#include "../../src/assets/tim.h"

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "asset_check <disc.cue | disc.iso | folder>\n"); return 2; }
    DiscFs *fs = discfs_open(argv[1]);
    if (!fs) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    static Tim t[4096];
    for (int i = 0; i < discfs_count(fs); i++) {
        const char *p = discfs_path(fs, i), *ext = strrchr(p, '.');
        if (!ext || (strcmp(ext, ".TIM") && strcmp(ext, ".TMS") && strcmp(ext, ".TPC") && strcmp(ext, ".PCX"))) continue;
        size_t len = 0;
        uint8_t *d = discfs_read(fs, i, &len);
        if (!d) { printf("%s unreadable\n", p); continue; }
        if (!strcmp(ext, ".PCX")) {
            int w, h;
            uint8_t *rgba = pcx_rgba(d, len, &w, &h);
            if (rgba) printf("%s 0 %dx%d %08x\n", p, w, h, crc32_of(rgba, (size_t)w * h * 4));
            free(rgba);
        } else {
            int n = tim_scan(d, len, !strcmp(ext, ".TPC"), t, 4096);
            for (int k = 0; k < n; k++) {
                uint8_t *rgba = malloc((size_t)t[k].w * t[k].h * 4);
                if (rgba) {
                    tim_rgba(&t[k], 0, rgba);
                    printf("%s %d %dx%d %08x\n", p, k, t[k].w, t[k].h, crc32_of(rgba, (size_t)t[k].w * t[k].h * 4));
                    free(rgba);
                }
                tim_free(&t[k]);
            }
        }
        free(d);
    }
    discfs_close(fs);
    return 0;
}
