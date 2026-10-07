/* save.c - see opendana/save.h for the on-disk format and the design constraints.
 *
 * Everything here is pure C11 (stdio/stdlib/string only), so the save logic is
 * unit-tested without SDL. The format is deliberately explicit: no struct dump,
 * an FNV-1a checksum and a version byte, so a corrupted or foreign file is
 * rejected instead of silently loading garbage. */
#include "opendana/save.h"
#include "opendana/userdir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Byte offsets, kept in one place so the writer and the reader agree. */
#define OFF_MAGIC   0
#define OFF_VERSION 4
#define OFF_WORLD   5
#define OFF_CURSOR  6
#define OFF_RESERVED 7
#define OFF_CLEARED 8
#define OFF_CKSUM   (OFF_CLEARED + OD_SAVE_CLEARED_BYTES) /* 27 */
#define CKSUM_LEN   (OFF_CKSUM - OFF_VERSION)              /* 23 */

int od_save_serialize(uint8_t *out, size_t cap, const od_progress *p)
{
    if (!out || !p || cap < OD_SAVE_SIZE)
        return -1;

    memset(out, 0, OD_SAVE_SIZE);
    out[OFF_MAGIC + 0] = OD_SAVE_MAGIC_0;
    out[OFF_MAGIC + 1] = OD_SAVE_MAGIC_1;
    out[OFF_MAGIC + 2] = OD_SAVE_MAGIC_2;
    out[OFF_MAGIC + 3] = OD_SAVE_MAGIC_3;
    out[OFF_VERSION] = (uint8_t)OD_SAVE_VERSION;

    int world = p->world;
    int cursor = p->cursor;
    if (world < 0) world = 0;
    if (world >= OD_WORLD_COUNT) world = OD_WORLD_COUNT - 1;
    if (cursor < 0) cursor = 0;
    if (cursor >= OD_MAP_SLOTS) cursor = OD_MAP_SLOTS - 1;
    out[OFF_WORLD] = (uint8_t)world;
    out[OFF_CURSOR] = (uint8_t)cursor;
    out[OFF_RESERVED] = 0;

    memcpy(out + OFF_CLEARED, p->cleared, OD_SAVE_CLEARED_BYTES);

    uint32_t sum = od_fnv1a32(out + OFF_VERSION, CKSUM_LEN, OD_FNV1A32_INIT);
    out[OFF_CKSUM + 0] = (uint8_t)(sum >> 0);
    out[OFF_CKSUM + 1] = (uint8_t)(sum >> 8);
    out[OFF_CKSUM + 2] = (uint8_t)(sum >> 16);
    out[OFF_CKSUM + 3] = (uint8_t)(sum >> 24);
    return OD_SAVE_SIZE;
}

od_status od_save_deserialize(const uint8_t *buf, size_t len, od_progress *p)
{
    if (!p)
        return OD_ERR_RANGE;
    if (!buf)
        return OD_ERR_RANGE;
    if (len < OD_SAVE_SIZE)
        return OD_ERR_RANGE;

    if (memcmp(buf + OFF_MAGIC, "ODSV", 4) != 0)
        return OD_ERR_FORMAT;
    if (buf[OFF_VERSION] != OD_SAVE_VERSION)
        return OD_ERR_FORMAT;

    uint32_t want = od_rd32(buf + OFF_CKSUM);
    uint32_t got = od_fnv1a32(buf + OFF_VERSION, CKSUM_LEN, OD_FNV1A32_INIT);
    if (want != got)
        return OD_ERR_FORMAT;

    if (buf[OFF_WORLD] >= OD_WORLD_COUNT || buf[OFF_CURSOR] >= OD_MAP_SLOTS)
        return OD_ERR_RANGE;

    od_progress tmp;
    od_progress_reset(&tmp);
    memcpy(tmp.cleared, buf + OFF_CLEARED, OD_SAVE_CLEARED_BYTES);
    /* Mask the padding bits of the last byte so they never count as levels. */
    for (int i = OD_LEVEL_COUNT; i < OD_SAVE_CLEARED_BYTES * 8; i++)
        tmp.cleared[i >> 3] &= (uint8_t)~(1u << (i & 7));
    tmp.world = buf[OFF_WORLD];
    tmp.cursor = buf[OFF_CURSOR];

    *p = tmp;
    return OD_OK;
}

