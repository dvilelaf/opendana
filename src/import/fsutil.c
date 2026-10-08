/* fsutil.c - portable filesystem helpers. See fsutil.h.
 *
 * Paths are UTF-8 everywhere. On Windows each helper widens the path to UTF-16
 * and uses the wide C runtime / Win32 API: the narrow API (fopen, _mkdir...)
 * interprets bytes in the ANSI code page, so "ñ" or "日本" in a ROM path or in
 * %LOCALAPPDATA% failed before C6.29 (C6.22 PUB-8). */
#ifdef _WIN32
#define _CRT_RAND_S
#endif
#include "fsutil.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <wchar.h>
#include <windows.h>
#else
#include <signal.h>
#endif

static int separator(char c)
{
    return c == '/'
#ifdef _WIN32
        || c == '\\'
#endif
        ;
}

static char *last_separator(char *path)
{
    char *last = NULL;
    for (char *p = path; *p; ++p)
        if (separator(*p)) last = p;
    return last;
}

#ifdef _WIN32
/* UTF-8 -> malloc'd UTF-16. NULL when the bytes are not valid UTF-8. */
static wchar_t *widen(const char *utf8)
{
    if (utf8 == NULL) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

wchar_t *od_fs_widen(const char *utf8)
{
    return widen(utf8);
}

/* UTF-16 -> UTF-8 into a caller buffer. 0 on success. */
static int narrow(const wchar_t *w, char *out, size_t cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    return n > 0 ? 0 : -1;
}

static int wstat_path(const char *path, struct _stat64 *st)
{
    wchar_t *w = widen(path);
    if (w == NULL) { errno = EINVAL; return -1; }
    /* The CRT's stat fails on "dir\" or "dir/": strip trailing separators
     * (but keep a drive root such as "C:\"). */
    size_t n = wcslen(w);
    while (n > 1 && (w[n - 1] == L'/' || w[n - 1] == L'\\') && !(n == 3 && w[1] == L':'))
        w[--n] = L'\0';
    int rc = _wstat64(w, st);
    free(w);
    return rc;
}
#endif

FILE *od_fs_fopen(const char *path, const char *mode)
{
    if (path == NULL || mode == NULL) return NULL;
#ifdef _WIN32
    wchar_t *wp = widen(path), *wm = widen(mode);
    FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;
    free(wp);
    free(wm);
    return f;
#else
    return fopen(path, mode);
#endif
}

static int path_stat_mode(const char *path, int *is_dir, int *is_reg, unsigned long long *size)
{
#ifdef _WIN32
    struct _stat64 st;
    if (wstat_path(path, &st) != 0) return -1;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
#endif
    if (is_dir) *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    if (is_reg) *is_reg = S_ISREG(st.st_mode) ? 1 : 0;
    if (size) *size = (unsigned long long)st.st_size;
    return 0;
}

static int make_dir(const char *path)
{
#ifdef _WIN32
    wchar_t *w = widen(path);
    if (w == NULL) return -1;
    int rc = _wmkdir(w);
    free(w);
#else
    int rc = mkdir(path, 0777);
#endif
    if (rc == 0) return 0;
    int is_dir = 0;
    return errno == EEXIST && path_stat_mode(path, &is_dir, NULL, NULL) == 0 && is_dir ? 0 : -1;
}

int od_fs_mkdtemp(char *path)
{
    if (!path) return -1;
    size_t n = strlen(path);
    if (n < 6 || strcmp(path + n - 6, "XXXXXX") != 0) return -1;
#ifdef _WIN32
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        for (size_t i = n - 6; i < n; ++i) {
            unsigned int value;
            if (rand_s(&value) != 0) return -1;
            path[i] = chars[value % (sizeof(chars) - 1)];
        }
        wchar_t *w = widen(path);
        if (w == NULL) return -1;
        int rc = _wmkdir(w);
        free(w);
        if (rc == 0) return 0;
        if (errno != EEXIST) return -1;
    }
    return -1;
#else
    return mkdtemp(path) != NULL ? 0 : -1;
#endif
}

void od_fs_join(char *out, size_t cap, const char *dir, const char *name)
{
    if (dir == NULL || dir[0] == '\0' || strcmp(dir, ".") == 0) {
        snprintf(out, cap, "%s", name);
        return;
    }
    size_t n = strlen(dir);
    if (separator(dir[n - 1]))
        snprintf(out, cap, "%s%s", dir, name);
    else
        snprintf(out, cap, "%s/%s", dir, name);
}

