/* the session files. Pure C11, own temporary files.
 *
 *   - CORE-7: save.dat is replaced atomically (a hard link to the old file
 *     keeps the old bytes: the name now points at a new file, the old one was
 *     never truncated in place) and an invalid file is copied aside to
 *     `<save>.invalid` before the write, never lost.
 *   - New game: od_save_backup copies the save to `<save>.bak`.
 *   - the per-user directories (XDG branch; the Windows branch
 *     is checked by tests/session-run.sh with mingw + wine). */
#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "opendana/save.h"
#ifndef SESSION_BASE_CONTROL
#include "opendana/userdir.h"
#endif
#include "opendana/worldmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
/* A hard link, as on POSIX (the old inode keeps the old bytes). */
#define link(from, to) (CreateHardLinkA((to), (from), NULL) ? 0 : -1)
#endif

static void session_tmp(char *out, size_t cap, const char *name)
{
    const char *dir = getenv("TMPDIR");
#ifdef _WIN32
    if (dir == NULL || dir[0] == '\0')
        dir = getenv("TEMP");
#endif
    if (dir == NULL || dir[0] == '\0')
        dir = "/tmp";
    snprintf(out, cap, "%s/opendana-c631-%ld-%s", dir, (long)getpid(), name);
}

static size_t slurp(const char *path, unsigned char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static void test_session_save_atomic_replace(void)
{
    char path[512], link_path[600];
    session_tmp(path, sizeof path, "atomic.dat");
    snprintf(link_path, sizeof link_path, "%s.link", path);
    remove(path);
    remove(link_path);

    od_progress a, b, back;
    od_progress_reset(&a);
    od_progress_complete(&a, 0);
    od_progress_reset(&b);
    od_progress_complete(&b, 0);
    od_progress_complete(&b, 1);
    CHECK_EQ_U32(od_save_write(path, &a), OD_OK);
    CHECK_EQ_U32(link(path, link_path), 0);
    CHECK_EQ_U32(od_save_write(path, &b), OD_OK);

    /* The name has the new save; the old inode (the link) still has the old
     * one: the write replaced the file instead of truncating it in place. */
    CHECK_EQ_U32(od_save_read(path, &back), OD_OK);
    CHECK_EQ_U32(od_progress_clear_count(&back), 2);
    CHECK_EQ_U32(od_save_read(link_path, &back), OD_OK);
    CHECK_EQ_U32(od_progress_clear_count(&back), 1);

    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *t = fopen(tmp, "rb");
    CHECK(t == NULL);              /* no temporary left behind */
    if (t) fclose(t);
    remove(path);
    remove(link_path);
}

static void test_session_save_keeps_invalid(void)
{
    char path[512], keep[600], keep2[600];
    session_tmp(path, sizeof path, "invalid.dat");
    snprintf(keep, sizeof keep, "%s.invalid", path);
    snprintf(keep2, sizeof keep2, "%s.invalid-2", path);
    remove(path);
    remove(keep);
    remove(keep2);

    static const unsigned char junk[] = "not a save, but the player's bytes";
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL)
        return;
    fwrite(junk, 1, sizeof junk, f);
    fclose(f);

    od_progress p, back;
    od_progress_reset(&p);
    od_progress_complete(&p, 0);
    CHECK_EQ_U32(od_save_write(path, &p), OD_OK);
    CHECK_EQ_U32(od_save_read(path, &back), OD_OK);

    unsigned char got[256];
    size_t n = slurp(keep, got, sizeof got);
    CHECK_EQ_U32(n, sizeof junk);
    CHECK(n == sizeof junk && memcmp(got, junk, sizeof junk) == 0);

    /* A second invalid file does not overwrite the first copy. */
    f = fopen(path, "wb");
    if (f) {
        fwrite("x", 1, 1, f);
        fclose(f);
    }
    CHECK_EQ_U32(od_save_write(path, &p), OD_OK);
    CHECK_EQ_U32(slurp(keep, got, sizeof got), sizeof junk);
    CHECK_EQ_U32(slurp(keep2, got, sizeof got), 1);

    /* A valid file is replaced without any copy. */
    remove(keep);
    CHECK_EQ_U32(od_save_write(path, &p), OD_OK);
    f = fopen(keep, "rb");
    CHECK(f == NULL);
    if (f) fclose(f);
    remove(path);
    remove(keep);
    remove(keep2);
}

/* SESSION_BASE_CONTROL compiles only the two CORE-7 cases, so
 * tests/session-run.sh can run them against an unfixed build (positive
 * control): there the atomic and invalid-copy checks must fail. */
