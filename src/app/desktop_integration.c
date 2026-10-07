/* desktop_integration.c - opt-in freedesktop launcher and icons (C6.46). */
#define _POSIX_C_SOURCE 200809L
#include "desktop_integration.h"

#include <stdio.h>
#include <string.h>

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define ICON_NAME "opendana"
#define PATH_CAP 4096

static const int k_sizes[] = {16, 24, 32, 48, 64, 128, 256, 512};
#define N_SIZES ((int)(sizeof k_sizes / sizeof k_sizes[0]))

static void say(char *msg, size_t cap, const char *fmt, const char *a)
{
    if (msg != NULL && cap > 0)
        snprintf(msg, cap, fmt, a != NULL ? a : "");
}

static int join(char *out, const char *a, const char *b)
{
    int n = snprintf(out, PATH_CAP, "%s/%s", a, b);
    return n > 0 && n < PATH_CAP ? 0 : -1;
}

static int data_home(const od_desktop_cfg *cfg, char *out)
{
    const char *d = cfg != NULL ? cfg->data_home : NULL;
    if (d == NULL || d[0] == '\0') {
        d = getenv("XDG_DATA_HOME");
        if (d != NULL && d[0] != '/')
            d = NULL;                 /* the spec: relative values are invalid */
    }
    if (d != NULL && d[0] != '\0') {
        int n = snprintf(out, PATH_CAP, "%s", d);
        return n > 0 && n < PATH_CAP ? 0 : -1;
    }
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0')
        return -1;
    int n = snprintf(out, PATH_CAP, "%s/.local/share", home);
    return n > 0 && n < PATH_CAP ? 0 : -1;
}

static int exe_path(char *out)
{
    ssize_t n = readlink("/proc/self/exe", out, PATH_CAP - 1);
    if (n <= 0)
        return -1;
    out[n] = '\0';
    return 0;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* The hicolor tree shipped with the package. */
static int icon_source(const od_desktop_cfg *cfg, char *out)
{
    const char *s = cfg != NULL ? cfg->icon_src : NULL;
    if (s == NULL || s[0] == '\0')
        s = getenv("OD_DESKTOP_ICON_SRC");
    if (s != NULL && s[0] != '\0') {
        int n = snprintf(out, PATH_CAP, "%s", s);
        return n > 0 && n < PATH_CAP && is_dir(out) ? 0 : -1;
    }
    char exe[PATH_CAP];
    if (exe_path(exe) != 0)
        return -1;
    char *slash = strrchr(exe, '/');
    if (slash == NULL)
        return -1;
    *slash = '\0';                    /* .../bin  (AppImage: usr/bin)        */
    int n = snprintf(out, PATH_CAP, "%s/../share/icons/hicolor", exe);
    return n > 0 && n < PATH_CAP && is_dir(out) ? 0 : -1;
}

static int icon_files_present(const char *src)
{
    char p[PATH_CAP], rel[64];
    for (int i = 0; i < N_SIZES; i++) {
        snprintf(rel, sizeof rel, "%dx%d/apps/%s.png", k_sizes[i], k_sizes[i], ICON_NAME);
        if (join(p, src, rel) != 0 || !is_file(p))
            return 0;
    }
    return join(p, src, "scalable/apps/" ICON_NAME ".svg") == 0 && is_file(p);
}

int od_desktop_supported(void) { return 1; }

int od_desktop_packaged(const od_desktop_cfg *cfg)
{
    const char *ai = getenv("APPIMAGE");
    if (ai != NULL && ai[0] != '\0')
        return 1;
    char src[PATH_CAP];
    return icon_source(cfg, src) == 0;
}

static int desktop_path(const od_desktop_cfg *cfg, char *out)
{
    char base[PATH_CAP];
    if (data_home(cfg, base) != 0)
        return -1;
    return join(out, base, "applications/" ICON_NAME ".desktop");
}

static int file_has_marker(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (strcmp(line, OD_DESKTOP_MARKER) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

int od_desktop_installed(const od_desktop_cfg *cfg)
{
    char p[PATH_CAP];
    return desktop_path(cfg, p) == 0 && file_has_marker(p);
}

static int mkdir_p(const char *path)
{
    char tmp[PATH_CAP];
    int n = snprintf(tmp, sizeof tmp, "%s", path);
    if (n <= 0 || n >= (int)sizeof tmp)
        return -1;
    for (char *c = tmp + 1; *c; c++) {
        if (*c != '/')
            continue;
        *c = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return -1;
        *c = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int parent_mkdir(const char *file)
{
    char dir[PATH_CAP];
    snprintf(dir, sizeof dir, "%s", file);
    char *s = strrchr(dir, '/');
    if (s == NULL)
        return -1;
    *s = '\0';
    return mkdir_p(dir);
}

/* Write `len` bytes to `path` atomically (temp file in the same folder). */
static int write_atomic(const char *path, const void *data, size_t len)
{
    char tmp[PATH_CAP];
    if (snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid()) >= (int)sizeof tmp ||
        parent_mkdir(path) != 0)
        return -1;
    FILE *f = fopen(tmp, "wb");
    if (f == NULL)
        return -1;
    int ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

static int copy_file(const char *from, const char *to)
{
    FILE *f = fopen(from, "rb");
    if (f == NULL)
        return -1;
    long size = 0;
    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && size < (8L << 20) &&
        fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)size)) != NULL &&
        fread(buf, 1, (size_t)size, f) == (size_t)size) {
        fclose(f);
        int rc = write_atomic(to, buf, (size_t)size);
        free(buf);
        return rc;
    }
    fclose(f);
    free(buf);
    return -1;
}

/* Exec= value per the Desktop Entry spec: a quoted argument in which " ` $ \
 * are escaped with a backslash, and the whole string-value escaped again
 * (every backslash doubled); % becomes %%. */
static int quote_exec(const char *path, char *out, size_t cap)
{
    size_t n = 0;
    if (cap < 3)
        return -1;
    out[n++] = '"';
    for (const char *c = path; *c; c++) {
        if (n + 6 >= cap)
            return -1;
        if (*c == '%') {
            out[n++] = '%'; out[n++] = '%';
        } else if (*c == '\\') {
            memcpy(out + n, "\\\\\\\\", 4); n += 4;
        } else if (*c == '"' || *c == '`' || *c == '$') {
            out[n++] = '\\'; out[n++] = '\\'; out[n++] = *c;
        } else if (*c == '\n') {
            return -1;
        } else {
            out[n++] = *c;
        }
    }
    out[n++] = '"';
    out[n] = '\0';
    return 0;
}

/* Run `prog args...` without a shell, output discarded. 0 = ran and succeeded. */
static int spawn_quiet(const char *prog, const char *a1, const char *a2,
                       const char *a3, const char *a4)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execlp(prog, prog, a1, a2, a3, a4, (char *)NULL);
        _exit(127);                  /* not installed */
    }
    int st;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static void refresh_database(const char *apps_dir)
{
    (void)spawn_quiet("update-desktop-database", "-q", apps_dir, NULL, NULL);
}