/* C6.31 (CORE-7): an existing file that is not a valid save is never lost to
 * the next write. It is copied aside first, to the first free name of
 * `<path>.invalid`, `<path>.invalid-2`, ... (the last one is reused). */
static void save_keep_invalid(const char *path)
{
    od_progress probe;
    od_status st = od_save_read(path, &probe);
    if (st == OD_OK || st == OD_ERR_IO)
        return;                 /* valid, or nothing there to keep */
    char keep[1200];
    for (int i = 1; i <= 99; i++) {
        int n = i == 1 ? snprintf(keep, sizeof keep, "%s.invalid", path)
                       : snprintf(keep, sizeof keep, "%s.invalid-%d", path, i);
        if (n < 0 || (size_t)n >= sizeof keep)
            return;
        FILE *f = od_userdir_fopen(keep, "rb");
        if (f == NULL)
            break;
        fclose(f);
    }
    if (od_userdir_copy_file(path, keep) == OD_OK)
        fprintf(stderr, "save: kept the invalid %s as %s\n", path, keep);
    else
        fprintf(stderr, "save: cannot keep the invalid %s\n", path);
}

od_status od_save_write(const char *path, const od_progress *p)
{
    if (!path || !p)
        return OD_ERR_RANGE;

    uint8_t buf[OD_SAVE_SIZE];
    int n = od_save_serialize(buf, sizeof(buf), p);
    if (n <= 0)
        return OD_ERR_RANGE;

    save_keep_invalid(path);
    /* C6.31 (CORE-7): temporary file + replace, so a crash or a
     * full disk leaves the previous save intact instead of a truncated one. */
    return od_userdir_write_atomic_bg(path, buf, (size_t)n);
}

od_status od_save_backup(const char *path, char *backup, size_t cap)
{
    if (!path || !backup || cap == 0)
        return OD_ERR_RANGE;
    od_userdir_flush();
    int n = snprintf(backup, cap, "%s.bak", path);
    if (n < 0 || (size_t)n >= cap) {
        backup[0] = '\0';
        return OD_ERR_RANGE;
    }
    return od_userdir_copy_file(path, backup);
}

od_status od_save_read(const char *path, od_progress *p)
{
    if (!path || !p)
        return OD_ERR_RANGE;

    od_userdir_flush();   /* C6.41: a queued write is the newest content */
    FILE *f = od_userdir_fopen(path, "rb");
    if (!f)
        return OD_ERR_IO;

    uint8_t buf[OD_SAVE_SIZE];
    size_t n = fread(buf, 1, sizeof(buf), f);
    int extra = fgetc(f); /* any trailing byte makes the file malformed */
    fclose(f);

    if (n != sizeof(buf))
        return OD_ERR_FORMAT;
    if (extra != EOF)
        return OD_ERR_FORMAT;
    return od_save_deserialize(buf, n, p);
}

int od_save_is_older_format(const char *path)
{
    if (!path)
        return 0;
    od_userdir_flush();
    FILE *f = od_userdir_fopen(path, "rb");
    if (!f)
        return 0;
    uint8_t buf[OD_SAVE_SIZE];
    size_t n = fread(buf, 1, sizeof(buf), f);
    int extra = fgetc(f);
    fclose(f);
    if (n != sizeof(buf) || extra != EOF || buf[OFF_VERSION] != OD_SAVE_VERSION)
        return 0;
    if (od_rd32(buf + OFF_CKSUM) != od_fnv1a32(buf + OFF_VERSION, CKSUM_LEN, OD_FNV1A32_INIT))
        return 0;
    return memcmp(buf + OFF_MAGIC, "ODSV", 4) != 0;
}

od_status od_save_delete(const char *path)
{
    if (!path)
        return OD_ERR_RANGE;
    od_userdir_flush();
    /* A missing file is not an error: there is simply nothing to delete. */
    return od_userdir_remove(path) == 0 ? OD_OK : OD_ERR_IO;
}

void od_save_default_path(char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    /* C6.31 (PUB-12): the shared per-user directory (LOCALAPPDATA on Windows,
     * Application Support on macOS, XDG elsewhere). The current directory is
     * only the last resort when no per-user base exists at all. */
    if (od_userdir_path(OD_USERDIR_DATA, "save.dat", out, cap) == OD_OK)
        return;
    snprintf(out, cap, "opendana.sav");
}
