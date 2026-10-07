/* platform.c - implementation of the portable path/asset layer.
 *
 * Pure C11/SDL-free so it is part of opendana_core and can be unit-tested without a
 * window. See include/opendana/platform.h for the contract.
 */
#include "opendana/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#  include <TargetConditionals.h>
#  include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#  include <direct.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <windows.h>
#  define OD_MKDIR1(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  if defined(__linux__)
#    include <unistd.h>
#  endif
#  define OD_MKDIR1(p) mkdir((p), 0700)
#endif

static int path_is_dir(const char *path)
{
#if defined(_WIN32)
    struct _stat st;
    return _stat(path, &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static od_status copy_str(char *out, size_t cap, const char *s)
{
    if (out == NULL || cap == 0)
        return OD_ERR_RANGE;
    if (s == NULL) {
        out[0] = '\0';
        return OD_OK;
    }
    size_t n = strlen(s);
    if (n >= cap) {
        out[0] = '\0';
        return OD_ERR_RANGE;
    }
    memcpy(out, s, n + 1);
    return OD_OK;
}

/* Drop the final path component, in place. */
static void strip_filename(char *path)
{
    for (size_t i = strlen(path); i > 0; i--) {
        char c = path[i - 1];
        if (c == '/' || c == '\\') {
            path[i - 1] = '\0';
            return;
        }
    }
    path[0] = '\0';
}

/* Create `path` and any missing parents. Returns 1 on success. */
static int ensure_dir(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return 0;
    if (path_is_dir(path))
        return 1;

    char tmp[1200];
    size_t len = strlen(path);
    if (len >= sizeof(tmp))
        return 0;
    memcpy(tmp, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char save = tmp[i];
            tmp[i] = '\0';
            /* Never try to mkdir a bare drive prefix such as "C:". */
            if (!(i == 2 && tmp[1] == ':'))
                (void)OD_MKDIR1(tmp);
            tmp[i] = save;
        }
    }
    if (OD_MKDIR1(tmp) == 0)
        return 1;
    return path_is_dir(tmp);
}

const char *od_platform_name(void)
{
#if defined(__EMSCRIPTEN__)
    return "emscripten";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__)
#  if TARGET_OS_IPHONE || TARGET_OS_TV
    return "ios";
#  else
    return "macos";
#  endif
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__unix__)
    return "unix";
#else
    return "unknown";
#endif
}

char *od_platform_join(char *out, size_t cap, const char *dir, const char *name)
{
    if (out == NULL || cap == 0)
        return out;
    const char *n = name != NULL ? name : "";
    size_t nlen = strlen(n);

    if (dir == NULL || dir[0] == '\0' || strcmp(dir, ".") == 0) {
        size_t copy = nlen < cap - 1 ? nlen : cap - 1;
        memcpy(out, n, copy);
        out[copy] = '\0';
        return out;
    }

    size_t dlen = strlen(dir);
    size_t pos = dlen < cap - 1 ? dlen : cap - 1;
    memcpy(out, dir, pos);

    char last = dir[dlen - 1];
    if (last != '/' && last != '\\' && pos + 1 < cap)
        out[pos++] = '/';

    size_t room = cap - 1 - pos;
    size_t copy = nlen < room ? nlen : room;
    memcpy(out + pos, n, copy);
    out[pos + copy] = '\0';
    return out;
}

int od_platform_executable_dir(char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return 0;
    out[0] = '\0';

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || \
    (defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_TV))
    /* No meaningful executable directory on these sandboxes. */
    return 0;
#elif defined(_WIN32)
    char buf[4096];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (n == 0 || (size_t)n >= sizeof(buf))
        return 0;
    buf[sizeof(buf) - 1] = '\0';
    if (copy_str(out, cap, buf) != OD_OK)
        return 0;
    strip_filename(out);
    return out[0] != '\0';
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    if (size == 0)
        return 0;
    char *buf = (char *)malloc(size);
    if (buf == NULL)
        return 0;
    if (_NSGetExecutablePath(buf, &size) != 0) {
        free(buf);
        return 0;
    }
    od_status st = copy_str(out, cap, buf);
    free(buf);
    if (st != OD_OK)
        return 0;
    strip_filename(out);
    return out[0] != '\0';
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) {
        out[0] = '\0';
        return 0;
    }
    out[n] = '\0';
    strip_filename(out);
    return out[0] != '\0';
