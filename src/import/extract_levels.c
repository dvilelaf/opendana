/* extract_levels.c - levels and object records from the EU PRG.
 *
 * Reading only: the pointer table (PRG 0x6000), the 30-byte backing mask per
 * level (PRG 0x4000), the command stream at each record pointer, the player
 * start (PRG 0x5194) and the level metadata (PRG 0x522A). Emits
 * levels/world-NN.json with the JSON shape of the package.
 */
#include "fsutil.h"
#include "jsonw.h"
#include "rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEVEL_COUNT 150
#define LEVELS_PER_WORLD 10
#define GRID_W 16
#define GRID_H 15
#define GRID_CELLS (GRID_W * GRID_H)

#define OFF_POINTERS 0x6000
#define OFF_MASK 0x4000
#define OFF_PLAYER 0x5194
#define OFF_METADATA 0x522A

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Decode one level's command stream into the level grid and the ordered object
 * list. Returns 0 on success. */
static int decode_level(const uint8_t *prg, int index, const uint16_t *pointers,
                        uint8_t grid[GRID_CELLS], od_jw *objects)
{
    const uint8_t *mask = prg + OFF_MASK + 30 * index;
    for (int b = 0; b < 30; b++)
        for (int shift = 7; shift >= 0; shift--)
            grid[b * 8 + (7 - shift)] = (uint8_t)((mask[b] >> shift) & 1);

    uint32_t pos = pointers[index];
    uint32_t limit = (index < LEVEL_COUNT - 1) ? pointers[index + 1] : 0x8000u;
    while (pos < limit && prg[pos] != 0x00) {
        uint8_t kind = prg[pos];
        if (kind >= 0xA0)
            return -1;
        if (pos + 2 > limit)
            return -1;
        if (kind >= 0x80) {
            od_jw *o = od_jw_object();
            od_jw_add(o, "special", od_jw_integer(kind));
            od_jw_add(o, "argument", od_jw_integer(prg[pos + 1]));
            od_jw_push(objects, o);
            pos += 2;
        } else {
            uint8_t count = prg[pos + 1];
            if (count == 0 || pos + 2 + count >= limit)
                return -1;
            const uint8_t *coords = prg + pos + 2;
            od_jw *positions = od_jw_array();
            for (uint8_t i = 0; i < count; i++) {
                uint8_t c = coords[i];
                if (c >= GRID_CELLS)
                    return -1;
                grid[c] = kind;
                od_jw *pair = od_jw_array();
                od_jw_push(pair, od_jw_integer(c % GRID_W));
                od_jw_push(pair, od_jw_integer(c / GRID_W));
                od_jw_push(positions, pair);
            }
            od_jw *o = od_jw_object();
            od_jw_add(o, "type", od_jw_integer(kind));
            od_jw_add(o, "positions", positions);
            od_jw_push(objects, o);
            pos += 2 + count;
        }
    }
    if (pos >= limit || prg[pos] != 0x00)
        return -1;
    if (index < LEVEL_COUNT - 1 && pos + 1 != limit)
        return -1;
    return 0;
}

static od_jw *grid_node(const uint8_t grid[GRID_CELLS])
{
    static const char hex[] = "0123456789abcdef";
    od_jw *rows = od_jw_array();
    for (int y = 0; y < GRID_H; y++) {
        char row[GRID_W + 1];
        for (int x = 0; x < GRID_W; x++)
            row[x] = hex[grid[y * GRID_W + x] & 0x0F];
        row[GRID_W] = '\0';
        od_jw_push(rows, od_jw_string(row));
    }
    return rows;
}

