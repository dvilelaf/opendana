/* content_gfx.c - load the graphics and audio of the content package (C2.2).
 *
 * The package is a layered VFS (content.h). This module reads the graphics and
 * audio sections declared by `pack.json` and feeds them to the pure-C loaders:
 *
 *   graphics/chr/chr-NN.png   -> od_tile_chr_from_png (stb_image) -> raw CHR
 *   graphics/sprites/poses.json -> od_sprites_load_json
 *   worlds.json               -> od_world_gfx_load_json
 *   palettes/master.json      -> od_palette_master_load_json
 *   audio/sequence.json       -> rebuilt ODSEQ1 stream (data layer)
 *   screens/boss-arena.json   -> the ROUND 10-10 arena grid
 *   screens/retry.json        -> the TRY AGAIN / NEW GAME artwork
 *   screens/hearth.json       -> the menus' hearth illustration sprites
 *
 * Nothing here is compiled in: the game starts from `content/base` alone. The
 * module keeps the loaded CHR so `od_assets_load` can expose it, and re-applies
 * a later VFS (with mods) to the bound asset bundle. */
#include "opendana/content_gfx.h"
#include "opendana/cinematic.h"
#include "opendana/interactive_setup.h"
#include "content_snapshot.h"

#include "cJSON.h"
#include "opendana/assets.h"
#include "opendana/compose.h"
#include "opendana/palette.h"
#include "opendana/scene_map.h"
#include "opendana/tile.h"
#include "opendana/world_gfx.h"

#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *g_chr;
static size_t g_chr_size;
static uint8_t g_boss_arena[256];
static uint8_t g_retry_tiles[96];
static uint8_t g_retry_palette[4];
static uint8_t g_retry_cursor_cols[2];
static unsigned g_retry_cursor_tile;
static od_map_art g_map_art;
static od_hearth_art g_hearth_art;
static od_magicword_art g_magicword_art;
static od_password_art g_password_art;
static od_castle_info g_castles[OD_CASTLE_WORLDS];
static od_castle_info g_world_clears[OD_WORLD_CLEAR_CLIPS];
static od_screen_layouts g_layouts;
static od_static_layouts *g_static_layouts;
static od_static_magicword *g_static_magicword;
static void static_magicword_free(od_static_magicword *a)
{
    if (!a) return;
    for (unsigned i = 0; i < 5; ++i) free((void *)a->screen[i].records);
    free((void *)a->question[0].records);
    free(a);
}
static char *g_audio_hex;
static size_t g_audio_hex_size;
static int g_loaded;
static od_assets *g_bound;
static int g_in_default_load;
/* The layers (base + mods) of the last successful load, kept so the scene
 * clips and the level-entry fade resolve through the same package (§L9). */
static od_content_vfs g_vfs;
static int g_vfs_valid;

/* C6.2-owned candidate. Never publish a partial screen/CHR/audio load.
 * Worlds, palettes and C6.6 compose caches share its commit boundary. */
typedef struct {
    uint8_t *chr;
    size_t chr_size;
    uint8_t boss_arena[256], retry_tiles[96], retry_palette[4], retry_cursor_cols[2];
    unsigned retry_cursor_tile;
    od_map_art map_art;
    od_hearth_art hearth_art;
    od_magicword_art magicword_art;
    od_password_art password_art;
    od_castle_info castles[OD_CASTLE_WORLDS], world_clears[OD_WORLD_CLEAR_CLIPS];
    od_screen_layouts layouts;
    od_static_layouts *static_layouts;
    od_static_magicword *static_magicword;
    char *audio_hex;
    size_t audio_hex_size;
    od_world_content_stage *worlds;
    od_palette_content_stage *palettes;
    od_compose_content_stage *compose;
    od_interactive_setup_stage *interactive;
    od_transfer_structure_stage *transfer;
} gfx_candidate;

static void candidate_free(gfx_candidate *c)
{
    if (c == NULL) return;
    free(c->chr);
    free(c->audio_hex);
    free(c->static_layouts);
    static_magicword_free(c->static_magicword);
    od_world_content_discard(c->worlds);
    od_palette_content_discard(c->palettes);
    od_compose_content_discard(c->compose);
    od_interactive_setup_discard(c->interactive);
    od_transfer_structure_discard(c->transfer);
    free(c);
}

static void candidate_commit(gfx_candidate *c)
{
    od_world_content_commit(c->worlds); c->worlds = NULL;
    od_palette_content_commit(c->palettes); c->palettes = NULL;
    od_compose_content_commit(c->compose); c->compose = NULL;
    if (c->interactive) od_interactive_setup_commit(c->interactive);
    else od_interactive_setup_reset();
    c->interactive = NULL;
    if (c->transfer) od_transfer_structure_commit(c->transfer);
    else od_transfer_structure_reset();
    c->transfer = NULL;
    free(g_chr); g_chr = c->chr; c->chr = NULL; g_chr_size = c->chr_size;
    free(g_audio_hex); g_audio_hex = c->audio_hex; c->audio_hex = NULL;
    g_audio_hex_size = c->audio_hex_size;
    memcpy(g_boss_arena, c->boss_arena, sizeof g_boss_arena);
    memcpy(g_retry_tiles, c->retry_tiles, sizeof g_retry_tiles);
    memcpy(g_retry_palette, c->retry_palette, sizeof g_retry_palette);
    memcpy(g_retry_cursor_cols, c->retry_cursor_cols, sizeof g_retry_cursor_cols);
    g_retry_cursor_tile = c->retry_cursor_tile;
    g_map_art = c->map_art; g_hearth_art = c->hearth_art;
    g_magicword_art = c->magicword_art; g_password_art = c->password_art;
    memcpy(g_castles, c->castles, sizeof g_castles);
    memcpy(g_world_clears, c->world_clears, sizeof g_world_clears);
    g_layouts = c->layouts;
    free(g_static_layouts); g_static_layouts = c->static_layouts; c->static_layouts = NULL;
    static_magicword_free(g_static_magicword);
    g_static_magicword = c->static_magicword; c->static_magicword = NULL;
}

const od_static_layouts *od_gfx_static_layouts(void) { return g_static_layouts; }
const od_static_magicword *od_gfx_static_magicword(void) { return g_static_magicword; }

const uint8_t *od_gfx_chr(void) { return g_chr; }
size_t od_gfx_chr_size(void) { return g_chr_size; }
unsigned od_gfx_boss_arena_count(void) { return (unsigned)sizeof(g_boss_arena); }
const uint8_t *od_gfx_boss_arena(void) { return g_boss_arena; }
const uint8_t *od_gfx_retry_tiles(void) { return g_retry_tiles; }
const uint8_t *od_gfx_retry_palette(void) { return g_retry_palette; }
const uint8_t *od_gfx_retry_cursor_cols(void) { return g_retry_cursor_cols; }
unsigned od_gfx_retry_cursor_tile(void) { return g_retry_cursor_tile; }
const od_map_art *od_gfx_map_art(void) { return g_map_art.loaded ? &g_map_art : NULL; }
const od_hearth_art *od_gfx_hearth_art(void)
{
    return g_hearth_art.loaded ? &g_hearth_art : NULL;
}
const od_magicword_art *od_gfx_magicword_art(void)
{
    return g_magicword_art.loaded ? &g_magicword_art : NULL;
}
const od_screen_layouts *od_gfx_layouts(void)
{
    return g_layouts.loaded ? &g_layouts : NULL;
}
const od_password_art *od_gfx_password_art(void)
{
    return g_password_art.loaded ? &g_password_art : NULL;
}
const od_castle_info *od_gfx_castle(int world_index)
{
    if (world_index < 0 || world_index >= OD_CASTLE_WORLDS)
        return NULL;
    return g_castles[world_index].loaded ? &g_castles[world_index] : NULL;
}
const od_castle_info *od_gfx_world_clear(int clip_index)
{
    if (clip_index < 0 || clip_index >= OD_WORLD_CLEAR_CLIPS)
        return NULL;
    return g_world_clears[clip_index].loaded ? &g_world_clears[clip_index] : NULL;
}
const char *od_gfx_audio_hex(size_t *len)
{
    if (len)
        *len = g_audio_hex_size;
    return g_audio_hex;
}
int od_gfx_loaded(void) { return g_loaded; }

void od_gfx_bind_assets(od_assets *assets) { g_bound = assets; }

/* ------------------------------------------------------------------------- */
/* JSON helpers                                                               */
/* ------------------------------------------------------------------------- */

static const cJSON *jget(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

/* Read a single string-array field of `doc` into a NUL-joined list. Returns the
 * count, or -1 on error. */static int read_path_list(const cJSON *doc, const char *key, char out[][256], int cap)
{
    const cJSON *arr = jget(doc, key);
    if (arr == NULL)
        return 0;
    if (!cJSON_IsArray(arr))
        return -1;
    int n = cJSON_GetArraySize(arr);
    if (n > cap)
        return -1;
    for (int i = 0; i < n; i++) {
        const cJSON *v = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= 256)
            return -1;
        snprintf(out[i], 256, "%s", v->valuestring);
    }
    return n;
}

static od_status read_vfs_text(const od_content_vfs *vfs, const char *rel,
                                char **out, size_t *size)
{
    return od_content_vfs_read(vfs, rel, out, size);
}

/* ------------------------------------------------------------------------- */
/* Load                                                                       */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* CHR patches (C6.29)                                                        */
/* ------------------------------------------------------------------------- */

/* Internal, implemented in content.c. */
cJSON *od_content_mod_decl(const od_content_layer *layer);
void od_content_set_error(const char *msg);

static od_status patch_error(const char *mod, const char *png, const char *why)
{
    char msg[512];
    snprintf(msg, sizeof msg, "mod '%s': tile patch '%.200s': %s", mod, png, why);
    od_content_set_error(msg);
    return OD_ERR_FORMAT;
}

/* A mod's "graphics.chr_patches": [{"png": "graphics/...png", "tile": N}].
 * The PNG holds whole 8x8 tiles, row by row, written over the package CHR from
 * tile N on. Every pixel is one of the four greys 0/85/170/255 (the same
 * mapping as the package sheets) or fully transparent (alpha 0: keep the
 * package pixel). Anything else, a size that is not whole tiles or tiles past
 * the end of the CHR, is an error naming the mod. A mod changes the sprites
 * with its own pixels, without shipping any sheet of the original. */