int od_fs_exists(const char *path)
{
    return path != NULL && path_stat_mode(path, NULL, NULL, NULL) == 0;
}

int od_fs_is_dir(const char *path)
{
    int is_dir = 0;
    return path != NULL && path_stat_mode(path, &is_dir, NULL, NULL) == 0 && is_dir;
}

int od_fs_file_size(const char *path, unsigned long long *size)
{
    int is_dir = 0, is_reg = 0;
    unsigned long long n = 0;
    if (path == NULL || path_stat_mode(path, &is_dir, &is_reg, &n) != 0) return -1;
    if (is_dir || !is_reg) return -2;
    if (size) *size = n;
    return 0;
}

int od_fs_mkdir_p(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return -1;
    char tmp[4096];
    size_t n = strlen(path);
    if (n >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, n + 1);
    /* Strip a trailing slash (except for the root). */
    size_t root = separator(tmp[0]) ? 1 : 0;
#ifdef _WIN32
    if (n >= 3 && tmp[1] == ':' && separator(tmp[2])) root = 3;
    /* Do not attempt mkdir on a UNC server/share prefix. */
    if (n >= 2 && separator(tmp[0]) && separator(tmp[1])) {
        root = 2;
        while (root < n && !separator(tmp[root])) ++root;
        if (root < n) ++root;
        while (root < n && !separator(tmp[root])) ++root;
        if (root < n) ++root;
    }
#endif
    while (n > root && n > 1 && separator(tmp[n - 1]))
        tmp[--n] = '\0';
    for (char *p = tmp + (root ? root : 1); *p != '\0'; p++) {
        if (separator(*p)) {
            char sep = *p;
            *p = '\0';
            if (make_dir(tmp) != 0)
                return -1;
            *p = sep;
        }
    }
    if (make_dir(tmp) != 0)
        return -1;
    return 0;
}

int od_fs_write(const char *path, const void *data, size_t len)
{
    if (od_fs_mkdir_parent(path) != 0) return -1;
    FILE *f = od_fs_fopen(path, "wb");
    if (f == NULL)
        return -1;
    int ok = (len == 0) || fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

int od_fs_mkdir_parent(const char *path)
{
    char dir[4096];
    if (!path || !*path || strlen(path) >= sizeof(dir)) return -1;
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = last_separator(dir);
    if (slash == NULL)
        return 0;
#ifdef _WIN32
    if (slash == dir + 2 && dir[1] == ':') return 0;
#endif
    *slash = '\0';
    if (dir[0] == '\0' || strcmp(dir, ".") == 0)
        return 0;
    return od_fs_mkdir_p(dir);
}

int od_fs_list_dir(const char *dir, int (*fn)(const char *name, void *ctx), void *ctx)
{
    if (dir == NULL || fn == NULL) return -1;
#ifdef _WIN32
    wchar_t *w = widen(dir);
    if (w == NULL) return -1;
    _WDIR *d = _wopendir(w);
    free(w);
    if (d == NULL) return -1;
    struct _wdirent *e;
    while ((e = _wreaddir(d)) != NULL) {
        char name[1024];
        if (narrow(e->d_name, name, sizeof name) != 0) continue;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (fn(name, ctx) != 0) break;
    }
    _wclosedir(d);
#else
    DIR *d = opendir(dir);
    if (d == NULL) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (fn(e->d_name, ctx) != 0) break;
    }
    closedir(d);
#endif
    return 0;
}

typedef struct {
    const char *parent;
    int rc;
    int (*each)(const char *path);
} tree_walk;

static int walk_child(const char *name, void *ctx)
{
    tree_walk *t = (tree_walk *)ctx;
    char child[4096];
    if (strlen(t->parent) + strlen(name) + 2 > sizeof(child)) {
        t->rc = -1;
        return 0;
    }
    od_fs_join(child, sizeof(child), t->parent, name);
    if (t->each(child) != 0)
        t->rc = -1;
    return 0;
}

static int remove_dir_entry(const char *path)
{
#ifdef _WIN32
    wchar_t *w = widen(path);
    if (w == NULL) return -1;
    int rc = _wrmdir(w);
    free(w);
    return rc;
#else
    return rmdir(path);
#endif
}

