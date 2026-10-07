/* import.h - import the user's EU ROM into the moddable content package.
 *
 * A single command,
 *
 *     opendana --import-rom "/path/to/your-rom.nes"
 *
 * reads the original ROM once, verifies its SHA-256 against the pinned EU
 * image, extracts the whole game content into the package format and writes it
 * atomically under the user's XDG data directory. After that the ROM is never
 * needed again: the game starts from the imported content.
 *
 * The import runs entirely in C inside the binary (decision D4). It contains
 * ROM addresses and read rules, never bytes of the original.
 */
#ifndef OD_IMPORT_H
#define OD_IMPORT_H

#include "opendana/common.h"

#include <stddef.h>

#include "opendana/userdir.h"

/* The only accepted ROM. */
#define OD_ROM_SHA256 "853646c50889dfb7a74c2acf7cf3b76f42d044745433f706dca33f47d826cd1d"

/* Its size: 16-byte iNES header + 8 x 16 KiB PRG + 16 x 8 KiB CHR. The importer
 * checks it before reading the file (C6.29). */
#define OD_ROM_EU_SIZE 262160u

/* Application directory under the per-user data folder (od_userdir). */
#define OD_IMPORT_APP_DIR OD_USERDIR_APP

/* Relative path of the imported package inside the data home. */
#define OD_IMPORT_CONTENT_REL "content/base"

/* Fill `out` with the directory the importer writes to and the game reads from:
 * `override` when non-NULL (tests), else
 * $XDG_DATA_HOME/opendana/content/base, else
 * ~/.local/share/opendana/content/base. Returns OD_ERR_RANGE when the
 * buffer is too small. */
od_status od_import_default_dir(char *out, size_t cap, const char *override);

/* Import the EU ROM at `rom_path` into `dest_dir`. Every extractor runs into a
 * sibling temporary directory `<dest>.tmp-<pid>-XXXXXX`; the result is checked
 * against the versioned reference manifest (hashes only),
 * flushed to disk and only then renamed into place, so a failure, a `kill` or
 * a mismatch leaves no half content. When `manifest_path` is non-NULL a copy
 * of the produced sorted `sha256  relative/path` manifest is written there; it
 * is an output only, never read back as the reference (C6.29, C6.22 PUB-6).
 * `err` (optional) receives a human-readable reason on failure. */
od_status od_import_rom(const char *rom_path, const char *dest_dir,
                          const char *manifest_path, char *err, size_t err_cap);

/* Same as od_import_rom, checking against `reference` (manifest text) instead
 * of the built-in one. For tests that prove a mismatch is refused. */
od_status od_import_rom_with_reference(const char *rom_path, const char *dest_dir,
                                         const char *reference, const char *manifest_out,
                                         char *err, size_t err_cap);

/* The built-in reference manifest text (hashes and relative paths only). */
const char *od_import_reference_manifest(void);

/* Remove the leftovers of an interrupted import next to `dest_dir`:
 * `<dest>.tmp-*` of a process that is gone, and `<dest>.old-*` (restored as
 * `dest_dir` when the destination is missing, i.e. the import was killed
 * between its two renames). Returns how many entries were cleaned, 0 when
 * there was nothing, -1 on error. Called by the importer and at startup. */
int od_import_recover(const char *dest_dir);

/* 1 when `dir` holds a `pack.json` (an imported or development package). */
int od_import_content_present(const char *dir);

/* Resolve the content base with the C5.8 search order: `cli_override` (from
 * --content) wins, then $OD_CONTENT, then the XDG imported package, then the
 * repository development paths (delegated to od_content_resolve_base). */
od_status od_import_resolve_content(char *out, size_t cap, const char *cli_override);

#endif /* OD_IMPORT_H */