static od_status apply_chr_patches(const od_content_vfs *vfs, uint8_t *chr)
{
    for (int li = 1; li < vfs->count; li++) {
        const od_content_layer *layer = &vfs->layers[li];
        cJSON *decl = od_content_mod_decl(layer);
        if (decl == NULL)
            return OD_ERR_FORMAT;
        const cJSON *patches = jget(jget(decl, "graphics"), "chr_patches");
        const cJSON *patch;
        od_status st = OD_OK;
        cJSON_ArrayForEach(patch, patches) {
            const cJSON *png = jget(patch, "png"), *tile = jget(patch, "tile");
            if (!cJSON_IsString(png) || !cJSON_IsNumber(tile)) {
                st = patch_error(layer->id, "?", "needs \"png\" and \"tile\"");
                break;
            }
            int first = tile->valueint;
            if (tile->valuedouble != (double)first || first < 0 ||
                first >= (int)(OD_CHR_BYTES / OD_TILE_BYTES)) {
                st = patch_error(layer->id, png->valuestring, "\"tile\" must be 0..8191");
                break;
            }
            char *data = NULL;
            size_t size = 0;
            st = read_vfs_text(vfs, png->valuestring, &data, &size);
            if (st != OD_OK) {
                patch_error(layer->id, png->valuestring, "cannot read it");
                break;
            }
            int w = 0, h = 0, channels = 0;
            stbi_uc *img = stbi_load_from_memory((const stbi_uc *)data, (int)size,
                                                 &w, &h, &channels, 4);
            free(data);
            if (img == NULL) {
                st = patch_error(layer->id, png->valuestring, "not a PNG image");
                break;
            }
            int tiles_x = w / 8, tiles = (w / 8) * (h / 8);
            if (w <= 0 || h <= 0 || w % 8 != 0 || h % 8 != 0) {
                stbi_image_free(img);
                st = patch_error(layer->id, png->valuestring,
                                 "its size is not a whole number of 8x8 tiles");
                break;
            }
            if ((size_t)first + (size_t)tiles > OD_CHR_BYTES / OD_TILE_BYTES) {
                stbi_image_free(img);
                st = patch_error(layer->id, png->valuestring,
                                 "its tiles run past the end of the tile image (tile 8191)");
                break;
            }
            for (int t = 0; t < tiles && st == OD_OK; t++) {
                uint8_t *dst = chr + (size_t)(first + t) * OD_TILE_BYTES;
                int tx = (t % tiles_x) * 8, ty = (t / tiles_x) * 8;
                for (int y = 0; y < 8 && st == OD_OK; y++) {
                    for (int x = 0; x < 8; x++) {
                        const stbi_uc *px = img + ((size_t)(ty + y) * (size_t)w + (size_t)(tx + x)) * 4;
                        if (px[3] == 0)
                            continue;   /* transparent: keep the package pixel */
                        int v = px[0] == 0 ? 0 : px[0] == 85 ? 1 : px[0] == 170 ? 2 :
                                px[0] == 255 ? 3 : -1;
                        if (px[3] != 255 || px[1] != px[0] || px[2] != px[0] || v < 0) {
                            st = patch_error(layer->id, png->valuestring,
                                             "every pixel must be grey 0, 85, 170 or 255, "
                                             "or fully transparent");
                            break;
                        }
                        uint8_t bit = (uint8_t)(0x80u >> x);
                        dst[y] = (uint8_t)((v & 1) ? (dst[y] | bit) : (dst[y] & ~bit));
                        dst[8 + y] = (uint8_t)((v & 2) ? (dst[8 + y] | bit) : (dst[8 + y] & ~bit));
                    }
                }
            }
            stbi_image_free(img);
            if (st != OD_OK)
                break;
        }
        cJSON_Delete(decl);
        if (st != OD_OK)
            return st;
    }
    return OD_OK;
}

static od_status load_chr(const od_content_vfs *vfs, const cJSON *graphics, gfx_candidate *c)
{
    char paths[16][256];
    int n = read_path_list(graphics, "chr", paths, 16);
    if (n <= 0)
        return OD_ERR_FORMAT;
    c->chr = (uint8_t *)malloc(OD_CHR_BYTES);
    if (c->chr == NULL)
        return OD_ERR_NOMEM;
    memset(c->chr, 0, OD_CHR_BYTES);
    size_t tiles_done = 0;
    for (int i = 0; i < n; i++) {
        char *data = NULL;
        size_t size = 0;
        od_status st = read_vfs_text(vfs, paths[i], &data, &size);
        if (st != OD_OK)
            return st;
        if (tiles_done * OD_TILE_BYTES >= OD_CHR_BYTES) {
            free(data);
            return OD_ERR_FORMAT;
        }
        int tiles = od_tile_chr_from_png(data, (int)size,
                                          c->chr + tiles_done * OD_TILE_BYTES,
                                          OD_CHR_BYTES - tiles_done * OD_TILE_BYTES);
        free(data);
        if (tiles < 0)
            return OD_ERR_FORMAT;
        tiles_done += (size_t)tiles;
    }
    if (tiles_done * OD_TILE_BYTES != OD_CHR_BYTES)
        return OD_ERR_FORMAT;
    c->chr_size = OD_CHR_BYTES;
    /* C6.29: the mods' own tile patches go over the package sheets. */
    return apply_chr_patches(vfs, c->chr);
}

static od_status prepare_compose(const od_content_vfs *vfs, const cJSON *graphics,
    gfx_candidate *candidate)
{
    char paths[4][256];
    char *buffers[4] = {0}, *arena = NULL;
    const char *texts[4] = {0};
    size_t sizes[4] = {0}, arena_size = 0;
    int n = read_path_list(graphics, "sprites", paths, 4);
    if (n <= 0) return OD_ERR_FORMAT;
    od_status st = OD_OK;
    for (int i = 0; i < n; i++) {
        st = read_vfs_text(vfs, paths[i], &buffers[i], &sizes[i]);
        texts[i] = buffers[i];
        if (st != OD_OK) break;
    }
    if (st == OD_OK)
        st = read_vfs_text(vfs, "screens/boss-background.json", &arena, &arena_size);
    if (st == OD_OK)
        st = od_compose_content_prepare(&candidate->compose, texts, sizes, (size_t)n,
            arena, arena_size);
    for (int i = 0; i < n; ++i) free(buffers[i]);
    free(arena);
    return st;
}

/* Read the whole ordered family before preparing it. Nothing is published by
 * either API, including when an earlier document is invalid and a later one
 * is valid. The stage owns its data after these input buffers are freed. */
static od_status prepare_graphic_family(const od_content_vfs *vfs,
    const cJSON *pack, const char *key, gfx_candidate *candidate, int worlds)
{
    char paths[4][256];
    const char *texts[4] = {0};
    char *buffers[4] = {0};
    size_t sizes[4] = {0};
    int n = read_path_list(pack, key, paths, 4);
    if (n <= 0) return OD_ERR_FORMAT;
    od_status st = OD_OK;
    for (int i = 0; i < n; ++i) {
        char *text = NULL;
        st = read_vfs_text(vfs, paths[i], &text, &sizes[i]);
        texts[i] = text;
        buffers[i] = text;
        if (st != OD_OK) break;
    }
    if (st == OD_OK) {
        if (worlds) st = od_world_content_prepare(&candidate->worlds, texts, sizes, (size_t)n);
        else st = od_palette_content_prepare(&candidate->palettes, texts, sizes, (size_t)n);
    }
    for (int i = 0; i < n; ++i) free(buffers[i]);
    return st;
}

/* screens/map.json: the WORLD map sprite tables and the castle overlay,
 * produced by the importer. */
static int json_byte(const cJSON *v, uint8_t *out)
{
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 255)
        return 0;
    *out = (uint8_t)(int)v->valuedouble;
    return 1;
}

static int json_word(const cJSON *v, uint16_t *out)
{
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 8191)
        return 0;
    *out = (uint16_t)(int)v->valuedouble;
    return 1;
}

static int json_sprites(const cJSON *arr, od_map_sprite *out, uint8_t *count)
{
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : -1;
    if (n < 0 || n > OD_MAP_LIST_MAX)
        return 0;
    for (int i = 0; i < n; i++) {
        const cJSON *s = cJSON_GetArrayItem(arr, i);
        uint8_t b[4];
        if (!cJSON_IsArray(s) || cJSON_GetArraySize(s) != 4)
            return 0;
        for (int k = 0; k < 4; k++)
            if (!json_byte(cJSON_GetArrayItem(s, k), &b[k]))
                return 0;
        out[i].dy = b[0]; out[i].tile = b[1]; out[i].attr = b[2]; out[i].dx = b[3];
    }
    *count = (uint8_t)n;
    return 1;
}

