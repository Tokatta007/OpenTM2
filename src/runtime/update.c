/* The updater (update.h). */
#include "update.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../settings.h"

#ifndef TM2PC_VERSION
#define TM2PC_VERSION "v0.0.0"
#endif

static SDL_AtomicInt state;
static SDL_AtomicInt progress;
static SDL_AtomicInt got_bytes, total_bytes, cancel; /* (the download: bytes so far, of; the player closed its window) */
static char base_dir[1024];
static char latest[64], zip_url[1024], error_text[160];

UpdateState update_state(void) { return (UpdateState)SDL_GetAtomicInt(&state); }
const char *update_version(void) { return latest; }
const char *update_current(void) { return TM2PC_VERSION; }
int update_progress(void) { return SDL_GetAtomicInt(&progress); }
void update_bytes(int *got, int *total) { *got = SDL_GetAtomicInt(&got_bytes); *total = SDL_GetAtomicInt(&total_bytes); }
void update_cancel(void) { SDL_SetAtomicInt(&cancel, 1); }
const char *update_error(void) { return error_text; }

/* "v1.2.10" against "v1.2.9": numbers part by part (a leading v and anything after the numbers ignored) */
static int version_newer(const char *a, const char *b)
{
    while (*a && (*a < '0' || *a > '9')) a++;
    while (*b && (*b < '0' || *b > '9')) b++;
    for (int i = 0; i < 4; i++) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x > y;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* the player's own files and folders: never written by an update */
static int protected_path(const char *rel)
{
    static const char *const keep[] = {"tm2pc.ini", "tm2pc.log", "tm2pc-prev.log", "profiles/", "music/", "mods/", "screenshots/", "update/"};
    for (size_t i = 0; i < sizeof keep / sizeof keep[0]; i++) {
        size_t n = strlen(keep[i]);
        if (keep[i][n - 1] == '/' ? !SDL_strncasecmp(rel, keep[i], n) : !SDL_strcasecmp(rel, keep[i])) return 1;
    }
    return strstr(rel, "..") != NULL || rel[0] == '/' || rel[0] == '\\' || strchr(rel, ':') != NULL; /* (inside the folder only) */
}

/* a JSON string's value after "key": (GitHub's answer; enough for its plain strings) */
static int json_string(const char *json, const char *key, const char *from, char *out, size_t cap, const char **end)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(from ? from : json, k);
    if (!p) return 0;
    p = strchr(p + strlen(k), ':');
    if (!p) return 0;
    while (*++p == ' ') {}
    if (*p != '"') return 0;
    size_t n = 0;
    for (p++; *p && *p != '"' && n + 1 < cap; p++) out[n++] = *p == '\\' && p[1] ? *++p : *p;
    out[n] = 0;
    if (end) *end = p;
    return 1;
}

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include "miniz.h"

