/* state.h - save states: the `.odstate` format (C11, no SDL; C5.3).
 *
 * A save state is the whole mutable simulation session
 * (`od_flow_state`, `include/opendana/flow.h`) plus the sequencer's state, in a
 * small versioned file:
 *
 *     magic     4   "ODST"
 *     format    2   OD_STATE_FORMAT
 *     build     8   od_state_build_id(): the game build the state belongs to
 *     content   4   legacy guard of loaded levels/CHR/palette (not D3 identity)
 *     tick      8   simulation tick the state was taken at
 *     date      8   Unix time of the save
 *     level     4   level index (for the menu list)
 *     world     2   world number, 1-based
 *     round     2   round number, 1-based
 *     payload   n   `od_flow_state` bytes (transient pointers zeroed) followed
 *                   by the opaque sequencer block and optional backend data
 *     build    32   SHA-256 of the actual executable (product context)
 *     content  32   SHA-256 of parser-defined effective runtime resource bytes
 *     hostlen   4   length of pointer-free audio backend data
 *     engine    4   whether this snapshot has an actual sequencer engine
 *     checksum  4   FNV-1a of every byte before it
 *
 * A state is valid for the same format and the same content (D3, C6.43): a
 * different format id or content digest is rejected with a warning and the
 * running game is left untouched. C6.62 adds a layout-independent section
 * (format 3, between the payload and the identity footer, its byte length in
 * the four bytes before the footer): the ROM and mod identities, the session at
 * the start of the level in play and every input since. A state whose raw dump
 * does not fit this build (other layout) or whose content bytes changed, but
 * which comes from the same ROM and mod set, is restored by entering that level
 * the way `--play-level` does and replaying the inputs. The pointer members inside `od_flow_state`
 * are stored zeroed (or as a tiny kind index) and rebuilt on load by
 * `od_state_load` and `od_flow_state_rebind`. Format 1 is rejected: it omitted
 * product SDL audio/content and cannot be safely upgraded. */
#ifndef OD_EXT_STATE_H
#define OD_EXT_STATE_H

#include "opendana/common.h"
#include "opendana/flow.h"
#include "opendana/level.h"

#include <stddef.h>
#include <stdint.h>

#define OD_STATE_MAGIC "ODST"
#define OD_STATE_FORMAT 3u
/* Format 2 (no resume section) still loads when the layout fingerprint and
 * the content identity both match. */
#define OD_STATE_FORMAT_NO_RESUME 2u

/* Refuse absurd files before allocating (a real state is a few hundred KB). */
#define OD_STATE_MAX_BYTES (16u * 1024u * 1024u)

typedef struct {
    int valid;              /* header parsed and checksum verified */
    unsigned format;
    uint64_t build_id;
    uint32_t content;
    long tick;
    uint64_t date;          /* Unix seconds */
    int level;              /* level index, -1 when none */
    int world;              /* 1-based */
    int round;              /* 1-based */
} od_state_info;

/* Hash of the structure sizes/offsets this build serialises plus a manual ABI
 * constant. Any change to `od_flow_state`'s layout changes it, so a state of
 * another build is detected. */
uint64_t od_state_build_id(void);

/* FNV-1a32 of the level package (grids, commands, start cells) plus the level
 * count. Same content -> same value; a different pack or mod -> different. */
uint32_t od_state_content_hash(const od_levelset *levels);

/* Product identity requires C6.2's sealed immutable LOADED-byte snapshot.
 * A paths-only inventory (even readable) is insufficient; without snapshot
 * provenance vfs_digest fails closed. No directory or partial-resource fallback.
 * file_digest alone is not proof of bytes already consumed by a loader. */
/* C6.43: why the last od_state_load_ex refused a file (NONE after a load that
 * worked or failed for another reason, such as a missing file). */
typedef enum {
    OD_STATE_REJECT_NONE = 0,
    OD_STATE_REJECT_FORMAT,    /* another format/version of the state */
    OD_STATE_REJECT_CONTENT    /* saved with different imported content */
} od_state_reject;
od_state_reject od_state_last_reject(void);

