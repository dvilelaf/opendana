/* save.h - persistent player progress (the 150-level cleared bitmap) on disk.
 *
 * Pure C11, no SDL: only stdio/stdlib. The whole point is that the save file is
 * self-describing (magic + version + checksum) and can be tested without a
 * window or a ROM.
 *
 * On-disk layout (little-endian, exactly 31 bytes):
 *
 *   off  size  field
 *   ---  ----  ---------------------------------------------------------------
 *    0    4    magic "ODSV"
 *    4    1    version = OD_SAVE_VERSION
 *    5    1    world   0..OD_WORLD_COUNT-1 (current world shown on the map)
 *    6    1    cursor  0..OD_MAP_SLOTS-1   (selected map marker)
 *    7    1    reserved, 0
 *    8   19    cleared bitmap: level i -> bit (i & 7) of byte (i >> 3)
 *   27    4    FNV-1a 32 checksum of bytes 4..26 (version..cleared)
 *
 * There is no raw struct dump: every field is written explicitly, so struct
 * padding and endianness of the host never leak into the file.
 */
#ifndef OD_SAVE_H
#define OD_SAVE_H

#include "opendana/common.h"
#include "opendana/worldmap.h"

#define OD_SAVE_VERSION       1u
#define OD_SAVE_CLEARED_BYTES ((OD_LEVEL_COUNT + 7) / 8)          /* 19 */
#define OD_SAVE_MAGIC_0 'O'
#define OD_SAVE_MAGIC_1 'D'
#define OD_SAVE_MAGIC_2 'S'
#define OD_SAVE_MAGIC_3 'V'
#define OD_SAVE_SIZE (4 + 1 + 1 + 1 + 1 + OD_SAVE_CLEARED_BYTES + 4) /* 31 */

/* Serialise `p` into `out` (must hold >= OD_SAVE_SIZE bytes). Returns the
 * number of bytes written (OD_SAVE_SIZE) or -1 if `out` is too small. */
int od_save_serialize(uint8_t *out, size_t cap, const od_progress *p);

/* Parse and validate a buffer. Returns OD_OK on success, OD_ERR_RANGE when
 * `len` is too short, OD_ERR_FORMAT on bad magic/version/checksum, and
 * OD_ERR_RANGE when the stored world/cursor are out of range. `p` is only
 * modified on success. */
od_status od_save_deserialize(const uint8_t *buf, size_t len, od_progress *p);

/* Read/write the save file. `read` returns OD_ERR_IO when the file is missing
 * or unreadable (callers then start from a reset progress). `write` returns
 * OD_ERR_IO when the parent directory does not exist or is not writable.
 * The write is atomic (temporary file + replace, od_userdir_write_atomic) and
 * an existing file that is not a valid save is first copied aside to
 * `<path>.invalid` (C6.31, CORE-7). */
od_status od_save_write(const char *path, const od_progress *p);
od_status od_save_read(const char *path, od_progress *p);

/* Non-zero when the file has the size, version and checksum of a save but another
 * header word: written by an older version, which this one does not read. */
int od_save_is_older_format(const char *path);

/* Copy the save at `path` to `<path>.bak` (the name is written to `backup`),
 * replacing an older backup. OD_ERR_IO when there is no file to copy. Used by
 * the modern menu's "New game" before it clears the progress (C6.31). */
od_status od_save_backup(const char *path, char *backup, size_t cap);

/* Best-effort delete. Missing files count as success. */
od_status od_save_delete(const char *path);

/* Default save path, in the caller's buffer: `save.dat` in the per-user data
 * directory (opendana/userdir.h: $XDG_DATA_HOME/opendana, else
 * ~/.local/share/opendana; %LOCALAPPDATA%\opendana on Windows;
 * ~/Library/Application Support/opendana on macOS), else ./opendana.sav when
 * no per-user base exists at all. The app creates the parent directory. */
void od_save_default_path(char *out, size_t cap);

#endif /* OD_SAVE_H */
