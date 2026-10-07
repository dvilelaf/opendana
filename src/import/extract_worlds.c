/* extract_worlds.c - worlds.json.
 *
 * The empty floor, the terrain autotile and the ice connectivity are exact
 * formulas over the per-world CHR bank; the objects (pipes, stone, jar,
 * hazard) follow the composition tables read from the ROM. The per-world
 * background palettes come from the ROM's 10x16 table at PRG 0x5E00.
 *
 * The committed JSON also carries the capture-derived sprite palettes
 * (palette_ram[16..31]) and a couple of `known` flags; this port writes the
 * ROM-derived bytes for palette_ram[0..15] and the per-type `bg_pal` map, so
 * the background rendering is identical to the committed package.
 */
#include "fsutil.h"
#include "jsonw.h"
#include "rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EDGE_NONE 0
#define EDGE_EMPTY 1
#define EDGE_ICE 2

#define TERRAIN_BASE 0x440
#define WORLD_STEP 0x40
#define ICE_OFFSET 0x740

/* Background palette (BG palette 0..3) of cells 0x00..0x0F: the ROM's 16-byte
 * table at PRG 0x54D9 (read at import time, not a copy in our source). */
#define OFF_BG_PAL 0x54D9

/* Base CHR tile of the fire cell (0x0E), per world 1..15 (measured). */
static const int FIRE_BASE[15] = {
    0x700, 0x988, 0x990, 0x998, 0x9A0, 0x9A8, 0x9B0, 0x9B8, 0x9F0, 0x9F8,
    0x700, 0x708, 0x710, 0x718, 0x720,
};

/* Index into the ROM's sprite-palette table at PRG 0x5F20 (16 bytes each), per
 * world 1..15: the ROM's 15-byte table at PRG 0x1F1CB (read at import time; it
 * equals the sprite palette the game loads for each world). */
#define OFF_SPRITE_PAL_IDX 0x1F1CB

static int bank_world(int world) { return world < 10 ? world : world - 10; }

typedef struct {
    int left[3][2];
    int right[3][2];
    int has_edges;
} cell;

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

static void set_flat(cell *c, const int lp[2], const int rp[2])
{
    for (int e = 0; e < 3; e++) {
        c->left[e][0] = lp[0];
        c->left[e][1] = lp[1];
        c->right[e][0] = rp[0];
        c->right[e][1] = rp[1];
    }
    c->has_edges = 0;
}

static void empty_columns(cell *c, int world)
{
    int base = TERRAIN_BASE + bank_world(world) * WORLD_STEP;
    int lp[2] = {base + 0x40, base + 0x42};
    int rp[2] = {base + 0x41, base + 0x43};
    set_flat(c, lp, rp);
}

static void terrain_columns(cell *c, int world)
{
    int base = TERRAIN_BASE + bank_world(world) * WORLD_STEP;
    static const int l[3][2] = {{0x48, 0x4A}, {0x44, 0x46}, {0x4C, 0x4E}};
    static const int r[3][2] = {{0x49, 0x4B}, {0x45, 0x47}, {0x4D, 0x4F}};
    for (int e = 0; e < 3; e++) {
        c->left[e][0] = base + l[e][0];
        c->left[e][1] = base + l[e][1];
        c->right[e][0] = base + r[e][0];
        c->right[e][1] = base + r[e][1];
    }
    c->has_edges = 1;
}

static void ice_columns(cell *c, int type)
{
    static const int ids[4][4] = {
        {0x18, 0x19, 0x1A, 0x1B}, /* 0x05 */
        {0x18, 0x1D, 0x1A, 0x1F}, /* 0x03 */
        {0x1C, 0x19, 0x1E, 0x1B}, /* 0x04 */
        {0x1C, 0x1D, 0x1E, 0x1F}, /* 0x02 */
    };
    int row = (type == 0x05) ? 0 : (type == 0x03) ? 1 : (type == 0x04) ? 2 : 3;
    const int *v = ids[row];
    int lp[2] = {ICE_OFFSET + v[0], ICE_OFFSET + v[2]};
    int rp[2] = {ICE_OFFSET + v[1], ICE_OFFSET + v[3]};
    set_flat(c, lp, rp);
}

static void fire_columns(cell *c, int world)
{
    int base = FIRE_BASE[world];
    int lp[2] = {base, base + 2};
    int rp[2] = {base + 1, base + 3};
    set_flat(c, lp, rp);
}

static int pattern_tile(int pattern, int world)
{
    int slot = pattern >> 6;
    int low = pattern & 63;
    if (slot == 0)
        return ICE_OFFSET + low;
    if (slot == 1)
        return TERRAIN_BASE + bank_world(world) * WORLD_STEP + 0x40 + low;
    if (slot == 2)
        return ((world == 8 || world == 9) ? 0x1D : 0x1C) * 64 + low;
    return 0x1C * 64 + low;
}

static int left_class(int neighbour)
{
    if (neighbour == 0x01)
        return EDGE_NONE;
    if (neighbour == 0x02 || neighbour == 0x03)
        return EDGE_ICE;
    return EDGE_EMPTY;
}

static int right_class(int neighbour)
{
    if (neighbour == 0x01)
        return EDGE_NONE;
    if (neighbour == 0x02 || neighbour == 0x04)
        return EDGE_ICE;
    return EDGE_EMPTY;
}

