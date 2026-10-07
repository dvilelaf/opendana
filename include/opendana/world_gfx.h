/* world_gfx.h - per-world level graphics for OpenDana.
 *
 * The composition tables in compose.c are only valid for world 1. This module
 * exposes the per-world cell-type -> CHR tile + background palette mapping for
 * the whole game, loaded from the content package (`worlds.json`).
 *
 * Levels 0..149 are the fifteen worlds (world = level/10, as in worldmap.h):
 * worlds 1..10 are the regular rounds and worlds 11..15 the hidden ones
 * (LEVEL 100..149, ROUND 11-1 .. 15-10), reached with the title-screen
 * SELECT+B x10 trick.
 *
 * Pure C11, no SDL, no file I/O: the caller hands the JSON text. The caller
 * composes with the same scheme as compose.c: a level cell uses four 8x8 CHR
 * tiles (TL, TR, BL, BR) and one of the four background palettes of the world's
 * palette RAM. */
#ifndef OD_WORLD_GFX_H
#define OD_WORLD_GFX_H

#include "opendana/common.h"

/* Preparation consumes already-read VFS bytes, validates every document in
 * order (last complete dataset wins), and never publishes. Commit consumes
 * the handle without allocation/IO/failure; discard is NULL-safe. */
typedef struct od_world_content_stage od_world_content_stage;
od_status od_world_content_prepare(od_world_content_stage **out,
    const char *const *json, const size_t *sizes, size_t count);
void od_world_content_commit(od_world_content_stage *stage);
void od_world_content_discard(od_world_content_stage *stage);

#include <stdint.h>

#define OD_WORLD_GFX_WORLDS 15  /* worlds 1..15 (indices 0..14)      */
#define OD_WORLD_GFX_CELLS  16  /* the 150 levels only use 0x00..0x0F */

/* Neighbour-edge class of one side of a cell with an autotile column (the solid
 * 0x01 and the object types that blend with ice).
 *
 * The game does not draw a plain wall block: each side of a solid cell gets a
 * different border depending on what sits next to it. The class is derived
 * from the neighbour's grid type by od_world_gfx_edge_class():
 *
 *   NONE  - the neighbour is solid (0x01) or, on the right, off-grid: the
 *           interior column (no border).
 *   EMPTY - the neighbour is the empty floor (0x00), off-grid to the left, or
 *           any other type (objects, fire): the black wall border.
 *   ICE   - the neighbour is ice (0x02..0x05) presenting an open side, i.e.
 *           type 0x02/0x03 to the left or 0x02/0x04 to the right: the blended
 *           ice border (tile ids 0x4C..0x4F in the terrain bank).
 *
 * Non-solid types only distinguish NONE from ICE (an empty neighbour keeps the
 * interior column, so their EMPTY class aliases NONE).
 *
 * The rule was derived from the reference nametable of all 50 hidden captures
 * and one regular capture (level 62): 6826 solid cells, 0 exceptions. */
typedef enum {
    OD_EDGE_NONE  = 0,
    OD_EDGE_EMPTY = 1,
    OD_EDGE_ICE   = 2
} od_edge_class;

/* One 2x2 CHR tile block with the background palette it is drawn in. */
typedef struct {
    uint16_t tiles[4]; /* TL, TR, BL, BR; indices into assets/tiles.chr */
    uint8_t  bg_pal;   /* 0..3, index into palette_ram[bg_pal*4 .. +3]   */
} od_gfx_tiles;

/* Graphics for one cell type.
 *
 * A cell is a 2x2 block of 8x8 CHR tiles.  Its columns are independent: the
 * left column (TL, BL) depends only on the left neighbour's edge class and the
 * right column (TR, BR) only on the right neighbour's.  Most types are a
 * single fixed block, so all three entries of `left`/`right` are equal; the
 * solid 0x01 and the ice-blending object types differ.
 *
 * `has_edges` is 1 when the three entries are not all equal. */
typedef struct {
    uint8_t known;      /* 1 = derived from an reference capture */
    uint8_t has_edges;  /* 1 = left[]/right[] differ per edge class */
    uint16_t left[3][2];  /* (TL, BL) per od_edge_class */
    uint16_t right[3][2]; /* (TR, BR) per od_edge_class */
    uint8_t  bg_pal;      /* 0..3, index into palette_ram              */
} od_gfx_cell;

/* All graphics of one world: the PPU palette RAM snapshot and the 16 cell
 * types the levels use.  Cell type t is cells[t] (t in 0x00..0x0F). */
typedef struct {
    int world;                 /* 0..14 (world 1..15) */
    uint8_t palette_ram[32];   /* PPU palette RAM, NES 6-bit colour indices */
    od_gfx_cell cells[OD_WORLD_GFX_CELLS];
} od_world_gfx;

/* Number of worlds in the table (always OD_WORLD_GFX_WORLDS). */
int od_world_gfx_count(void);

/* Load the table from a package `worlds.json` document (C2.2). Requires all
 * fifteen worlds; any other world count is a format error. */
od_status od_world_gfx_load_json(const char *json, size_t len);

/* Drop the loaded table (used by tests). */
void od_world_gfx_reset(void);
int od_world_gfx_loaded(void);

/* World by index (0..14), or NULL when out of range. */
const od_world_gfx *od_world_gfx_for_world(int world);

/* World that renders level_index (0..149), or NULL for an out-of-range level.
 * Levels 100..149 (worlds 11..15) are the hidden rounds and resolve to their
 * extracted graphics. */
const od_world_gfx *od_world_gfx_for_level(int level_index);

/* Cell graphics for a type (0x00..0x0F); NULL if the type or world is invalid. */
const od_gfx_cell *od_world_gfx_cell(const od_world_gfx *world, uint8_t cell_type);

/* The world's PPU palette RAM (32 bytes); NULL if world is NULL. */
const uint8_t *od_world_gfx_palette(const od_world_gfx *world);

/* Edge class of a cell side given the neighbour grid type.  `left` selects the
 * left side (its off-grid rule differs from the right's).  Exposed so the
 * extractor and tests can share the exact rule. */
od_edge_class od_world_gfx_edge_class(int neighbour_type, int left);

/* Resolve the tiles + palette a cell of `cell_type` uses.  `left_type` and
 * `right_type` are the neighbouring grid cell types (0..15); a negative value
 * means off-grid (the left edge counts as the empty floor, the right edge as
 * solid, matching the captured frames).  Returns 0 on success, -1 on bad
 * input. */
int od_world_gfx_resolve(const od_world_gfx *world, uint8_t cell_type,
                          int left_type, int right_type, od_gfx_tiles *out);

#endif /* OD_WORLD_GFX_H */