od_status od_import_levels(const od_rom *rom, const char *dir, char *err, size_t err_cap)
{
    if (rom->prg_size < 0x8000) {
        set_err(err, err_cap, "PRG ROM too small for the level tables");
        return OD_ERR_FORMAT;
    }
    const uint8_t *prg = rom->prg;
    uint16_t pointers[LEVEL_COUNT];
    for (int i = 0; i < LEVEL_COUNT; i++)
        pointers[i] = (uint16_t)(rd16(prg + OFF_POINTERS + 2 * i) - 0x4000);
    if (pointers[0] != 0x612C) {
        set_err(err, err_cap, "unrecognised level pointer table");
        return OD_ERR_FORMAT;
    }
    for (int i = 0; i + 1 < LEVEL_COUNT; i++) {
        if (pointers[i] >= pointers[i + 1]) {
            set_err(err, err_cap, "level pointer table is not strictly increasing");
            return OD_ERR_FORMAT;
        }
    }

    od_jw *levels[LEVEL_COUNT];
    memset(levels, 0, sizeof(levels));
    for (int i = 0; i < LEVEL_COUNT; i++) {
        uint8_t grid[GRID_CELLS];
        od_jw *objects = od_jw_array();
        if (decode_level(prg, i, pointers, grid, objects) != 0) {
            od_jw_free(objects);
            for (int k = 0; k < i; k++)
                od_jw_free(levels[k]);
            set_err(err, err_cap, "malformed level record");
            return OD_ERR_FORMAT;
        }
        uint8_t player = prg[OFF_PLAYER + i];
        uint8_t metadata = prg[OFF_METADATA + i];
        int world = i / LEVELS_PER_WORLD + 1;
        int round = i % LEVELS_PER_WORLD + 1;
        char name[16];
        snprintf(name, sizeof(name), "%d-%d", world, round);

        od_jw *player_node = od_jw_object();
        od_jw_add(player_node, "x", od_jw_integer(player % GRID_W));
        od_jw_add(player_node, "y", od_jw_integer(player / GRID_W));

        od_jw *entry = od_jw_object();
        od_jw_add(entry, "index", od_jw_integer(i));
        od_jw_add(entry, "number", od_jw_integer(i + 1));
        od_jw_add(entry, "world", od_jw_integer(world));
        od_jw_add(entry, "round", od_jw_integer(round));
        od_jw_add(entry, "name", od_jw_string(name));
        od_jw_add(entry, "player", player_node);
        od_jw_add(entry, "metadata", od_jw_integer(metadata));
        od_jw_add(entry, "grid", grid_node(grid));
        od_jw_add(entry, "objects", objects);
        levels[i] = entry;
    }

    int rc = OD_OK;
    {
        char levels_dir[4096];
        od_fs_join(levels_dir, sizeof(levels_dir), dir, "levels");
        if (od_fs_mkdir_p(levels_dir) != 0) {
            for (int i = 0; i < LEVEL_COUNT; i++)
                od_jw_free(levels[i]);
            set_err(err, err_cap, "cannot create the levels directory");
            return OD_ERR_IO;
        }
    }
    for (int world = 1; world <= LEVEL_COUNT / LEVELS_PER_WORLD; world++) {
        char id[32];
        snprintf(id, sizeof(id), "world-%02d", world);
        od_jw *doc = od_jw_object();
        od_jw_add(doc, "schema_version", od_jw_integer(1));
        od_jw_add(doc, "id", od_jw_string(id));
        od_jw_add(doc, "world", od_jw_integer(world));
        od_jw_add(doc, "grid_width", od_jw_integer(GRID_W));
        od_jw_add(doc, "grid_height", od_jw_integer(GRID_H));
        od_jw *list = od_jw_array();
        for (int r = 0; r < LEVELS_PER_WORLD; r++) {
            int idx = (world - 1) * LEVELS_PER_WORLD + r;
            od_jw_push(list, levels[idx]);
            levels[idx] = NULL; /* the document owns it now */
        }
        od_jw_add(doc, "levels", list);

        char rel[64];
        snprintf(rel, sizeof(rel), "levels/%s.json", id);
        char path[4096];
        od_fs_join(path, sizeof(path), dir, rel);
        FILE *f = od_fs_fopen(path, "wb");
        if (f == NULL) {
            od_jw_free(doc);
            rc = OD_ERR_IO;
            break;
        }
        od_jw_write(f, doc, 2);
        fputc('\n', f);
        fclose(f);
        od_jw_free(doc);
    }
    /* Whatever happened, free the entries not consumed by the docs. */
    for (int i = 0; i < LEVEL_COUNT; i++)
        od_jw_free(levels[i]);
    if (rc != OD_OK) {
        set_err(err, err_cap, "cannot write the level files");
        return rc;
    }
    return OD_OK;
}
