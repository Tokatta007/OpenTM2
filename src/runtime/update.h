/* The updater (the user's: an update check at boot, then ask, download, swap the files, restart; docs/RELEASE.md).
 *
 * A release is a zip of the game's own files (tm2pc.exe, fonts/, ui/, ...) as they go next to the executable; the
 * latest one is found by a GitHub Releases "latest" answer (tag_name, an asset whose name ends in .zip). The player's
 * own files are never written: tm2pc.ini, profiles/, music/, mods/, screenshots/, tm2pc.log. Windows only (WinHTTP);
 * elsewhere it finds nothing. */
#ifndef TM2PC_UPDATE_H
#define TM2PC_UPDATE_H

typedef enum {
    UPDATE_IDLE,        /* not checked (off, or not started) */
    UPDATE_CHECKING,    /* asking */
    UPDATE_NONE,        /* this is the newest, or the answer could not be had */
    UPDATE_AVAILABLE,   /* a newer one: update_version() */
    UPDATE_DOWNLOADING, /* update_progress() percent */
    UPDATE_READY,       /* the new files in place: restart to use them */
    UPDATE_FAILED       /* the download or the swap failed (the old files kept): update_error() */
} UpdateState;

/* at boot: the folder of the executable (ending in a slash), then the check in the background (the [update] settings;
 * TM2PC_UPDATE_URL overrides the address). The files an earlier update set aside (*.old) are removed first. */
void update_start(const char *base);
UpdateState update_state(void);
const char *update_version(void); /* the newer version's name ("v1.2.0") */
const char *update_current(void); /* this build's */
int update_progress(void);        /* 0..100 while downloading */
const char *update_error(void);
void update_install(void);        /* UPDATE_AVAILABLE: download and swap, in the background */
void update_restart(void);        /* UPDATE_READY: the new executable started, this one ends */
void update_bytes(int *got, int *total); /* the download so far (total 0: not known) */
void update_cancel(void);         /* the download stopped (the old files kept) */
/* at start, before the game's window (the user's: the update all before the game): the check (a few seconds at most),
 * then "New update vX is available..." Yes / No; Yes: a window with the download's bar, then the new version started
 * (this one ends). Returns when the game should start (no update, No, the window closed, or a failure). */
void update_pregame(const char *base);

#endif
