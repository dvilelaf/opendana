/* fsutil.h - filesystem helpers for the ROM importer (POSIX and MinGW).
 *
 * Every path is UTF-8. On Windows the helpers convert it to UTF-16 and call the
 * wide API (_wfopen, _wmkdir, MoveFileExW...), so a ROM or a data directory
 * under "C:\Users\Núñez\juegos\日本" works (C6.29, C6.22 PUB-8). */
#ifndef OD_IMPORT_FSUTIL_H
#define OD_IMPORT_FSUTIL_H

#include <stddef.h>
#include <stdio.h>

void od_fs_join(char *out, size_t cap, const char *dir, const char *name);

/* fopen() for a UTF-8 path (wide _wfopen on Windows). */
FILE *od_fs_fopen(const char *path, const char *mode);

/* Atomically create a new private directory from a mutable ...XXXXXX template.
 * Never reuses an existing directory. 0 on success. */
int od_fs_mkdtemp(char *path);

/* Create `path` and any missing parents (like `mkdir -p`). 0 on success. */
int od_fs_mkdir_p(const char *path);

/* Write a whole file, creating parent directories. 0 on success. */
int od_fs_write(const char *path, const void *data, size_t len);

/* 1 when `path` exists. */
int od_fs_exists(const char *path);

/* 1 when `path` is a directory (following links). */
int od_fs_is_dir(const char *path);

/* Size of a regular file. 0 on success; -1 when missing/unreadable; -2 when
 * `path` is a directory or not a regular file. */
int od_fs_file_size(const char *path, unsigned long long *size);

/* Recursively remove a file or directory; succeeds when it does not exist. */
int od_fs_remove_r(const char *path);

/* rename(2) wrapper (MoveFileExW on Windows): 0 on success. */
int od_fs_rename(const char *from, const char *to);

/* Read a whole file into a malloc'd, NUL-terminated buffer. NULL on error.
 * `size` receives the byte length without the terminator. */
char *od_fs_read(const char *path, size_t *size);

/* Create the parent directory of `path` (if any). 0 on success. */
int od_fs_mkdir_parent(const char *path);

/* Flush every file and directory under `path` to stable storage (fsync on
 * POSIX, FlushFileBuffers on Windows). 0 on success. */
int od_fs_sync_tree(const char *path);

/* Flush the directory that contains `path`, so a rename into it is durable.
 * A no-op that succeeds on Windows (MoveFileEx is asked to write through). */
int od_fs_sync_parent(const char *path);

/* Call `fn(name, ctx)` for every entry of `dir` except "." and "..", with the
 * name in UTF-8. Stops early when `fn` returns non-zero. 0 when the directory
 * was listed (also when empty), -1 when it cannot be opened. */
int od_fs_list_dir(const char *dir, int (*fn)(const char *name, void *ctx), void *ctx);

/* 1 when the process `pid` is (probably) still running, 0 when it is gone. */
int od_fs_process_alive(long pid);

/* The current process id. */
long od_fs_getpid(void);

/* getenv() as UTF-8 (GetEnvironmentVariableW on Windows, where the narrow
 * getenv is in the ANSI code page). 1 when set and non-empty and it fits. */
int od_fs_getenv(const char *name, char *out, size_t cap);

#ifdef _WIN32
#include <wchar.h>
/* UTF-8 -> malloc'd UTF-16 (free() it). NULL when not valid UTF-8. */
wchar_t *od_fs_widen(const char *utf8);
#endif

#endif /* OD_IMPORT_FSUTIL_H */