/* GTK trusts icon-theme.cache while it is not older than the top hicolor dir,
 * and writing into NxN/apps does not touch that dir. So after adding or
 * removing our icons: if a cache exists, rebuild it; if that cannot be done
 * (and when there is no cache) bump the dir mtime. Never create a cache. */
static void refresh_icon_cache(const char *base)
{
    char theme[PATH_CAP], cache[PATH_CAP];
    if (join(theme, base, "icons/hicolor") != 0 || join(cache, theme, "icon-theme.cache") != 0)
        return;
    if (is_file(cache) &&
        spawn_quiet("gtk-update-icon-cache", "-q", "-f", "-t", theme) == 0)
        return;
    (void)utimensat(AT_FDCWD, theme, NULL, 0);   /* NULL = now */
}

int od_desktop_install(const od_desktop_cfg *cfg, char *msg, size_t cap)
{
    char base[PATH_CAP], dpath[PATH_CAP], src[PATH_CAP];
    if (data_home(cfg, base) != 0 || desktop_path(cfg, dpath) != 0) {
        say(msg, cap, "Cannot find your data folder (HOME is not set)%s", "");
        return OD_DESKTOP_ERROR;
    }
    if (icon_source(cfg, src) != 0 || !icon_files_present(src)) {
        say(msg, cap, "The package icons were not found; nothing was installed%s", "");
        return OD_DESKTOP_NO_ICONS;
    }
    if (is_file(dpath) && !file_has_marker(dpath)) {
        say(msg, cap, "%s exists and was not made by OpenDana; left alone", dpath);
        return OD_DESKTOP_FOREIGN;
    }
    char exe[PATH_CAP];
    const char *ex = cfg != NULL ? cfg->exec_path : NULL;
    if (ex == NULL || ex[0] == '\0') {
        ex = getenv("APPIMAGE");
        if (ex == NULL || ex[0] == '\0') {
            if (exe_path(exe) != 0) {
                say(msg, cap, "Cannot find the running program%s", "");
                return OD_DESKTOP_ERROR;
            }
            ex = exe;
        }
    }
    char qexec[PATH_CAP + 16];
    if (quote_exec(ex, qexec, sizeof qexec) != 0) {
        say(msg, cap, "Unusable program path%s", "");
        return OD_DESKTOP_ERROR;
    }
    /* Icons first, the launcher last: a launcher never points to missing icons. */
    char from[PATH_CAP], to[PATH_CAP], rel[96], r2[128];
    for (int i = 0; i < N_SIZES; i++) {
        snprintf(rel, sizeof rel, "%dx%d/apps/%s.png", k_sizes[i], k_sizes[i], ICON_NAME);
        snprintf(r2, sizeof r2, "icons/hicolor/%s", rel);
        if (join(from, src, rel) != 0 || join(to, base, r2) != 0 || copy_file(from, to) != 0) {
            say(msg, cap, "Could not install icon %s", rel);
            return OD_DESKTOP_ERROR;
        }
    }
    if (join(from, src, "scalable/apps/" ICON_NAME ".svg") != 0 ||
        join(to, base, "icons/hicolor/scalable/apps/" ICON_NAME ".svg") != 0 ||
        copy_file(from, to) != 0) {
        say(msg, cap, "Could not install the scalable icon%s", "");
        return OD_DESKTOP_ERROR;
    }
    char text[2 * PATH_CAP];
    int n = snprintf(text, sizeof text,
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=OpenDana\n"
        "Comment=Ice and fire puzzle game\n"
        "Exec=%s\n"
        "Icon=" ICON_NAME "\n"
        "Terminal=false\n"
        "Categories=Game;\n"
        "StartupWMClass=" ICON_NAME "\n"
        OD_DESKTOP_MARKER "\n", qexec);
    if (n <= 0 || n >= (int)sizeof text || write_atomic(dpath, text, (size_t)n) != 0) {
        say(msg, cap, "Could not write %s", dpath);
        return OD_DESKTOP_ERROR;
    }
    char apps[PATH_CAP];
    if (join(apps, base, "applications") == 0)
        refresh_database(apps);
    refresh_icon_cache(base);
    say(msg, cap, "Added OpenDana to the applications menu (%s)", dpath);
    return OD_DESKTOP_OK;
}