static int load_map_art(const cJSON *doc, od_map_art *a)
{
    memset(a, 0, sizeof *a);
    const cJSON *worlds = jget(doc, "worlds"), *panel = jget(doc, "panel");
    const cJSON *poses = jget(doc, "hero_poses"), *frame = jget(doc, "frame");
    const cJSON *digits = jget(doc, "digits"), *objects = jget(doc, "objects");
    const cJSON *castle = jget(doc, "castle_open"), *overlay = jget(doc, "castle_overlay");
    if (!cJSON_IsArray(worlds) || cJSON_GetArraySize(worlds) != OD_MAP_WORLD_COUNT ||
        !panel || !cJSON_IsArray(poses) || cJSON_GetArraySize(poses) != 4 || !frame ||
        !digits || !cJSON_IsArray(objects) ||
        cJSON_GetArraySize(objects) != OD_MAP_OBJECTS || !castle ||
        !cJSON_IsArray(overlay) || cJSON_GetArraySize(overlay) > OD_MAP_OVERLAY_MAX)
        return 0;
    for (int w = 0; w < OD_MAP_WORLD_COUNT; w++) {
        const cJSON *e = cJSON_GetArrayItem(worlds, w);
        if (!json_byte(jget(e, "x"), &a->world_x[w]) ||
            !json_byte(jget(e, "y"), &a->world_y[w]))
            return 0;
    }
    const cJSON *px = jget(panel, "x");
    if (!cJSON_IsArray(px) || cJSON_GetArraySize(px) != OD_MAP_ROUND_COUNT ||
        !json_byte(jget(panel, "y"), &a->panel_y))
        return 0;
    for (int r = 0; r < OD_MAP_ROUND_COUNT; r++)
        if (!json_byte(cJSON_GetArrayItem(px, r), &a->panel_x[r]))
            return 0;
    for (int p = 0; p < 4; p++) {
        const cJSON *pose = cJSON_GetArrayItem(poses, p);
        if (!cJSON_IsArray(pose) || cJSON_GetArraySize(pose) != 8)
            return 0;
        for (int i = 0; i < 8; i++) {
            const cJSON *pair = cJSON_GetArrayItem(pose, i);
            if (!cJSON_IsArray(pair) || cJSON_GetArraySize(pair) != 2 ||
                !json_byte(cJSON_GetArrayItem(pair, 0), &a->poses[p][i][0]) ||
                !json_byte(cJSON_GetArrayItem(pair, 1), &a->poses[p][i][1]))
                return 0;
        }
    }
    if (!json_byte(jget(frame, "tile"), &a->frame_tile) ||
        !json_byte(jget(frame, "attr"), &a->frame_attr) ||
        !json_byte(jget(digits, "y"), &a->digit_y) ||
        !json_byte(jget(digits, "x"), &a->digit_x))
        return 0;
    for (int k = 0; k < OD_MAP_OBJECTS; k++) {
        const cJSON *o = cJSON_GetArrayItem(objects, k);
        const cJSON *frames = jget(o, "frames");
        od_map_object *ob = &a->objects[k];
        int n = cJSON_IsArray(frames) ? cJSON_GetArraySize(frames) : -1;
        if (n <= 0 || n > OD_MAP_OBJECT_FRAMES ||
            !json_byte(jget(o, "y"), &ob->y) || !json_byte(jget(o, "x"), &ob->x))
            return 0;
        for (int f = 0; f < n; f++) {
            const cJSON *fr = cJSON_GetArrayItem(frames, f);
            if (!json_byte(jget(fr, "duration"), &ob->frames[f].duration) ||
                !ob->frames[f].duration ||
                !json_sprites(jget(fr, "sprites"), ob->frames[f].sprites,
                              &ob->frames[f].count))
                return 0;
        }
        ob->frame_count = (uint8_t)n;
    }
    if (!json_byte(jget(castle, "y"), &a->castle_y) ||
        !json_byte(jget(castle, "x"), &a->castle_x) ||
        !json_sprites(jget(castle, "sprites"), a->castle, &a->castle_count))
        return 0;
    const cJSON *fpal = jget(doc, "frame_palette");
    const cJSON *fcol = jget(fpal, "colors");
    if (!json_byte(jget(fpal, "entry"), &a->frame_pal_entry) || a->frame_pal_entry > 31 ||
        !cJSON_IsArray(fcol) || cJSON_GetArraySize(fcol) != 3)
        return 0;
    for (int k = 0; k < 3; k++)
        if (!json_byte(cJSON_GetArrayItem(fcol, k), &a->frame_pal_colors[k]))
            return 0;
    const cJSON *banks = jget(doc, "castle_banks");
    if (!cJSON_IsArray(banks) || cJSON_GetArraySize(banks) != 4)
        return 0;
    for (int k = 0; k < 4; k++)
        if (!json_byte(cJSON_GetArrayItem(banks, k), &a->castle_banks[k]))
            return 0;
    int n = cJSON_GetArraySize(overlay);
    for (int i = 0; i < n; i++) {
        const cJSON *cell = cJSON_GetArrayItem(overlay, i);
        const cJSON *idx = cJSON_GetArrayItem(cell, 0);
        if (!cJSON_IsArray(cell) || cJSON_GetArraySize(cell) != 2 ||
            !cJSON_IsNumber(idx) || idx->valuedouble < 0 || idx->valuedouble > 2047 ||
            !json_byte(cJSON_GetArrayItem(cell, 1), &a->overlay_value[i]))
            return 0;
        a->overlay_index[i] = (uint16_t)(int)idx->valuedouble;
    }
    a->overlay_count = (unsigned)n;
    /* C6.18: the map's own palette and PPU setup (v1 layouts.json:map). A v2
     * package without it keeps the legacy inheritance (static_loaded = 0). */
    const cJSON *st = jget(doc, "static");
    if (st) {
        const cJSON *pal = jget(st, "palette");
        const cJSON *table = jget(st, "band_table");
        const cJSON *split = jget(st, "split_banks");
        const cJSON *sprite = jget(st, "sprite_banks");
        if (!cJSON_IsArray(pal) || cJSON_GetArraySize(pal) != 32 ||
            !cJSON_IsArray(table) || cJSON_GetArraySize(table) != 12 ||
            !cJSON_IsArray(split) || cJSON_GetArraySize(split) != 4 ||
            !cJSON_IsArray(sprite) || cJSON_GetArraySize(sprite) != 4 ||
            !json_byte(jget(st, "mask"), &a->mask) ||
            !json_byte(jget(st, "bands"), &a->bands) || a->bands > 5)
            return 0;
        for (int i = 0; i < 32; i++)
            if (!json_byte(cJSON_GetArrayItem(pal, i), &a->palette[i]))
                return 0;
        for (int i = 0; i < 12; i++)
            if (!json_word(cJSON_GetArrayItem(table, i), &a->band_table[i]))
                return 0;
        for (int i = 0; i < 4; i++)
            if (!json_word(cJSON_GetArrayItem(split, i), &a->split_banks[i]) ||
                !json_word(cJSON_GetArrayItem(sprite, i), &a->sprite_banks[i]))
                return 0;
        a->static_loaded = 1;
    }
    a->loaded = 1;
    return 1;
}

/* screens/hearth.json: the menus' hearth illustration, produced by the importer. */
static int load_hearth_groups(const cJSON *arr, od_hearth_group *out, uint8_t *count)
{
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : -1;
    if (n <= 0 || n > OD_HEARTH_GROUPS)
        return 0;
    for (int g = 0; g < n; g++) {
        const cJSON *e = cJSON_GetArrayItem(arr, g);
        if (!json_byte(jget(e, "y"), &out[g].y) || !json_byte(jget(e, "x"), &out[g].x) ||
            !json_sprites(jget(e, "sprites"), out[g].sprites, &out[g].count))
            return 0;
    }
    *count = (uint8_t)n;
    return 1;
}

/* One layout: [[count|flags, PPU address, [bytes]], ...]. */
static int load_menu_layout(const cJSON *arr, od_menu_layout *l)
{
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : -1;
    if (n <= 0 || n > OD_MENU_LAYOUT_RECORDS)
        return 0;
    for (int i = 0; i < n; i++) {
        const cJSON *r = cJSON_GetArrayItem(arr, i);
        const cJSON *addr = cJSON_GetArrayItem(r, 1), *data = cJSON_GetArrayItem(r, 2);
        od_menu_record *out = &l->records[i];
        if (!cJSON_IsArray(r) || cJSON_GetArraySize(r) != 3 ||
            !json_byte(cJSON_GetArrayItem(r, 0), &out->count) ||
            !cJSON_IsNumber(addr) || addr->valuedouble < 0 || addr->valuedouble > 0xFFFF ||
            !cJSON_IsArray(data) || cJSON_GetArraySize(data) != (out->count & 0x3F) ||
            (out->count & 0x3F) == 0)
            return 0;
        out->addr = (uint16_t)addr->valuedouble;
        for (int k = 0; k < (out->count & 0x3F); k++)
            if (!json_byte(cJSON_GetArrayItem(data, k), &out->data[k]))
                return 0;
    }
    l->record_count = (uint8_t)n;
    return 1;
}

static int load_byte_rows(const cJSON *rows, uint8_t *out, int n_rows, int n_cols)
{
    if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) != n_rows)
        return 0;
    for (int r = 0; r < n_rows; r++) {
        const cJSON *row = cJSON_GetArrayItem(rows, r);
        if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != n_cols)
            return 0;
        for (int c = 0; c < n_cols; c++)
            if (!json_byte(cJSON_GetArrayItem(row, c), &out[r * n_cols + c]))
                return 0;
    }
    return 1;
}

/* hearth.json "screen" (PISTAS-B): palette, illustration, attributes and the
 * menu's PPU words. */
static int load_hearth_screen(const cJSON *sc, od_hearth_art *a)
{
    const cJSON *pal = jget(sc, "palette"), *il = jget(sc, "illustration");
    const cJSON *at = jget(sc, "attributes"), *ppu = jget(sc, "ppu");
    if (!cJSON_IsArray(pal) || cJSON_GetArraySize(pal) != 32)
        return 0;
    for (int i = 0; i < 32; i++)
        if (!json_byte(cJSON_GetArrayItem(pal, i), &a->palette[i]) || a->palette[i] > 63)
            return 0;
    if (!json_byte(jget(il, "x"), &a->illus_x) || a->illus_x > 16 ||
        !json_byte(jget(il, "y"), &a->illus_y) || a->illus_y > 14 ||
        !load_byte_rows(jget(il, "rows"), &a->illus[0][0], 16, 16) ||
        !json_byte(jget(at, "fill"), &a->attr_fill) ||
        !json_byte(jget(at, "x"), &a->attr_x) || a->attr_x > 4 ||
        !json_byte(jget(at, "y"), &a->attr_y) || a->attr_y > 4 ||
        !load_byte_rows(jget(at, "rows"), &a->attr[0][0], 4, 4))
        return 0;
    int n = cJSON_IsArray(ppu) ? cJSON_GetArraySize(ppu) : -1;
    if (n < 0 || n > 24)
        return 0;
    for (int i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(ppu, i);
        const cJSON *f = cJSON_GetArrayItem(e, 0), *v = cJSON_GetArrayItem(e, 1);
        if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) != 2 || !cJSON_IsNumber(f) ||
            !cJSON_IsNumber(v) || f->valuedouble < 2080 || f->valuedouble > 2381 ||
            v->valuedouble < 0 || v->valuedouble > 8191)
            return 0;
        a->ppu_field[i] = (uint16_t)f->valuedouble;
        a->ppu_value[i] = (uint16_t)v->valuedouble;
    }
    a->ppu_count = (uint8_t)n;
    a->has_screen = 1;
    return 1;
}

