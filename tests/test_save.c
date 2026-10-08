/* Save file: serialise/deserialise round trip, corruption rejection, the
 * missing-file default and the default-path selection. Pure C11, no SDL. */
#include "test.h"

#include "opendana/save.h"
#include "opendana/worldmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Byte offsets mirror src/core/save.c; the format is documented in
 * include/opendana/save.h. */
#define OFF_VERSION 4
#define OFF_WORLD   5
#define OFF_CURSOR  6
#define OFF_CLEARED 8
#define OFF_CKSUM   (OD_SAVE_SIZE - 4)
#define CKSUM_LEN   (OFF_CKSUM - OFF_VERSION)

static void test_save_roundtrip(void)
{
    printf("- save: serialize/deserialize round trip\n");
    od_progress p;
    od_progress_reset(&p);
    od_progress_set_cleared(&p, 0, 1);
    od_progress_set_cleared(&p, 7, 1);
    od_progress_set_cleared(&p, 63, 1);
    od_progress_set_cleared(&p, 99, 1);
    od_progress_set_cleared(&p, 149, 1);
    p.world = 4;
    p.cursor = 7;

    CHECK_EQ_U32(OD_SAVE_SIZE, 31);
    CHECK_EQ_U32(OD_SAVE_CLEARED_BYTES, 19);

    uint8_t buf[OD_SAVE_SIZE];
    int n = od_save_serialize(buf, sizeof(buf), &p);
    CHECK_EQ_U32(n, OD_SAVE_SIZE);
    CHECK_EQ_U32(buf[0], OD_SAVE_MAGIC_0);
    CHECK_EQ_U32(buf[1], OD_SAVE_MAGIC_1);
    CHECK_EQ_U32(buf[2], OD_SAVE_MAGIC_2);
    CHECK_EQ_U32(buf[3], OD_SAVE_MAGIC_3);
    CHECK_EQ_U32(buf[OFF_VERSION], OD_SAVE_VERSION);
    CHECK_EQ_U32(buf[OFF_WORLD], 4);
    CHECK_EQ_U32(buf[OFF_CURSOR], 7);

    od_progress q;
    CHECK_EQ_U32(od_save_deserialize(buf, (size_t)n, &q), OD_OK);
    CHECK_EQ_U32(q.world, 4);
    CHECK_EQ_U32(q.cursor, 7);
    CHECK_EQ_MEM(q.cleared, p.cleared, OD_SAVE_CLEARED_BYTES);
    CHECK_EQ_U32(od_progress_clear_count(&q), 5);
    for (int i = 0; i < OD_LEVEL_COUNT; i++)
        CHECK_EQ_U32(od_progress_cleared(&q, i), od_progress_cleared(&p, i));

    /* Serialising into a buffer that is too small is refused. */
    uint8_t tiny[OD_SAVE_SIZE - 1];
    CHECK(od_save_serialize(tiny, sizeof(tiny), &p) < 0);
}

static void test_save_reject(void)
{
    printf("- save: checksum/version/magic rejection\n");
    od_progress p;
    od_progress_reset(&p);
    od_progress_set_cleared(&p, 3, 1);
    p.world = 1;
    p.cursor = 2;

    uint8_t buf[OD_SAVE_SIZE];
    int n = od_save_serialize(buf, sizeof(buf), &p);

    od_progress q;
    /* Too short. */
    CHECK_EQ_U32(od_save_deserialize(buf, (size_t)n - 1, &q), OD_ERR_RANGE);

    uint8_t b[OD_SAVE_SIZE];

    /* Bad magic. */
    memcpy(b, buf, sizeof(b));
    b[0] = 'X';
    CHECK_EQ_U32(od_save_deserialize(b, (size_t)n, &q), OD_ERR_FORMAT);

    /* Wrong version. */
    memcpy(b, buf, sizeof(b));
    b[OFF_VERSION] = 99;
    CHECK_EQ_U32(od_save_deserialize(b, (size_t)n, &q), OD_ERR_FORMAT);

    /* A flipped progress bit must be caught by the checksum. */
    memcpy(b, buf, sizeof(b));
    b[OFF_CLEARED + 0] ^= 0x01;
    CHECK_EQ_U32(od_save_deserialize(b, (size_t)n, &q), OD_ERR_FORMAT);

    /* A corrupted checksum. */
    memcpy(b, buf, sizeof(b));
    b[OFF_CKSUM] ^= 0xFF;
    CHECK_EQ_U32(od_save_deserialize(b, (size_t)n, &q), OD_ERR_FORMAT);

    /* A valid checksum over an out-of-range world is still rejected. */
    memcpy(b, buf, sizeof(b));
    b[OFF_WORLD] = 99;
    uint32_t sum = od_fnv1a32(b + OFF_VERSION, CKSUM_LEN, OD_FNV1A32_INIT);
    b[OFF_CKSUM + 0] = (uint8_t)(sum >> 0);
    b[OFF_CKSUM + 1] = (uint8_t)(sum >> 8);
    b[OFF_CKSUM + 2] = (uint8_t)(sum >> 16);
    b[OFF_CKSUM + 3] = (uint8_t)(sum >> 24);
    CHECK_EQ_U32(od_save_deserialize(b, (size_t)n, &q), OD_ERR_RANGE);
}

