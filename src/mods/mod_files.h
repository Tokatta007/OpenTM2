/* A mod's files for online play: what may travel, its fingerprint, receiving it safely (mod_files.c). */
#ifndef TM2PC_MOD_FILES_H
#define TM2PC_MOD_FILES_H
#include <stdint.h>

enum { MOD_FILE_NAME = 32, MOD_FILES_MAX = 64 };
#define MOD_FILE_MAX (40L * 1024 * 1024)   /* bytes a file: an ending's video; every other file 16 MB at most (mod_files.c) */
#define MOD_TOTAL_MAX (512L * 1024 * 1024) /* bytes a mod, whatever the player's limit */

typedef struct {
    char name[MOD_FILE_NAME];
    long size;
    uint64_t hash; /* FNV-1a 64 of its bytes */
} ModFile;

int mod_file_allowed(int kind, const char *name);                     /* the name may travel with that kind of mod */
int mod_file_sniff(const char *name, const uint8_t *data, long size); /* the contents look like what the name says */
/* a mod folder's allowed files (name order) and its fingerprint: the count, -1 if a file cannot be read */
int mod_package(const char *dir, int kind, ModFile *files, int max, uint64_t *fingerprint);

typedef struct {
    char game_folder[600], id[48], uid[40], why[120]; /* why: the reason a mod was refused */
    int kind, count, open;
    uint64_t fingerprint;
    ModFile files[MOD_FILES_MAX];
    long got[MOD_FILES_MAX], total, done;
} ModReceive;
/* limit_bytes: the player's download limit (0: none). 0 and r->why if refused. */
int mod_receive_begin(ModReceive *r, const char *game_folder, int kind, const char *id, const char *uid, uint64_t fingerprint,
                      const ModFile *files, int count, long limit_bytes);
int mod_receive_chunk(ModReceive *r, int file, long offset, const uint8_t *data, int len);
int mod_receive_finish(ModReceive *r, char *installed, int size); /* 1: in mods/ (installed: its folder) */
void mod_receive_cancel(ModReceive *r);

/* tm2pc --test-mod-download <mod folder> <a game folder to install into>: a mod sent through the receiver as a
 * server would send it, then the refusals (a tampered byte, a bad name, a program as a model, the limit). 0: all
 * as expected. */
int mod_files_selftest(const char *mod_dir, const char *game_folder);
#endif