/* One record: [count|flags, PPU address, [bytes]]. */
static int load_menu_record(const cJSON *r, od_menu_record *out)
{
    const cJSON *addr = cJSON_GetArrayItem(r, 1), *data = cJSON_GetArrayItem(r, 2);
    if (!cJSON_IsArray(r) || cJSON_GetArraySize(r) != 3 ||
        !json_byte(cJSON_GetArrayItem(r, 0), &out->count) || (out->count & 0x3F) == 0 ||
        !cJSON_IsNumber(addr) || addr->valuedouble < 0x2000 || addr->valuedouble > 0x23BF ||
        !cJSON_IsArray(data) || cJSON_GetArraySize(data) != (out->count & 0x3F))
        return 0;
    out->addr = (uint16_t)addr->valuedouble;
    for (int k = 0; k < (out->count & 0x3F); k++)
        if (!json_byte(cJSON_GetArrayItem(data, k), &out->data[k]))
            return 0;
    return 1;
}

/* screens/password.json (PISTAS-B): the magicword box's message records. */
static int load_password_art(const cJSON *doc, od_password_art *a)
{
    memset(a, 0, sizeof *a);
    if (!load_menu_record(jget(doc, "prompt"), &a->prompt) ||
        !load_menu_record(jget(doc, "error"), &a->error))
        return 0;
    a->loaded = 1;
    return 1;
}

static int load_hearth_art(const cJSON *doc, od_hearth_art *a)
{
    memset(a, 0, sizeof *a);
    const cJSON *fire = jget(doc, "fire"), *frames = jget(fire, "frames");
    const cJSON *bank = jget(doc, "bank"), *values = jget(bank, "values");
    int n = cJSON_IsArray(frames) ? cJSON_GetArraySize(frames) : -1;
    if (!load_hearth_groups(jget(doc, "groups"), a->groups[0], &a->group_count[0]) ||
        !load_hearth_groups(jget(doc, "groups_confirmed"), a->groups[1], &a->group_count[1]) ||
        n <= 0 || n > OD_MAP_OBJECT_FRAMES ||
        !json_byte(jget(fire, "y"), &a->fire.y) || !json_byte(jget(fire, "x"), &a->fire.x) ||
        !json_byte(jget(bank, "slot"), &a->bank_slot) || a->bank_slot > 3 ||
        !json_byte(jget(bank, "period"), &a->bank_period) || !a->bank_period ||
        !cJSON_IsArray(values) || cJSON_GetArraySize(values) != 2 ||
        !json_byte(cJSON_GetArrayItem(values, 0), &a->bank_values[0]) ||
        !json_byte(cJSON_GetArrayItem(values, 1), &a->bank_values[1]))
        return 0;
    for (int f = 0; f < n; f++) {
        const cJSON *fr = cJSON_GetArrayItem(frames, f);
        if (!json_byte(jget(fr, "duration"), &a->fire.frames[f].duration) ||
            !a->fire.frames[f].duration ||
            !json_sprites(jget(fr, "sprites"), a->fire.frames[f].sprites,
                          &a->fire.frames[f].count))
            return 0;
    }
    a->fire.frame_count = (uint8_t)n;
    /* WHATNEXT-FADE: the panel layouts and the queue limit are optional (a
     * mod's older hearth.json keeps the settled panel). */
    const cJSON *layouts = jget(doc, "layouts");
    if (layouts) {
        if (!json_byte(jget(doc, "queue_capacity"), &a->queue_capacity) ||
            !a->queue_capacity ||
            !load_menu_layout(jget(layouts, "begin"), &a->layouts[OD_MENU_LAYOUT_BEGIN]) ||
            !load_menu_layout(jget(layouts, "whatnext"), &a->layouts[OD_MENU_LAYOUT_WHATNEXT]))
            return 0;
    }
    /* PISTAS-B: the menu screen under the panel (optional for an older mod;
     * without it "What next?" shows only its panel). */
    const cJSON *screen = jget(doc, "screen");
    if (screen && !load_hearth_screen(screen, a))
        return 0;
    a->loaded = 1;
    return 1;
}

/* screens/magicword.json: "Today's magicword", produced by the importer. */
static int json_bytes(const cJSON *arr, uint8_t *out, int n)
{
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != n)
        return 0;
    for (int i = 0; i < n; i++)
        if (!json_byte(cJSON_GetArrayItem(arr, i), &out[i]))
            return 0;
    return 1;
}

static int json_u16(const cJSON *v, uint16_t *out)
{
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 65535)
        return 0;
    *out = (uint16_t)(int)v->valuedouble;
    return 1;
}

/* A program: [{"op": "records", "records": [[count, addr, [data]]...]},
 * {"op": "yield"}, {"op": "word"}]. */
static int load_mw_program(const cJSON *arr, od_magicword_art *a, od_mw_step *steps,
                           unsigned *count)
{
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : -1;
    if (n <= 0 || n > OD_MW_STEPS)
        return 0;
    for (int i = 0; i < n; i++) {
        const cJSON *st = cJSON_GetArrayItem(arr, i), *op = jget(st, "op");
        if (!cJSON_IsString(op))
            return 0;
        od_mw_step *s = &steps[i];
        memset(s, 0, sizeof *s);
        if (strcmp(op->valuestring, "yield") == 0) {
            s->op = OD_MW_OP_YIELD;
        } else if (strcmp(op->valuestring, "word") == 0) {
            s->op = OD_MW_OP_WORD;
        } else if (strcmp(op->valuestring, "records") == 0) {
            const cJSON *recs = jget(st, "records");
            int r = cJSON_IsArray(recs) ? cJSON_GetArraySize(recs) : -1;
            if (r <= 0 || a->record_count + (unsigned)r > OD_MW_RECORDS)
                return 0;
            s->op = OD_MW_OP_RECORDS;
            s->first = (uint8_t)a->record_count;
            s->count = (uint8_t)r;
            for (int k = 0; k < r; k++) {
                const cJSON *rec = cJSON_GetArrayItem(recs, k);
                od_mw_record *o = &a->records[a->record_count++];
                const cJSON *data = cJSON_GetArrayItem(rec, 2);
                if (!cJSON_IsArray(rec) || cJSON_GetArraySize(rec) != 3 ||
                    !json_byte(cJSON_GetArrayItem(rec, 0), &o->count) ||
                    !json_u16(cJSON_GetArrayItem(rec, 1), &o->addr) ||
                    (o->count & 0x3F) == 0 ||
                    !json_bytes(data, o->data, o->count & 0x3F))
                    return 0;
            }
        } else {
            return 0;
        }
    }
    *count = (unsigned)n;
    return 1;
}

static int load_magicword_art(const cJSON *doc, od_magicword_art *a)
{
    memset(a, 0, sizeof *a);
    const cJSON *song = jget(doc, "song"), *word = jget(doc, "word");
    const cJSON *cur = jget(doc, "cursor"), *cyc = jget(doc, "cycle");
    const cJSON *night = jget(doc, "night"), *ncyc = jget(night, "cycle");
    const cJSON *tabs = jget(cyc, "bg_tables"), *cues = jget(night, "cues");
    const cJSON *dur = jget(night, "duration"), *ptick = jget(night, "palette_tick");
    if (!json_bytes(jget(doc, "palette"), a->palette, 32) ||
        !json_bytes(jget(doc, "bg_banks"), a->bg_banks, 4) ||
        !json_bytes(jget(doc, "sprite_banks"), a->sprite_banks, 2) ||
        !json_byte(jget(doc, "mask"), &a->mask) ||
        !json_byte(jget(doc, "queue_capacity"), &a->queue_capacity) || !a->queue_capacity ||
        !json_byte(jget(doc, "program_start"), &a->program_start) ||
        !json_byte(jget(doc, "palette_age"), &a->palette_age) ||
        !json_byte(jget(song, "id"), &a->song_id) ||
        !json_byte(jget(song, "age"), &a->song_age) ||
        !load_mw_program(jget(doc, "screen"), a, a->screen, &a->screen_steps) ||
        !load_mw_program(jget(doc, "question"), a, a->question, &a->question_steps) ||
        !json_u16(jget(word, "address"), &a->word_addr) ||
        !json_byte(jget(word, "count"), &a->word_count) ||
        !json_byte(jget(word, "group"), &a->word_group) || !a->word_group ||
        !json_byte(jget(word, "gap_tile"), &a->word_gap) ||
        !json_bytes(jget(word, "glyphs"), a->glyphs, 32) ||
        !json_byte(jget(cur, "y"), &a->cursor_y) ||
        !json_byte(jget(cur, "tile"), &a->cursor_tile) ||
        !json_byte(jget(cur, "attr"), &a->cursor_attr) ||
        !json_bytes(jget(cur, "x"), a->cursor_x, 2) ||
        !json_byte(jget(cyc, "bg_mask"), &a->bg_mask) ||
        !json_bytes(jget(cyc, "bg_entries"), a->bg_entries, 3) ||
        !cJSON_IsArray(tabs) || cJSON_GetArraySize(tabs) != 3 ||
        !json_byte(jget(cyc, "sprite_mask"), &a->sprite_mask) ||
        !json_byte(jget(cyc, "sprite_entry"), &a->sprite_entry) ||
        !json_bytes(jget(cyc, "sprite_table"), a->sprite_table, 5) ||
        !cJSON_IsNumber(dur) || dur->valuedouble < 1 || dur->valuedouble > 65535 ||
        !cJSON_IsNumber(ptick) || ptick->valuedouble < 0 || ptick->valuedouble > 65535 ||
        !json_bytes(jget(night, "palette"), a->night_palette, 32) ||
        !json_byte(jget(ncyc, "mask"), &a->night_mask) ||
        !json_byte(jget(ncyc, "entry"), &a->night_entry) ||
        !json_bytes(jget(ncyc, "table"), a->night_table, 4) ||
        !cJSON_IsArray(cues) || cJSON_GetArraySize(cues) > OD_MW_CUES)
        return 0;
    if (a->sprite_entry >= 32 || a->night_entry >= 32)
        return 0;
    for (int k = 0; k < 3; k++)
        if (a->bg_entries[k] >= 32 || !json_bytes(cJSON_GetArrayItem(tabs, k), a->bg_tables[k], 3))
            return 0;
    for (int k = 0; k < cJSON_GetArraySize(cues); k++) {
        const cJSON *c = cJSON_GetArrayItem(cues, k);
        if (!cJSON_IsArray(c) || cJSON_GetArraySize(c) != 2 ||
            !json_u16(cJSON_GetArrayItem(c, 0), &a->night_cues[k].tick) ||
            !json_byte(cJSON_GetArrayItem(c, 1), &a->night_cues[k].id))
            return 0;
    }
    a->night_cue_count = (unsigned)cJSON_GetArraySize(cues);
    a->night_duration = (unsigned)dur->valuedouble;
    a->night_palette_tick = (unsigned)ptick->valuedouble;
    a->loaded = 1;
    return 1;
}

