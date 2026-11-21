/* The files of a disc image or a game folder (discfs.h). */
#include "discfs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

typedef struct {
    char path[128];     /* upper case, for finding */
    char real[128];     /* (a folder's) as it is named */
    uint32_t lba, size; /* (a disc's) */
} Entry;

struct DiscFs {
    FILE *f;            /* a disc image (NULL: a folder) */
    int sector, user;   /* bytes a sector, and where its 2048 user bytes start */
    char folder[1024];  /* a folder's root, ending in a slash */
    Entry *e;
    int n, cap;
};

static void add(DiscFs *fs, const char *path, uint32_t lba, uint32_t size)
{
    if (fs->n == fs->cap) {
        int cap = fs->cap ? fs->cap * 2 : 256;
        Entry *e = realloc(fs->e, (size_t)cap * sizeof *e);
        if (!e) return;
        fs->e = e;
        fs->cap = cap;
    }
    Entry *x = &fs->e[fs->n++];
    snprintf(x->path, sizeof x->path, "%s", path);
    snprintf(x->real, sizeof x->real, "%s", path);
    for (char *p = x->path; *p; p++) *p = (char)toupper((unsigned char)*p);
    x->lba = lba;
    x->size = size;
}

static int read_sector(DiscFs *fs, uint32_t lba, uint8_t out[2048])
{
    if (fseek(fs->f, (long)lba * fs->sector + fs->user, SEEK_SET)) return 0;
    return fread(out, 1, 2048, fs->f) == 2048;
}

/* ISO 9660: a directory's records, its subdirectories walked (depth: a bad disc cannot loop for ever) */
static void walk(DiscFs *fs, uint32_t lba, uint32_t size, const char *prefix, int depth)
{
    if (depth > 8 || size > 1u << 20) return;
    uint8_t s[2048];
    for (uint32_t done = 0; done < size; done += 2048, lba++) {
        if (!read_sector(fs, lba, s)) return;
        for (int at = 0; at < 2048 && s[at];) {
            int len = s[at];
            if (at + len > 2048 || len < 34) break;
            const uint8_t *r = s + at;
            uint32_t ext = (uint32_t)r[2] | r[3] << 8 | r[4] << 16 | (uint32_t)r[5] << 24;
            uint32_t sz = (uint32_t)r[10] | r[11] << 8 | r[12] << 16 | (uint32_t)r[13] << 24;
            int flags = r[25], nl = r[32];
            if (nl == 1 && (r[33] == 0 || r[33] == 1)) { at += len; continue; } /* (. and ..) */
            char name[64];
            int k = 0;
            for (; k < nl && k < 63 && r[33 + k] != ';'; k++) name[k] = (char)r[33 + k];
            name[k] = 0;
            if (k && name[k - 1] == '.') name[k - 1] = 0;
            char path[128];
            snprintf(path, sizeof path, "%s%s", prefix, name);
            if (flags & 2) {
                char sub[130];
                snprintf(sub, sizeof sub, "%s/", path);
                walk(fs, ext, sz, sub, depth + 1);
            } else {
                add(fs, path, ext, sz);
            }
            at += len;
        }
    }
}

/* a .cue's first FILE (the data track), beside the .cue */
static int cue_data_track(const char *cue, char *out, size_t cap)
{
    FILE *f = fopen(cue, "r");
    if (!f) return 0;
    char line[1024];
    int ok = 0;
    while (!ok && fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (strncmp(p, "FILE", 4)) continue;
        char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!q1 || !q2) continue;
        *q2 = 0;
        const char *slash = strrchr(cue, '/'), *back = strrchr(cue, '\\');
        if (back > slash) slash = back;
        snprintf(out, cap, "%.*s%s", slash ? (int)(slash - cue + 1) : 0, cue, q1 + 1);
        ok = 1;
    }
    fclose(f);
    return ok;
}

static void walk_folder(DiscFs *fs, const char *dir, const char *prefix, int depth)
{
    if (depth > 8) return;
#ifdef _WIN32
    char pat[1200];
    snprintf(pat, sizeof pat, "%s*", dir);
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(d.cFileName, ".") || !strcmp(d.cFileName, "..")) continue;
        char path[128], sub[1200];
        snprintf(path, sizeof path, "%s%s", prefix, d.cFileName);
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char pre[130];
            snprintf(sub, sizeof sub, "%s%s\\", dir, d.cFileName);
            snprintf(pre, sizeof pre, "%s/", path);
            walk_folder(fs, sub, pre, depth + 1);
        } else {
            add(fs, path, 0, d.nFileSizeLow);
        }
    } while (FindNextFileA(h, &d));
    FindClose(h);
#else
    DIR *dd = opendir(dir);
    if (!dd) return;
    for (struct dirent *d; (d = readdir(dd));) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
        char path[128], sub[1200];
        snprintf(path, sizeof path, "%s%s", prefix, d->d_name);
        snprintf(sub, sizeof sub, "%s%s", dir, d->d_name);
        struct stat st;
        if (stat(sub, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            char pre[130];
            strcat(sub, "/");
            snprintf(pre, sizeof pre, "%s/", path);
            walk_folder(fs, sub, pre, depth + 1);
        } else {
            add(fs, path, 0, (uint32_t)st.st_size);
        }
    }
    closedir(dd);