static void test_save_file(void)
{
    printf("- save: file round trip and missing-file default\n");
    const char *path = "od_test_save.tmp";
    remove(path);

    /* Missing file: the caller starts from a reset progress. */
    od_progress r;
    od_progress_reset(&r);
    CHECK_EQ_U32(od_save_read(path, &r), OD_ERR_IO);
    CHECK_EQ_U32(od_progress_clear_count(&r), 0);
    CHECK_EQ_U32(od_progress_unlocked(&r, 0), 1);
    CHECK_EQ_U32(od_progress_unlocked(&r, 1), 0);
    /* Deleting a missing file is idempotent. */
    CHECK_EQ_U32(od_save_delete(path), OD_OK);

    /* Write, then read back: clearing round 1 leaves the cursor on round 2. */
    od_progress p;
    od_progress_reset(&p);
    od_progress_complete(&p, 0);
    CHECK_EQ_U32(od_save_write(path, &p), OD_OK);

    od_progress q;
    CHECK_EQ_U32(od_save_read(path, &q), OD_OK);
    CHECK_EQ_U32(od_progress_cleared(&q, 0), 1);
    CHECK_EQ_U32(od_progress_clear_count(&q), 1);
    CHECK_EQ_U32(q.world, 0);
    CHECK_EQ_U32(q.cursor, 1);
    CHECK_EQ_U32(od_save_delete(path), OD_OK);

    /* A file with a trailing byte is malformed, not accepted silently. */
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        fputc('x', f);
        fclose(f);
    }
    CHECK_EQ_U32(od_save_read(path, &q), OD_ERR_FORMAT);
    remove(path);
}

#ifndef _WIN32   /* the Windows branch is tests/test_windows_userdir.c */
static void test_save_default_path(void)
{
    printf("- save: default path respects the environment\n");
    char path[1024];

    char xdg[512] = {0}, home[512] = {0};
    char *old_xdg = getenv("XDG_DATA_HOME");
    char *old_home = getenv("HOME");
    int had_xdg = old_xdg && old_xdg[0];
    int had_home = old_home && old_home[0];
    if (had_xdg)
        snprintf(xdg, sizeof(xdg), "%s", old_xdg);
    if (had_home)
        snprintf(home, sizeof(home), "%s", old_home);

#if defined(__APPLE__)
    /* C6.70: on macOS the per-user data folder is ~/Library/Application Support (userdir.c)
     * and XDG_DATA_HOME is ignored: the same two environments, the platform's answers. */
    setenv("HOME", "/tmp/opendana-home", 1);
    setenv("XDG_DATA_HOME", "/tmp/opendana-xdg", 1);
    od_save_default_path(path, sizeof(path));
    CHECK_EQ_U32(strcmp(path, "/tmp/opendana-home/Library/Application Support/opendana/save.dat"), 0);

    unsetenv("XDG_DATA_HOME");
    od_save_default_path(path, sizeof(path));
    CHECK_EQ_U32(strcmp(path, "/tmp/opendana-home/Library/Application Support/opendana/save.dat"), 0);
#else
    setenv("XDG_DATA_HOME", "/tmp/opendana-xdg", 1);
    od_save_default_path(path, sizeof(path));
    CHECK_EQ_U32(strcmp(path, "/tmp/opendana-xdg/opendana/save.dat"), 0);

    unsetenv("XDG_DATA_HOME");
    setenv("HOME", "/tmp/opendana-home", 1);
    od_save_default_path(path, sizeof(path));
    CHECK_EQ_U32(strcmp(path, "/tmp/opendana-home/.local/share/opendana/save.dat"), 0);
#endif

    unsetenv("HOME");
    od_save_default_path(path, sizeof(path));
    CHECK_EQ_U32(strcmp(path, "opendana.sav"), 0);

    /* Restore the environment for any later test. */
    if (had_xdg)
        setenv("XDG_DATA_HOME", xdg, 1);
    else
        unsetenv("XDG_DATA_HOME");
    if (had_home)
        setenv("HOME", home, 1);
    else
        unsetenv("HOME");
}
#endif

OD_TEST(test_save_roundtrip)
OD_TEST(test_save_reject)
OD_TEST(test_save_file)
#ifndef _WIN32
OD_TEST(test_save_default_path)
#endif