/* screens/castles.json: {"worlds": [{"world": 1..9, "duration": N,
 * "skip_from": T, "frame_tick_lost": [t, ...], "cues": [[tick, id], ...]}, ...]}
 * (CASTILLOS). */
/* One recorded cinematic's timing: "duration", "skip_from" (absent = START is
 * not read: skip_from = duration), "frame_tick_lost" and "cues" (castles.json and
 * world-clears.json share the shape). */
static int parse_clip_info(const cJSON *w, int skip_required, od_castle_info *c)
{
    const cJSON *dur = jget(w, "duration");
    const cJSON *skip = jget(w, "skip_from"), *cues = jget(w, "cues");
    if (!cJSON_IsNumber(dur) || !cJSON_IsArray(cues) ||
        (skip_required && !cJSON_IsNumber(skip)) || (skip && !cJSON_IsNumber(skip)))
        return 0;
    int count = cJSON_GetArraySize(cues);
    if (dur->valuedouble < 1 || dur->valuedouble > 65535 ||
        (skip && (skip->valuedouble < 0 || skip->valuedouble >= dur->valuedouble)) ||
        count > OD_CASTLE_CUES_MAX)
        return 0;
    c->duration = (unsigned)dur->valuedouble;
    c->skip_from = skip ? (unsigned)skip->valuedouble : c->duration;
    const cJSON *lost = jget(w, "frame_tick_lost");
    if (lost) {
        int nl = cJSON_IsArray(lost) ? cJSON_GetArraySize(lost) : -1;
        if (nl < 0 || nl > 4)
            return 0;
        for (int k = 0; k < nl; k++) {
            const cJSON *t = cJSON_GetArrayItem(lost, k);
            if (!cJSON_IsNumber(t) || t->valuedouble < 0 ||
                t->valuedouble >= c->duration)
                return 0;
            c->frame_tick_lost[k] = (uint16_t)t->valuedouble;
        }
        c->frame_tick_lost_count = (unsigned)nl;
    }
    for (int k = 0; k < count; k++) {
        const cJSON *e = cJSON_GetArrayItem(cues, k);
        const cJSON *t = cJSON_GetArrayItem(e, 0), *id = cJSON_GetArrayItem(e, 1);
        if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) != 2 ||
            !cJSON_IsNumber(t) || !cJSON_IsNumber(id) ||
            t->valuedouble < 0 || t->valuedouble >= c->duration ||
            id->valuedouble < 0 || id->valuedouble > 255 ||
            (k && t->valuedouble < c->cues[k - 1].tick))
            return 0;
        c->cues[k].tick = (uint16_t)t->valuedouble;
        c->cues[k].id = (uint8_t)id->valuedouble;
    }
    c->cue_count = (unsigned)count;
    c->loaded = 1;
    return 1;
}

/* screens/layouts.json (C4.2 PISTAS-A): a display is {"words": [[field,
 * value], ...] (non-zero words from 32 on), "palette": [32]}; the title adds
 * its palette keyframes, sprite banks, last-tick banks and colour cycle. */
static int json_u(const cJSON *v, unsigned max, unsigned *out)
{
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > max)
        return 0;
    *out = (unsigned)v->valuedouble;
    return 1;
}

static int load_layout_words(const cJSON *disp, uint16_t out[OD_LAYOUT_WORDS])
{
    memset(out, 0, OD_LAYOUT_WORDS * sizeof out[0]);
    const cJSON *words = jget(disp, "words");
    if (!cJSON_IsArray(words))
        return 0;
    int n = cJSON_GetArraySize(words);
    for (int i = 0; i < n; i++) {
        const cJSON *w = cJSON_GetArrayItem(words, i);
        unsigned field, value;
        if (!cJSON_IsArray(w) || cJSON_GetArraySize(w) != 2 ||
            !json_u(cJSON_GetArrayItem(w, 0), OD_LAYOUT_WORDS - 1, &field) ||
            !json_u(cJSON_GetArrayItem(w, 1), 8191, &value) || field < 32)
            return 0;
        out[field] = (uint16_t)value;
    }
    const cJSON *pal = jget(disp, "palette");
    if (pal) {
        if (!cJSON_IsArray(pal) || cJSON_GetArraySize(pal) != 32)
            return 0;
        for (int k = 0; k < 32; k++) {
            unsigned v;
            if (!json_u(cJSON_GetArrayItem(pal, k), 63, &v))
                return 0;
            out[k] = (uint16_t)v;
        }
    }
    return 1;
}

static int load_layouts(const cJSON *doc, od_screen_layouts *l)
{
    memset(l, 0, sizeof *l);
    const cJSON *title = jget(doc, "title");
    const cJSON *close = jget(doc, "menu_close");
    if (!title || !load_layout_words(title, l->title) ||
        !load_layout_words(jget(doc, "menu"), l->menu) ||
        !close || !load_layout_words(close, l->menu_close) ||
        !load_layout_words(jget(doc, "map"), l->map) ||
        !json_u(jget(close, "frames"), 65535, &l->menu_close_frames) ||
        !l->menu_close_frames)
        return 0;
    const cJSON *keys = jget(title, "palettes");
    if (!cJSON_IsArray(keys) || cJSON_GetArraySize(keys) < 1 ||
        cJSON_GetArraySize(keys) > OD_LAYOUT_TITLE_KEYS)
        return 0;
    l->title_key_count = (unsigned)cJSON_GetArraySize(keys);
    for (unsigned i = 0; i < l->title_key_count; i++) {
        const cJSON *key = cJSON_GetArrayItem(keys, (int)i);
        const cJSON *pal = jget(key, "palette");
        unsigned tick;
        if (!json_u(jget(key, "tick"), 65535, &tick) || !cJSON_IsArray(pal) ||
            cJSON_GetArraySize(pal) != 32 ||
            (i && tick <= l->title_key_tick[i - 1]))
            return 0;
        l->title_key_tick[i] = (uint16_t)tick;
        for (int k = 0; k < 32; k++) {
            unsigned v;
            if (!json_u(cJSON_GetArrayItem(pal, k), 63, &v))
                return 0;
            l->title_key_palette[i][k] = (uint8_t)v;
        }
    }
    const cJSON *banks = jget(title, "sprite_banks_before");
    if (!cJSON_IsArray(banks) || cJSON_GetArraySize(banks) != 2)
        return 0;
    for (int k = 0; k < 2; k++) {
        unsigned v;
        if (!json_u(cJSON_GetArrayItem(banks, k), 8191, &v))
            return 0;
        l->title_sprite_banks_before[k] = (uint16_t)v;
    }
    const cJSON *end = jget(title, "end");
    const cJSON *end_words = jget(end, "words");
    if (!json_u(jget(title, "sprite_banks_from"), 65535, &l->title_sprite_banks_from) ||
        !json_u(jget(end, "tick"), 65535, &l->title_end_tick) ||
        !cJSON_IsArray(end_words) ||
        cJSON_GetArraySize(end_words) > OD_LAYOUT_END_WORDS)
        return 0;
    l->title_end_count = (unsigned)cJSON_GetArraySize(end_words);
    for (unsigned i = 0; i < l->title_end_count; i++) {
        const cJSON *w = cJSON_GetArrayItem(end_words, (int)i);
        unsigned field, value;
        if (!cJSON_IsArray(w) || cJSON_GetArraySize(w) != 2 ||
            !json_u(cJSON_GetArrayItem(w, 0), OD_LAYOUT_WORDS - 1, &field) ||
            !json_u(cJSON_GetArrayItem(w, 1), 8191, &value))
            return 0;
        l->title_end[i][0] = (uint16_t)field;
        l->title_end[i][1] = (uint16_t)value;
    }
    const cJSON *colour = jget(title, "colour");
    const cJSON *table = jget(colour, "table");
    if (!json_u(jget(colour, "entry"), 31, &l->colour_entry) ||
        !cJSON_IsArray(table) || cJSON_GetArraySize(table) < 3 ||
        cJSON_GetArraySize(table) > 8)
        return 0;
    l->colour_count = (unsigned)cJSON_GetArraySize(table);
    for (int k = 0; k < (int)l->colour_count; k++) {
        unsigned v;
        /* Raw bytes of the original: past the three colours they are code; the palette
         * keeps their low six bits (screens.c masks them). */
        if (!json_u(cJSON_GetArrayItem(table, k), 255, &v))
            return 0;
        l->colour_table[k] = (uint8_t)v;
    }
    l->loaded = 1;
    return 1;
}

static int load_castles(const cJSON *doc, od_castle_info *out)
{
    od_castle_info parsed[OD_CASTLE_WORLDS];
    memset(parsed, 0, sizeof parsed);
    const cJSON *worlds = jget(doc, "worlds");
    if (!cJSON_IsArray(worlds))
        return 0;
    int n = cJSON_GetArraySize(worlds);
    for (int i = 0; i < n; i++) {
        const cJSON *w = cJSON_GetArrayItem(worlds, i);
        const cJSON *world = jget(w, "world");
        if (!cJSON_IsNumber(world))
            return 0;
        int idx = (int)world->valuedouble - 1;
        if (idx < 0 || idx >= OD_CASTLE_WORLDS || parsed[idx].loaded ||
            !parse_clip_info(w, 1, &parsed[idx]))
            return 0;
    }
    memcpy(out, parsed, sizeof parsed);
    return 1;
}

/* screens/world-clears.json: {"clips": [{"clip": 1..10, "duration": N,
 * "skip_from": T (optional), "frame_tick_lost": [...], "cues": [...]}, ...]}
 * (END-OF-WORLD): clips 1..9 are the end-of-world cinematics, 10 the castle
 * opening after world 9. */
