/* Disc image access: a .cue with one .bin per track (raw 2352-byte sectors),
 * or a single track-1 .bin. Data reads come from track 1 (Mode 2 Form 1,
 * 2048 user bytes at offset 24); the track table serves CdGetToc and CD
 * audio. ISO 9660 lookup for CdSearchFile. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

#define RAW 2352

Track cd_tracks[100];
int cd_track_count;
static FILE *data_file;

static void dir_of(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s", path);
    char *s = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b > s) s = b;
    if (s) s[1] = 0; else out[0] = 0;
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

int cd_open(const char *image)
{
    const char *ext = strrchr(image, '.');
    cd_track_count = 0;
    if (ext && (!strcmp(ext, ".cue") || !strcmp(ext, ".CUE"))) {
        FILE *f = fopen(image, "r");
        if (!f) return 0;
        char dir[512], line[1024];
        dir_of(image, dir, sizeof dir);
        uint32_t lba = 0;
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            while (isspace((unsigned char)*p)) p++;
            if (!strncmp(p, "FILE", 4)) {
                char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : 0;
                if (!q1 || !q2 || cd_track_count >= 99) continue;
                *q2 = 0;
                Track *t = &cd_tracks[cd_track_count++];
                snprintf(t->path, sizeof t->path, "%s%s", dir, q1 + 1);
                long size = file_size(t->path);
                t->start = t->file_start = lba;
                t->sectors = size > 0 ? (uint32_t)(size / RAW) : 0;
                t->audio = 0;
                lba += t->sectors;
            } else if (!strncmp(p, "TRACK", 5) && cd_track_count) {
                cd_tracks[cd_track_count - 1].audio = strstr(p, "AUDIO") != 0;
            } else if (!strncmp(p, "INDEX 01", 8) && cd_track_count) {
                int m, s, fr;
                if (sscanf(p + 8, "%d:%d:%d", &m, &s, &fr) == 3)
                    cd_tracks[cd_track_count - 1].start += (uint32_t)((m * 60 + s) * 75 + fr);
            }
        }
        fclose(f);
    } else {
        Track *t = &cd_tracks[cd_track_count++];
        snprintf(t->path, sizeof t->path, "%s", image);
        long size = file_size(image);
        t->start = t->file_start = 0;
        t->sectors = size > 0 ? (uint32_t)(size / RAW) : 0;
    }
    if (!cd_track_count) return 0;
    data_file = fopen(cd_tracks[0].path, "rb");
    return data_file != 0;
}

int cd_read_sector(uint32_t lba, uint8_t *dst)
{
    if (!data_file || lba >= cd_tracks[0].sectors) return 0;
    fseek(data_file, (long)lba * RAW + 24, SEEK_SET);
    return fread(dst, 1, 2048, data_file) == 2048;
}

/* Read raw 2352-byte sectors from any track (CD audio). */
int cd_read_raw(uint32_t lba, uint8_t *dst)
{
    for (int i = 0; i < cd_track_count; i++) {
        Track *t = &cd_tracks[i];
        if (lba >= t->start && lba < t->start + t->sectors) {
            FILE *f = fopen(t->path, "rb");
            if (!f) return 0;
            fseek(f, (long)(lba - t->start) * RAW, SEEK_SET);
            int ok = fread(dst, 1, RAW, f) == RAW;
            fclose(f);
            return ok;
        }
    }
    return 0;
}

static int name_eq(const uint8_t *rec_name, int len, const char *want, int want_len)
{
    /* ISO names carry ";1"; compare case-insensitively up to it */
    int n = 0;
    while (n < len && rec_name[n] != ';') n++;
    if (n != want_len) return 0;
    for (int i = 0; i < n; i++)
        if (toupper(rec_name[i]) != toupper((unsigned char)want[i])) return 0;
    return 1;
}

static int lookup(uint32_t dir_lba, uint32_t dir_size, const char *name, int name_len,
                  uint32_t *lba, uint32_t *size, int *is_dir)
{
    uint8_t sec[2048];
    for (uint32_t s = 0; s * 2048 < dir_size; s++) {
        if (!cd_read_sector(dir_lba + s, sec)) return 0;
        for (int o = 0; o < 2048;) {
            int len = sec[o];
            if (!len) break;
            const uint8_t *r = sec + o;
            if (name_eq(r + 33, r[32], name, name_len)) {
                *lba = r[2] | r[3] << 8 | r[4] << 16 | (uint32_t)r[5] << 24;
                *size = r[10] | r[11] << 8 | r[12] << 16 | (uint32_t)r[13] << 24;
                *is_dir = (r[25] & 2) != 0;
                return 1;
            }
            o += len;
        }
    }
    return 0;
}

/* path like "\\SCREEN\\CPFONT.TIM;1" or "/SCREEN/CPFONT.TIM" */
int cd_find_file(const char *path, uint32_t *lba, uint32_t *size)
{
    uint8_t pvd[2048];
    if (!cd_read_sector(16, pvd)) return 0;
    uint32_t dl = pvd[158] | pvd[159] << 8 | pvd[160] << 16 | (uint32_t)pvd[161] << 24;
    uint32_t ds = pvd[166] | pvd[167] << 8 | pvd[168] << 16 | (uint32_t)pvd[169] << 24;
    const char *p = path;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        const char *e = p;
        while (*e && *e != '\\' && *e != '/' && *e != ';') e++;
        if (e == p) break;
        int is_dir = 0;
        if (!lookup(dl, ds, p, (int)(e - p), &dl, &ds, &is_dir)) return 0;
        while (*e == ';' || isdigit((unsigned char)*e)) e++;
        p = e;
        if (!*p) {
            *lba = dl;
            *size = ds;
            return 1;
        }
    }
    return 0;
}
