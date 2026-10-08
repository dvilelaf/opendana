/* userdir.c - per-user data/config directories and atomic writes (C6.31).
 * See opendana/userdir.h for the contract and the per-platform table. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "opendana/userdir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#include "../import/fsutil.h"   /* the one UTF-8 -> UTF-16 implementation (C6.29) */
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

/* Files this module copies are tiny (save.dat 31 bytes, settings.ini < 1 KB). */
#define USERDIR_COPY_MAX (1024u * 1024u)

static od_status put(char *out, size_t cap, const char *fmt, const char *a,
                      const char *b, const char *c)
{
    int n = snprintf(out, cap, fmt, a, b, c);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return OD_ERR_RANGE;
    }
    return OD_OK;
}

#if !defined(_WIN32)   /* Windows reads its folders through od_fs_getenv() below */
static const char *env_nonempty(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' ? v : NULL;
}
#endif

#if defined(_WIN32)
/* The base folder as UTF-8, whatever the ANSI code page (C6.36): the file
 * helpers below widen it again, so a profile with CJK characters on a Western
 * Windows works. %name% (optionally + suffix) through the wide environment. */
static int win_env(const char *name, const char *suffix, char *out, size_t cap)
{
    char v[2048];
    if (!od_fs_getenv(name, v, sizeof v))
        return 0;
    int n = snprintf(out, cap, "%s%s", v, suffix != NULL ? suffix : "");
    return n > 0 && (size_t)n < cap;
}
#endif

/* The per-user base directory, without the application folder. */
static od_status base_dir(od_userdir_kind kind, char *out, size_t cap)
{
#if defined(_WIN32)
    int ok = kind == OD_USERDIR_CONFIG
           ? (win_env("APPDATA", NULL, out, cap) ||
              win_env("USERPROFILE", "\\AppData\\Roaming", out, cap))
           : (win_env("LOCALAPPDATA", NULL, out, cap) ||
              win_env("USERPROFILE", "\\AppData\\Local", out, cap));
    if (!ok) {
        out[0] = '\0';
        return OD_ERR_IO;
    }
    return OD_OK;
#elif defined(__APPLE__) && !(TARGET_OS_IPHONE || TARGET_OS_TV)
    (void)kind;
    const char *home = env_nonempty("HOME");
    if (home == NULL) {
        out[0] = '\0';
        return OD_ERR_IO;
    }
    return put(out, cap, "%s%s%s", home, "/Library/Application Support", "");
#else
    const char *xdg = env_nonempty(kind == OD_USERDIR_CONFIG ? "XDG_CONFIG_HOME"
                                                              : "XDG_DATA_HOME");
    if (xdg != NULL)
        return put(out, cap, "%s%s%s", xdg, "", "");
    const char *home = env_nonempty("HOME");
    if (home == NULL) {
        out[0] = '\0';
        return OD_ERR_IO;
    }
    return put(out, cap, "%s%s%s", home,
               kind == OD_USERDIR_CONFIG ? "/.config" : "/.local/share", "");
#endif
}

od_status od_userdir(od_userdir_kind kind, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return OD_ERR_RANGE;
    char base[2048];
    od_status st = base_dir(kind, base, sizeof base);
    if (st != OD_OK) {
        out[0] = '\0';
        return st;
    }
    return put(out, cap, "%s/%s%s", base, OD_USERDIR_APP, "");
}

od_status od_userdir_path(od_userdir_kind kind, const char *rel, char *out,
                            size_t cap)
{
    if (out == NULL || cap == 0 || rel == NULL || rel[0] == '\0')
        return OD_ERR_RANGE;
    char dir[2048];
    od_status st = od_userdir(kind, dir, sizeof dir);
    if (st != OD_OK) {
        out[0] = '\0';
        return st;
    }
    st = put(out, cap, "%s/%s%s", dir, rel, "");
    return st;
}

FILE *od_userdir_fopen(const char *path, const char *mode)
{
#if defined(_WIN32)
    return od_fs_fopen(path, mode);
#else
    return fopen(path, mode);
#endif
}

int od_userdir_remove(const char *path)
{
    if (path == NULL)
        return -1;
#if defined(_WIN32)
    return od_fs_remove_r(path);
#else
    return remove(path) == 0 || errno == ENOENT ? 0 : -1;
#endif
}