#else
    return 0;
#endif
}

int od_platform_asset_dir_valid(const char *dir)
{
    if (dir == NULL || dir[0] == '\0')
        return 0;
    char path[1200];
    od_platform_join(path, sizeof(path), dir, "levels.bin");
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

od_status od_platform_resolve_assets(char *out, size_t cap, const char *cli_override)
{
    if (out == NULL || cap == 0)
        return OD_ERR_RANGE;
    out[0] = '\0';

    if (cli_override != NULL && cli_override[0] != '\0')
        return copy_str(out, cap, cli_override);

    const char *env = getenv("OD_ASSETS");
    if (env != NULL && env[0] != '\0')
        return copy_str(out, cap, env);

#if defined(__EMSCRIPTEN__)
    /* --preload-file assets@/assets places the files here. */
    return copy_str(out, cap, "/assets");
#else
    char exe[1200];
    char candidates[3][1200];
    int n = 0;

    if (od_platform_executable_dir(exe, sizeof(exe))) {
        od_platform_join(candidates[n], sizeof(candidates[0]), exe, "assets");
        n++;
        od_platform_join(candidates[n], sizeof(candidates[0]), exe, "../assets");
        n++;
    }
    snprintf(candidates[n], sizeof(candidates[0]), "assets");
    n++;

    for (int i = 0; i < n; i++) {
        if (od_platform_asset_dir_valid(candidates[i]))
            return copy_str(out, cap, candidates[i]);
    }

    /* Nothing valid found: hand back the first candidate so the loader can
     * report a concrete path instead of an empty string. */
    return copy_str(out, cap, candidates[0]);
#endif
}

od_status od_platform_user_data_dir(char *out, size_t cap, int create)
{
    if (out == NULL || cap == 0)
        return OD_ERR_RANGE;
    out[0] = '\0';

    char dir[1200];

    const char *env = getenv("OD_DATA_DIR");
    if (env != NULL && env[0] != '\0') {
        if (copy_str(dir, sizeof(dir), env) != OD_OK)
            return OD_ERR_RANGE;
    } else {
#if defined(__EMSCRIPTEN__)
        snprintf(dir, sizeof(dir), "%s", "/persistent/opendana");
#elif defined(_WIN32)
        const char *base = getenv("APPDATA");
        if (base == NULL || base[0] == '\0')
            base = ".";
        od_platform_join(dir, sizeof(dir), base, "opendana");
#elif defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_TV)
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
            home = ".";
        snprintf(dir, sizeof(dir), "%s/Documents/opendana", home);
#elif defined(__APPLE__)
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
            home = ".";
        snprintf(dir, sizeof(dir), "%s/Library/Application Support/opendana", home);
#elif defined(__ANDROID__)
        /* App-private storage; the front end may point OD_DATA_DIR at
         * SDL_AndroidGetInternalStoragePath(). */
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
            home = ".";
        snprintf(dir, sizeof(dir), "%s/opendana", home);
#else
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg != NULL && xdg[0] != '\0') {
            od_platform_join(dir, sizeof(dir), xdg, "opendana");
        } else {
            const char *home = getenv("HOME");
            if (home == NULL || home[0] == '\0')
                home = ".";
            snprintf(dir, sizeof(dir), "%s/.local/share/opendana", home);
        }
#endif
    }

    if (create && !ensure_dir(dir))
        return OD_ERR_IO;

    return copy_str(out, cap, dir);
}

od_status od_platform_user_data_path(char *out, size_t cap, const char *name, int create)
{
    char dir[1200];
    od_status st = od_platform_user_data_dir(dir, sizeof(dir), create);
    if (st != OD_OK)
        return st;
    if (out == NULL || cap == 0)
        return OD_ERR_RANGE;
    od_platform_join(out, cap, dir, name);
    return OD_OK;
}
