/* install / uninstall of the applications-menu launcher, in a temporary
 * XDG data home. Never touches the real ~/.local/share. Usage:
 *   desktop-integration ICON_SRC_HICOLOR_DIR WORK_DIR */
#define _POSIX_C_SOURCE 200809L
#include "../src/app/desktop_integration.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define EXPECT(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char data[1024], src[1024], work[900];

static int exists(const char *rel)
{
    char p[4096];
    snprintf(p, sizeof p, "%s/%.200s", data, rel);
    return access(p, F_OK) == 0;
}

static void put(const char *rel, const char *text)
{
    char p[4096];
    snprintf(p, sizeof p, "%s/%.200s", data, rel);
    for (char *c = p + 1; *c; c++)       /* mkdir -p, no shell (PATH may be cleared) */
        if (*c == '/') { *c = '\0'; mkdir(p, 0755); *c = '/'; }
    FILE *f = fopen(p, "w");
    if (!f) abort();
    fputs(text, f);
    fclose(f);
}

static char *slurp(const char *rel)
{
    char p[4096];
    snprintf(p, sizeof p, "%s/%.200s", data, rel);
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void fresh(void)
{
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", data, data);
    if (system(cmd) != 0) abort();
}


/* ---- icon-theme.cache (the owner's Plank defect) ------------------------- */
static int mtime_cmp(const char *rel_a, const char *rel_b)   /* >0: a newer than b */
{
    char a[4096], b[4096];
    struct stat sa, sb;
    snprintf(a, sizeof a, "%s/%.200s", data, rel_a);
    snprintf(b, sizeof b, "%s/%.200s", data, rel_b);
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return -2;
    if (sa.st_mtim.tv_sec != sb.st_mtim.tv_sec)
        return sa.st_mtim.tv_sec > sb.st_mtim.tv_sec ? 1 : -1;
    return (sa.st_mtim.tv_nsec > sb.st_mtim.tv_nsec) - (sa.st_mtim.tv_nsec < sb.st_mtim.tv_nsec);
}

static int cache_mentions(const char *name)
{
    char p[4096];
    snprintf(p, sizeof p, "%s/icons/hicolor/icon-theme.cache", data);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    for (size_t i = 0; i + strlen(name) <= n; i++)
        if (memcmp(buf + i, name, strlen(name)) == 0) return 1;
    return 0;
}

/* A cache made by "another app", not older than the top hicolor dir. */
static void make_stale_cache(int real_cache)
{
    char p[4096], cmd[8192], c[4096];
    put("icons/hicolor/48x48/apps/other.png", "x");
    snprintf(p, sizeof p, "%s/icons/hicolor", data);
    if (real_cache) {
        snprintf(cmd, sizeof cmd, "gtk-update-icon-cache -q -f -t '%s' >/dev/null 2>&1", p);
        if (system(cmd) != 0) abort();
    } else {
        put("icons/hicolor/icon-theme.cache", "stale");
    }
    struct timespec old[2] = {{time(NULL) - 3600, 0}, {time(NULL) - 3600, 0}};
    snprintf(c, sizeof c, "%s/icons/hicolor/icon-theme.cache", data);
    utimensat(AT_FDCWD, c, old, 0);
    utimensat(AT_FDCWD, p, old, 0);        /* the dir is not newer than the cache */
}

static void test_icon_cache(const od_desktop_cfg *cfg)
{
    char msg[800];
    int have_tool = system("command -v gtk-update-icon-cache >/dev/null 2>&1") == 0;
    const char *old_path = getenv("PATH");
    char *saved = old_path ? strdup(old_path) : NULL;

    if (have_tool) {
        fresh();
        make_stale_cache(1);
        EXPECT(!cache_mentions("opendana"), "the stale cache lacks opendana");
        EXPECT(od_desktop_install(cfg, msg, sizeof msg) == OD_DESKTOP_OK, "install: %s", msg);
        EXPECT(cache_mentions("opendana"), "the cache was rebuilt and lists opendana");
        EXPECT(mtime_cmp("icons/hicolor/icon-theme.cache", "icons/hicolor") >= 0,
               "the cache is not older than the hicolor dir");
        EXPECT(od_desktop_uninstall(cfg, msg, sizeof msg) == OD_DESKTOP_OK, "uninstall: %s", msg);
        EXPECT(!cache_mentions("opendana"), "after uninstall the cache no longer lists opendana");
        EXPECT(mtime_cmp("icons/hicolor/icon-theme.cache", "icons/hicolor") >= 0,
               "uninstall: the cache is not older than the dir");
    }
    /* No gtk-update-icon-cache: the dir mtime must beat the cache. */
    fresh();
    make_stale_cache(0);
    setenv("PATH", "/nonexistent", 1);
    EXPECT(od_desktop_install(cfg, msg, sizeof msg) == OD_DESKTOP_OK, "install without tool: %s", msg);
    EXPECT(mtime_cmp("icons/hicolor", "icons/hicolor/icon-theme.cache") > 0,
           "without the tool the hicolor dir is newer than the cache");
    char *c = slurp("icons/hicolor/icon-theme.cache");
    EXPECT(c && strcmp(c, "stale") == 0, "the cache itself was not rewritten");
    make_stale_cache(0);
    EXPECT(od_desktop_uninstall(cfg, msg, sizeof msg) == OD_DESKTOP_OK, "uninstall without tool");
    EXPECT(mtime_cmp("icons/hicolor", "icons/hicolor/icon-theme.cache") > 0,
           "uninstall without the tool: the dir is newer than the cache");
    if (saved) { setenv("PATH", saved, 1); free(saved); }

    /* No cache: none is ever created (install nor uninstall). */
    fresh();
    EXPECT(od_desktop_install(cfg, msg, sizeof msg) == OD_DESKTOP_OK, "install, no cache");
    EXPECT(!exists("icons/hicolor/icon-theme.cache"), "install created a cache");
    EXPECT(od_desktop_uninstall(cfg, msg, sizeof msg) == OD_DESKTOP_OK, "uninstall, no cache");
    EXPECT(!exists("icons/hicolor/icon-theme.cache"), "uninstall created a cache");
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s ICON_SRC WORK\n", argv[0]); return 2; }
    snprintf(src, sizeof src, "%s", argv[1]);
    snprintf(work, sizeof work, "%s", argv[2]);
    snprintf(data, sizeof data, "%s/xd", work);
    char msg[800];
    od_desktop_cfg cfg = { data, "/opt/open hero/Open$Player \"x\".AppImage", src };
    static const int sizes[] = {16, 24, 32, 48, 64, 128, 256, 512};

    EXPECT(od_desktop_supported() == 1, "supported on Linux");

    /* Nothing is written until asked. */
    fresh();
    EXPECT(!od_desktop_installed(&cfg), "not installed at first");
    EXPECT(!exists("applications") && !exists("icons"), "nothing created before install");

    /* Install: launcher + 8 PNG sizes + SVG, marker, StartupWMClass, quoted Exec. */
    EXPECT(od_desktop_install(&cfg, msg, sizeof msg) == OD_DESKTOP_OK, "install: %s", msg);
    EXPECT(od_desktop_installed(&cfg), "installed after install");
    for (int i = 0; i < 8; i++) {
        char rel[96];
        snprintf(rel, sizeof rel, "icons/hicolor/%dx%d/apps/opendana.png", sizes[i], sizes[i]);
        EXPECT(exists(rel), "missing %s", rel);
    }
    EXPECT(exists("icons/hicolor/scalable/apps/opendana.svg"), "missing the SVG");
    char *d = slurp("applications/opendana.desktop");
    EXPECT(d != NULL, "launcher not written");
    if (d) {
        EXPECT(strstr(d, "\nIcon=opendana\n"), "Icon=opendana");
        EXPECT(strstr(d, "\nStartupWMClass=opendana\n"), "StartupWMClass");
        EXPECT(strstr(d, "\nCategories=Game;\n"), "Categories");
        EXPECT(strstr(d, "\n" OD_DESKTOP_MARKER "\n"), "marker");
        EXPECT(strstr(d, "\nExec=\"/opt/open hero/Open\\\\$Player \\\\\"x\\\\\".AppImage\"\n"),
               "Exec quoting: %s", d);
    }
    /* Reinstall is idempotent. */
    EXPECT(od_desktop_install(&cfg, msg, sizeof msg) == OD_DESKTOP_OK, "reinstall: %s", msg);

    /* APPIMAGE wins over the binary path when no Exec is given. */
    od_desktop_cfg auto_cfg = { data, NULL, src };
    setenv("APPIMAGE", "/home/u/OpenDana.AppImage", 1);
    EXPECT(od_desktop_packaged(&auto_cfg) == 1, "packaged under $APPIMAGE");
    EXPECT(od_desktop_install(&auto_cfg, msg, sizeof msg) == OD_DESKTOP_OK, "appimage install");
    d = slurp("applications/opendana.desktop");
    EXPECT(d && strstr(d, "\nExec=\"/home/u/OpenDana.AppImage\"\n"), "Exec is $APPIMAGE");
    unsetenv("APPIMAGE");
    od_desktop_cfg dev_cfg = { data, NULL, "/nonexistent" };
    EXPECT(od_desktop_packaged(&dev_cfg) == 0, "a development build is not packaged");

    /* Uninstall removes only ours. */
    put("applications/other.desktop", "[Desktop Entry]\nName=Other\n");
    put("icons/hicolor/48x48/apps/other.png", "x");
    put("icons/hicolor/48x48/apps/opendana-extra.png", "x");
    EXPECT(od_desktop_uninstall(&cfg, msg, sizeof msg) == OD_DESKTOP_OK, "uninstall: %s", msg);
    EXPECT(!od_desktop_installed(&cfg), "not installed after uninstall");
    EXPECT(!exists("applications/opendana.desktop"), "launcher removed");
    EXPECT(!exists("icons/hicolor/48x48/apps/opendana.png"), "png removed");
    EXPECT(!exists("icons/hicolor/scalable/apps/opendana.svg"), "svg removed");
    EXPECT(exists("applications/other.desktop"), "foreign launcher kept");
    EXPECT(exists("icons/hicolor/48x48/apps/other.png"), "foreign icon kept");
    EXPECT(exists("icons/hicolor/48x48/apps/opendana-extra.png"), "similar icon kept");

    /* A launcher without our marker is never overwritten or removed. */
    fresh();
    put("applications/opendana.desktop", "[Desktop Entry]\nName=Mine\nExec=/bin/true\n");
    put("icons/hicolor/48x48/apps/opendana.png", "hand-made");
    EXPECT(od_desktop_install(&cfg, msg, sizeof msg) == OD_DESKTOP_FOREIGN, "foreign install: %s", msg);
    EXPECT(!od_desktop_installed(&cfg), "a foreign launcher is not 'installed'");
    d = slurp("applications/opendana.desktop");
    EXPECT(d && strstr(d, "Name=Mine"), "foreign launcher untouched by install");
    EXPECT(!exists("icons/hicolor/16x16/apps/opendana.png"), "install wrote nothing else");
    EXPECT(od_desktop_uninstall(&cfg, msg, sizeof msg) == OD_DESKTOP_FOREIGN, "foreign uninstall");
    EXPECT(strstr(msg, "left alone") != NULL, "says it left it alone: %s", msg);
    EXPECT(exists("applications/opendana.desktop") &&
           exists("icons/hicolor/48x48/apps/opendana.png"), "foreign launcher and icon kept");
    d = slurp("icons/hicolor/48x48/apps/opendana.png");
    EXPECT(d && strcmp(d, "hand-made") == 0, "foreign icon content kept");

    /* Missing package icons: clean failure, nothing written. */
    fresh();
    od_desktop_cfg bad = { data, "/x/opendana", "/nonexistent" };
    EXPECT(od_desktop_install(&bad, msg, sizeof msg) == OD_DESKTOP_NO_ICONS, "no icons: %s", msg);
    EXPECT(!exists("applications") && !exists("icons"), "nothing written without icons");
    char partial[2048];
    snprintf(partial, sizeof partial, "%s/partial", work);
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s/48x48/apps' && cp '%s/48x48/apps/opendana.png' '%s/48x48/apps/'",
             partial, partial, src, partial);
    if (system(cmd) != 0) abort();
    od_desktop_cfg part = { data, "/x/opendana", partial };
    EXPECT(od_desktop_install(&part, msg, sizeof msg) == OD_DESKTOP_NO_ICONS, "partial icons refused");
    EXPECT(!exists("applications") && !exists("icons"), "nothing written with partial icons");

    test_icon_cache(&cfg);

    printf(failures ? "desktop-integration: %d FAILURE(S)\n" : "desktop-integration: all checks passed\n", failures);
    return failures ? 1 : 0;
}