/* GET url into memory (max bytes), or to a file: 1 when the answer was 200 */
static int http_get(const char *url, char **mem, size_t *len, size_t max, FILE *file)
{
    wchar_t wurl[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024)) return 0;
    URL_COMPONENTS u;
    wchar_t host[256], path[1024];
    memset(&u, 0, sizeof u);
    u.dwStructSize = sizeof u;
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &u)) return 0;
    wchar_t agent[64];
    MultiByteToWideChar(CP_UTF8, 0, "TM2PC-Updater/" TM2PC_VERSION, -1, agent, 64);
    HINTERNET s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    int ok = 0;
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json, */*\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        DWORD total = 0;
        sz = sizeof total;
        if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total,
                                 &sz, WINHTTP_NO_HEADER_INDEX))
            total = 0;
        if (code == 200) {
            size_t got = 0;
            if (!mem) { SDL_SetAtomicInt(&total_bytes, (int)total); SDL_SetAtomicInt(&got_bytes, 0); }
            char *buf = mem ? malloc(max + 1) : NULL;
            ok = !mem || buf;
            for (DWORD avail = 0; ok && WinHttpQueryDataAvailable(r, &avail) && avail;) {
                char chunk[16384];
                DWORD want = avail < sizeof chunk ? avail : sizeof chunk, read = 0;
                if (!WinHttpReadData(r, chunk, want, &read) || !read) break;
                if (mem) {
                    if (got + read > max) { ok = 0; break; }
                    memcpy(buf + got, chunk, read);
                } else if (fwrite(chunk, 1, read, file) != read) { ok = 0; break; }
                got += read;
                if (!mem) SDL_SetAtomicInt(&got_bytes, (int)got);
                if (SDL_GetAtomicInt(&cancel)) { ok = 0; break; } /* (the player stopped it) */
                if (total) SDL_SetAtomicInt(&progress, (int)(got * 100 / total));
            }
            if (mem) {
                if (ok) { buf[got] = 0; *mem = buf; *len = got; }
                else free(buf);
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

static char exe_path[1100]; /* (this executable, as it started) */
static int SDLCALL check_thread(void *url)
{
    char *json = NULL;
    size_t len = 0;
    int found = 0;
    if (http_get((const char *)url, &json, &len, 1 << 20, NULL) && json_string(json, "tag_name", NULL, latest, sizeof latest, NULL)) {
        const char *at = json, *end;
        char u[1024];
        while (json_string(json, "browser_download_url", at, u, sizeof u, &end)) { /* the first .zip */
            size_t n = strlen(u);
            if (n > 4 && !SDL_strcasecmp(u + n - 4, ".zip")) { snprintf(zip_url, sizeof zip_url, "%s", u); break; }
            at = end;
        }
        found = zip_url[0] && version_newer(latest, TM2PC_VERSION);
        fprintf(stderr, "update: the newest is %s (this is %s)%s\n", latest, TM2PC_VERSION,
                found ? ": an update" : zip_url[0] ? "" : ", no .zip with it");
    } else {
        fprintf(stderr, "update: no answer from %s\n", (const char *)url);
    }
    free(json);
    free(url);
    SDL_SetAtomicInt(&state, found ? UPDATE_AVAILABLE : UPDATE_NONE);
    if (found && getenv("TM2PC_UPDATE_AUTO")) { /* tests: the download and the swap at once */
        int SDLCALL install_thread(void *unused);
        SDL_SetAtomicInt(&state, UPDATE_DOWNLOADING);
        install_thread(NULL);
    }
    return 0;
}

static void fail(const char *what)
{
    snprintf(error_text, sizeof error_text, "%s", what);
    fprintf(stderr, "update: %s\n", what);
    SDL_SetAtomicInt(&state, UPDATE_FAILED);
}

/* the zip's files over the old ones: each old one set aside as NAME.old (a running executable can be renamed), the
 * new one in its place; anything failing puts the old ones back */
int SDLCALL install_thread(void *unused)
{
    (void)unused;
    char dir[1100], zpath[1200];
    snprintf(dir, sizeof dir, "%supdate", base_dir);
    CreateDirectoryA(dir, NULL);
    snprintf(zpath, sizeof zpath, "%s\\%s.zip", dir, latest);
    FILE *f = fopen(zpath, "wb");
    if (!f) { fail("cannot write the download"); return 0; }
    int got = http_get(zip_url, NULL, NULL, 0, f);
    if (fclose(f) != 0) got = 0;
    if (!got) { remove(zpath); fail(SDL_GetAtomicInt(&cancel) ? "the update was stopped" : "the download failed"); return 0; }
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, zpath, 0)) { fail("the download is not a zip"); return 0; }
    int n = (int)mz_zip_reader_get_num_files(&z), done = 0, ok = 1;
    char (*moved)[1100] = calloc((size_t)n + 1, sizeof *moved); /* (the files set aside: put back on a failure) */
    for (int i = 0; ok && i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, (mz_uint)i, &st)) { ok = 0; break; }
        char rel[512];
        snprintf(rel, sizeof rel, "%s", st.m_filename);
        for (char *q = rel; *q; q++) if (*q == '\\') *q = '/';
        char *top = strchr(rel, '/'); /* (a zip of one folder, "TM2PC-v1.2.0/...": its contents) */
        const char *r = rel;
        if (top && n > 1) {
            int all = 1;
            for (int k = 0; k < n && all; k++) {
                char nm[512];
                mz_zip_reader_get_filename(&z, (mz_uint)k, nm, sizeof nm);
                all = !strncmp(nm, rel, (size_t)(top - rel + 1));
            }
            if (all) r = top + 1;
        }
        if (!*r || protected_path(r)) continue;
        char target[1100], fresh[1200], old[1200];
        snprintf(target, sizeof target, "%s%s", base_dir, r);
        for (char *q = target; *q; q++) if (*q == '/') *q = '\\';
        if (mz_zip_reader_is_file_a_directory(&z, (mz_uint)i)) { CreateDirectoryA(target, NULL); continue; }
        for (char *q = target + strlen(base_dir); *q; q++) /* (its folders) */
            if (*q == '\\') { *q = 0; CreateDirectoryA(target, NULL); *q = '\\'; }
        snprintf(fresh, sizeof fresh, "%s.new", target);
        snprintf(old, sizeof old, "%s.old", target);
        if (!mz_zip_reader_extract_to_file(&z, (mz_uint)i, fresh, 0)) { ok = 0; break; }
        if (GetFileAttributesA(target) != INVALID_FILE_ATTRIBUTES) {
            if (!MoveFileExA(target, old, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(fresh); ok = 0; break; }
            snprintf(moved[done++], sizeof moved[0], "%s", target);
        }
        if (!MoveFileExA(fresh, target, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(fresh); ok = 0; break; }
    }
    mz_zip_reader_end(&z);
    if (!ok) { /* the old files back */
        for (int i = done - 1; i >= 0; i--) {
            char old[1200];
            snprintf(old, sizeof old, "%s.old", moved[i]);
            MoveFileExA(old, moved[i], MOVEFILE_REPLACE_EXISTING);
        }
        free(moved);
        fail("the new files could not be put in place");
        return 0;
    }
    char list[1200]; /* (the files set aside: removed at the next start) */
    snprintf(list, sizeof list, "%s\\old.txt", dir);
    FILE *l = fopen(list, "a");
    for (int i = 0; l && i < done; i++) fprintf(l, "%s.old\n", moved[i]);
    if (l) fclose(l);
    free(moved);
    remove(zpath);
    fprintf(stderr, "update: %s in place (%d files)\n", latest, n);
    SDL_SetAtomicInt(&state, UPDATE_READY);
    return 0;
}