#endif
}

DiscFs *discfs_open(const char *path)
{
    DiscFs *fs = calloc(1, sizeof *fs);
    if (!fs) return NULL;
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    int is_dir = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    int is_dir = !stat(path, &st) && S_ISDIR(st.st_mode);
#endif
    if (is_dir) { /* an installed game */
        size_t n = strlen(path);
        snprintf(fs->folder, sizeof fs->folder, "%s%s", path, n && (path[n - 1] == '/' || path[n - 1] == '\\') ? "" : "/");
        walk_folder(fs, fs->folder, "/", 0);
        if (!fs->n) { discfs_close(fs); return NULL; }
        return fs;
    }
    char image[1100];
    const char *ext = strrchr(path, '.');
    if (ext && (!strcmp(ext, ".cue") || !strcmp(ext, ".CUE") || !strcmp(ext, ".Cue"))) { if (!cue_data_track(path, image, sizeof image)) { free(fs); return NULL; } }
    else snprintf(image, sizeof image, "%s", path);
    fs->f = fopen(image, "rb");
    if (!fs->f) { free(fs); return NULL; }
    /* the sector size: raw (a sync pattern: 00 FF x10 00; its mode byte says where the data is) or 2048 */
    uint8_t head[16];
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    fs->sector = 2048;
    fs->user = 0;
    if (fread(head, 1, 16, fs->f) == 16 && !memcmp(head, sync, 12)) {
        fs->sector = 2352;
        fs->user = head[15] == 2 ? 24 : 16; /* (Mode 2 Form 1: an 8-byte subheader before the data) */
    }
    uint8_t pvd[2048];
    if (!read_sector(fs, 16, pvd) || memcmp(pvd + 1, "CD001", 5)) { discfs_close(fs); return NULL; }
    const uint8_t *root = pvd + 156;
    walk(fs, (uint32_t)root[2] | root[3] << 8 | root[4] << 16 | (uint32_t)root[5] << 24,
         (uint32_t)root[10] | root[11] << 8 | root[12] << 16 | (uint32_t)root[13] << 24, "/", 0);
    if (!fs->n) { discfs_close(fs); return NULL; }
    return fs;
}

void discfs_close(DiscFs *fs)
{
    if (!fs) return;
    if (fs->f) fclose(fs->f);
    free(fs->e);
    free(fs);
}

int discfs_count(const DiscFs *fs) { return fs ? fs->n : 0; }
uint32_t discfs_size(const DiscFs *fs, int i) { return fs && i >= 0 && i < fs->n ? fs->e[i].size : 0; }
int discfs_raw(DiscFs *fs, int i, uint32_t n, uint8_t out[2352])
{
    if (!fs || !fs->f || fs->sector != 2352 || i < 0 || i >= fs->n) return 0;
    if (fseek(fs->f, (long)(fs->e[i].lba + n) * 2352, SEEK_SET)) return 0;
    return fread(out, 1, 2352, fs->f) == 2352;
}
const char *discfs_path(const DiscFs *fs, int i) { return fs && i >= 0 && i < fs->n ? fs->e[i].path : ""; }

int discfs_find(const DiscFs *fs, const char *path)
{
    for (int i = 0; fs && i < fs->n; i++) {
        const char *a = fs->e[i].path, *b = path;
        /* (case ignored; '-' and '_' the same: the PC disc's ISO 9660 names have '_' where its installed files have '-') */
        while (*a && (toupper((unsigned char)*a) == toupper((unsigned char)*b) || ((*a == '-' || *a == '_') && (*b == '-' || *b == '_'))))
            a++, b++;
        if (!*a && !*b) return i;
    }
    return -1;
}

uint8_t *discfs_read(DiscFs *fs, int i, size_t *len)
{
    if (!fs || i < 0 || i >= fs->n) return NULL;
    Entry *e = &fs->e[i];
    uint8_t *out = malloc(e->size ? e->size : 1);
    if (!out) return NULL;
    if (!fs->f) { /* a folder's file */
        char p[1200];
        snprintf(p, sizeof p, "%s%s", fs->folder, e->real + 1);
        FILE *f = fopen(p, "rb");
        size_t got = f ? fread(out, 1, e->size, f) : 0;
        if (f) fclose(f);
        if (got != e->size) { free(out); return NULL; }
    } else {
        uint8_t s[2048];
        for (uint32_t done = 0; done < e->size; done += 2048) {
            if (!read_sector(fs, e->lba + done / 2048, s)) { free(out); return NULL; }
            uint32_t n = e->size - done < 2048 ? e->size - done : 2048;
            memcpy(out + done, s, n);
        }
    }
    *len = e->size;
    return out;
}