static int load_world_clears(const cJSON *doc, od_castle_info *out)
{
    od_castle_info parsed[OD_WORLD_CLEAR_CLIPS];
    memset(parsed, 0, sizeof parsed);
    const cJSON *clips = jget(doc, "clips");
    if (!cJSON_IsArray(clips))
        return 0;
    int n = cJSON_GetArraySize(clips);
    for (int i = 0; i < n; i++) {
        const cJSON *c = cJSON_GetArrayItem(clips, i);
        const cJSON *clip = jget(c, "clip");
        if (!cJSON_IsNumber(clip))
            return 0;
        int idx = (int)clip->valuedouble - 1;
        if (idx < 0 || idx >= OD_WORLD_CLEAR_CLIPS || parsed[idx].loaded ||
            !parse_clip_info(c, 0, &parsed[idx]))
            return 0;
    }
    memcpy(out, parsed, sizeof parsed);
    return 1;
}

static int cinematic_unique_keys(const cJSON *node);
static int static_keys(const cJSON *o, const char *keys, unsigned count)
{
    if (!cJSON_IsObject(o)) return 0;
    unsigned n = 0;
    for (const cJSON *p = o->child; p; p = p->next) {
        int found = 0;
        for (const char *k = keys; *k;) {
            const char *end = strchr(k, '|'); size_t len = end ? (size_t)(end-k) : strlen(k);
            if (p->string && strlen(p->string) == len && !memcmp(k,p->string,len)) found = 1;
            k += len; if (*k) ++k;
        }
        if (!found) return 0;
        ++n;
    }
    return n == count;
}
static int static_u(const cJSON *v, unsigned max, unsigned *out)
{
    return json_u(v,max,out) && v->valuedouble == (double)*out;
}
static int static_eq(const cJSON *o, const char *key, unsigned expected)
{
    unsigned n; return static_u(jget(o,key),expected,&n) && n == expected;
}
static int static_bytes(const cJSON *a, uint8_t *out, unsigned n, unsigned max)
{
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != (int)n) return 0;
    for (unsigned i=0;i<n;++i) { unsigned v;
        if (!static_u(cJSON_GetArrayItem(a,(int)i),max,&v)) return 0;
        out[i]=(uint8_t)v;
    }
    return 1;
}
static int static_byte(const cJSON *o, const char *key, uint8_t *out)
{
    unsigned n; if (!static_u(jget(o,key),255,&n)) return 0; *out=(uint8_t)n; return 1;
}
static od_status load_static_layouts(const cJSON *doc, gfx_candidate *c)
{
    if (!static_keys(doc,"schema_version|id|map_bases",3) || !cinematic_unique_keys(doc)) return OD_ERR_FORMAT;
    const cJSON *bases=jget(doc,"map_bases");
    if (!cJSON_IsArray(bases) || cJSON_GetArraySize(bases)!=2) return OD_ERR_FORMAT;
    c->static_layouts=calloc(1,sizeof *c->static_layouts);
    if (!c->static_layouts) return OD_ERR_NOMEM;
    for (unsigned i=0;i<2;++i) {
        const cJSON *b=cJSON_GetArrayItem(bases,(int)i);
        od_static_map_base *out=&c->static_layouts->map_bases[i];
        if (!static_keys(b,"scene_index|chr_mode|background_banks|ciram",4) ||
            !static_eq(b,"scene_index",i?43:2) || !static_eq(b,"chr_mode",0) ||
            !static_bytes(jget(b,"background_banks"),out->background_banks,4,255) ||
            !static_bytes(jget(b,"ciram"),out->ciram,2048,255)) return OD_ERR_FORMAT;
        out->scene_index=(uint8_t)(i?43:2);
    }
    return OD_OK;
}
static od_status static_command(const cJSON *o, od_static_mw_command *out, od_static_mw_op expected)
{
    const cJSON *op=jget(o,"op");
    const char *name=expected==OD_STATIC_MW_RECORDS?"records":expected==OD_STATIC_MW_YIELD?"yield":"word";
    if (!cJSON_IsString(op) || strcmp(op->valuestring,name) ||
        !static_keys(o,expected==OD_STATIC_MW_RECORDS?"op|records":"op",expected==OD_STATIC_MW_RECORDS?2:1)) return OD_ERR_FORMAT;
    out->op=expected;
    if (expected!=OD_STATIC_MW_RECORDS) return OD_OK;
    const cJSON *records=jget(o,"records");
    if (!cJSON_IsArray(records) || cJSON_GetArraySize(records)>4096) return OD_ERR_FORMAT;
    out->record_count=(size_t)cJSON_GetArraySize(records);
    od_static_mw_record *rows=out->record_count?calloc(out->record_count,sizeof *rows):NULL;
    if (out->record_count && !rows) return OD_ERR_NOMEM;
    out->records=rows;
    for (size_t i=0;i<out->record_count;++i) {
        const cJSON *row=cJSON_GetArrayItem(records,(int)i); unsigned count,addr;
        if (!cJSON_IsArray(row) || cJSON_GetArraySize(row)!=3 ||
            !static_u(cJSON_GetArrayItem(row,0),187,&count) ||
            (!(count>=1 && count<=59) && !(count>=129 && count<=187))) return OD_ERR_FORMAT;
        if (!static_u(cJSON_GetArrayItem(row,1),12287,&addr) || addr<8192 ||
            !static_bytes(cJSON_GetArrayItem(row,2),rows[i].data,count&63,255)) return OD_ERR_FORMAT;
        rows[i].count=(uint8_t)count; rows[i].address=(uint16_t)addr;
    }
    return OD_OK;
}
static od_status load_static_magicword(const cJSON *doc, gfx_candidate *c)
{
    if (!static_keys(doc,"schema_version|id|palette_buffer|chr|song_request|screen|question|word|cursor|cycle|queue_capacity_bytes|post_goodnight",12) ||
        !cinematic_unique_keys(doc)) return OD_ERR_FORMAT;
    od_static_magicword *a=calloc(1,sizeof *a);
    if (!a) return OD_ERR_NOMEM;
    c->static_magicword=a;
    const cJSON *word=jget(doc,"word"),*cur=jget(doc,"cursor"),*cy=jget(doc,"cycle"),*post=jget(doc,"post_goodnight"),*pc=jget(post,"cycle");
    const cJSON *screen=jget(doc,"screen"),*question=jget(doc,"question"),*tabs=jget(cy,"bg_tables");
    unsigned address;
    if (!static_bytes(jget(doc,"palette_buffer"),a->palette_buffer,32,63) ||
        !static_bytes(jget(doc,"chr"),a->chr,6,255) || !static_byte(doc,"song_request",&a->song_request) ||
        !static_eq(doc,"queue_capacity_bytes",60) ||
        !cJSON_IsArray(screen) || cJSON_GetArraySize(screen)!=5 ||
        !cJSON_IsArray(question) || cJSON_GetArraySize(question)!=1 ||
        !static_keys(word,"address|count|group|gap_tile|glyphs",5) ||
        !static_u(jget(word,"address"),12287,&address) || address<8192 ||
        !static_eq(word,"count",26) || !static_eq(word,"group",8) ||
        !static_byte(word,"gap_tile",&a->word.gap_tile) || !static_bytes(jget(word,"glyphs"),a->word.glyphs,32,255) ||
        !static_keys(cur,"y|tile|attr|x",4) || !static_byte(cur,"y",&a->cursor.y) ||
        !static_byte(cur,"tile",&a->cursor.tile) || !static_byte(cur,"attr",&a->cursor.attr) ||
        !static_bytes(jget(cur,"x"),a->cursor.x,2,255) ||
        !static_keys(cy,"bg_mask|bg_entries|bg_tables|sprite_mask|sprite_entry|sprite_table",6) ||
        !static_eq(cy,"bg_mask",15) || !static_bytes(jget(cy,"bg_entries"),a->cycle.bg_entries,3,255) ||
        a->cycle.bg_entries[0]!=5 || a->cycle.bg_entries[1]!=6 || a->cycle.bg_entries[2]!=7 ||
        !cJSON_IsArray(tabs) || cJSON_GetArraySize(tabs)!=3 || !static_eq(cy,"sprite_mask",7) ||
        !static_eq(cy,"sprite_entry",19) || !static_bytes(jget(cy,"sprite_table"),a->cycle.sprite_table,5,255) ||
        !static_keys(post,"palette_buffer|brightness_bg|brightness_sprite|cycle",4) ||
        !static_bytes(jget(post,"palette_buffer"),a->post_goodnight.palette_buffer,32,63) ||
        !static_eq(post,"brightness_bg",15) || !static_eq(post,"brightness_sprite",15) ||
        !static_keys(pc,"mask|entry|colours",3) || !static_eq(pc,"mask",7) || !static_eq(pc,"entry",3) ||
        !static_bytes(jget(pc,"colours"),a->post_goodnight.cycle.colours,4,255)) return OD_ERR_FORMAT;
    a->queue_capacity_bytes=60; a->word.address=(uint16_t)address; a->word.count=26; a->word.group=8;
    a->cycle.bg_mask=15; a->cycle.sprite_mask=7; a->cycle.sprite_entry=19;
    a->post_goodnight.brightness_bg=15; a->post_goodnight.brightness_sprite=15;
    a->post_goodnight.cycle.mask=7; a->post_goodnight.cycle.entry=3;
    for (unsigned i=0;i<3;++i) if (!static_bytes(cJSON_GetArrayItem(tabs,(int)i),a->cycle.bg_tables[i],3,255)) return OD_ERR_FORMAT;
    for (unsigned i=0;i<5;++i) {
        od_status st=static_command(cJSON_GetArrayItem(screen,(int)i),&a->screen[i],i==4?OD_STATIC_MW_WORD:(i&1)?OD_STATIC_MW_YIELD:OD_STATIC_MW_RECORDS);
        if (st!=OD_OK) return st;
    }
    return static_command(cJSON_GetArrayItem(question,0),&a->question[0],OD_STATIC_MW_RECORDS);
}

