/* Disc image access: a .cue with one .bin per track (raw 2352-byte sectors) or one .bin for the whole disc,
 * or a single track-1 .bin. Data reads come from track 1 (Mode 2 Form 1,
 * 2048 user bytes at offset 24); the track table serves CdGetToc and CD
 * audio. ISO 9660 lookup for CdSearchFile. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#ifdef _WIN32
#include <windows.h>
#endif

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

/* A .cue with one FILE whose .bin is not there under that name (a .cue made for another dump's name, e.g. "[SCUS-94306]"
 * against "(U)"): the .bin named as the .cue, else the only .bin in its folder, as emulators take it. 1 with out set. */
int cue_bin_fallback(const char *cue, char *out, size_t cap)
{
    FILE *f = fopen(cue, "r");
    if (!f) return 0;
    char line[1024], named[1024] = "";
    int files = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (strncmp(p, "FILE", 4)) continue;
        char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : 0;
        if (q1 && q2) snprintf(named, sizeof named, "%.*s", (int)(q2 - q1 - 1), q1 + 1);
        files++;
    }
    fclose(f);
    char dir[512], path[1100];
    dir_of(cue, dir, sizeof dir);
    snprintf(path, sizeof path, "%s%s", dir, named);
    if (files != 1 || !*named || file_size(path) >= 0) return 0; /* (several files, or it is there: nothing to do) */
    const char *ext = strrchr(cue, '.');
    snprintf(path, sizeof path, "%.*s.bin", ext ? (int)(ext - cue) : (int)strlen(cue), cue);
    if (file_size(path) > 0) {
        snprintf(out, cap, "%s", path);
        return 1;
    }
#ifdef _WIN32
    char pat[1100];
    snprintf(pat, sizeof pat, "%s*.bin", dir);
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n++ == 0) snprintf(path, sizeof path, "%s%s", dir, d.cFileName);
    } while (FindNextFileA(h, &d));
    FindClose(h);
    if (n == 1) {
        snprintf(out, cap, "%s", path);
        return 1;
    }
#endif
    return 0;
}

int cd_open(const char *image)
{
    const char *ext = strrchr(image, '.');
    cd_track_count = 0;
    if (ext && (!strcmp(ext, ".cue") || !strcmp(ext, ".CUE"))) {
        FILE *f = fopen(image, "r");
        if (!f) return 0;
        char dir[512], line[1024], path[512] = "";
        dir_of(image, dir, sizeof dir);
        /* Each FILE holds one track (a .bin per track, as Redump's) or several (one .bin for the whole disc): a
         * track's INDEX lines are offsets into its file. A file's first track starts at the file's start (its pregap
         * in the file); a later one at its INDEX 00 (else 01); each runs to the next track of its file or the file's end. */
        uint32_t lba = 0, file_lba = 0, file_sectors = 0;
        int first_in_file = 0, have_file = 0;
        static int32_t idx0[100], idx1[100];
        static uint8_t first[100];
        static uint32_t file_end[100];
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            while (isspace((unsigned char)*p)) p++;
            int n, m, s, fr;
            if (!strncmp(p, "FILE", 4)) {
                char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : 0;
                if (!q1 || !q2) continue;
                *q2 = 0;
                snprintf(path, sizeof path, "%s%s", dir, q1 + 1);
                if (file_size(path) < 0) { /* (not under that name: the .bin beside it, cue_bin_fallback) */
                    char alt[512];
                    if (cue_bin_fallback(image, alt, sizeof alt)) snprintf(path, sizeof path, "%s", alt);
                }
                long size = file_size(path);
                file_lba = lba;
                file_sectors = size > 0 ? (uint32_t)(size / RAW) : 0;
                lba += file_sectors;
                first_in_file = have_file = 1;
            } else if (!strncmp(p, "TRACK", 5) && have_file && cd_track_count < 99) {
                int i = cd_track_count++;
                Track *t = &cd_tracks[i];
                snprintf(t->path, sizeof t->path, "%s", path);
                t->file_lba = file_lba;
                t->audio = strstr(p, "AUDIO") != 0;
                idx0[i] = -1;
                idx1[i] = 0;
                first[i] = (uint8_t)first_in_file;
                file_end[i] = file_lba + file_sectors;
                first_in_file = 0;
            } else if (!strncmp(p, "INDEX", 5) && cd_track_count && sscanf(p + 5, "%d %d:%d:%d", &n, &m, &s, &fr) == 4) {
                int32_t at = (m * 60 + s) * 75 + fr;
                if (n == 0) idx0[cd_track_count - 1] = at;
                else if (n == 1) idx1[cd_track_count - 1] = at;
            }
        }
        fclose(f);
        for (int i = 0; i < cd_track_count; i++) {
            Track *t = &cd_tracks[i];
            t->file_start = first[i] ? t->file_lba : t->file_lba + (uint32_t)(idx0[i] >= 0 ? idx0[i] : idx1[i]);
            t->start = t->file_lba + (uint32_t)idx1[i];
        }
        for (int i = 0; i < cd_track_count; i++) {
            uint32_t end = i + 1 < cd_track_count && !first[i + 1] ? cd_tracks[i + 1].file_start : file_end[i];
            cd_tracks[i].sectors = end > cd_tracks[i].file_start ? end - cd_tracks[i].file_start : 0;
        }
    } else {
        Track *t = &cd_tracks[cd_track_count++];
        snprintf(t->path, sizeof t->path, "%s", image);
        long size = file_size(image);
        t->start = t->file_start = t->file_lba = 0;
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
            /* (a track of a whole-disc .bin: from the file's start; a .bin per track: as before, from INDEX 01) */
            uint32_t base = t->file_lba != t->file_start ? t->file_lba : t->start;
            fseek(f, (long)(lba - base) * RAW, SEEK_SET);
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
