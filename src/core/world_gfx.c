/* world_gfx.c - per-world level graphics.
 *
 * The tables come from the content package (`worlds.json`), loaded at runtime
 * from the JSON produced by the importer. Worlds 1..10 are the regular rounds
 * and 11..15 the hidden ones (LEVEL 100..149).
 *
 * Pure C11, no SDL: the caller hands us the JSON text. */
#include "opendana/world_gfx.h"

#include "cJSON.h"

#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define OD_WORLD_GFX_REGULAR 10

static od_world_gfx g_worlds[OD_WORLD_GFX_WORLDS];
static int g_loaded;

int od_world_gfx_loaded(void) { return g_loaded; }

void od_world_gfx_reset(void)
{
    memset(g_worlds, 0, sizeof(g_worlds));
    g_loaded = 0;
}

int od_world_gfx_count(void)
{
    return OD_WORLD_GFX_WORLDS;
}

const od_world_gfx *od_world_gfx_for_world(int world)
{
    if (!g_loaded || world < 0 || world >= OD_WORLD_GFX_WORLDS)
        return NULL;
    return &g_worlds[world];
}

const od_world_gfx *od_world_gfx_for_level(int level_index)
{
    /* Levels 0..149 are the fifteen worlds, ten rounds each.  Levels 100..149
     * are the hidden rounds (worlds 11..15). */
    if (level_index < 0 || level_index >= OD_WORLD_GFX_WORLDS * 10)
        return NULL;
    return od_world_gfx_for_world(level_index / 10);
}

const od_gfx_cell *od_world_gfx_cell(const od_world_gfx *world, uint8_t cell_type)
{
    if (world == NULL || cell_type >= OD_WORLD_GFX_CELLS)
        return NULL;
    return &world->cells[cell_type];
}

const uint8_t *od_world_gfx_palette(const od_world_gfx *world)
{
    return (world == NULL) ? NULL : world->palette_ram;
}

od_edge_class od_world_gfx_edge_class(int neighbour_type, int left)
{
    if (neighbour_type == 0x01)
        return OD_EDGE_NONE;                      /* solid: interior      */
    if (neighbour_type < 0)
        return left ? OD_EDGE_EMPTY : OD_EDGE_NONE; /* off-grid asymmetry */
    /* Ice presents the blended border only when the side facing us is open:
     * left/right connectivity is encoded by the ice connectivity type. */
    if (neighbour_type == 0x02 || (left ? neighbour_type == 0x03
                                        : neighbour_type == 0x04))
        return OD_EDGE_ICE;
    return OD_EDGE_EMPTY;                         /* floor / objects / fire */
}

int od_world_gfx_resolve(const od_world_gfx *world, uint8_t cell_type,
                          int left_type, int right_type, od_gfx_tiles *out)
{
    if (world == NULL || out == NULL || cell_type >= OD_WORLD_GFX_CELLS)
        return -1;
    const od_gfx_cell *cell = &world->cells[cell_type];
    int ls = (int)od_world_gfx_edge_class(left_type, 1);
    int rs = (int)od_world_gfx_edge_class(right_type, 0);
    out->tiles[0] = cell->left[ls][0];
    out->tiles[1] = cell->right[rs][0];
    out->tiles[2] = cell->left[ls][1];
    out->tiles[3] = cell->right[rs][1];
    out->bg_pal = cell->bg_pal;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* JSON loader                                                                */
/* ------------------------------------------------------------------------- */

static int value_as_int(const cJSON *v, int *out)
{
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v) ? 1 : 0;
        return 1;
    }
    if (cJSON_IsNumber(v)) {
        if (!(v->valuedouble >= INT_MIN && v->valuedouble <= INT_MAX) ||
            v->valuedouble != (double)(int)v->valuedouble) return 0;
        *out = (int)v->valuedouble;
        return 1;
    }
    if (cJSON_IsString(v)) {
        char *end = NULL;
        errno = 0;
        long number = strtol(v->valuestring, &end, 0);
        if (errno || end == v->valuestring || *end || number < INT_MIN || number > INT_MAX)
            return 0;
        *out = (int)number;
        return 1;
    }
    return 0;
}

static int load_pair(const cJSON *row, uint16_t out[2])
{
    if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != 2)
        return 0;
    for (int k = 0; k < 2; k++) {
        int v;
        if (!value_as_int(cJSON_GetArrayItem(row, k), &v) || v < 0 || v > 0xFFFF)
            return 0;
        out[k] = (uint16_t)v;
    }
    return 1;
}