static void remove_old(void)
{
    char list[1200];
    snprintf(list, sizeof list, "%supdate\\old.txt", base_dir);
    FILE *l = fopen(list, "r");
    if (!l) return;
    char line[1200];
    int left = 0;
    while (fgets(line, sizeof line, l)) {
        line[strcspn(line, "\r\n")] = 0;
        size_t n = strlen(line);
        if (n > 4 && !strcmp(line + n - 4, ".old") && !strncmp(line, base_dir, strlen(base_dir)) && !DeleteFileA(line) &&
            GetFileAttributesA(line) != INVALID_FILE_ATTRIBUTES)
            left++;
    }
    fclose(l);
    if (!left) remove(list);
}

void update_start(const char *base)
{
    snprintf(base_dir, sizeof base_dir, "%s", base);
    if (!GetModuleFileNameA(NULL, exe_path, sizeof exe_path)) snprintf(exe_path, sizeof exe_path, "%stm2pc.exe", base_dir);
    remove_old();
    /* (started by RecompLauncher: it installs the updates itself, so the game never asks; its rule, recomplauncher.json) */
    const char *url = getenv("TM2PC_UPDATE_URL") ? getenv("TM2PC_UPDATE_URL")
                    : settings_update_check && !getenv("RECOMPLAUNCHER") ? settings_update_url : NULL;
    if (!url || !*url) return;
    SDL_SetAtomicInt(&state, UPDATE_CHECKING);
    SDL_Thread *t = SDL_CreateThread(check_thread, "update check", SDL_strdup(url));
    if (!t) SDL_SetAtomicInt(&state, UPDATE_NONE);
    else if (getenv("TM2PC_UPDATE_WAIT")) SDL_WaitThread(t, NULL); /* tests: the answer before the game goes on */
    else SDL_DetachThread(t);
}

void update_install(void)
{
    if (update_state() != UPDATE_AVAILABLE && update_state() != UPDATE_FAILED) return;
    SDL_SetAtomicInt(&progress, 0);
    SDL_SetAtomicInt(&state, UPDATE_DOWNLOADING);
    SDL_Thread *t = SDL_CreateThread(install_thread, "update install", NULL);
    if (t) SDL_DetachThread(t);
    else fail("cannot start the download");
}

void update_restart(void)
{
    if (update_state() != UPDATE_READY) return;
    const char *exe = exe_path; /* (this one's name, as it started: the new one there now) */
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "%s", GetCommandLineA()); /* (as it was started: its disc, its options) */
    if (getenv("TM2PC_UPDATE_NO_RESTART")) { fprintf(stderr, "update: would restart as %s\n", latest); exit(0); } /* tests */
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, base_dir, &si, &pi)) { fail("cannot start the new version"); return; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    fprintf(stderr, "update: restarting as %s\n", latest);
    exit(0);
}

#else /* (not Windows: no updater) */

void update_start(const char *base) { (void)base; }
void update_install(void) {}
void update_restart(void) {}

#endif