od_status od_state_file_digest(const char *path, uint8_t out[32]);
od_status od_state_vfs_digest(const od_content_vfs *vfs, uint8_t out[32]);
/* Inventory comes from the content parser, NOT directory enumeration. This
 * helper hashes only supplied runtime paths via the authoritative VFS reader.
 * Order/duplicates/physical roots do not matter, effective resolved bytes do.
 * The caller must supply complete coverage (partial inventories are module
 * evidence only). SHA-256 encoding matches C6.2: sorted paths, each
 * [LE64 path byte length, path bytes, LE64 data byte length, data bytes].
 * Output is left untouched on any failed read. */
/* C6.62: the SHA-256 of the source image recorded in the package and a digest of the layer
 * identifiers of the mods on top of it. Both are left zero when absent. */
od_status od_state_rom_identity(const od_content_vfs *vfs, uint8_t rom[32],
                                uint8_t mods[32]);
od_status od_state_resources_digest(const od_content_vfs *vfs,
    const char *const *paths, size_t count, uint8_t out[32]);

/* Explicit backend context. The caller owns synchronization for audio/host.
 * NULL context is module-only: it fingerprints loaded levels/CHR/palette and
 * saves flow.audio_core. Product callers must supply the frozen loaded package
 * digest and executable digest, and the ACTUAL sequencer (SDL or core).
 * host is pointer-free backend data, restored only after all checks pass.
 * A non-OK runtime scene_sound_status (including unknown values) rejects both
 * export/import; only legitimate init/reset, not a slot, clears that latch. */
typedef struct {
    uint8_t build[32], content[32];
    /* C6.62: what a state taken now can be resumed on after the engine's
     * structures or the content bytes change. `rom` is the SHA-256 of the
     * ROM the package was imported from (zero = unknown: no resume across
     * content changes), `mods` identifies the mod set. */
    uint8_t rom[32], mods[32];
    od_audio *audio;
    void *host;
    size_t host_size;
    /* Optional initialized live screen owner, required for native password
     * candidates. Product binds exactly &flow->screens; never serialized. */
    const od_screens *screen_owner;
} od_state_context;
od_status od_state_save_ex(const od_flow *f, const char *path,
                            const od_state_context *ctx);
od_status od_state_load_ex(od_flow *f, const char *path,
                            const od_state_context *ctx);

/* Write `f`'s session to `path` atomically. Returns OD_OK or an error.
 * The flow must be initialised (od_flow_init); its own screens and core
 * sequencer form the context (module identity: no executable/package digest).
 * OD_ERR_RANGE for a flow driving the SDL device (od_audio_sdl_state_save). */
od_status od_state_save(const od_flow *f, const char *path);

/* Load `path` into `f`. On any error (missing/corrupt/other build/other
 * content/values out of range) `f` is left untouched and OD_ERR_* is
 * returned. A state written with another audio backend (sound on/off, SDL
 * device or core engine) loads: the sequencer is restored when both sides
 * have one, else the live one restarts silent until the next cue (C6.31).
 * A running --record script is closed (od_flow_timeline_break). */
od_status od_state_load(od_flow *f, const char *path);

/* Read only the header of `path` (for the menu list). Returns OD_OK when the
 * file is structurally valid; `out->valid` is set accordingly. */
od_status od_state_peek(const char *path, od_state_info *out);

/* `states/slot-<n>.odstate` in the per-user data directory (opendana/userdir.h;
 * LOCALAPPDATA on Windows), else `slot-<n>.odstate` in the current directory
 * when no per-user base exists at all. */
void od_state_default_path(char *out, size_t cap, int slot);

/* FNV-1a32 over `len` bytes (the file checksum, exposed for tests). */
uint32_t od_state_checksum(const void *data, size_t len);

#endif /* OD_EXT_STATE_H */