static od_status load_screens(const od_content_vfs *vfs, const cJSON *screens, gfx_candidate *c)
{
    char paths[16][256];
    int n = read_path_list(screens, "screens", paths, 16);
    if (n <= 0)
        return OD_ERR_FORMAT;
    int got_arena = 0, got_retry = 0;
    for (int i = 0; i < n; i++) {
        for (int prior=0;prior<i;++prior) if (!strcmp(paths[prior],paths[i])) return OD_ERR_FORMAT;
        char *data = NULL;
        size_t size = 0;
        od_status st = read_vfs_text(vfs, paths[i], &data, &size);
        if (st != OD_OK)
            return st;
        cJSON *doc = cJSON_ParseWithLength(data, size);
        free(data);
        if (doc == NULL)
            return OD_ERR_FORMAT;
        const cJSON *id = jget(doc, "id");
        const char *which = cJSON_IsString(id) ? id->valuestring : "";
        unsigned version=0;
        if (!strcmp(which,"layouts") || !strcmp(which,"magicword")) {
            const char *required=!strcmp(paths[i],"screens/layouts.json")?"layouts":
                !strcmp(paths[i],"screens/magicword.json")?"magicword":NULL;
            if (!required || strcmp(required,which) || !static_u(jget(doc,"schema_version"),2,&version) || !version ||
                !cinematic_unique_keys(doc)) { cJSON_Delete(doc); return OD_ERR_FORMAT; }
        } else if (!strcmp(paths[i],"screens/layouts.json") || !strcmp(paths[i],"screens/magicword.json")) {
            cJSON_Delete(doc); return OD_ERR_FORMAT;
        }
        int ok = 1;
        if (strcmp(which, "boss-arena") == 0) {
            const cJSON *cells = jget(doc, "cells");
            ok = json_bytes(cells, c->boss_arena, 256);
            got_arena = ok;
        } else if (strcmp(which, "retry") == 0) {
            const cJSON *tiles = jget(doc, "tiles");
            const cJSON *pal = jget(doc, "palette");
            const cJSON *cols = jget(doc, "cursor_cols");
            const cJSON *ctile = jget(doc, "cursor_tile");
            ok = json_bytes(tiles, c->retry_tiles, 96) &&
                 json_bytes(pal, c->retry_palette, 4) &&
                 json_bytes(cols, c->retry_cursor_cols, 2) &&
                 json_u(ctile, 255, &c->retry_cursor_tile);
            for (int k = 0; k < 4 && ok; ++k)
                if (c->retry_palette[k] > 63) ok = 0;
            got_retry = ok;
        } else if (strcmp(which, "map") == 0) {
            ok = load_map_art(doc, &c->map_art);
        } else if (strcmp(which, "hearth") == 0) {
            ok = load_hearth_art(doc, &c->hearth_art);
        } else if (strcmp(which, "magicword") == 0) {
            if (version==2) st=load_static_magicword(doc,c);
            else ok = load_magicword_art(doc, &c->magicword_art);
        } else if (strcmp(which, "password") == 0) {
            ok = load_password_art(doc, &c->password_art);
        } else if (strcmp(which, "interactive-setup") == 0) {
            od_interactive_setup_stage *next = NULL;
            st = od_interactive_setup_prepare_doc(doc, &next);
            if (st != OD_OK) { cJSON_Delete(doc); return st; }
            od_interactive_setup_discard(c->interactive);
            c->interactive = next;
        } else if (strcmp(which, "transfer-structure") == 0) {
            od_transfer_structure_stage *next = NULL;
            st = od_transfer_structure_prepare_doc(doc, &next);
            if (st != OD_OK) { cJSON_Delete(doc); return st; }
            od_transfer_structure_discard(c->transfer);
            c->transfer = next;
        } else if (strcmp(which, "castles") == 0) {
            ok = load_castles(doc, c->castles);
        } else if (strcmp(which, "world-clears") == 0) {
            ok = load_world_clears(doc, c->world_clears);
        } else if (strcmp(which, "layouts") == 0) {
            if (version==2) st=load_static_layouts(doc,c);
            else ok = load_layouts(doc, &c->layouts);
        } else if (strcmp(which, "boss-background") == 0) {
            /* The external compose loader validates and owns this state. */
            ok = 1;
        } else ok = 0;
        cJSON_Delete(doc);
        if (st!=OD_OK) return st;
        if (!ok)
            return OD_ERR_FORMAT;
    }
    return (got_arena && got_retry) ? OD_OK : OD_ERR_FORMAT;
}

static od_status load_audio(const od_content_vfs *vfs, const cJSON *audio, gfx_candidate *c)
{
    char paths[4][256];
    int n = read_path_list(audio, "audio", paths, 4);
    if (n <= 0)
        return OD_ERR_FORMAT;
    /* Rebuild the exact ODSEQ1 stream the sequencer consumes. */
    size_t cap = 1u << 20;
    char *text = (char *)malloc(cap);
    if (text == NULL)
        return OD_ERR_NOMEM;
    size_t len = 0;
    len += (size_t)snprintf(text + len, cap - len, "ODSEQ1\n");
    for (int i = 0; i < n; i++) {
        char *data = NULL;
        size_t size = 0;
        od_status st = read_vfs_text(vfs, paths[i], &data, &size);
        if (st != OD_OK) {
            free(text);
            return st;
        }
        cJSON *doc = cJSON_ParseWithLength(data, size);
        free(data);
        if (doc == NULL) {
            free(text);
            return OD_ERR_FORMAT;
        }
        const cJSON *writes = jget(doc, "writes");
        if (!cJSON_IsArray(writes)) {
            cJSON_Delete(doc);
            free(text);
            return OD_ERR_FORMAT;
        }
        int count = cJSON_GetArraySize(writes);
        for (int k = 0; k < count; k++) {
            const cJSON *w = cJSON_GetArrayItem(writes, k);
            unsigned bank, address;
            const cJSON *bytes = jget(w, "data");
            if (!json_u(jget(w, "bank"), 255, &bank) ||
                !json_u(jget(w, "address"), 65535, &address) || !cJSON_IsArray(bytes) ||
                len + (size_t)cJSON_GetArraySize(bytes) * 2 + 16 >= cap) {
                cJSON_Delete(doc);
                free(text);
                return OD_ERR_FORMAT;
            }
            len += (size_t)snprintf(text + len, cap - len, "%02x %04x ", bank, address);
            int bn = cJSON_GetArraySize(bytes);
            for (int b = 0; b < bn; b++) {
                unsigned byte;
                if (!json_u(cJSON_GetArrayItem(bytes, b), 255, &byte)) {
                    cJSON_Delete(doc); free(text); return OD_ERR_FORMAT;
                }
                len += (size_t)snprintf(text + len, cap - len, "%02x", byte);
            }
            len += (size_t)snprintf(text + len, cap - len, "\n");
        }
        cJSON_Delete(doc);
    }
    c->audio_hex = text;
    c->audio_hex_size = len;
    return OD_OK;
}

/* cJSON's lookup takes the first duplicate key. Semantic documents must not
 * hide a second family/table/command value behind that lookup. */
static int cinematic_unique_keys(const cJSON *node)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, node) {
        if (cJSON_IsObject(node)) {
            for (const cJSON *prior = node->child; prior != item; prior = prior->next)
                if (prior->string && item->string &&
                    strcmp(prior->string, item->string) == 0) return 0;
        }
        if (!cinematic_unique_keys(item)) return 0;
    }
    return 1;
}

/* Validate declared semantic cinematic documents through THIS layered VFS.
 * Ownership/execution stays in screens/cinematic; legacy track declarations
 * in historical packs are still checked there, not treated as JSON here. */
static od_status load_cinematic_documents(const od_content_vfs *vfs, const cJSON *pack)
{
    const cJSON *list = jget(pack, "cinematics");
    if (list == NULL) return OD_OK; /* synthetic/module packs may omit it */
    if (!cJSON_IsArray(list)) return OD_ERR_FORMAT;
    unsigned json_documents = 0, legacy_documents = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, list) {
        if (!cJSON_IsString(entry)) return OD_ERR_FORMAT;
        const char *rel = entry->valuestring;
        /* A repeated declaration is not a second semantic resource. Reject it
         * before parsing or counting coverage for this candidate snapshot. */
        for (const cJSON *prior = list->child; prior != entry; prior = prior->next)
            if (cJSON_IsString(prior) && strcmp(prior->valuestring, rel) == 0)
                return OD_ERR_FORMAT;
        size_t n = strlen(rel);
        if (n >= 5 && strcmp(rel + n - 5, ".json") == 0) {
            if (strcmp(rel, "screens/cinematics/castle-appears.json") != 0 &&
                strcmp(rel, "screens/cinematics/bootstrap.json") != 0 &&
                strcmp(rel, "screens/cinematics/editor-entry.json") != 0 &&
                strcmp(rel, "screens/cinematics/castle-round10.json") != 0 &&
                strcmp(rel, "screens/cinematics/world-clear.json") != 0 &&
                strcmp(rel, "screens/cinematics/goodnight-hidden.json") != 0 &&
                strcmp(rel, "screens/cinematics/boss-ending.json") != 0 &&
                strcmp(rel, "screens/cinematics/post-ending.json") != 0)
                return OD_ERR_FORMAT; /* no fabricated loader for other families */
            od_status st;
            od_cinematic_data *data = NULL;
#if defined(OD_CINEMATIC_TEXT_API) && OD_CINEMATIC_TEXT_API == 1
            char *text = NULL; size_t size = 0;
            st = read_vfs_text(vfs, rel, &text, &size);
            const char *required_id = NULL;
            if (strcmp(rel, "screens/cinematics/editor-entry.json") == 0)
                required_id = "editor-entry";
            else if (strcmp(rel, "screens/cinematics/castle-round10.json") == 0)
                required_id = "castle-round10";
            else if (strcmp(rel, "screens/cinematics/world-clear.json") == 0)
                required_id = "world-clear";
            else if (strcmp(rel, "screens/cinematics/goodnight-hidden.json") == 0)
                required_id = "goodnight-hidden";
            else if (strcmp(rel, "screens/cinematics/boss-ending.json") == 0)
                required_id = "boss-ending";
            else if (strcmp(rel, "screens/cinematics/post-ending.json") == 0)
                required_id = "post-ending";
            if (st == OD_OK && required_id != NULL) {
                /* Bind this declared route to its family using the SAME pinned
                 * bytes. The opaque typed parser below owns full validation;
                 * no invented family getter, second disk read or publication. */
                cJSON *root = cJSON_ParseWithLength(text, size);
                const cJSON *id = jget(root, "id");
                unsigned ids = 0;
                const cJSON *item = NULL;
                cJSON_ArrayForEach(item, root) {
                    if (item->string && strcmp(item->string, "id") == 0) ++ids;
                }
                if (!cJSON_IsObject(root) || !cinematic_unique_keys(root) ||
                    ids != 1 || !cJSON_IsString(id) ||
                    strcmp(id->valuestring, required_id) != 0) st = OD_ERR_FORMAT;
                cJSON_Delete(root);
            }
            if (st == OD_OK) st = od_cinematic_load_text(&data, text, size);
            free(text);
#else
            if (strcmp(rel, "screens/cinematics/editor-entry.json") == 0 ||
                strcmp(rel, "screens/cinematics/castle-round10.json") == 0 ||
                strcmp(rel, "screens/cinematics/world-clear.json") == 0 ||
                strcmp(rel, "screens/cinematics/goodnight-hidden.json") == 0 ||
                strcmp(rel, "screens/cinematics/boss-ending.json") == 0 ||
                strcmp(rel, "screens/cinematics/post-ending.json") == 0)
                return OD_ERR_FORMAT; /* new family requires the pure typed API */
            /* Compatibility parser still performs mutable file IO. It may
             * validate historical module packs, but NEVER certifies snapshot
             * cinematic coverage. The writer supplies the pure text API. */
            char path[4096];
            st = od_content_vfs_resolve(vfs, rel, path, sizeof path);
            if (st != OD_OK) return st;
            st = od_cinematic_load(&data, path);
#endif
            od_cinematic_free(data);
            if (st != OD_OK) return st;
            ++json_documents;
        } else if (strncmp(rel, "screens/tracks/", 15) != 0 ||
                   n < 6 || (strcmp(rel + n - 6, ".track") != 0 &&
                             strcmp(rel + n - 5, ".fade") != 0)) {
            return OD_ERR_FORMAT;
        } else ++legacy_documents;
    }