int od_userdir_mkdir_p(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return -1;
#if defined(_WIN32)
    return od_fs_mkdir_p(path);
#else
    char dir[2048];
    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir)
        return -1;
    for (char *p = dir + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return mkdir(dir, 0755) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

int od_userdir_rename(const char *from, const char *to)
{
#if defined(_WIN32)
    return od_fs_rename(from, to);
#else
    return rename(from, to);
#endif
}

od_status od_userdir_write_atomic(const char *path, const void *data, size_t len)
{
    if (path == NULL || (data == NULL && len != 0))
        return OD_ERR_RANGE;
    char tmp[2100];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return OD_ERR_RANGE;
    FILE *f = od_userdir_fopen(tmp, "wb");
    if (f == NULL)
        return OD_ERR_IO;
    size_t w = len ? fwrite(data, 1, len, f) : 0;
    int bad = w != len || fflush(f) != 0;
    /* The bytes reach the disk before the name points at them, so a crash
     * leaves either the old file or the new one, never a truncated one. */
#if defined(_WIN32)
    if (!bad && _commit(_fileno(f)) != 0)
        bad = 1;
#else
    if (!bad && fsync(fileno(f)) != 0)
        bad = 1;
#endif
    if (fclose(f) != 0)
        bad = 1;
    if (bad) {
        od_userdir_remove(tmp);
        return OD_ERR_IO;
    }
#if defined(_WIN32)
    /* Never unlink the old file on failure. ReplaceFile keeps it if the
     * replacement fails; MoveFileEx covers a new file and filesystems without
     * ReplaceFile. */
    wchar_t *wp = od_fs_widen(path), *wt = od_fs_widen(tmp);
    int replaced = 0;
    if (wp != NULL && wt != NULL) {
        if (GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES) {
            replaced = ReplaceFileW(wp, wt, NULL, 0, NULL, NULL) != 0;
            if (!replaced)
                replaced = MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING |
                                       MOVEFILE_WRITE_THROUGH) != 0;
        } else {
            replaced = MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING |
                                   MOVEFILE_WRITE_THROUGH) != 0;
        }
    }
    free(wp);
    free(wt);
    if (!replaced) {
        od_userdir_remove(tmp);
        return OD_ERR_IO;
    }
#else
    if (rename(tmp, path) != 0) {
        od_userdir_remove(tmp);
        return OD_ERR_IO;
    }
#endif
    return OD_OK;
}

static od_userdir_async g_async;

void od_userdir_set_async(const od_userdir_async *a)
{
    if (a == NULL) {
        od_userdir_flush();
        memset(&g_async, 0, sizeof g_async);
    } else {
        g_async = *a;
    }
}

od_status od_userdir_write_atomic_bg(const char *path, const void *data, size_t len)
{
    if (path == NULL || (data == NULL && len != 0))
        return OD_ERR_RANGE;
    if (g_async.submit != NULL && g_async.submit(g_async.ctx, path, data, len) == 0)
        return OD_OK;
    return od_userdir_write_atomic(path, data, len);
}

void od_userdir_flush(void)
{
    if (g_async.flush != NULL)
        g_async.flush(g_async.ctx);
}

od_status od_userdir_copy_file(const char *from, const char *to)
{
    if (from == NULL || to == NULL)
        return OD_ERR_RANGE;
    FILE *f = od_userdir_fopen(from, "rb");
    if (f == NULL)
        return OD_ERR_IO;
    unsigned char *buf = malloc(USERDIR_COPY_MAX);
    if (buf == NULL) {
        fclose(f);
        return OD_ERR_NOMEM;
    }
    size_t n = fread(buf, 1, USERDIR_COPY_MAX, f);
    int bad = ferror(f) || fgetc(f) != EOF;   /* larger than the limit */
    fclose(f);
    od_status st = bad ? OD_ERR_IO : od_userdir_write_atomic(to, buf, n);
    free(buf);
    return st;
}

#if defined(_WIN32)
unsigned long od_userdir_win_attrs(const char *path)
{
    wchar_t *w = od_fs_widen(path);
    DWORD a = w != NULL ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a;
}

void *od_userdir_win_open_noreparse(const char *path)
{
    wchar_t *w = od_fs_widen(path);
    if (w == NULL)
        return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(w);
    return h == INVALID_HANDLE_VALUE ? NULL : h;
}

int od_userdir_win_delete(const char *path)
{
    wchar_t *w = od_fs_widen(path);
    if (w == NULL)
        return -1;
    int ok = DeleteFileW(w) || GetLastError() == ERROR_FILE_NOT_FOUND;
    free(w);
    return ok ? 0 : -1;
}

int od_userdir_win_mkdir(const char *path)
{
    wchar_t *w = od_fs_widen(path);
    if (w == NULL)
        return 0;
    int ok = CreateDirectoryW(w, NULL) != 0;
    free(w);
    return ok;
}

int od_userdir_win_rmdir(const char *path)
{
    wchar_t *w = od_fs_widen(path);
    if (w == NULL)
        return -1;
    int ok = RemoveDirectoryW(w) != 0;
    free(w);
    return ok ? 0 : -1;
}

int od_userdir_win_move_replace(const char *from, const char *to)
{
    wchar_t *a = od_fs_widen(from), *b = od_fs_widen(to);
    int ok = a != NULL && b != NULL &&
             MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a);
    free(b);
    return ok ? 0 : -1;
}
#endif
