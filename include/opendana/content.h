/* content.h - layered content package loader (C2.1).
 *
 * The game's levels and object records live in a moddable package:
 *
 *     content/base/pack.json
 *     content/base/levels/world-01.json ... world-15.json
 *     mods/<id>/mod.json             (a layer that overrides files by path)
 *
 * The loader parses JSON with the vendored cJSON (external/cJSON), reconstructs
 * the exact `levels.bin` / `enemies.bin` byte streams in memory and hands them to
 * the verified parsers in level.c / enemy.c. The JSON is therefore a lossless,
 * editable view of the data.
 *
 * The VFS resolves a relative path across the base layer and every mod layer in
 * load order; the last layer that has the file wins. This is what lets a mod
 * replace one level without owning the whole package.
 *
 * Nothing here depends on the current working directory: `od_content_resolve_base`
 * locates the default package next to the executable (defect A11).
 */
#ifndef OD_CONTENT_H
#define OD_CONTENT_H

#include "opendana/common.h"
#include "opendana/enemy.h"
#include "opendana/level.h"

#define OD_CONTENT_SCHEMA_VERSION 1
/* pack.json and mod.json: 2 since C6.53 (neutral file names and keys); a
 * version-1 package or mod is stale and must be imported or migrated again. */
#define OD_PACK_SCHEMA_VERSION 2
#define OD_CONTENT_MAX_LAYERS 16

typedef struct {
    int schema_version;
    char id[64];
    char name[128];
    char version[32];
    int level_count;
    int world_count;
} od_content_info;

/* One layer of the VFS. `root` is the directory holding pack.json / mod.json. */
typedef struct {
    char id[64];
    char root[1024];
    int is_mod;
    int schema_version;
} od_content_layer;

typedef struct {
    od_content_layer layers[OD_CONTENT_MAX_LAYERS];
    int count;
    /* Opaque pinned-read snapshot. Initialized by vfs_init; do not serialize
     * or shallow-copy ownership. Ordinary const borrows live until vfs_free. */
    struct od_content_snapshot *snapshot;
} od_content_vfs;

/* Resolve the default base package directory independently of the CWD.
 * `cli_override` (from --content) wins verbatim; then $OD_CONTENT; then
 * `<exe>/content/base`, `<exe>/../content/base`, `content/base`. The first
 * candidate that contains pack.json wins, otherwise the first candidate is
 * returned so the caller can report a concrete path. */
od_status od_content_resolve_base(char *out, size_t cap, const char *cli_override);

/* `--mod` takes a directory or a mod ID.
 * A directory holding mod.json, or any path with '/', is kept verbatim; a bare
 * ID resolves to `<base>/../../mods/<id>` (the repository's mods/) or
 * `<base>/../mods/<id>` (content/mods/), never through the CWD. OD_ERR_IO when
 * no such mod exists. */
od_status od_content_resolve_mod(char *out, size_t cap, const char *base_dir,
                                   const char *arg);

/* Build a VFS from a base directory and optional mod directories. Each mod
 * directory must contain mod.json (the base directory must contain pack.json).
 * The base is layer 0. */
od_status od_content_vfs_init(od_content_vfs *vfs, const char *base_dir,
                                const char *const *mod_dirs, int mod_count);

/* Resolve `rel` across the layers. Returns OD_ERR_IO when no layer has it. */
od_status od_content_vfs_resolve(const od_content_vfs *vfs, const char *rel,
                                   char *out, size_t cap);

/* Read a whole file through the VFS into a malloc'd buffer (NUL-terminated for
 * JSON convenience; `size` is the byte length without the terminator). */
od_status od_content_vfs_read(const od_content_vfs *vfs, const char *rel,
                                char **data, size_t *size);

void od_content_vfs_free(od_content_vfs *vfs);

#define OD_CONTENT_RESOURCES_API 1
#define OD_CONTENT_SNAPSHOT_API 1
/* Full consumed inventory, NOT a directory/manifest walker. Only available
 * after tables, levels, graphics/audio and eager scene provider all complete.
 * Metadata is validated/pinned, not hashed. Missing provider => IO, NULL/0.
 * Vector and strings are malloc'd; caller frees each string and the vector. */
od_status od_content_vfs_resources(const od_content_vfs *vfs,
    char ***paths, size_t *count);
/* SHA256 over sorted relative path/byte-length/immutable loaded bytes;
 * output unchanged on failure. No roots, layer names or ignored TXT included. */
od_status od_content_vfs_snapshot_digest(const od_content_vfs *vfs, uint8_t out[32]);
/* Scene-writer completion contract: call ONLY after successful eager init of
 * ALL mandatory scenes, with EVERY read through this same VFS. A mutable
 * fopen/resolve loader or lazy scene cannot certify completion. This freezes
 * the resource set: later reads of unconsumed paths fail, never hit disk. */
od_status od_content_vfs_finish_scenes(const od_content_vfs *vfs);

/* The whole package, reconstructed as the exact levels.bin / enemies.bin. */
typedef struct {
    od_content_info info;
    od_levelset levels;
    od_enemyset enemies;
} od_content_pack;

od_status od_content_pack_load(od_content_pack *out, const od_content_vfs *vfs);
void od_content_pack_free(od_content_pack *out);

/* Print one line per level (used by `--list-levels`): index, world-round, size. */
void od_content_pack_describe(const od_content_pack *pack);

/* A single level loaded from a JSON file (standalone, owned command bytes).
 * `world` / `round` / `number` are metadata; `number` is 0 when the file does
 * not hold it. The grid is inline in `level`; commands point into `buffer`. */
typedef struct {
    od_level level;
    uint8_t *buffer;
    size_t buffer_size;
    int number;
    int world;
    int round;
    char source[1024];
} od_content_level;

/* Load a level file. A world file (top-level "levels" array) selects the entry
 * whose "round" matches `round` (1..10), or the first when `round` <= 0. A
 * single-level file (top-level "grid") loads directly; `round` is then only
 * informational. */
od_status od_content_level_load(od_content_level *out, const char *path, int round);
void od_content_level_free(od_content_level *out);

/* Validation (`--validate`). `path` may be a pack directory, a mod directory or
 * a single JSON level/world file. The report is filled even on error. */
typedef struct {
    int packs;
    int levels;
    int objects;
    int errors;
    char message[512];
} od_validate_report;

od_status od_content_validate(const char *path, od_validate_report *report);

/* C6.33: identity of an imported package. `reference` is manifest text, one
 * "<sha256 hex>  <relative/path>" line per file (the importer's format). Only
 * the files it lists are read and hashed, so the check costs one read of the
 * package (about 2 MB) and depends on nothing but those bytes. OD_OK when
 * every listed file exists under `base_dir` with that hash; OD_ERR_FORMAT
 * when one is missing or different (`why`: how many and the first one) or
 * the reference is malformed; OD_ERR_RANGE on a missing argument. */
od_status od_content_check_identity(const char *base_dir, const char *reference,
                                      char *why, size_t cap);

/* The same against the reference manifest embedded in this binary
 * (src/import/content-manifest.sha256, C6.29): OD_OK when `base_dir` is
 * exactly what this version's `--import-rom` writes. A package imported by an
 * older version, or edited afterwards, gives OD_ERR_FORMAT. */
od_status od_content_check_imported(const char *base_dir, char *why, size_t cap);

#endif /* OD_CONTENT_H */