#ifndef SESSION_BASE_CONTROL
static void test_session_save_backup(void)
{
    char path[512], bak[600];
    session_tmp(path, sizeof path, "backup.dat");
    remove(path);
    CHECK_EQ_U32(od_save_backup(path, bak, sizeof bak), OD_ERR_IO); /* none */

    od_progress p, back;
    od_progress_reset(&p);
    for (int i = 0; i < 12; i++)
        od_progress_complete(&p, i);
    CHECK_EQ_U32(od_save_write(path, &p), OD_OK);
    CHECK_EQ_U32(od_save_backup(path, bak, sizeof bak), OD_OK);
    char want[600];
    snprintf(want, sizeof want, "%s.bak", path);
    CHECK(strcmp(bak, want) == 0);
    CHECK_EQ_U32(od_save_read(bak, &back), OD_OK);
    CHECK_EQ_U32(od_progress_clear_count(&back), 12);
    remove(path);
    remove(bak);
}

#ifndef _WIN32   /* the Windows branch is tests/test_windows_userdir.c */
static void test_session_userdir_xdg(void)
{
    char *old_data = getenv("XDG_DATA_HOME"), *old_cfg = getenv("XDG_CONFIG_HOME");
    char *old_home = getenv("HOME");
    char data[512] = "", cfg[512] = "", home[512] = "";
    if (old_data) snprintf(data, sizeof data, "%s", old_data);
    if (old_cfg) snprintf(cfg, sizeof cfg, "%s", old_cfg);
    if (old_home) snprintf(home, sizeof home, "%s", old_home);

    char out[1024];
#if defined(__APPLE__)
    /* C6.70: macOS keeps data and settings under ~/Library/Application Support and ignores
     * XDG_*: with XDG set and unset the answers are the same. */
    setenv("HOME", "/tmp/c631-home", 1);
    setenv("XDG_DATA_HOME", "/tmp/c631-data", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/c631-cfg", 1);
    CHECK_EQ_U32(od_userdir(OD_USERDIR_DATA, out, sizeof out), OD_OK);
    CHECK(strcmp(out, "/tmp/c631-home/Library/Application Support/" OD_USERDIR_APP) == 0);
    CHECK_EQ_U32(od_userdir_path(OD_USERDIR_CONFIG, "settings.ini", out, sizeof out), OD_OK);
    CHECK(strcmp(out, "/tmp/c631-home/Library/Application Support/" OD_USERDIR_APP "/settings.ini") == 0);

    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CONFIG_HOME");
    CHECK_EQ_U32(od_userdir_path(OD_USERDIR_DATA, "states/slot-1.odstate", out, sizeof out), OD_OK);
    CHECK(strcmp(out, "/tmp/c631-home/Library/Application Support/" OD_USERDIR_APP "/states/slot-1.odstate") == 0);
    CHECK_EQ_U32(od_userdir(OD_USERDIR_CONFIG, out, sizeof out), OD_OK);
    CHECK(strcmp(out, "/tmp/c631-home/Library/Application Support/" OD_USERDIR_APP) == 0);
#else
    setenv("XDG_DATA_HOME", "/tmp/c631-data", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/c631-cfg", 1);
    CHECK_EQ_U32(od_userdir(OD_USERDIR_DATA, out, sizeof out), OD_OK);
    CHECK(strcmp(out, "/tmp/c631-data/" OD_USERDIR_APP) == 0);
    CHECK_EQ_U32(od_userdir_path(OD_USERDIR_CONFIG, "settings.ini", out, sizeof out), OD_OK);
    CHECK(strcmp(out, "/tmp/c631-cfg/" OD_USERDIR_APP "/settings.ini") == 0);

    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/tmp/c631-home", 1);
    CHECK_EQ_U32(od_userdir_path(OD_USERDIR_DATA, "states/slot-1.odstate", out, sizeof out), OD_OK);
    CHECK(strcmp(out, "/tmp/c631-home/.local/share/" OD_USERDIR_APP "/states/slot-1.odstate") == 0);
    CHECK_EQ_U32(od_userdir(OD_USERDIR_CONFIG, out, sizeof out), OD_OK);
    CHECK(strcmp(out, "/tmp/c631-home/.config/" OD_USERDIR_APP) == 0);
#endif

    /* No per-user base at all: an error, never the current directory. */
    unsetenv("HOME");
    CHECK_EQ_U32(od_userdir(OD_USERDIR_DATA, out, sizeof out), OD_ERR_IO);
    CHECK(out[0] == '\0');
    char tiny[8];
    setenv("HOME", "/tmp/c631-home", 1);
    CHECK_EQ_U32(od_userdir(OD_USERDIR_DATA, tiny, sizeof tiny), OD_ERR_RANGE);

    if (data[0]) setenv("XDG_DATA_HOME", data, 1); else unsetenv("XDG_DATA_HOME");
    if (cfg[0]) setenv("XDG_CONFIG_HOME", cfg, 1); else unsetenv("XDG_CONFIG_HOME");
    if (home[0]) setenv("HOME", home, 1); else unsetenv("HOME");
}
#endif

#endif /* SESSION_BASE_CONTROL */

OD_TEST(test_session_save_atomic_replace)
OD_TEST(test_session_save_keeps_invalid)
#ifndef SESSION_BASE_CONTROL
OD_TEST(test_session_save_backup)
#ifndef _WIN32
OD_TEST(test_session_userdir_xdg)
#endif
#endif