static int load_cell(const cJSON *cell, od_gfx_cell *out)
{
    if (!cJSON_IsObject(cell))
        return 0;
    const cJSON *known = cJSON_GetObjectItemCaseSensitive(cell, "known");
    const cJSON *has_edges = cJSON_GetObjectItemCaseSensitive(cell, "has_edges");
    const cJSON *bg_pal = cJSON_GetObjectItemCaseSensitive(cell, "bg_pal");
    const cJSON *left = cJSON_GetObjectItemCaseSensitive(cell, "left");
    const cJSON *right = cJSON_GetObjectItemCaseSensitive(cell, "right");
    int known_v, edges_v, pal_v;
    if (!value_as_int(known, &known_v) || !value_as_int(has_edges, &edges_v) ||
        !value_as_int(bg_pal, &pal_v) || pal_v < 0 || pal_v > 3 ||
        !cJSON_IsArray(left) || cJSON_GetArraySize(left) != 3 ||
        !cJSON_IsArray(right) || cJSON_GetArraySize(right) != 3)
        return 0;
    out->known = (uint8_t)(known_v != 0);
    out->has_edges = (uint8_t)(edges_v != 0);
    out->bg_pal = (uint8_t)pal_v;
    for (int e = 0; e < 3; e++) {
        if (!load_pair(cJSON_GetArrayItem(left, e), out->left[e]) ||
            !load_pair(cJSON_GetArrayItem(right, e), out->right[e]))
            return 0;
    }
    return 1;
}

struct od_world_content_stage {
    od_world_gfx worlds[OD_WORLD_GFX_WORLDS];
    int loaded;
};

static od_status world_parse(od_world_content_stage *candidate, const char *json, size_t len)
{
    if (json == NULL)
        return OD_ERR_FORMAT;
    cJSON *doc = cJSON_ParseWithLength(json, len);
    if (doc == NULL)
        return OD_ERR_FORMAT;
    const cJSON *worlds = cJSON_GetObjectItemCaseSensitive(doc, "worlds");
    if (!cJSON_IsArray(worlds) || cJSON_GetArraySize(worlds) != OD_WORLD_GFX_WORLDS) {
        cJSON_Delete(doc);
        return OD_ERR_FORMAT;
    }

    od_world_gfx staged[OD_WORLD_GFX_WORLDS];
    memset(staged, 0, sizeof(staged));
    int seen[OD_WORLD_GFX_WORLDS] = { 0 };
    od_status st = OD_OK;
    for (int i = 0; i < OD_WORLD_GFX_WORLDS && st == OD_OK; i++) {
        const cJSON *entry = cJSON_GetArrayItem(worlds, i);
        int world = 0;
        if (!cJSON_IsObject(entry) ||
            !value_as_int(cJSON_GetObjectItemCaseSensitive(entry, "world"), &world) ||
            world < 1 || world > OD_WORLD_GFX_WORLDS) {
            st = OD_ERR_FORMAT;
            break;
        }
        od_world_gfx *w = &staged[world - 1];
        w->world = world - 1;
        const cJSON *pal = cJSON_GetObjectItemCaseSensitive(entry, "palette_ram");
        if (!cJSON_IsArray(pal) || cJSON_GetArraySize(pal) != 32) {
            st = OD_ERR_FORMAT;
            break;
        }
        for (int k = 0; k < 32; k++) {
            int v;
            if (!value_as_int(cJSON_GetArrayItem(pal, k), &v) || v < 0 || v > 0x3F) {
                st = OD_ERR_FORMAT;
                break;
            }
            w->palette_ram[k] = (uint8_t)v;
        }
        const cJSON *cells = cJSON_GetObjectItemCaseSensitive(entry, "cells");
        if (!cJSON_IsObject(cells)) {
            st = OD_ERR_FORMAT;
            break;
        }
        for (int t = 0; t < OD_WORLD_GFX_CELLS && st == OD_OK; t++) {
            char key[4];
            snprintf(key, sizeof(key), "%02X", t);
            const cJSON *cell = cJSON_GetObjectItemCaseSensitive(cells, key);
            if (cell == NULL) {
                snprintf(key, sizeof(key), "%02x", t);
                cell = cJSON_GetObjectItemCaseSensitive(cells, key);
            }
            if (!load_cell(cell, &w->cells[t]))
                st = OD_ERR_FORMAT;
        }
        seen[world - 1] = 1;
    }
    cJSON_Delete(doc);
    if (st != OD_OK)
        return st;
    for (int i = 0; i < OD_WORLD_GFX_WORLDS; i++)
        if (!seen[i])
            return OD_ERR_FORMAT;
    memcpy(candidate->worlds, staged, sizeof staged);
    candidate->loaded = 1;
    return OD_OK;
}

od_status od_world_content_prepare(od_world_content_stage **out,
    const char *const *json, const size_t *sizes, size_t count)
{
    if (out == NULL || json == NULL || sizes == NULL || count == 0)
        return OD_ERR_FORMAT;
    od_world_content_stage *stage = calloc(1, sizeof *stage);
    if (stage == NULL) return OD_ERR_NOMEM;
    for (size_t i = 0; i < count; ++i) {
        od_status st = world_parse(stage, json[i], sizes[i]);
        if (st != OD_OK) { free(stage); return st; }
    }
    *out = stage;
    return OD_OK;
}

void od_world_content_commit(od_world_content_stage *stage)
{
    if (stage == NULL) return;
    memcpy(g_worlds, stage->worlds, sizeof g_worlds);
    g_loaded = stage->loaded;
    free(stage);
}

void od_world_content_discard(od_world_content_stage *stage) { free(stage); }

od_status od_world_gfx_load_json(const char *json, size_t len)
{
    od_world_content_stage *stage = NULL;
    od_status st = od_world_content_prepare(&stage, &json, &len, 1);
    if (st == OD_OK) od_world_content_commit(stage);
    return st;
}
