/* userdir.h - the per-user data and config directories (C6.31).
 *
 * One helper answers "where may the game keep the player's files?" for every
 * module that writes them (save.dat, the save states, settings.ini):
 *
 *   platform        data (save, states)                 config (settings)
 *   Linux / BSD     $XDG_DATA_HOME/opendana        $XDG_CONFIG_HOME/opendana
 *                   else ~/.local/share/opendana   else ~/.config/opendana
 *   Windows         %LOCALAPPDATA%\opendana        %APPDATA%\opendana
 *                   (read through the wide API as UTF-8, C6.36; else
 *                   %USERPROFILE%\AppData\...)
 *   macOS           ~/Library/Application Support/opendana (both)
 *
 * Before C6.31 the save, the states and the settings only knew XDG/HOME, so on
 * Windows (no HOME) they fell back to the current directory (PUB-12/UI-19).
 * The application folder name is the single constant OD_USERDIR_APP; a later
 * rename (C6.25) changes it here and migrates.
 *
 * Pure C11 plus the platform calls that need it (Win32 environment and file
 * replacement); no SDL. */
#ifndef OD_USERDIR_H
#define OD_USERDIR_H

#include "opendana/common.h"

#include <stddef.h>
#include <stdio.h>

/* The application folder inside the per-user directories. */
#define OD_USERDIR_APP "opendana"

typedef enum {
    OD_USERDIR_DATA = 0,   /* progress, states, catalogue, screenshots */
    OD_USERDIR_CONFIG = 1  /* settings.ini, controller mappings        */
} od_userdir_kind;

/* The directory itself (no trailing separator). OD_ERR_IO when no per-user
 * base can be determined (no XDG/HOME, no LOCALAPPDATA/APPDATA/USERPROFILE);
 * OD_ERR_RANGE when `cap` is too small. `out` is "" on any error. */
od_status od_userdir(od_userdir_kind kind, char *out, size_t cap);

/* od_userdir() joined with the relative path `rel` ("save.dat",
 * "states/slot-1.odstate"). Same errors. On macOS a file that exists only in
 * the pre-C6.31 location (~/.local/share or ~/.config, as save.c/settings.c
 * used to resolve) is copied to the new one the first time it is asked for,
 * so the player's progress is not left behind. */
od_status od_userdir_path(od_userdir_kind kind, const char *rel, char *out,
                            size_t cap);

/* Write `len` bytes to `path` atomically: a temporary file next to it, then a
 * replace that never leaves a half-written file and never unlinks the old one
 * on failure (rename on POSIX, ReplaceFile/MoveFileEx on Windows). The parent
 * directory must exist. */
od_status od_userdir_write_atomic(const char *path, const void *data, size_t len);

/* C6.41: write off the game thread. The fsync that makes the write durable
 * costs 60-235 ms on an ordinary disk, and the clear of a round saved the
 * progress on the frame that ends it (a visible hitch). A host that has a
 * worker installs it here (the core stays free of threads and SDL); the
 * submit hook must copy `data`, keep submissions in order and run each with
 * od_userdir_write_atomic, so atomicity and durability are unchanged. */
typedef struct {
    void *ctx;
    /* Queue a copy of `data` for `path`. Returns 0 when queued, nonzero when
     * it could not be (the caller then writes synchronously). */
    int (*submit)(void *ctx, const char *path, const void *data, size_t len);
    /* Block until every queued write has reached the disk. */
    void (*flush)(void *ctx);
} od_userdir_async;
void od_userdir_set_async(const od_userdir_async *a);   /* NULL = synchronous */

/* Same contract as od_userdir_write_atomic, but handed to the async worker when
 * one is installed (the status is then OD_OK at queueing; a failure of the
 * background write is reported on stderr by the worker). */
od_status od_userdir_write_atomic_bg(const char *path, const void *data, size_t len);

/* Wait for the queued writes (no-op without a worker). Called before any
 * read of a file that may still be queued, and at exit. */
void od_userdir_flush(void);

/* Copy `from` to `to` atomically (od_userdir_write_atomic). OD_ERR_IO when
 * `from` cannot be read. Files are small (save, settings): read whole. */
od_status od_userdir_copy_file(const char *from, const char *to);

/* File access for the paths this module returns (C6.36). On Windows they are
 * UTF-8 and these go through the wide API (the narrow fopen/rename/remove read
 * them in the ANSI code page); elsewhere they are the libc calls. Every
 * module that opens a user file uses them. */
FILE *od_userdir_fopen(const char *path, const char *mode);
/* Remove a file; 0 also when it does not exist. -1 on failure. */
int od_userdir_remove(const char *path);
/* rename(): wide MoveFileExW on Windows (it does not replace an existing
 * file there, as before). 0 on success. */
int od_userdir_rename(const char *from, const char *to);
/* Create `path` and its missing parents. 0 on success or when it exists. */
int od_userdir_mkdir_p(const char *path);

#if defined(_WIN32)
/* Windows only: the Win32 file calls the hardened thumbnail code needs, on
 * UTF-8 paths through the wide API (C6.36). Attributes are INVALID (0xFFFFFFFF)
 * on failure. */
unsigned long od_userdir_win_attrs(const char *path);
/* Open for reading without following a reparse point; the HANDLE, or NULL. */
void *od_userdir_win_open_noreparse(const char *path);
/* 0 when the file was deleted or did not exist, -1 otherwise. */
int od_userdir_win_delete(const char *path);
/* 1 when the directory was created (not when it exists). */
int od_userdir_win_mkdir(const char *path);
int od_userdir_win_rmdir(const char *path);
/* Atomic move that replaces `to`: 0 on success. */
int od_userdir_win_move_replace(const char *from, const char *to);
#endif

#endif /* OD_USERDIR_H */