int od_desktop_uninstall(const od_desktop_cfg *cfg, char *msg, size_t cap)
{
    char base[PATH_CAP], dpath[PATH_CAP];
    if (data_home(cfg, base) != 0 || desktop_path(cfg, dpath) != 0) {
        say(msg, cap, "Cannot find your data folder (HOME is not set)%s", "");
        return OD_DESKTOP_ERROR;
    }
    int foreign = is_file(dpath) && !file_has_marker(dpath);
    if (foreign) {          /* it may use our icon name: touch nothing */
        say(msg, cap, "%s was not made by OpenDana; left alone", dpath);
        return OD_DESKTOP_FOREIGN;
    }
    int removed = 0;
    if (is_file(dpath) && remove(dpath) == 0)
        removed++;
    char p[PATH_CAP], rel[128];
    for (int i = 0; i < N_SIZES; i++) {
        snprintf(rel, sizeof rel, "icons/hicolor/%dx%d/apps/%s.png", k_sizes[i], k_sizes[i], ICON_NAME);
        if (join(p, base, rel) == 0 && is_file(p) && remove(p) == 0)
            removed++;
    }
    if (join(p, base, "icons/hicolor/scalable/apps/" ICON_NAME ".svg") == 0 &&
        is_file(p) && remove(p) == 0)
        removed++;
    char apps[PATH_CAP];
    if (removed > 0 && join(apps, base, "applications") == 0 && is_dir(apps))
        refresh_database(apps);
    if (removed > 0)
        refresh_icon_cache(base);
    say(msg, cap, removed > 0 ? "Removed OpenDana from the applications menu%s"
                              : "OpenDana was not in the applications menu%s", "");
    return OD_DESKTOP_OK;
}

#else  /* not Linux: compiled out */

int od_desktop_supported(void) { return 0; }
int od_desktop_packaged(const od_desktop_cfg *c) { (void)c; return 0; }
int od_desktop_installed(const od_desktop_cfg *c) { (void)c; return 0; }
int od_desktop_install(const od_desktop_cfg *c, char *m, size_t n)
{
    (void)c;
    if (m != NULL && n > 0) snprintf(m, n, "Not supported on this system");
    return OD_DESKTOP_UNSUPPORTED;
}
int od_desktop_uninstall(const od_desktop_cfg *c, char *m, size_t n)
{
    (void)c;
    if (m != NULL && n > 0) snprintf(m, n, "Not supported on this system");
    return OD_DESKTOP_UNSUPPORTED;
}
#endif
