/* content_gfx.h - graphics and audio of the content package (C2.2).
 *
 * Loads the pure-C graphics/audio modules (CHR, sprites, worlds, palettes,
 * screens) and the audio data from a layered content VFS. `od_assets_load`
 * calls `od_gfx_load_default`, and content.c re-applies a VFS with mods to
 * the bound asset bundle through `od_gfx_apply`.
 */
#ifndef OD_CONTENT_GFX_H
#define OD_CONTENT_GFX_H

#include "opendana/common.h"
#include "opendana/content.h"
#include "opendana/transfer_structure.h"

#include "opendana/assets.h"
#include "opendana/scene_map.h"
#include "opendana/scene_whatnext.h"
#include "opendana/scene_magicword.h"

/* Load every graphics/audio section declared by the pack. */
od_status od_gfx_load(const od_content_vfs *vfs);

/* Scene writer reads the SAME pinned bytes as startup loaders, no lazy
 * default package. Complete only after every mandatory scene loaded eagerly
 * and no mutable fopen/resource bypass remains. Until then states fail closed. */
od_status od_gfx_content_read(const char *rel, char **data, size_t *size);
od_status od_gfx_scene_resources_complete(void);

/* Resolve the default (or overridden) base package and load from it. */
od_status od_gfx_load_default(const char *base_override);

/* Reload from `vfs` and update the bound asset bundle (used when mods are
 * layered on after the initial load). No-op when nothing is bound. */
od_status od_gfx_apply(const od_content_vfs *vfs);

void od_gfx_bind_assets(od_assets *assets);
void od_gfx_reset(void);
int od_gfx_loaded(void);

/* Resolve a package-relative path (e.g. "screens/tracks/intro.track") through
 * the layers of the last successful load, mods first. OD_ERR_IO when nothing
 * has been loaded or no layer has the file. */
od_status od_gfx_resolve(const char *rel, char *out, size_t cap);

/* The raw CHR reconstructed from the PNG sheets (OD_CHR_BYTES, owned here). */
const uint8_t *od_gfx_chr(void);
size_t od_gfx_chr_size(void);

/* Package-only extracts. The compiled bridge for screens.c still lives in
 * src/core/tables/ while that file is owned by another checkpoint. */
const uint8_t *od_gfx_boss_arena(void);
unsigned od_gfx_boss_arena_count(void);
const uint8_t *od_gfx_retry_tiles(void);
const uint8_t *od_gfx_retry_palette(void);
const uint8_t *od_gfx_retry_cursor_cols(void);
unsigned od_gfx_retry_cursor_tile(void);

/* screens/map.json: the WORLD map sprite tables and castle overlay, or NULL
 * when the package does not declare it (WORLD-MAP). */
const od_map_art *od_gfx_map_art(void);

/* screens/hearth.json: the hearth illustration of the two menus, or NULL when
 * the package does not declare it (WHATNEXT). */
const od_hearth_art *od_gfx_hearth_art(void);

/* screens/magicword.json: "Today's magicword", its question and the
 * good-night cinematic's cues and loop, or NULL when the package does not
 * declare it (MAGICWORD). */
const od_magicword_art *od_gfx_magicword_art(void);

/* screens/layouts.json (C4.2 PISTAS-A): the
 * static pictures of the title, the "Where to begin from?" menu, its close and
 * the WORLD map, observed in the PAL reference, as full scene displays (palette,
 * nametables, banks, mask, bands, sprites; OD_SCENE_WORDS words). Their
 * dynamic parts are logic in screens.c. */
#define OD_LAYOUT_WORDS 2382
#define OD_LAYOUT_TITLE_KEYS 8
#define OD_LAYOUT_END_WORDS 8
typedef struct {
    int loaded;
    /* The title settled after its setup (title clip tick 128), its palette
     * keyframes (setup fade-in and the loop palette), the sprite banks before tick
     * `sprite_banks_from`, the banks its last tick writes, and the colour cycle
     * (palette `colour_entry` from the colour table). */
    uint16_t title[OD_LAYOUT_WORDS];
    unsigned title_key_count;
    uint16_t title_key_tick[OD_LAYOUT_TITLE_KEYS];
    uint8_t title_key_palette[OD_LAYOUT_TITLE_KEYS][32];
    uint16_t title_sprite_banks_before[2];
    unsigned title_sprite_banks_from;
    unsigned title_end_tick, title_end_count;
    uint16_t title_end[OD_LAYOUT_END_WORDS][2];
    unsigned colour_entry;
    /* Colour table from index 0: the counter is 0..2 in play; the editor can leave
     * 3..6 and the title loop then reads the bytes after the three colours
     * (EDITOR-COLOR). */
    unsigned colour_count;
    uint8_t colour_table[8];
    /* The menu settled (text and cursor), its close's first frame and length. */
    uint16_t menu[OD_LAYOUT_WORDS];
    uint16_t menu_close[OD_LAYOUT_WORDS];
    unsigned menu_close_frames;
    /* The WORLD map background the scene_map logic draws over. */
    uint16_t map[OD_LAYOUT_WORDS];
} od_screen_layouts;
/* NULL when the package does not declare screens/layouts.json. */
const od_screen_layouts *od_gfx_layouts(void);