/* The EU composition tables give the two columns per edge class. */
static void object_columns(cell *c, const uint8_t *b2c0, const uint8_t *b3c0,
                           const uint8_t *b4c0, int type, int world)
{
    static const int class_neighbour[3] = {0x01, 0x00, 0x02};
    const uint8_t *table[2] = {b2c0, b3c0};
    for (int side = 0; side < 2; side++) {
        int offs[2] = {side == 0 ? 0 : 1, side == 0 ? 2 : 3};
        for (int e = 0; e < 3; e++) {
            int base = b4c0[table[side][type * 16 + class_neighbour[e]]];
            int col[2] = {pattern_tile(base + offs[0], world),
                          pattern_tile(base + offs[1], world)};
            if (side == 0) {
                c->left[e][0] = col[0];
                c->left[e][1] = col[1];
            } else {
                c->right[e][0] = col[0];
                c->right[e][1] = col[1];
            }
        }
    }
    c->has_edges = (c->left[EDGE_NONE][0] != c->left[EDGE_ICE][0]) ||
                   (c->right[EDGE_NONE][0] != c->right[EDGE_ICE][0]);
}

static od_jw *pair_node(const int pair[2])
{
    od_jw *row = od_jw_array();
    char v[16];
    snprintf(v, sizeof(v), "0x%03X", pair[0]);
    od_jw_push(row, od_jw_string(v));
    snprintf(v, sizeof(v), "0x%03X", pair[1]);
    od_jw_push(row, od_jw_string(v));
    return row;
}

static od_jw *columns_node(const int cols[3][2])
{
    od_jw *arr = od_jw_array();
    for (int e = 0; e < 3; e++)
        od_jw_push(arr, pair_node(cols[e]));
    return arr;
}

od_status od_import_worlds(const od_rom *rom, const char *dir, char *err, size_t err_cap)
{
    const uint8_t *prg = rom->prg;
    const uint8_t *b2c0 = prg + 0x52C0;
    const uint8_t *b3c0 = prg + 0x53C0;
    const uint8_t *b4c0 = prg + 0x54C0;
    if (rom->prg_size < OFF_SPRITE_PAL_IDX + 15) {
        set_err(err, err_cap, "PRG ROM too small for the world tables");
        return OD_ERR_FORMAT;
    }
    const uint8_t *bg_pal = prg + OFF_BG_PAL;
    const uint8_t *sprite_pal_idx = prg + OFF_SPRITE_PAL_IDX;

    od_jw *doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "id", od_jw_string("worlds"));
    od_jw_add(doc, "note",
               od_jw_string("per-world cell-type (0x00..0x0F) -> CHR tiles "
                             "(TL,TR,BL,BR) + background palette; a cell is two "
                             "columns (left = TL,BL; right = TR,BR) indexed by "
                             "the neighbour edge class 0=none, 1=empty floor, "
                             "2=ice; only the solid 0x01 differs"));
    od_jw *worlds = od_jw_array();
    for (int world = 0; world < 15; world++) {
        cell cells[16];
        memset(cells, 0, sizeof(cells));
        empty_columns(&cells[0x00], world);
        terrain_columns(&cells[0x01], world);
        for (int t = 0x02; t <= 0x05; t++)
            ice_columns(&cells[t], t);
        static const int objects[] = {0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0F};
        for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); i++)
            object_columns(&cells[objects[i]], b2c0, b3c0, b4c0, objects[i], world);
        fire_columns(&cells[0x0E], world);

        od_jw *w = od_jw_object();
        od_jw_add(w, "world", od_jw_integer(world + 1));
        od_jw *pal = od_jw_array();
        const uint8_t *rom_pal = prg + 0x5E00 + (world % 10) * 16;
        for (int i = 0; i < 16; i++) {
            char v[8];
            snprintf(v, sizeof(v), "0x%02X", rom_pal[i]);
            od_jw_push(pal, od_jw_string(v));
        }
        const uint8_t *sp = prg + 0x5F20 + sprite_pal_idx[world] * 16;
        for (int i = 0; i < 16; i++) {
            char v[8];
            uint8_t value = sp[i];
            if (i == 0 || i == 4 || i == 8 || i == 12)
                value = 0x00;
            snprintf(v, sizeof(v), "0x%02X", value);
            od_jw_push(pal, od_jw_string(v));
        }
        od_jw_add(w, "palette_ram", pal);

        od_jw *cl = od_jw_object();
        for (int t = 0; t < 16; t++) {
            od_jw *c = od_jw_object();
            int known = 1;
            /* The hidden-world captures never showed these object cells. */
            if ((world == 10 && (t == 0x0C || t == 0x0D)) ||
                (world == 11 && t == 0x0A))
                known = 0;
            od_jw_add(c, "known", od_jw_integer(known));
            od_jw_add(c, "has_edges", od_jw_bool(cells[t].has_edges));
            od_jw_add(c, "left", columns_node(cells[t].left));
            od_jw_add(c, "right", columns_node(cells[t].right));
            od_jw_add(c, "bg_pal", od_jw_integer(bg_pal[t]));
            char key[8];
            snprintf(key, sizeof(key), "%02X", t);
            od_jw_add(cl, key, c);
        }
        od_jw_add(w, "cells", cl);
        od_jw_push(worlds, w);
    }
    od_jw_add(doc, "worlds", worlds);

    char path[4096];
    od_fs_join(path, sizeof(path), dir, "worlds.json");
    od_fs_mkdir_parent(path);
    FILE *f = od_fs_fopen(path, "wb");
    if (f == NULL) {
        od_jw_free(doc);
        set_err(err, err_cap, "cannot write worlds.json");
        return OD_ERR_IO;
    }
    od_jw_write(f, doc, 1);
    fputc('\n', f);
    fclose(f);
    od_jw_free(doc);
    return OD_OK;
}
