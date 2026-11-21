/* The files of a disc image or a game folder, for the first-time setup (docs/FIRST_RUN_SETUP.md): the PS1 disc as
 * .cue / .bin (raw 2352-byte sectors, its data track), the PC version's disc as .iso (2048-byte sectors) or its
 * installed folder. Paths as the disc has them, upper case, from the root: "/SHELLDB1/TITLE.TPC" (no ";1"). */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct DiscFs DiscFs;

DiscFs *discfs_open(const char *path);   /* NULL: not a disc image or folder this reads */
void discfs_close(DiscFs *fs);
int discfs_count(const DiscFs *fs);       /* its files */
const char *discfs_path(const DiscFs *fs, int i);
int discfs_find(const DiscFs *fs, const char *path); /* the file's index (case ignored), -1: none */
/* a file's bytes (malloc'ed; *len its size): NULL if it cannot be read */
uint8_t *discfs_read(DiscFs *fs, int i, size_t *len);
/* a disc image's raw 2352-byte sector n of file i (its subheader too: a movie's audio and video): 0 if not one */
int discfs_raw(DiscFs *fs, int i, uint32_t n, uint8_t out[2352]);
uint32_t discfs_size(const DiscFs *fs, int i);