#if defined(OD_CINEMATIC_TEXT_API) && OD_CINEMATIC_TEXT_API == 1
    if (json_documents && !legacy_documents)
        od_content_snapshot_completed(vfs, OD_SNAPSHOT_CINEMATICS);
#else
    (void)json_documents; (void)legacy_documents;
#endif
    return OD_OK;
}

/* Internal bundle gate shared by VFS/assets startup. It only parses and frees
 * semantic documents: no graphics/table publication, no scene execution. */
od_status od_content_cinematics_validate(const od_content_vfs *vfs)
{
    char *text = NULL;
    size_t size = 0;
    od_status st = read_vfs_text(vfs, "pack.json", &text, &size);
    if (st != OD_OK) return st;
    cJSON *pack = cJSON_ParseWithLength(text, size);
    free(text);
    if (pack == NULL) return OD_ERR_FORMAT;
    st = load_cinematic_documents(vfs, pack);
    cJSON_Delete(pack);
    return st;
}

od_status od_gfx_load(const od_content_vfs *vfs)
{
    if (vfs == NULL)
        return OD_ERR_FORMAT;
    char *text = NULL;
    size_t size = 0;
    od_status st = read_vfs_text(vfs, "pack.json", &text, &size);
    if (st != OD_OK)
        return st;
    cJSON *pack = cJSON_ParseWithLength(text, size);
    free(text);
    if (pack == NULL)
        return OD_ERR_FORMAT;

    const cJSON *graphics = jget(pack, "graphics");
    gfx_candidate *candidate = calloc(1, sizeof *candidate);
    if (candidate == NULL) { cJSON_Delete(pack); return OD_ERR_NOMEM; }
    /* Every resource is prepared before any graphics cache is published. */
    st = load_cinematic_documents(vfs, pack);
    if (st == OD_OK)
        st = load_chr(vfs, graphics, candidate);
    if (st == OD_OK)
        st = load_screens(vfs, pack, candidate);
    if (st == OD_OK)
        st = load_audio(vfs, pack, candidate);
    if (st == OD_OK)
        st = prepare_graphic_family(vfs, pack, "worlds", candidate, 1);
    if (st == OD_OK)
        st = prepare_graphic_family(vfs, pack, "palettes", candidate, 0);
    if (st == OD_OK)
        st = prepare_compose(vfs, graphics, candidate);
    cJSON_Delete(pack);

    if (st == OD_OK) {
        candidate_commit(candidate);
        g_loaded = 1;
        if (g_vfs_valid) od_content_snapshot_release(g_vfs.snapshot);
        g_vfs = *vfs;
        od_content_snapshot_retain(g_vfs.snapshot);
        g_vfs_valid = 1;
        od_content_snapshot_completed(vfs, OD_SNAPSHOT_GFX);
    }
    candidate_free(candidate);
    return st;
}

/* C6.29 (PUB-9): the same preparation as od_gfx_load, never published. For
 * `--validate`: names the failing stage in `stage`. Internal, declared in
 * content.c. */
od_status od_gfx_check(const od_content_vfs *vfs, char *stage, size_t cap)
{
    if (vfs == NULL)
        return OD_ERR_FORMAT;
    char *text = NULL;
    size_t size = 0;
    snprintf(stage, cap, "pack.json");
    od_status st = read_vfs_text(vfs, "pack.json", &text, &size);
    if (st != OD_OK)
        return st;
    cJSON *pack = cJSON_ParseWithLength(text, size);
    free(text);
    if (pack == NULL)
        return OD_ERR_FORMAT;
    const cJSON *graphics = jget(pack, "graphics");
    gfx_candidate *candidate = calloc(1, sizeof *candidate);
    if (candidate == NULL) { cJSON_Delete(pack); return OD_ERR_NOMEM; }
    snprintf(stage, cap, "graphics (CHR sheets and patches)");
    st = load_chr(vfs, graphics, candidate);
    if (st == OD_OK) {
        snprintf(stage, cap, "screens");
        st = load_screens(vfs, pack, candidate);
    }
    if (st == OD_OK) {
        snprintf(stage, cap, "audio");
        st = load_audio(vfs, pack, candidate);
    }
    if (st == OD_OK) {
        snprintf(stage, cap, "worlds");
        st = prepare_graphic_family(vfs, pack, "worlds", candidate, 1);
    }
    if (st == OD_OK) {
        snprintf(stage, cap, "palettes");
        st = prepare_graphic_family(vfs, pack, "palettes", candidate, 0);
    }
    if (st == OD_OK) {
        snprintf(stage, cap, "sprites");
        st = prepare_compose(vfs, graphics, candidate);
    }
    cJSON_Delete(pack);
    candidate_free(candidate);
    return st;
}

od_status od_gfx_resolve(const char *rel, char *out, size_t cap)
{
    if (!g_vfs_valid)
        return OD_ERR_IO;
    return od_content_vfs_resolve(&g_vfs, rel, out, cap);
}

od_status od_gfx_content_read(const char *rel, char **data, size_t *size)
{
    if (data) *data = NULL;
    if (size) *size = 0;
    if (!g_vfs_valid) return OD_ERR_IO;
    return od_content_vfs_read(&g_vfs, rel, data, size);
}

od_status od_gfx_scene_resources_complete(void)
{
    return g_vfs_valid ? od_content_vfs_finish_scenes(&g_vfs) : OD_ERR_IO;
}

od_status od_gfx_load_default(const char *base_override)
{
    char base[1024];
    if (od_content_resolve_base(base, sizeof(base), base_override) != OD_OK)
        return OD_ERR_IO;
    od_content_vfs vfs;
    /* The VFS init calls od_gfx_apply() for a previously bound bundle; during
     * the default load there is nothing to re-apply yet. */
    g_in_default_load = 1;
    od_status st = od_content_vfs_init(&vfs, base, NULL, 0);
    if (st == OD_OK) {
        st = od_gfx_load(&vfs);
        od_content_vfs_free(&vfs);
    }
    g_in_default_load = 0;
    return st;
}

od_status od_gfx_apply(const od_content_vfs *vfs)
{
    if (g_bound == NULL || g_in_default_load)
        return OD_OK;
    /* Reserve the bound-bundle copy BEFORE any candidate can be published. */
    uint8_t *copy = malloc(OD_CHR_BYTES);
    if (copy == NULL) return OD_ERR_NOMEM;
    od_status st = od_gfx_load(vfs);
    if (st != OD_OK) {
        free(copy);
        return st;
    }
    /* Swap the bound bundle's CHR for the newly loaded one. */
    memcpy(copy, g_chr, g_chr_size);
    free(g_bound->chr);
    g_bound->chr = copy;
    g_bound->chr_size = g_chr_size;
    const od_world_gfx *world = od_world_gfx_for_world(0);
    if (world != NULL)
        memcpy(g_bound->palette_ram.ram, world->palette_ram, 32);
    return st;
}

void od_gfx_reset(void)
{
    od_interactive_setup_reset();
    od_transfer_structure_reset();
    if (g_vfs_valid) od_content_snapshot_release(g_vfs.snapshot);
    memset(&g_vfs, 0, sizeof g_vfs);
    g_vfs_valid = 0;
    free(g_chr); g_chr = NULL; g_chr_size = 0;
    free(g_audio_hex); g_audio_hex = NULL; g_audio_hex_size = 0;
    memset(g_boss_arena, 0, sizeof(g_boss_arena));
    memset(g_retry_tiles, 0, sizeof(g_retry_tiles));
    memset(g_retry_palette, 0, sizeof(g_retry_palette));
    memset(g_retry_cursor_cols, 0, sizeof(g_retry_cursor_cols));
    g_retry_cursor_tile = 0;
    memset(&g_map_art, 0, sizeof(g_map_art));
    memset(&g_hearth_art, 0, sizeof(g_hearth_art));
    memset(&g_magicword_art, 0, sizeof(g_magicword_art));
    memset(&g_password_art, 0, sizeof(g_password_art));
    memset(g_castles, 0, sizeof(g_castles));
    memset(g_world_clears, 0, sizeof(g_world_clears));
    memset(&g_layouts, 0, sizeof(g_layouts));
    free(g_static_layouts); g_static_layouts=NULL;
    static_magicword_free(g_static_magicword); g_static_magicword=NULL;
    od_sprites_reset();
    od_world_gfx_reset();
    g_loaded = 0;
}