static int remove_file_entry(const char *path)
{
#ifdef _WIN32
    wchar_t *w = widen(path);
    if (w == NULL) return -1;
    int rc = _wremove(w);
    free(w);
    return rc;
#else
    return remove(path);
#endif
}

int od_fs_remove_r(const char *path)
{
    if (!path || !*path) return -1;
#ifdef _WIN32
    wchar_t *w = widen(path);
    if (w == NULL) return -1;
    DWORD attrs = GetFileAttributesW(w);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        free(w);
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : -1;
    }
    /* stat() follows junctions/symlinks on Windows: never recurse into them. */
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        int ok = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(w) : DeleteFileW(w);
        free(w);
        return ok ? 0 : -1;
    }
    free(w);
    int is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    int is_dir = S_ISDIR(st.st_mode);
#endif
    if (is_dir) {
        tree_walk t = { path, 0, od_fs_remove_r };
        if (od_fs_list_dir(path, walk_child, &t) != 0)
            return -1;
        if (t.rc == 0 && remove_dir_entry(path) != 0)
            t.rc = -1;
        return t.rc;
    }
    return remove_file_entry(path) == 0 ? 0 : -1;
}

int od_fs_rename(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t *wf = widen(from), *wt = widen(to);
    int ok = wf && wt && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(wf);
    free(wt);
    return ok ? 0 : -1;
#else
    return rename(from, to);
#endif
}

char *od_fs_read(const char *path, size_t *size)
{
    FILE *f = od_fs_fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    if (size != NULL)
        *size = (size_t)len;
    return buf;
}

/* ------------------------------------------------------------------------- */
/* Durability                                                                 */
/* ------------------------------------------------------------------------- */

static int sync_one(const char *path)
{
#ifdef _WIN32
    wchar_t *w = widen(path);
    if (w == NULL) return -1;
    DWORD attrs = GetFileAttributesW(w);
    if (attrs == INVALID_FILE_ATTRIBUTES) { free(w); return -1; }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) { free(w); return 0; }
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok ? 0 : -1;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    /* Some filesystems refuse fsync on a directory descriptor: not an error. */
    if (rc != 0 && (errno == EINVAL || errno == EBADF || errno == ENOTSUP)) rc = 0;
    close(fd);
    return rc;
#endif
}

static int sync_tree_entry(const char *path)
{
    if (od_fs_is_dir(path)) {
        tree_walk t = { path, 0, sync_tree_entry };
        if (od_fs_list_dir(path, walk_child, &t) != 0) return -1;
        if (sync_one(path) != 0) t.rc = -1;
        return t.rc;
    }
    return sync_one(path);
}

int od_fs_sync_tree(const char *path)
{
    if (path == NULL || !od_fs_exists(path)) return -1;
    return sync_tree_entry(path);
}

int od_fs_sync_parent(const char *path)
{
#ifdef _WIN32
    (void)path;
    return 0;
#else
    char dir[4096];
    if (!path || !*path || strlen(path) >= sizeof(dir)) return -1;
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = last_separator(dir);
    if (slash == NULL) snprintf(dir, sizeof dir, ".");
    else if (slash == dir) dir[1] = '\0';
    else *slash = '\0';
    return sync_one(dir);
#endif
}

/* ------------------------------------------------------------------------- */
/* Processes                                                                  */
/* ------------------------------------------------------------------------- */

int od_fs_getenv(const char *name, char *out, size_t cap)
{
    if (name == NULL || out == NULL || cap == 0)
        return 0;
    out[0] = '\0';
#ifdef _WIN32
    wchar_t *wname = widen(name);
    if (wname == NULL) return 0;
    wchar_t value[4096];
    DWORD n = GetEnvironmentVariableW(wname, value, (DWORD)(sizeof value / sizeof value[0]));
    free(wname);
    if (n == 0 || n >= sizeof value / sizeof value[0]) return 0;
    return narrow(value, out, cap) == 0 && out[0] != '\0';
#else
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0' || strlen(v) >= cap) return 0;
    snprintf(out, cap, "%s", v);
    return 1;
#endif
}

long od_fs_getpid(void)
{
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

int od_fs_process_alive(long pid)
{
    if (pid <= 0) return 0;
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (h == NULL) return GetLastError() == ERROR_ACCESS_DENIED ? 1 : 0;
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    if (kill((pid_t)pid, 0) == 0) return 1;
    return errno == EPERM ? 1 : 0;
#endif
}