/* C6.18 static v2 data. These are NOT legacy clip-ready scene displays.
 * Borrowed immutable pointers expire at the next successful load/reset. */
typedef struct {
    uint8_t scene_index, chr_mode, background_banks[4], ciram[2048];
} od_static_map_base;
typedef struct { od_static_map_base map_bases[2]; } od_static_layouts;
typedef struct { uint8_t count, data[59]; uint16_t address; } od_static_mw_record;
typedef enum { OD_STATIC_MW_RECORDS, OD_STATIC_MW_YIELD, OD_STATIC_MW_WORD } od_static_mw_op;
typedef struct {
    od_static_mw_op op;
    size_t record_count;
    const od_static_mw_record *records;
} od_static_mw_command;
typedef struct {
    uint8_t palette_buffer[32], chr[6], song_request, queue_capacity_bytes;
    od_static_mw_command screen[5], question[1];
    struct { uint16_t address; uint8_t count, group, gap_tile, glyphs[32]; } word;
    struct { uint8_t y, tile, attr, x[2]; } cursor;
    struct {
        uint8_t bg_mask, bg_entries[3], bg_tables[3][3];
        uint8_t sprite_mask, sprite_entry, sprite_table[5];
    } cycle;
    struct {
        uint8_t palette_buffer[32], brightness_bg, brightness_sprite;
        struct { uint8_t mask, entry, colours[4]; } cycle;
    } post_goodnight;
} od_static_magicword;
const od_static_layouts *od_gfx_static_layouts(void);
const od_static_magicword *od_gfx_static_magicword(void);

/* screens/password.json: the magicword box's message records, or NULL when
 * the package does not declare it (PISTAS-B). */
const od_password_art *od_gfx_password_art(void);

/* screens/castles.json (CASTILLOS): per world 1..9 (index 0..8) the length of
 * its round-10 castle clip and the sound requests the original's script makes on
 * given clip ticks (direct writes and queue requests, in order), produced by
 * the importer. */
#define OD_CASTLE_WORLDS 9
#define OD_CASTLE_CUES_MAX 32
typedef struct { uint16_t tick; uint8_t id; } od_castle_cue;
typedef struct {
    int loaded;
    unsigned duration;   /* clip frames: the cinematic window plus the one map frame after it */
    unsigned skip_from;  /* first clip tick with START accepted */
    unsigned frame_tick_lost_count;
    uint16_t frame_tick_lost[4]; /* clip ticks whose display pass the script's setup loses */
    unsigned cue_count;
    od_castle_cue cues[OD_CASTLE_CUES_MAX];
} od_castle_info;
/* NULL when the package does not declare the world's castle. */
const od_castle_info *od_gfx_castle(int world_index);

/* screens/world-clears.json (END-OF-WORLD): the end-of-world cinematics the
 * original plays when a round-10 clear completes world 1..9 (index 0..8)
 * and the castle opening that follows world 9's (index 9), with the same
 * timing fields as the castles
 * (skip_from == duration: START is not read). Produced by the importer.
 * CASTLE-APPEARS: indices 10..19 are the castle appearance of worlds 1..10
 * (same clip kind), clips 11..20 of the file. */
#define OD_WORLD_CLEAR_CLIPS 20
#define OD_WORLD_CLEAR_CASTLE_OPEN 9
#define OD_WORLD_CLEAR_CASTLE_APPEARS 10
/* NULL when the package does not declare the clip. */
const od_castle_info *od_gfx_world_clear(int clip_index);

/* The ODSEQ1 stream rebuilt from the package audio JSON; `len` is the byte
 * length. NULL until loaded. */
const char *od_gfx_audio_hex(size_t *len);

#endif /* OD_CONTENT_GFX_H */
