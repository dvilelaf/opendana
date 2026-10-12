/* compose.c - level -> colour-index frame.
 *
 * The composition data (tile and idle-pose IDs for world 1 and the idle legacy
 * API) is loaded from the content package (engine/presentation-data.json), not
 * compiled in. Live world/pose rendering uses the worlds.json/poses.json data.
 * The resolved presentation compositor is tested on complete frames, including
 * sprites and HUD. */
#include "opendana/compose.h"
#include "opendana/content_tables.h"
#include "opendana/presentation_data.h"

#include "cJSON.h"
#include "opendana/objects.h"
#include "opendana/tile.h"
#include "opendana/world_gfx.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef struct { int8_t x, y; uint8_t tile, attr; } od_pose_part;
typedef struct {
    uint8_t bank;
    uint16_t address;
    uint8_t chr_bank;
    uint16_t first, count;
} od_pose_entry;
typedef struct { unsigned wait, first, count; } od_steam_phase;

/* The sprites and their animation tables come from the content package
 * (`graphics/sprites/poses.json`, C2.2). The arrays are owned here and filled
 * by od_sprites_load_json(); nothing is compiled in. */
static od_pose_part *g_pose_parts;
static size_t g_pose_parts_count;
static od_pose_entry *g_poses;
static size_t g_poses_count;
static od_pose_part *g_steam_parts;
static size_t g_steam_parts_count;
static od_steam_phase *g_steam_phases;
static size_t g_steam_phases_count;
static uint8_t g_world_sprite_banks[10];
static uint8_t g_bg_banks[4][6];
static uint8_t g_bg_waits[4][6];
static int g_sprites_loaded;
struct od_sprites_candidate {
    od_pose_part *parts, *steam_parts;
    od_pose_entry *poses;
    od_steam_phase *phases;
    size_t parts_n, steam_parts_n, poses_n, phases_n;
    uint8_t world_banks[10], bg_banks[4][6], bg_waits[4][6];
};

void od_sprites_candidate_free(od_sprites_candidate *c)
{
    if (!c) return;
    free(c->parts); free(c->steam_parts); free(c->poses); free(c->phases);
    free(c);
}
static uint8_t g_arena_nt[2048], g_arena_palette[32];
static uint8_t g_arena_banks[4], g_arena_grid[256];
static uint8_t g_arena_base_palette[32], g_arena_fade_lut[64], g_arena_lower_banks[3];
static uint8_t g_arena_sprite_banks[2];
static uint8_t g_arena_attack_tiles[23 * 4];

static od_status arena_prepare(uint8_t data[2533], const char *text, size_t size)
{
    if (!text) return OD_ERR_FORMAT;
    od_status st = OD_OK;
    cJSON *doc = cJSON_ParseWithLength(text, size);
    if (!doc) return OD_ERR_FORMAT;
    const char *keys[] = {"ciram", "palette", "banks", "grid", "base_palette", "fade_lut", "lower_banks", "sprite_banks", "attack_tiles"};
    unsigned sizes[] = {2048, 32, 4, 256, 32, 64, 3, 2, 92};
    unsigned n = 0;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "schema_version");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1) st = OD_ERR_FORMAT;
    for (unsigned k = 0; k < 9 && st == OD_OK; k++) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(doc, keys[k]);
        if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != (int)sizes[k]) {
            st = OD_ERR_FORMAT; break;
        }
        for (unsigned i = 0; i < sizes[k]; i++) {
            const cJSON *v = cJSON_GetArrayItem(a, (int)i);
            unsigned max = k == 1 || k == 4 || k == 5 ? 63 :
                           k == 2 || k == 6 || k == 7 ? 127 : 255;
            if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > max ||
                v->valuedouble != (int)v->valuedouble) { st = OD_ERR_FORMAT; break; }
            data[n++] = (uint8_t)v->valuedouble;
        }
    }
    cJSON_Delete(doc);
    return st;
}

static void arena_commit(const uint8_t data[2533])
{
    memcpy(g_arena_nt, data, 2048);
    memcpy(g_arena_palette, data + 2048, 32);
    memcpy(g_arena_banks, data + 2080, 4);
    memcpy(g_arena_grid, data + 2084, 256);
    memcpy(g_arena_base_palette, data + 2340, 32);
    memcpy(g_arena_fade_lut, data + 2372, 64);
    memcpy(g_arena_lower_banks, data + 2436, 3);
    memcpy(g_arena_sprite_banks, data + 2439, 2);
    memcpy(g_arena_attack_tiles, data + 2441, 92);
}

od_status od_arena_load(const od_content_vfs *vfs)
{
    char *text = NULL;
    size_t size = 0;
    uint8_t data[2533];
    od_status st = od_content_vfs_read(vfs, "screens/boss-background.json", &text, &size);
    if (st == OD_OK) st = arena_prepare(data, text, size);
    free(text);
    if (st == OD_OK) arena_commit(data);
    return st;
}

const uint8_t *od_arena_hud_row(void)
{
    return g_arena_nt + 1024 + 28 * 32;
}

const uint8_t *od_palette_fade_lut(void)
{
    /* Extracted with the arena data. Brightness 0 maps every entry to black; an
     * unloaded table (all zero) is reported as missing. */
    return g_arena_fade_lut[0] == 0x0F ? g_arena_fade_lut : NULL;
}

static void sprites_clear(void)
{
    free(g_pose_parts); g_pose_parts = NULL; g_pose_parts_count = 0;
    free(g_poses); g_poses = NULL; g_poses_count = 0;
    free(g_steam_parts); g_steam_parts = NULL; g_steam_parts_count = 0;
    free(g_steam_phases); g_steam_phases = NULL; g_steam_phases_count = 0;
    memset(g_world_sprite_banks, 0, sizeof(g_world_sprite_banks));
    memset(g_bg_banks, 0, sizeof(g_bg_banks));
    memset(g_bg_waits, 0, sizeof(g_bg_waits));
    g_sprites_loaded = 0;
}

void od_sprites_reset(void) { sprites_clear(); }
int od_sprites_loaded(void) { return g_sprites_loaded; }
unsigned od_sprites_pose_count(void) { return (unsigned)g_poses_count; }
unsigned od_sprites_part_count(void) { return (unsigned)g_pose_parts_count; }
const uint8_t *od_sprites_world_banks(void) { return g_world_sprite_banks; }

static void *alloc_rows(size_t count, size_t width)
{
    return count ? calloc(count, width) : NULL;
}

static int sprite_integer(const cJSON *v, int min, int max)
{
    return cJSON_IsNumber(v) && v->valuedouble >= min && v->valuedouble <= max &&
           v->valuedouble == (int)v->valuedouble;
}

static od_status load_rows4(const cJSON *arr, od_pose_part **out, size_t *count)
{
    if (!cJSON_IsArray(arr))
        return OD_ERR_FORMAT;
    size_t n = (size_t)cJSON_GetArraySize(arr);
    od_pose_part *rows = (od_pose_part *)alloc_rows(n, sizeof(*rows));
    if (n && rows == NULL)
        return OD_ERR_NOMEM;
    for (size_t i = 0; i < n; i++) {
        const cJSON *row = cJSON_GetArrayItem(arr, (int)i);
        if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != 4) {
            free(rows);
            return OD_ERR_FORMAT;
        }
        for (int k = 0; k < 4; ++k) {
            if (!sprite_integer(cJSON_GetArrayItem(row, k), k < 2 ? -128 : 0,
                                k < 2 ? 127 : 255)) {
                free(rows); return OD_ERR_FORMAT;
            }
        }
        rows[i].x = (int8_t)(int)cJSON_GetArrayItem(row, 0)->valuedouble;
        rows[i].y = (int8_t)(int)cJSON_GetArrayItem(row, 1)->valuedouble;
        rows[i].tile = (uint8_t)(int)cJSON_GetArrayItem(row, 2)->valuedouble;
        rows[i].attr = (uint8_t)(int)cJSON_GetArrayItem(row, 3)->valuedouble;
    }
    free(*out);
    *out = rows;
    *count = n;
    return OD_OK;
}

od_status od_sprites_prepare_json(od_sprites_candidate **out, const char *json, size_t len)
{
    if (json == NULL || out == NULL)
        return OD_ERR_FORMAT;
    cJSON *doc = cJSON_ParseWithLength(json, len);
    if (doc == NULL)
        return OD_ERR_FORMAT;

    od_pose_part *parts = NULL, *steam_parts = NULL;
    size_t parts_n = 0, steam_parts_n = 0;
    od_pose_entry *poses = NULL;
    size_t poses_n = 0;
    od_steam_phase *phases = NULL;
    size_t phases_n = 0;
    od_status st = load_rows4(cJSON_GetObjectItemCaseSensitive(doc, "pose_parts"),
                                &parts, &parts_n);
    if (!sprite_integer(cJSON_GetObjectItemCaseSensitive(doc, "schema_version"), 1, 1))
        st = OD_ERR_FORMAT;
    if (st == OD_OK)
        st = load_rows4(cJSON_GetObjectItemCaseSensitive(doc, "steam_parts"),
                        &steam_parts, &steam_parts_n);

    const cJSON *banks = cJSON_GetObjectItemCaseSensitive(doc, "world_sprite_banks");
    const cJSON *pose_arr = cJSON_GetObjectItemCaseSensitive(doc, "poses");
    const cJSON *phase_arr = cJSON_GetObjectItemCaseSensitive(doc, "steam_phases");
    const cJSON *bg_banks = cJSON_GetObjectItemCaseSensitive(doc, "bg_banks");
    const cJSON *bg_waits = cJSON_GetObjectItemCaseSensitive(doc, "bg_waits");

    if (st == OD_OK && (!cJSON_IsArray(banks) || cJSON_GetArraySize(banks) != 10))
        st = OD_ERR_FORMAT;
    if (st == OD_OK && (!cJSON_IsArray(pose_arr)))
        st = OD_ERR_FORMAT;
    if (st == OD_OK && (!cJSON_IsArray(phase_arr) || cJSON_GetArraySize(phase_arr) != 6))
        st = OD_ERR_FORMAT;
    if (st == OD_OK && (!cJSON_IsArray(bg_banks) || cJSON_GetArraySize(bg_banks) != 4))
        st = OD_ERR_FORMAT;
    if (st == OD_OK && (!cJSON_IsArray(bg_waits) || cJSON_GetArraySize(bg_waits) != 4))
        st = OD_ERR_FORMAT;

    if (st == OD_OK) {
        poses_n = (size_t)cJSON_GetArraySize(pose_arr);
        poses = (od_pose_entry *)alloc_rows(poses_n, sizeof(*poses));
        phases_n = (size_t)cJSON_GetArraySize(phase_arr);
        phases = (od_steam_phase *)alloc_rows(phases_n, sizeof(*phases));
        if ((poses_n && poses == NULL) || (phases_n && phases == NULL))
            st = OD_ERR_NOMEM;
    }
    for (size_t i = 0; st == OD_OK && i < poses_n; i++) {
        const cJSON *row = cJSON_GetArrayItem(pose_arr, (int)i);
        if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != 5) { st = OD_ERR_FORMAT; break; }
        for (int k = 0; k < 5; ++k)
            if (!sprite_integer(cJSON_GetArrayItem(row, k), 0, k == 0 || k == 2 ? 255 : 65535))
                st = OD_ERR_FORMAT;
        if (st != OD_OK) break;
        poses[i].bank = (uint8_t)(int)cJSON_GetArrayItem(row, 0)->valuedouble;
        poses[i].address = (uint16_t)(int)cJSON_GetArrayItem(row, 1)->valuedouble;
        poses[i].chr_bank = (uint8_t)(int)cJSON_GetArrayItem(row, 2)->valuedouble;
        poses[i].first = (uint16_t)(int)cJSON_GetArrayItem(row, 3)->valuedouble;
        poses[i].count = (uint16_t)(int)cJSON_GetArrayItem(row, 4)->valuedouble;
        if ((size_t)poses[i].first + poses[i].count > parts_n)
            st = OD_ERR_FORMAT;
    }
    for (size_t i = 0; st == OD_OK && i < phases_n; i++) {
        const cJSON *row = cJSON_GetArrayItem(phase_arr, (int)i);
        if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != 3) { st = OD_ERR_FORMAT; break; }
        for (int k = 0; k < 3; ++k)
            if (!sprite_integer(cJSON_GetArrayItem(row, k), k == 0 ? 1 : 0,
                                k == 0 ? 255 : 65535)) st = OD_ERR_FORMAT;
        if (st != OD_OK) break;
        phases[i].wait = (unsigned)cJSON_GetArrayItem(row, 0)->valuedouble;
        phases[i].first = (unsigned)cJSON_GetArrayItem(row, 1)->valuedouble;
        phases[i].count = (unsigned)cJSON_GetArrayItem(row, 2)->valuedouble;
        if ((size_t)phases[i].first + phases[i].count > steam_parts_n)
            st = OD_ERR_FORMAT;
    }
    uint8_t world_banks[10];
    uint8_t bg_banks_v[4][6], bg_waits_v[4][6];
    if (st == OD_OK) {
        for (int i = 0; i < 10; i++) {
            const cJSON *v = cJSON_GetArrayItem(banks, i);
            if (!sprite_integer(v, 0, 255)) { st = OD_ERR_FORMAT; break; }
            world_banks[i] = (uint8_t)(int)v->valuedouble;
        }
    }
    for (int t = 0; st == OD_OK && t < 4; t++) {
        const cJSON *b = cJSON_GetArrayItem(bg_banks, t);
        const cJSON *w = cJSON_GetArrayItem(bg_waits, t);
        if (!cJSON_IsArray(b) || cJSON_GetArraySize(b) != 6 ||
            !cJSON_IsArray(w) || cJSON_GetArraySize(w) != 6) { st = OD_ERR_FORMAT; break; }
        for (int k = 0; k < 6; k++) {
            if (!sprite_integer(cJSON_GetArrayItem(b, k), 0, 255) ||
                !sprite_integer(cJSON_GetArrayItem(w, k), 1, 255)) {
                st = OD_ERR_FORMAT; break;
            }
            bg_banks_v[t][k] = (uint8_t)(int)cJSON_GetArrayItem(b, k)->valuedouble;
            bg_waits_v[t][k] = (uint8_t)(int)cJSON_GetArrayItem(w, k)->valuedouble;
        }
    }
    cJSON_Delete(doc);

    if (st != OD_OK) {
        free(parts); free(steam_parts); free(poses); free(phases);
        return st;
    }
    od_sprites_candidate *c = calloc(1, sizeof *c);
    if (!c) {
        free(parts); free(steam_parts); free(poses); free(phases);
        return OD_ERR_NOMEM;
    }
    c->parts = parts; c->parts_n = parts_n;
    c->steam_parts = steam_parts; c->steam_parts_n = steam_parts_n;
    c->poses = poses; c->poses_n = poses_n;
    c->phases = phases; c->phases_n = phases_n;
    memcpy(c->world_banks, world_banks, sizeof world_banks);
    memcpy(c->bg_banks, bg_banks_v, sizeof bg_banks_v);
    memcpy(c->bg_waits, bg_waits_v, sizeof bg_waits_v);
    *out = c;
    return OD_OK;
}

void od_sprites_commit(od_sprites_candidate *c)
{
    if (!c) return;
    sprites_clear();
    g_pose_parts = c->parts; g_pose_parts_count = c->parts_n;
    g_steam_parts = c->steam_parts; g_steam_parts_count = c->steam_parts_n;
    g_poses = c->poses; g_poses_count = c->poses_n;
    g_steam_phases = c->phases; g_steam_phases_count = c->phases_n;
    memcpy(g_world_sprite_banks, c->world_banks, sizeof g_world_sprite_banks);
    memcpy(g_bg_banks, c->bg_banks, sizeof g_bg_banks);
    memcpy(g_bg_waits, c->bg_waits, sizeof g_bg_waits);
    g_sprites_loaded = 1;
    free(c);
}

od_status od_sprites_load_json(const char *json, size_t len)
{
    od_sprites_candidate *c = NULL;
    od_status st = od_sprites_prepare_json(&c, json, len);
    if (st == OD_OK) od_sprites_commit(c);
    return st;
}

struct od_compose_content_stage {
    od_sprites_candidate *sprites;
    uint8_t arena[2533];
};

void od_compose_content_discard(od_compose_content_stage *s)
{
    if (!s) return;
    od_sprites_candidate_free(s->sprites);
    free(s);
}

od_status od_compose_content_prepare(od_compose_content_stage **out,
    const char *const *json, const size_t *sizes, size_t count,
    const char *arena_json, size_t arena_size)
{
    if (!out || !json || !sizes || !count || !arena_json) return OD_ERR_FORMAT;
    od_compose_content_stage *s = calloc(1, sizeof *s);
    if (!s) return OD_ERR_NOMEM;
    od_status st = OD_OK;
    for (size_t i = 0; i < count && st == OD_OK; ++i) {
        od_sprites_candidate *next = NULL;
        st = od_sprites_prepare_json(&next, json[i], sizes[i]);
        if (st == OD_OK) {
            od_sprites_candidate_free(s->sprites);
            s->sprites = next;
        }
    }
    if (st == OD_OK) st = arena_prepare(s->arena, arena_json, arena_size);
    if (st != OD_OK) { od_compose_content_discard(s); return st; }
    *out = s;
    return OD_OK;
}

void od_compose_content_commit(od_compose_content_stage *s)
{
    if (!s) return;
    od_sprites_commit(s->sprites);
    arena_commit(s->arena);
    free(s);
}


/* --- sub-scanline background fetch --------------------------------------
 * Model of the video chip's background pipeline for one PAL scanline. The tile
 * blit used elsewhere is the ideal result; this keeps the pattern address
 * register, the 16-bit lo/hi shift registers and the register-access timing.
 * See od_ppu_bg_row in compose.h for the contract. */
void od_ppu_bg_row(uint8_t out[256], const uint8_t ciram[2048],
                    const uint32_t bank_off[8], unsigned fine_x,
                    uint16_t v_init, const unsigned *perturb_dots,
                    const int8_t *perturb_delta, unsigned perturb_count,
                    const uint8_t *chr, const od_palette_ram *pal)
{
    uint16_t v = v_init, vbus = 0, par_chr = 0;
    uint16_t bg_lo = 0, bg_hi = 0, attr_lo = 0, attr_hi = 0;
    uint8_t octal = 0, commit = 0, fetch_data = 0;
    uint8_t lo_plane = 0, hi_plane = 0, attribute = 0, attr_latch = 0;
    uint8_t comp[257];
    unsigned pk = 0, dot;

    /* The half dot: shift the 16-bit lo/hi (and attribute) registers and apply
     * the pending fetch commit. A CPU register access can make the PPU miss it
     * (delta < 0) or run it twice (delta > 0). */
    #define OD_HALF(d)                                                          \
    do {                                                                         \
        if (((d) >= 1 && (d) <= 257) || ((d) >= 321 && (d) <= 336)) {            \
            bg_lo = (uint16_t)(bg_lo << 1);                                      \
            bg_hi = (uint16_t)((bg_hi << 1) | 1);                                \
            attr_lo = (uint16_t)((attr_lo << 1) | (attr_latch & 1));             \
            attr_hi = (uint16_t)((attr_hi << 1) | ((attr_latch >> 1) & 1));      \
        }                                                                        \
        if (commit & 1)                                                          \
            par_chr = (uint16_t)((par_chr & 0x100F) | ((vbus & 0xFF) << 4));     \
        if (commit & 2) {                                                        \
            uint8_t a_ = fetch_data;                                             \
            if ((v & 3) >= 2) a_ >>= 2;                                          \
            if (((v >> 5) & 3) >= 2) a_ >>= 4;                                   \
            attribute = a_ & 3;                                                  \
        }                                                                        \
        if (commit & 4) lo_plane = fetch_data;                                   \
        if (commit & 8) {                                                        \
            hi_plane = fetch_data;                                               \
            bg_lo = (uint16_t)((bg_lo & 0xFF00) | lo_plane);                     \
            bg_hi = (uint16_t)((bg_hi & 0xFF00) | hi_plane);                     \
            attr_latch = attribute;                                              \
            if ((v & 0x1F) == 31) v = (uint16_t)((v & 0xFFE0) ^ 0x0400);         \
            else v++;                                                            \
            v &= 0x7FFF;                                                         \
        }                                                                        \
        commit = 0;                                                              \
    } while (0)

    /* One dot: address on odd dots, read on even dots, then the half dot. */
    #define OD_ROW_STEP(d)                                                      \
    do {                                                                         \
        unsigned step_ = ((d) >= 257 && (d) <= 320) ? 1u : (((d) + 7u) & 7u);    \
        bool rd_ = ((d) & 1u) == 0, ale_ = ((d) & 1u) == 1;                      \
        if (step_ == 0) {                                                        \
            vbus = (uint16_t)(0x2000 | (v & 0x0FFF));                            \
        } else if (step_ == 1) {                                                 \
            vbus = (uint16_t)(0x2000 | (v & 0x0F00) | octal);                    \
            {                                                                    \
                uint16_t addr_ = (uint16_t)((vbus & 0x300) | octal |            \
                                            ((vbus & 0x400) ? 0x400 : 0));       \
                fetch_data = ciram[addr_ & 0x7FF];                               \
            }                                                                    \
            vbus = (uint16_t)((vbus & 0xFF00) | fetch_data);                     \
            commit |= 1;                                                         \
        } else if (step_ == 2) {                                                 \
            vbus = (uint16_t)(0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) |        \
                              ((v >> 2) & 0x07));                                \
        } else if (step_ == 3) {                                                 \
            vbus = (uint16_t)(0x2300 | (v & 0x0C00) | octal);                    \
            {                                                                    \
                uint16_t addr_ = (uint16_t)((vbus & 0x300) | octal |            \
                                            ((vbus & 0x400) ? 0x400 : 0));       \
                fetch_data = ciram[addr_ & 0x7FF];                               \
            }                                                                    \
            vbus = (uint16_t)((vbus & 0xFF00) | fetch_data);                     \
            commit |= 2;                                                         \
        } else if (step_ == 4) {                                                 \
            par_chr = (uint16_t)((par_chr & 0x0FF8) | ((v & 0x7000) >> 12));     \
            par_chr &= 0x1FF7;                                                   \
            vbus = par_chr;                                                      \
        } else if (step_ == 5) {                                                 \
            vbus = (uint16_t)((par_chr & 0xFF00) | octal);                       \
            {                                                                    \
                uint16_t addr_ = (uint16_t)(((vbus & 0x3F00) | octal) & 0x1FFF); \
                fetch_data = chr[bank_off[(addr_ >> 10) & 7] | (addr_ & 0x3FF)]; \
            }                                                                    \
            vbus = (uint16_t)((vbus & 0xFF00) | fetch_data);                     \
            commit |= 4;                                                         \
        } else if (step_ == 6) {                                                 \
            par_chr = (uint16_t)((par_chr & 0x0FF8) | ((v & 0x7000) >> 12));     \
            par_chr |= 8;                                                        \
            vbus = par_chr;                                                      \
        } else {                                                                 \
            vbus = (uint16_t)((par_chr & 0xFF00) | octal);                       \
            {                                                                    \
                uint16_t addr_ = (uint16_t)(((vbus & 0x3F00) | octal) & 0x1FFF); \
                fetch_data = chr[bank_off[(addr_ >> 10) & 7] | (addr_ & 0x3FF)]; \
            }                                                                    \
            vbus = (uint16_t)((vbus & 0xFF00) | fetch_data);                     \
            commit |= 8;                                                         \
        }                                                                        \
        if (ale_ && !rd_) octal = (uint8_t)vbus;                                 \
        int delta_ = 0;                                                          \
        if (pk < perturb_count && perturb_dots[pk] == (d)) {                     \
            delta_ = perturb_delta[pk];                                          \
            pk++;                                                                \
        }                                                                        \
        if (delta_ >= 0) {                                                       \
            OD_HALF(d);                                                         \
            if (delta_ > 0) OD_HALF(d);                                         \
        }                                                                        \
    } while (0)

    /* Prefetch that feeds the row's first two tiles, then the visible dots. */
    for (dot = 257; dot <= 340; dot++)
        OD_ROW_STEP(dot);
    for (dot = 1; dot <= 256; dot++) {
        if (dot == 256) {
            if ((v & 0x7000) != 0x7000) v += 0x1000;                 /* inc_y */
            else {
                v &= 0x0FFF;
                int y = (v & 0x03E0) >> 5;
                if (y == 29) { y = 0; v ^= 0x0800; }
                else if (y == 31) y = 0;
                else y++;
                v = (uint16_t)((v & 0xFC1F) | (y << 5));
            }
            v &= 0x7FFF;
        }
        {
            unsigned c = (unsigned)((bg_lo >> (15 - fine_x)) & 1) |
                         (unsigned)(((bg_hi >> (15 - fine_x)) & 1) << 1);
            unsigned p = (unsigned)((attr_lo >> (7 - fine_x)) & 1) |
                         (unsigned)(((attr_hi >> (7 - fine_x)) & 1) << 1);
            if (c == 0) p = 0;
            comp[dot] = (uint8_t)(c == 0 ? pal->ram[0] : pal->ram[p * 4 + c]);
        }
        OD_ROW_STEP(dot);
    }
    #undef OD_ROW_STEP
    #undef OD_HALF

    for (dot = 0; dot < 256; dot++)
        out[dot] = comp[dot + 1];
}

unsigned od_hero_pose(od_sprite *out, unsigned capacity, unsigned bank,
                       uint16_t pose, int anchor_x, int anchor_y, int facing)
{
    for (size_t i = 0; i < g_poses_count; i++) {
        const od_pose_entry *p = &g_poses[i];
        if (p->bank != bank || p->address != pose)
            continue;
        if (!out || capacity < p->count)
            return 0;
        for (unsigned j = 0; j < p->count; j++) {
            const od_pose_part *s = &g_pose_parts[p->first + j];
            out[j].x = (uint8_t)(anchor_x + (facing < 0 ? -s->x - 8 : s->x));
            /* underflow at the top skips the offscreen row. */
            int origin_y = anchor_y ? anchor_y - 1 : 0xEF;
            out[j].y = (uint8_t)(origin_y + s->y) + 1;
            out[j].tile = (uint16_t)((p->chr_bank & 0xFE) * 64 + s->tile);
            out[j].attr = s->attr ^ (facing < 0 ? 0x40 : 0);
        }
        return p->count;
    }
    return 0;
}

void od_compose_present(od_index_frame *fb, const od_presentation *p,
                         const uint8_t *chr)
{
    for (int y = 0; y < OD_SCREEN_H; y++) {
        unsigned selected[8], count = 0;
        for (unsigned i = 0; i < p->sprite_count && i < 64 && count < 8; i++) {
            if (y >= p->sprites[i].y && y < p->sprites[i].y + 8)
                selected[count++] = i;
        }
        /* the background is read (y + scroll) lines down, wrapping in the
         * 240-line field; sprites stay in screen space. */
        unsigned by = ((unsigned)y + p->scroll_y) % 240u;
        for (int x = 0; x < OD_SCREEN_W; x++) {
            unsigned tile = p->tiles[by / 8][x / 8];
            uint8_t bg = od_tile_pixel(chr + tile * OD_TILE_BYTES, x & 7, by & 7);
            unsigned palette = p->palettes[by / 8][x / 8] & 3;
            if (p->lava_on && y >= 208 && y < 216) {
                /* The strip's tile row starts two lines above line 208; its last two
                 * lines belong to the next tile row, which is blank. */
                unsigned sx = (unsigned)x + p->lava_x, line = (unsigned)y - 206u;
                bg = line < 8 ? od_tile_pixel(chr + (p->lava_base + (sx / 8) % 4) *
                                              OD_TILE_BYTES, sx & 7, line) : 0;
                palette = 0;
            }
            uint8_t color = p->palette.ram[bg ? palette * 4 + bg : 0];
            for (unsigned i = 0; i < count; i++) {
                const od_sprite *s = &p->sprites[selected[i]];
                int sx = x - s->x, sy = y - s->y;
                if (sx < 0 || sx >= 8)
                    continue;
                uint8_t v = od_tile_pixel(chr + s->tile * OD_TILE_BYTES,
                                          s->attr & 0x40 ? 7 - sx : sx,
                                          s->attr & 0x80 ? 7 - sy : sy);
                if (!v)
                    continue;
                if (!(s->attr & 0x20) || !bg)
                    color = p->palette.ram[16 + (s->attr & 3) * 4 + v];
                /* Even a sprite hidden behind BG wins over later sprites. */
                break;
            }
            fb->index[y * OD_SCREEN_W + x] = color & 63;
        }
    }
}

/* The ideal dot of the split handler's scroll write, in output pixels. The real
 * dot depends on the handler's timing; frames whose phase moves the write to the
 * next/previous tile fetch keep a few pixels of that artefact. The measured
 * canonical value is 199-204; 200 is the tile boundary the trace's dot 170
 * lands on after the fetch pipeline. */
#define OD_COMPOSE_SPLIT_X 200u

void od_compose_scene(od_index_frame *fb, const uint16_t *d,
                       unsigned scene, unsigned age,
                       unsigned visible_selection, const uint8_t *chr)
{
    unsigned band = 0, bands = d[2093] > 5 ? 5 : d[2093];
    unsigned sprite_count = d[2124] > 64 ? 64 : d[2124];
    for (unsigned y = 0; y < OD_SCREEN_H; ++y) {
        while (band+1 < bands && y >= d[2094+(band+1)*6]) ++band;
        const uint16_t *b = d+2094+band*6;
        /* Split row: the scanline above the next boundary. Keep this band's
         * coarse X / nametable / fine Y, but latch the next band's fine X. */
        unsigned bsx = b[1];
        int split = 0;
        if (band+1 < bands && y == (unsigned)d[2094+(band+1)*6] - 1) {
            const uint16_t *nb = d+2094+(band+1)*6;
            unsigned fine_in = nb[1] & 7u;
            unsigned bsx_fine = (b[1] & ~7u) | fine_in;
            if (bsx_fine != b[1]) { bsx = bsx_fine; split = 1; }
        }
        unsigned row_sprites[8], count = 0;
        for (unsigned i = 0; i < sprite_count && count < 8; ++i) {
            unsigned sy = d[2125+i*4+1];
            if (y >= sy && y < sy+8) row_sprites[count++] = i;
        }
        for (unsigned x = 0; x < OD_SCREEN_W; ++x) {
            unsigned color = d[0], opaque = 0;
            if ((d[2092]&8) && (x >= 8 || (d[2092]&2))) {
                unsigned row_sx = (split && x < OD_COMPOSE_SPLIT_X) ? b[1] : bsx;
                unsigned sx = x+row_sx, sy = y+b[2];
                unsigned nt = b[3] ^ ((sx/256)&1) ^ (((sy/240)&1)<<1);
                sx %= 256; sy %= 240;
                unsigned base = (d[2381]&2) ? (nt&2)*512 : (nt&1)*1024;
                unsigned tx = sx/8, ty = sy/8;
                unsigned tile = d[32+base+ty*32+tx];
                if (scene == OD_COMPOSE_SCENE_MENU && age > 29 && ty >= 19 &&
                    ty <= 23 && (ty&1) && tx == 17)
                    tile = ty == 19+visible_selection*2 ? 254 : 0;
                unsigned bank = d[2080+(b[4]&1)*4+tile/64];
                opaque = od_tile_pixel(chr + (size_t)(bank+tile%64)*OD_TILE_BYTES,
                                        sx&7, sy&7);
                unsigned attr = d[32+base+960+(ty/4)*8+tx/4];
                unsigned palette = (attr >> (((ty&2)<<1)|(tx&2))) & 3;
                if (opaque) color = d[palette*4+opaque];
            }
            if ((d[2092]&16) && (x >= 8 || (d[2092]&4))) {
                for (unsigned k = 0; k < count; ++k) {
                    const uint16_t *sp = d+2125+row_sprites[k]*4;
                    if (x < sp[0] || x >= (unsigned)sp[0]+8) continue;
                    unsigned sx = x-sp[0], sy = y-sp[1], attr = sp[3];
                    if (attr&64) sx ^= 7;
                    if (attr&128) sy ^= 7;
                    unsigned tile = d[2088+(b[5]&1)*2+sp[2]/128]+sp[2]%128;
                    unsigned value = od_tile_pixel(chr + (size_t)tile*OD_TILE_BYTES,
                                                    sx&7, sy&7);
                    if (!value) continue;
                    if (!(attr&32) || !opaque) color = d[16+(attr&3)*4+value];
                    break;
                }
            }
            fb->index[y*OD_SCREEN_W+x] = (uint8_t)(color&63);
        }
    }
}

/* Legacy tile/idle-pose IDs are package data, not composition constants. */

#define OD_CELL_EMPTY 0x00
#define OD_CELL_SOLID 0x01
#define OD_CELL_ICE   0x05
#define OD_CELL_FIRE  0x0E

static void put_cell(od_index_frame *fb, int cx, int cy, const uint16_t tiles[4],
                     uint8_t bg_pal, const uint8_t *chr, const od_palette_ram *pal)
{
    for (int k = 0; k < 4; k++) {
        const uint8_t *t = chr + (size_t)tiles[k] * OD_TILE_BYTES;
        int ox = k & 1, oy = k >> 1;
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint8_t v = od_tile_pixel(t, x, y);
                int px = cx * 16 + ox * 8 + x;
                int py = cy * 16 + oy * 8 + y;
                fb->index[py * OD_SCREEN_W + px] = pal->ram[bg_pal * 4 + v];
            }
        }
    }
}

void od_frame_clear(od_index_frame *fb, uint8_t bg_index)
{
    memset(fb->index, bg_index, sizeof(fb->index));
}

void od_render_cells(od_index_frame *fb, const uint8_t cells[OD_GRID_H][OD_GRID_W],
                      const uint8_t *chr, const od_palette_ram *pal)
{
    const od_presentation_data *data = od_presentation_data_get();
    if (!data) return; /* Loader diagnoses absence; do not render invented IDs. */
    for (int cy = 0; cy < OD_GRID_H; cy++) {
        for (int cx = 0; cx < OD_GRID_W; cx++) {
            uint8_t type = cells[cy][cx];
            const uint16_t *tiles;
            uint8_t bg_pal;

            switch (type) {
            case OD_CELL_EMPTY:
                tiles = data->cell_tiles[0];
                bg_pal = data->cell_palettes[0];
                break;
            case OD_CELL_SOLID: {
                /* Out-of-grid reads are asymmetric in the captured frame: a
                 * missing cell on the left counts as empty (left edge), a
                 * missing cell on the right counts as solid. */
                int left_empty = (cx == 0) ? 1 : (cells[cy][cx - 1] != OD_CELL_SOLID);
                int right_empty = (cx == OD_GRID_W - 1) ? 0
                                                         : (cells[cy][cx + 1] != OD_CELL_SOLID);
                unsigned variant = left_empty ? 2 : right_empty ? 3 : 1;
                tiles = data->cell_tiles[variant];
                bg_pal = data->cell_palettes[variant];
                break;
            }
            case OD_CELL_ICE:
                tiles = data->cell_tiles[4];
                bg_pal = data->cell_palettes[4];
                break;
            case OD_CELL_FIRE:
                tiles = data->cell_tiles[5];
                bg_pal = data->cell_palettes[5];
                break;
            default:
                /* Unlabelled types fall back to the empty floor for now. */
                tiles = data->cell_tiles[0];
                bg_pal = data->cell_palettes[0];
                break;
            }
            put_cell(fb, cx, cy, tiles, bg_pal, chr, pal);
        }
    }
}

void od_render_level_field(od_index_frame *fb, const od_level *lv,
                            const uint8_t *chr, const od_palette_ram *pal)
{
    od_render_cells(fb, lv->grid, chr, pal);
}

/* World-aware background using the per-world tiles + palette from the world
 * graphics module. Returns 0 when the level's world graphics are not
 * characterised (hidden levels 100..149), so the caller falls back to the
 * world-1 tables above. */
static int render_cells_world(od_index_frame *fb,
                              const uint8_t cells[OD_GRID_H][OD_GRID_W],
                              int level_index, const uint8_t *chr,
                              const od_palette_ram *pal)
{
    (void)pal; /* the world's own palette RAM is used for the background */
    const od_world_gfx *w = od_world_gfx_for_level(level_index);
    if (!w)
        return 0;

    od_palette_ram wp;
    memcpy(wp.ram, w->palette_ram, sizeof(wp.ram));

    for (int cy = 0; cy < OD_GRID_H; cy++) {
        for (int cx = 0; cx < OD_GRID_W; cx++) {
            uint8_t type = (uint8_t)(cells[cy][cx] & 0x0F);
            /* Out-of-grid reads match the captured frame: missing-left = empty,
             * missing-right = solid.  resolve() owns that asymmetry. */
            int left_type = (cx == 0) ? -1 : cells[cy][cx - 1];
            int right_type = (cx == OD_GRID_W - 1) ? -1 : cells[cy][cx + 1];
            od_gfx_tiles out;
            if (od_world_gfx_resolve(w, type, left_type, right_type, &out) != 0) {
                if (od_world_gfx_resolve(w, OD_CELL_EMPTY, -1, -1, &out) != 0)
                    continue;
            }
            put_cell(fb, cx, cy, out.tiles, out.bg_pal, chr, &wp);
        }
    }
    return 1;
}

static void put_sprite_layer(od_index_frame *fb, int dx, int dy,
                             const uint16_t tiles[3][2], uint8_t sprite_pal, int flip,
                             const uint8_t *chr, const od_palette_ram *pal)
{
    uint8_t base = (uint8_t)(16 + sprite_pal * 4);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 2; c++) {
            const uint8_t *t = chr + (size_t)tiles[r][flip ? 1 - c : c] * OD_TILE_BYTES;
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    int src = flip ? (7 - x) : x;
                    uint8_t v = od_tile_pixel(t, src, y);
                    if (v == 0)
                        continue; /* transparent */
                    int px = dx + c * 8 + x;
                    int py = dy + r * 8 + y;
                    if (px < 0 || px >= OD_SCREEN_W || py < 0 || py >= OD_SCREEN_H)
                        continue;
                    fb->index[py * OD_SCREEN_W + px] = pal->ram[base + v];
                }
            }
        }
    }
}

void od_render_hero(od_index_frame *fb, int cx, int cy, int facing,
                      const uint8_t *chr, const od_palette_ram *pal)
{
    const od_presentation_data *data = od_presentation_data_get();
    if (!data) return;
    int flip = (facing < 0) ? 1 : 0;
    /* Sprite Y stores the screen row minus one, so the visible pixels start one row
     * below it. This legacy API is the idle pose only; use od_hero_pose for live
     * poses. */
    int dx = cx * 16 + 1;
    int dy = (cy + 1) * 16 - 24;
    put_sprite_layer(fb, dx, dy, data->player_tiles[1], 1, flip, chr, pal);
    put_sprite_layer(fb, dx, dy, data->player_tiles[0], 0, flip, chr, pal);
}

/* --- moving flames -------------------------------------------------------
 * Every live controller slot draws the metasprite its tick selected
 * (`od_flame.pose`, package poses of the flame bank) around its integer centre:
 * x = state[7] minus the scroll (0 on a one-screen level) and y = state[4].
 * With bit 7 of state[0] set the part x mirrors to -x-8 and the attribute flips.
 * Slots are drawn 7..0, which is the emission order. Each pose first stores its
 * tile-bank byte, which the video update uploads to the sprite tile bank, so
 * every flame tile of the frame comes from the bank of the LAST slot drawn. The
 * part y is written as the sprite Y byte; the compositor row is one below it. */
static const od_pose_entry *flame_pose_entry(uint16_t address)
{
    for (size_t i = 0; i < g_poses_count; i++)
        if (g_poses[i].bank == 3 && g_poses[i].address == address)
            return &g_poses[i];
    return NULL;
}

static void present_flames(od_presentation *p, const od_game *g)
{
    const od_pose_entry *pose[OD_FLAME_MAX] = {0};
    int count = g->flame_count < OD_FLAME_MAX ? g->flame_count : OD_FLAME_MAX;
    unsigned r1 = 0;
    for (int i = count - 1; i >= 0; i--) {
        if (!g->flames[i].type) continue;
        pose[i] = flame_pose_entry(g->flames[i].pose);
        if (pose[i]) r1 = pose[i]->chr_bank;
    }
    for (int i = count - 1; i >= 0; i--) {
        const od_flame *f = &g->flames[i];
        if (!pose[i]) continue;
        int mirror = (f->state[0] & 0x80) != 0;
        for (unsigned j = 0; j < pose[i]->count && p->sprite_count < 64; j++) {
            const od_pose_part *s = &g_pose_parts[pose[i]->first + j];
            p->sprites[p->sprite_count++] = (od_sprite){
                (uint8_t)(f->state[7] + (mirror ? -s->x - 8 : s->x)),
                (uint8_t)(f->state[4] + s->y) + 1,
                (uint16_t)((r1 & 0xFE) * 64 + (s->tile & 0x7F)),
                (uint8_t)(s->attr ^ (mirror ? 0x40 : 0))};
        }
    }
}

unsigned od_background_bank(unsigned slot, unsigned world, unsigned long ticks)
{
    const uint8_t *banks, *waits;
    if (slot == 2) {
        banks = g_bg_banks[0]; waits = g_bg_waits[0];
    } else if (slot == 4 && (world == 9 || world == 10)) {
        banks = g_bg_banks[3]; waits = g_bg_waits[3];
    } else if (slot == 4) {
        banks = g_bg_banks[1]; waits = g_bg_waits[1];
    } else if (slot == 5) {
        banks = g_bg_banks[2]; waits = g_bg_waits[2];
    } else return 0;
    unsigned period = 0;
    for (unsigned i = 0; i < 6; i++) period += waits[i];
    ticks %= period;
    unsigned phase = 0;
    while (ticks >= waits[phase]) ticks -= waits[phase++];
    return banks[phase];
}

/* The tile bank for the playfield patterns covers every pattern 0..63, not only
 * the ice: the hazard, stone and pipe patterns follow the same animation
 * phases. The package stores them in the bank of the first phase. */
static void animate_r2_tiles(od_gfx_tiles *t, unsigned world, unsigned long ticks)
{
    unsigned first = od_background_bank(2, world, 0);
    unsigned bank = od_background_bank(2, world, ticks);
    for (unsigned k = 0; k < 4; k++)
        if (t->tiles[k] / 64 == first)
            t->tiles[k] = (uint16_t)(bank * 64 + (t->tiles[k] & 63));
}

/* Screen Y of an object at field line `y` under the vertical scroll, the
 * shared prologue of the moving block and the steam: 240 minus the scroll
 * (0 when the scroll is 0), plus y with an overflow adding 16 (256 is 16 mod
 * 240), and the top sixteen values folded to 0..15. Objects drawn without it
 * sat the scroll value off the scrolled background on the auto-scrolling rounds
 * and jumped when they settled into it. */
static unsigned scrolled_object_y(unsigned y, unsigned scroll)
{
    unsigned a = (0xF0u - scroll % 240u) & 0xFFu;
    if (a == 0xF0u)
        a = 0;
    a += y & 0xFFu;
    if (a > 0xFFu)
        a = (a & 0xFFu) + 0x10u;
    a &= 0xFFu;
    if (a >= 0xF0u)
        a &= 0x0Fu;
    return a;
}

/* The hero's sprites for this tick. While she travels a connector the sprite
 * routine draws the pipe segment of the cell she last stepped from, centred on
 * that cell, instead of her pose (before, she vanished until she came out). The
 * segments are the package's pipe poses, one per pipe type in table order. */
static unsigned hero_sprites(od_sprite *out, unsigned capacity,
                             const od_game *g)
{
    if (g->pipe_active && g->pipe_prev < OD_GRID_W * OD_GRID_H) {
        unsigned cx = g->pipe_prev % OD_GRID_W, cy = g->pipe_prev / OD_GRID_W;
        unsigned type = g->cells[cy][cx] & 15;
        if (type >= 6 && type <= 11) {
            unsigned want = type - 6, seen = 0;
            for (size_t i = 0; i < g_poses_count; i++) {
                if (g_poses[i].bank != 0 || seen++ != want)
                    continue;
                return od_hero_pose(out, capacity, 0, g_poses[i].address,
                                    (int)(cx * 16 + 8),
                                    (int)scrolled_object_y((cy * 16u) | 8u,
                                                           g->scroll_y), 1);
            }
        }
    }
    return od_hero_pose(out, capacity, g->hero_set, (uint16_t)g->hero_pose,
                        g->hero_px, g->hero_py, g->facing);
}

static void present_game(od_presentation *p, const od_game *g,
                         const od_palette_ram *pal, const unsigned long *ticks,
                         int emit_flames)
{
    memset(p, 0, sizeof(*p));
    const od_world_gfx *w = od_world_gfx_for_level(g->level_index);
    if (!w) w = od_world_gfx_for_world(0);
    p->scroll_y = g->scroll_y % 240u; /* scroll of the auto-scrolling rounds */
    if (g->scroll_enabled && ticks) {
        /* The strip's tiles follow the fire bank's animation (the table of worlds 1..8 in
         * every world, unlike the fire cells of world 9); its sideways scroll is the
         * game's `lava_x` (C6.74). */
        unsigned long tick = *ticks;
        p->lava_on = 1;
        p->lava_base = od_background_bank(4, 0, tick) * 64u + 4u; /* world 9 included */
        /* The strip is drawn at the frame's split, once that frame has counted: what the next tick
         * will count is already in it. */
        p->lava_x = (g->lava_x + (unsigned)od_game_lava_shows_next(g)) % 32u;
    }
    p->palette = *pal;
    if (w) memcpy(p->palette.ram, w->palette_ram, sizeof(p->palette.ram));
    /* The first step of a connector transit loads a palette entry (sprite palette 0,
     * colour 2) and re-uploads it, so the pipe segment is drawn in the pipe's pink;
     * the last step puts the usual colour back. Same window as the connector flag. */
    if (g->pipe_active) p->palette.ram[0x12] = 0x26;
    for (int cy = 0; cy < OD_GRID_H; cy++) {
        for (int cx = 0; cx < OD_GRID_W; cx++) {
            od_gfx_tiles cell;
            unsigned type = g->cells[cy][cx] & 15;
            int left = cx == 0 ? -1 : (g->cells[cy][cx - 1] & 15);
            int right = cx < 15 ? (g->cells[cy][cx + 1] & 15) : -1;
            if (od_world_gfx_resolve(w, (uint8_t)type, left, right, &cell))
                continue;
            /* Known gameplay background IDs: fire (animated bank), connected ice (second
             * animated bank). Resolve banks from the animation phase tables rather than
             * retaining the phase of the old world graphics capture. */
            if (ticks && (type == 14 || (type >= 2 && type <= 5))) {
                unsigned bank = od_background_bank(type == 14 ? 4 : 2,
                                                       od_game_world(g), *ticks);
                for (unsigned k = 0; k < 4; k++)
                    cell.tiles[k] = (uint16_t)(bank * 64 + (cell.tiles[k] & 63));
            }
            if (ticks && type != 14)
                animate_r2_tiles(&cell, (unsigned)od_game_world(g), *ticks);
            for (unsigned k = 0; k < 4; k++) {
                p->tiles[cy * 2 + k / 2][cx * 2 + k % 2] = cell.tiles[k];
                p->palettes[cy * 2 + k / 2][cx * 2 + k % 2] = cell.bg_pal;
            }
        }
    }
    /* The game already computes the asymmetric pixel anchor and pose. Resolve it
     * verbatim; deriving the pose from action or cell loses held phases. */
    p->sprite_count = hero_sprites(p->sprites, 64, g);
    /* Moving ice uses four tiles, palette 0, in top-left/top-right/bottom-left/
     * bottom-right order. Its tile bank comes from the package, separately from the
     * hero's. A zero X suppresses the object, including after its termination phase. */
    if (g->ice_px && p->sprite_count <= 60) {
        unsigned world_index = g->level_index >= 0 && g->level_index < 100
                             ? (unsigned)g->level_index / 10 : 0;
        unsigned base = (g_world_sprite_banks[world_index] & 0xFE) * 64;
        /* the field Y minus the vertical scroll, then minus one (0 wraps to the
         * top row). */
        unsigned a = scrolled_object_y((unsigned)g->ice_py, g->scroll_y);
        int origin_y = a ? (int)a - 1 : 0xEF;
        /* A lone stone in flight is mirrored into the ice fields, but its four tiles
         * follow the ice's in the bank. */
        unsigned kind = (!g->pending && g->stone.active) ? 4 : 0;
        for (unsigned k = 0; k < 4; k++) {
            od_sprite *s = &p->sprites[p->sprite_count++];
            *s = (od_sprite){(uint8_t)(g->ice_px - 8 + (int)(k % 2) * 8),
                             (uint8_t)(origin_y - 8 + (int)(k / 2) * 8) + 1,
                             (uint16_t)(base + 0x50 + kind + k), 0};
        }
    }
    if (emit_flames)
        present_flames(p, g);
}

void od_present_game(od_presentation *p, const od_game *g,
                      const od_palette_ram *pal)
{
    present_game(p, g, pal, NULL, 1);
}

/* qimg.pal marker: nothing to redraw for the queued cell. */
#define OD_QIMG_NONE 0xFE

/* Background palette index a cell renders with under `cells` (the world's
 * per-cell palette), used to keep the falling block's destination in its
 * pre-move palette until the attribute is rewritten. */
static uint8_t cell_bg_pal(const od_game *g,
                           const uint8_t cells[OD_GRID_H][OD_GRID_W],
                           unsigned cell)
{
    if (cell >= OD_GRID_W * OD_GRID_H) return 1;
    const od_world_gfx *w = od_world_gfx_for_level(g->level_index);
    if (!w) w = od_world_gfx_for_world(0);
    if (!w) return 1;
    unsigned cx = cell & 15, cy = cell >> 4;
    int left = cx == 0 ? -1 : (cells[cy][cx - 1] & 15);
    int right = cx < 15 ? (cells[cy][cx + 1] & 15) : -1;
    od_gfx_tiles t;
    if (od_world_gfx_resolve(w, cells[cy][cx] & 15, left, right, &t) != 0)
        return 1;
    return t.bg_pal;
}

/* Background column writes of the boss room (od_visual_state.colw). */
/* The grid byte as the background sees it: the attack carves its hazard types (0x0F, 0x10) into the
 * grid without writing the background, so they count as empty. */
static uint8_t colw_grid(const od_game *g, unsigned cell)
{
    uint8_t raw = g->cells[cell >> 4][cell & 15];
    return raw == 0x0F || raw == 0x10 ? 0 : raw;
}

/* Phase of the falling blocks that paint `cell` (their source and the cell below it): it changes
 * whenever the fall writer rewrites the cells it passes. */
static uint8_t colw_fall_sig(const od_visual_state *v, unsigned cell)
{
    unsigned sig = 0;
    for (unsigned i = 0; i < v->fall_count; i++) {
        if (v->fall[i].cell != cell && v->fall[i].cell + (unsigned)OD_GRID_W != cell) continue;
        unsigned w = v->fall[i].wait;
        sig = sig * 31 + v->fall[i].cell * 3 + (w == 0 ? 0 : w > 4 ? 1 : 2);
    }
    return (uint8_t)sig;
}

static void colw_clear(od_visual_state *v, unsigned cell, unsigned side)
{
    unsigned k = 0;
    for (unsigned i = 0; i < v->colw_count; i++)
        if (!(v->colw[i].cell == cell && v->colw[i].side == side))
            v->colw[k++] = v->colw[i];
    v->colw_count = k;
}

static void colw_note(od_visual_state *v, const od_game *g, unsigned src, unsigned cell,
                      unsigned side, unsigned type, unsigned nb, int attack)
{
    colw_clear(v, cell, side);
    if (v->colw_count == sizeof v->colw / sizeof v->colw[0]) {
        memmove(&v->colw[0], &v->colw[1], sizeof v->colw - sizeof v->colw[0]);
        v->colw_count--;
    }
    v->colw[v->colw_count].cell = (uint8_t)cell;
    v->colw[v->colw_count].side = (uint8_t)side;
    v->colw[v->colw_count].type = (uint8_t)type;
    v->colw[v->colw_count].nb = (uint8_t)nb;
    v->colw[v->colw_count].src = (uint8_t)src;
    v->colw[v->colw_count].sraw = colw_grid(g, src);
    v->colw[v->colw_count].mraw = colw_grid(g, cell);
    v->colw[v->colw_count].by_attack = (uint8_t)attack;
    v->colw[v->colw_count].fsig = colw_fall_sig(v, cell);
    v->colw_count++;
}

/* An entry whose source or margin cell changed in the grid, or whose falling blocks changed phase,
 * was covered by another write: it is gone for good, not only hidden while the change lasts. */
static void colw_prune(od_visual_state *v, const od_game *g)
{
    unsigned k = 0;
    for (unsigned i = 0; i < v->colw_count; i++) {
        unsigned cell = v->colw[i].cell, src = v->colw[i].src;
        if (cell < OD_GRID_W * OD_GRID_H && src < OD_GRID_W * OD_GRID_H &&
            colw_grid(g, cell) == v->colw[i].mraw && colw_grid(g, src) == v->colw[i].sraw &&
            colw_fall_sig(v, cell) == v->colw[i].fsig)
            v->colw[k++] = v->colw[i];
    }
    v->colw_count = k;
}

/* The attack's entries live until its next script step. */
static void colw_drop_attack(od_visual_state *v)
{
    unsigned k = 0;
    for (unsigned i = 0; i < v->colw_count; i++)
        if (!v->colw[i].by_attack) v->colw[k++] = v->colw[i];
    v->colw_count = k;
}

/* The writes of one cell `x`: its own two columns replace whatever was logged there and the
 * facing column of each horizontal neighbour is taken from the grid now. The attack's writer skips
 * a neighbour of the wall type (0x10) and looks the neighbour up by its own type only; the effect
 * drain's span skips only the column facing a wall type on its left and also uses the cell it faces. */
static void colw_cell(od_visual_state *v, const od_game *g, unsigned x, int attack)
{
    if (x >= OD_GRID_W * OD_GRID_H) return;
    unsigned cx = x & 15, cy = x >> 4;
    unsigned self = g->cells[cy][cx] & 15;
    colw_clear(v, x, 0);
    colw_clear(v, x, 1);
    /* The span writer indexes the right column of a cell by (type, raw byte of its right neighbour):
     * a wall neighbour (0x10) runs the index into the next type's row, so the column shown is the
     * one of the following type beside nothing. */
    if (!attack && cx < OD_GRID_W - 1 && g->cells[cy][cx + 1] == 0x10)
        colw_note(v, g, x, x, 1, (self + 1) & 15, 0, 0);
    if (cx > 0) {
        unsigned raw = g->cells[cy][cx - 1];
        if (raw != 0x10) colw_note(v, g, x, x - 1, 1, raw & 15, attack ? 0 : self, attack);
    }
    if (cx < OD_GRID_W - 1) {
        unsigned raw = g->cells[cy][cx + 1];
        if (raw != 0x10 || !attack) colw_note(v, g, x, x + 1, 0, raw & 15, attack ? 0 : self, attack);
    }
}

void od_visual_attack_step(od_visual_state *v, const od_game *g, const od_boss_attack *a)
{
    v->colw_on = 1;
    if (!a || !a->alive) {
        if (v->attack_seen) colw_drop_attack(v);
        v->attack_seen = 0;
        return;
    }
    const od_engine_tables *t = od_engine_tables_get();
    unsigned count = a->state[3];
    if (count > 31) count = 31;
    if (count < v->attack_seen) v->attack_seen = 0;
    if (!t) return;
    if (count != v->attack_seen) colw_drop_attack(v);
    for (unsigned i = v->attack_seen; i < count; i++) {
        unsigned cursor = t->boss_attack_offset[i];
        while (cursor + 1 < OD_BOSS_ATTACK_DATA && t->boss_attack_data[cursor] != 0xff) {
            colw_cell(v, g, t->boss_attack_data[cursor], 1);
            cursor += 2;
        }
    }
    v->attack_seen = (uint8_t)count;
}

void od_visual_init(od_visual_state *v, const od_game *g)
{
    memset(v, 0, sizeof(*v));
    v->game_frame = g->frame;
}

int od_visual_tick(od_visual_state *v, const od_game *before,
                     const od_game *after)
{
    if (v->game_frame != before->frame || after->frame != before->frame + 1)
        return 0;
    od_visual_state next = *v;
    unsigned added = 0;
    colw_prune(&next, after);
    /* Pushed block and cascade spans. The game already resolved these contacts;
     * presentation only retains their event cells. */
    if (after->fires < before->fires) {
        if (after->ice_state == 4 && before->ice_state != 4) {
            unsigned cell = (unsigned)(after->ice_py / 16 * 16 + after->ice_px / 16);
            if (cell < 240 && before->cells[cell / 16][cell % 16] == 14 && next.queued < 16) {
                next.qimg[next.queued].type = OD_CELL_FIRE;
                next.qimg[next.queued].pal = 0xFF;
                next.queue[next.queued++] = (uint8_t)cell;
                added++;
            }
        }
        /* A stone that put a fire out and slid on (an ice floor under it, the way ahead
         * free) is not consumed: its cell is the fire it just crossed. */
        if (after->stone.active) {
            unsigned cell = (unsigned)(after->stone.py / 16 * 16 + after->stone.px / 16);
            int queued = 0;
            for (unsigned i = 0; i < next.queued; i++)
                if (next.queue[i] == cell) queued = 1;
            if (!queued && cell < 240 && before->cells[cell / 16][cell % 16] == 14 &&
                after->cells[cell / 16][cell % 16] != 14 && next.queued < 16) {
                next.qimg[next.queued].type = OD_CELL_FIRE;
                next.qimg[next.queued].pal = 0xFF;
                next.queue[next.queued++] = (uint8_t)cell;
                added++;
            }
        }
        if (after->cascade_state == 0x83 && !(after->cascade_width & 128)) {
            /* Each put-out column is queued right to left, so the drain (last in, first
             * out) steams the leftmost first (reference 1-6 f5421: every 4 frames). */
            unsigned w = after->cascade_width;
            for (unsigned i = w; i-- > 0;) {
                unsigned cell = after->cascade_cell + i;
                if (cell < 240 && before->cells[cell / 16][cell % 16] == 14 && next.queued < 16) {
                    /* The landed run stays drawn in the fire's palette until
                     * its effect clears the cell (reference 1-6 f5422..f5466:
                     * the bar in pink, steamed away from the left). */
                    next.qimg[next.queued].type = before->drop_tiles[i];
                    next.qimg[next.queued].pal = cell_bg_pal(after, before->cells, cell);
                    next.qimg[next.queued].left = i ? before->drop_tiles[i - 1] : 0;
                    next.qimg[next.queued].right = i + 1 < w ? before->drop_tiles[i + 1] : 0;
                    next.queue[next.queued++] = (uint8_t)cell;
                    added++;
                }
            }
        }
        /* C6.47: a put-out flame lowers the counter without a fire cell (its effect is
         * retained by the hazard / shattered-column events below). */
        if (added + after->flame_kills != (unsigned)(before->fires - after->fires)) return 0;
    }
    /* Shattered columns are queued too, even without extinguishing a fire.
     * Missing these events rejected the visual tick and froze its clock (and the
     * last falling tiles) permanently. */
    if (after->cascade_state == 0x83 && before->cascade_state != 0x83 &&
        !(after->cascade_width & 128)) {
        for (unsigned i = after->cascade_width; i-- > 0;) {
            unsigned cell = after->cascade_cell + i;
            if (cell >= 240 || before->drop_tiles[i] == 0x0c) continue;
            unsigned below = (cell + 16) % 240;
            unsigned type = before->cells[cell / 16][cell % 16];
            unsigned support = after->cells[below / 16][below % 16];
            /* A column stopped by fire was queued above; only the ones the hazard stopped
             * are added here. */
            if (type != 14 && (support == 15 || support == 16 ||
                                (after->cascade_stop_mask >> i & 1u)) && next.queued < 16) {
                /* The column the flame or the hazard stopped stays drawn as the ice it
                 * fell with, in its own palette, until the queued effect steams it away
                 * (C6.72, 2-10 f17125..f17127, 10-10 f25171..f25173). */
                uint8_t cells[OD_GRID_H][OD_GRID_W];
                memcpy(cells, before->cells, sizeof(cells));
                cells[cell / 16][cell % 16] = before->drop_tiles[i];
                next.qimg[next.queued].type = before->drop_tiles[i];
                next.qimg[next.queued].pal =
                    cell_bg_pal(after, (const uint8_t (*)[OD_GRID_W])cells, cell);
                next.qimg[next.queued].left = i ? before->drop_tiles[i - 1] : 0;
                next.qimg[next.queued].right = i + 1 < after->cascade_width ?
                                               before->drop_tiles[i + 1] : 0;
                next.queue[next.queued++] = (uint8_t)cell;
                added++;
            }
        }
    }
    /* A moving ice block destroyed by a hazard (or by the boss) queues its own
     * cell too. */
    if (after->hazard_effect && next.queued < 16) {
        /* A moving flame put out by the block has no background tile, so nothing is
         * pending for the cell (OD_QIMG_NONE); a fire cell or a hazard keeps the fire
         * image until the effect (C6.72, 6-10 f19651). */
        next.qimg[next.queued].type = OD_CELL_FIRE;
        /* A block that slid through an empty cell leaves nothing to restore there either: the
         * background of that cell is only written when the effect lands. */
        unsigned hc = (unsigned)after->hazard_effect - 1u;
        next.qimg[next.queued].pal = (after->flame_kills > before->flame_kills ||
                                      (hc < OD_GRID_W * OD_GRID_H &&
                                       before->cells[hc / OD_GRID_W][hc % OD_GRID_W] != OD_CELL_FIRE)) ?
                                     OD_QIMG_NONE : 0xFF;
        next.queue[next.queued++] = (uint8_t)(hc);
        added++;
    }
    if (after->effect_count > before->effect_count + added) return 0;
    unsigned dispatched = before->effect_count + added - after->effect_count;
    unsigned disp_cells[8], disp_count = 0;
    while (dispatched--) {
        if (!next.queued) return 0;
        uint8_t cell = next.queue[--next.queued];
        unsigned i = 0;
        while (i < 16 && next.steam[i].active) i++;
        if (i == 16) return 0;
        next.steam[i].cell = cell;
        next.steam[i].active = 1;
        next.steam[i].phase = 0;
        next.steam[i].wait = 0;
        if (disp_count < sizeof(disp_cells) / sizeof(disp_cells[0]))
            disp_cells[disp_count++] = cell;
    }
    if (next.colw_on)
        for (unsigned j = 0; j < disp_count; j++) colw_cell(&next, after, disp_cells[j], 0);
    /* The effect dispatch runs BEFORE the slots advance. A slot ending this tick
     * cannot be reused by the dispatcher until the following tick. */
    for (unsigned i = 0; i < 16; i++) {
        if (!next.steam[i].active) continue;
        if (!next.steam[i].wait) {
            next.steam[i].wait = (uint8_t)g_steam_phases[0].wait;
        } else if (--next.steam[i].wait == 0) {
            if (++next.steam[i].phase == 6) next.steam[i].active = 0;
            else next.steam[i].wait = (uint8_t)g_steam_phases[next.steam[i].phase].wait;
        }
    }
    /* eight-tick drop: age the retained source tiles. */
    unsigned fall_keep = 0;
    for (unsigned i = 0; i < next.fall_count; i++) {
        if (next.fall[i].wait) next.fall[i].wait--;
        if (next.fall[i].wait) next.fall[fall_keep++] = next.fall[i];
    }
    next.fall_count = fall_keep;
    /* A drop that starts this tick cleared its source cells in the committed
     * grid (state 1 = ice, 2 = fire). Its image advances every eight ticks,
     * including intermediate rows absent from the grid. */
    unsigned cs = after->cascade_state & 3, bs = before->cascade_state & 3;
    int continued = cs == bs && after->cascade_cell != before->cascade_cell;
    if ((cs == 1 || cs == 2) && ((bs != 1 && bs != 2) || continued)) {
        unsigned width = after->cascade_width & 0x7F;
        if (!width) width = 1;
        /* The image is composed from the run copy: each cell's columns see the
         * previous/next element of the run (0 at the ends), not the emptied grid. */
        uint8_t run[17] = {0};
        for (unsigned i = 0; i < width && i < 16; i++) {
            unsigned cell = after->cascade_cell + i;
            if (cell >= OD_GRID_W * OD_GRID_H) break;
            run[i] = (uint8_t)(continued ? (cs == 2 ? 14 : after->drop_tiles[i])
                                         : before->cells[cell >> 4][cell & 15] & 15);
        }
        for (unsigned i = 0; i < width && next.fall_count < 16; i++) {
            unsigned cell = after->cascade_cell + i;
            if (cell >= OD_GRID_W * OD_GRID_H) break;
            unsigned type = run[i];
            /* The whole run is copied first, so a stone that joined it falls in the same
             * image as its ice. */
            if (type != 14 && type != 0x0C && (type < 2 || type > 5)) continue;
            unsigned dst = cell + OD_GRID_W;
            unsigned dst_quench = 0;
            for (unsigned j = 0; j < disp_count; j++)
                if (disp_cells[j] == dst) dst_quench = 1;
            next.fall[next.fall_count].cell = (uint8_t)cell;
            next.fall[next.fall_count].type = (uint8_t)type;
            next.fall[next.fall_count].wait = 8;
            next.fall[next.fall_count].dst_pal =
                cell_bg_pal(after, before->cells, dst);
            next.fall[next.fall_count].dst_quench = (uint8_t)dst_quench;
            next.fall[next.fall_count].left = i ? run[i - 1] : 0;
            next.fall[next.fall_count].right = run[i + 1];
            next.fall_count++;
        }
    }
    /* The wand's decision: keep the previous image of the cell and its two
     * neighbours (their edges depend on it) until the redraw clears
     * `magic_redraw`. */
    if (after->magic_redraw && !before->magic_redraw) {
        unsigned cx = after->magic_cell & 15, cy = after->magic_cell >> 4;
        next.magic_count = 0;
        for (int dx = -1; dx <= 1 && cy < OD_GRID_H; dx++) {
            int x = (int)cx + dx;
            if (x < 0 || x >= OD_GRID_W)
                continue;
            next.magic[next.magic_count].cell = (uint8_t)(cy * 16 + (unsigned)x);
            next.magic[next.magic_count].type = before->cells[cy][x] & 15;
            next.magic[next.magic_count].left =
                x > 0 ? before->cells[cy][x - 1] & 15 : 0xFF;
            next.magic[next.magic_count].right =
                x < OD_GRID_W - 1 ? before->cells[cy][x + 1] & 15 : 0xFF;
            next.magic_count++;
        }
    } else if (!after->magic_redraw) {
        next.magic_count = 0;
    }
    next.game_frame = after->frame;
    next.ticks++;
    /* The clock stops with the solved round's exit tail: its last step is the frame
     * the celebration flag is raised (reference R1 f3974), frozen from f3975 to the
     * map. */
    if (!before->exit_frames)
        next.oam_mode = (uint8_t)((next.oam_mode + 1) & 3);
    *v = next;
    return 1;
}

/* Reconstruction of the scene flags for the status-clock gate. The HUD only
 * steps while the scene flag is completely clear. od_game.freeze materialises
 * the freeze bits; the celebration animation also sets its own bit. */
uint8_t od_scene_flags(const od_game *game)
{
    return (uint8_t)(game->freeze || game->hero_action == 0x18);
}

void od_order_sprites(od_presentation *p, unsigned hero_count, unsigned mode)
{
    if (hero_count >= 64) return;
    od_sprite source[64];
    unsigned count = p->sprite_count < 64 ? p->sprite_count : 64;
    memcpy(source, p->sprites, sizeof(source));
    for (unsigned i = hero_count; i < 64; i++)
        p->sprites[i] = (od_sprite){0, 241, 0, 0};
    unsigned mid = hero_count + (64 - hero_count) / 2;
    unsigned cursor = mode & 2 ? mid : (mode & 1 ? 64 : hero_count);
    for (unsigned i = hero_count; i < count; i++) {
        if (mode & 1) {
            if (cursor == hero_count) cursor = 64;
            p->sprites[--cursor] = source[i];
        } else {
            p->sprites[cursor++] = source[i];
            if (cursor == 64) cursor = hero_count;
        }
    }
    p->sprite_count = 64;
}

/* Redraw one background cell with the resolved world tile and the live animated
 * tile bank, exactly as present_game() does for a committed cell. `yoff` shifts
 * the 16x16 object down by 0 or 8 pixels across the cell boundary, which is how
 * a falling block is rendered mid-cell (the grid keeps the source tile). */
static void draw_bg_cell_nb(od_presentation *p, const od_game *g, unsigned cell,
                            unsigned type, int left, int right,
                            unsigned long ticks, int yoff)
{
    if (cell >= OD_GRID_W * OD_GRID_H) return;
    const od_world_gfx *w = od_world_gfx_for_level(g->level_index);
    if (!w) w = od_world_gfx_for_world(0);
    unsigned cx = cell & 15, cy = cell >> 4;
    od_gfx_tiles cell_tiles;
    if (od_world_gfx_resolve(w, (uint8_t)type, left, right, &cell_tiles))
        return;
    /* Fire and connected ice animate their tile bank from the phase tables;
     * reconnectors use the second bank too. */
    if (type == 14 || (type >= 2 && type <= 5)) {
        unsigned bank = od_background_bank(type == 14 ? 4 : 2,
                                            od_game_world(g), ticks);
        for (unsigned k = 0; k < 4; k++)
            cell_tiles.tiles[k] = (uint16_t)(bank * 64 + (cell_tiles.tiles[k] & 63));
    }
    if (type != 14)
        animate_r2_tiles(&cell_tiles, (unsigned)od_game_world(g), ticks);
    for (unsigned k = 0; k < 4; k++) {
        unsigned ty = cy * 2 + k / 2 + (unsigned)(yoff / 8), tx = cx * 2 + k % 2;
        if (ty >= 30) continue;               /* clipped below the screen */
        p->tiles[ty][tx] = cell_tiles.tiles[k];
        p->palettes[ty][tx] = cell_tiles.bg_pal;
    }
}

/* As draw_bg_cell_nb(), with the live grid neighbours (the cell writer). */
static void draw_bg_cell_at(od_presentation *p, const od_game *g, unsigned cell,
                            unsigned type, unsigned long ticks, int yoff)
{
    if (cell >= OD_GRID_W * OD_GRID_H) return;
    unsigned cx = cell & 15, cy = cell >> 4;
    int left = cx == 0 ? -1 : (g->cells[cy][cx - 1] & 15);
    int right = cx < 15 ? (g->cells[cy][cx + 1] & 15) : -1;
    draw_bg_cell_nb(p, g, cell, type, left, right, ticks, yoff);
}

static void draw_bg_cell(od_presentation *p, const od_game *g, unsigned cell,
                         unsigned type, unsigned long ticks)
{
    draw_bg_cell_at(p, g, cell, type, ticks, 0);
}

/* As draw_bg_cell_at(), but forces the four background palette indices. A fire
 * drop writes the moved tile into the destination cell at once, before that
 * cell's attribute byte is rewritten, so the destination shows the source tile
 * in the destination's pre-move palette. */
static void draw_bg_cell_at_pal(od_presentation *p, const od_game *g,
                                unsigned cell, unsigned type,
                                unsigned long ticks, int yoff, uint8_t pal)
{
    draw_bg_cell_at(p, g, cell, type, ticks, yoff);
    if (cell >= OD_GRID_W * OD_GRID_H) return;
    unsigned cx = cell & 15, cy = cell >> 4;
    for (unsigned k = 0; k < 4; k++) {
        unsigned ty = cy * 2 + k / 2 + (unsigned)(yoff / 8), tx = cx * 2 + k % 2;
        if (ty >= 30) continue;
        p->palettes[ty][tx] = pal;
    }
}

/* Quench queue: the fire background tile is not removed from the background when
 * the counter drops; the write is queued and lands with the effect. The
 * committed grid already dropped the cell, so re-draw the fire tile for every
 * cell still pending in the visual queue. The ice sprite covers it in the
 * meantime, and once the effect is dispatched the steam takes over. */
/* Redraw `cell` as `type` beside `left`/`right`, keeping the tiles and palette
 * indices of its column `keep` (0 left, 1 right): a cell boundary is written as
 * the right column of one cell and the left column of the next. */
static void draw_bg_half(od_presentation *p, const od_game *g, unsigned cell,
                         unsigned type, int left, int right, unsigned keep,
                         unsigned long ticks)
{
    unsigned tx = (cell & 15) * 2 + keep, ty = (cell >> 4) * 2;
    uint16_t t0 = p->tiles[ty][tx], t1 = p->tiles[ty + 1][tx];
    uint8_t p0 = p->palettes[ty][tx], p1 = p->palettes[ty + 1][tx];
    draw_bg_cell_nb(p, g, cell, type, left, right, ticks, 0);
    p->tiles[ty][tx] = t0;
    p->tiles[ty + 1][tx] = t1;
    p->palettes[ty][tx] = p0;
    p->palettes[ty + 1][tx] = p1;
}

static void restore_pending_fire(od_presentation *p, const od_game *g,
                                 const od_visual_state *v)
{
    for (unsigned i = 0; i < v->queued; i++) {
        if (v->qimg[i].pal == OD_QIMG_NONE) continue;
        if (v->qimg[i].pal == 0xFF) {
            /* A stone that landed on the put-out fire has already written its own tile
             * there: its landing write comes after the fire is cleared. */
            if (v->queue[i] < OD_GRID_W * OD_GRID_H &&
                g->cells[v->queue[i] / OD_GRID_W][v->queue[i] % OD_GRID_W] == 0x0c)
                continue;
            draw_bg_cell(p, g, v->queue[i], OD_CELL_FIRE, v->ticks);
            continue;
        }
        unsigned cell = v->queue[i];
        if (cell >= OD_GRID_W * OD_GRID_H) continue;
        draw_bg_cell_nb(p, g, cell, v->qimg[i].type, v->qimg[i].left,
                        v->qimg[i].right, v->ticks, 0);
        unsigned cx = cell & 15, cy = cell >> 4;
        for (unsigned k = 0; k < 4; k++)
            p->palettes[cy * 2 + k / 2][cx * 2 + k % 2] = v->qimg[i].pal;
        /* Each boundary with a run neighbour stays as the falling image drew
         * it until a write on one side redraws it from the grid:
         * - a neighbour still queued: nothing written yet;
         * - a queued neighbour that has had its effect: the write that cleared
         *   it also drew this cell's facing column as the empty floor it is in
         *   the grid (reference 1-6 f5430: the bar left starts half a cell in);
         * - a block of the run that stayed: its facing column keeps the run
         *   tile it fell with until this cell's own effect (reference 1-4 f3808
         *   and f3822: the remaining ice joins the pink cell for three frames). */
        for (unsigned side = 0; side < 2; side++) {
            int nb = side ? v->qimg[i].right : v->qimg[i].left;
            if (!nb || cx == (side ? OD_GRID_W - 1u : 0u)) continue;
            unsigned n = side ? cell + 1 : cell - 1;
            int queued = 0;
            for (unsigned j = 0; j < v->queued; j++)
                if (v->queue[j] == n) queued = 1;
            if (queued) continue;
            if ((g->cells[cy][n & 15] & 15) == OD_CELL_EMPTY) {
                int gl = cx == 0 ? -1 : (g->cells[cy][cx - 1] & 15);
                int gr = cx < 15 ? (g->cells[cy][cx + 1] & 15) : -1;
                draw_bg_half(p, g, cell, OD_CELL_EMPTY, gl, gr, !side, v->ticks);
            } else if (side) {
                draw_bg_half(p, g, n, (unsigned)nb, v->qimg[i].type, 0, 1, v->ticks);
            } else {
                draw_bg_half(p, g, n, (unsigned)nb, 0, v->qimg[i].type, 0, v->ticks);
            }
        }
    }
}

/* Cascade drop. The grid commits the move when the block starts falling
 * (fire occupancy leads its landing by a cell), but the background rewrite only
 * lands at the end. The image therefore keeps the source tile and does not show
 * the destination yet: suppress the destination cell and paint the falling
 * block at the source, half a cell lower during the second half of the fall. */
/* The wand's cells keep their previous metatiles until the redraw
 * (od_visual_state.magic). */
static void restore_magic_cells(od_presentation *p, const od_game *g,
                                const od_visual_state *v)
{
    for (unsigned i = 0; i < v->magic_count && i < 3; i++) {
        int left = v->magic[i].left == 0xFF ? -1 : v->magic[i].left;
        int right = v->magic[i].right == 0xFF ? -1 : v->magic[i].right;
        draw_bg_cell_nb(p, g, v->magic[i].cell, v->magic[i].type, left, right,
                        v->ticks, 0);
    }
}

static void restore_falling_tiles(od_presentation *p, const od_game *g,
                                  const od_visual_state *v)
{
    for (unsigned i = 0; i < v->fall_count; i++) {
        unsigned src = v->fall[i].cell;
        unsigned dst = src + OD_GRID_W; /* one cell below */
        if (!v->fall[i].wait || dst >= OD_GRID_W * OD_GRID_H) continue;
        if (v->fall[i].wait > 4 && v->fall[i].dst_quench) {
            /* A fire drop into a just-quenched cell (effect dispatched this tick): while
             * the block is in the first half of its descent the destination still shows the
             * moved tile in the empty-floor palette the quench attribute write left. */
            draw_bg_cell_at_pal(p, g, dst, v->fall[i].type, v->ticks, 0,
                                v->fall[i].dst_pal);
        } else if (v->fall[i].type == OD_CELL_FIRE ||
                   (g->cells[dst / OD_GRID_W][dst % OD_GRID_W] & 15) != OD_CELL_FIRE) {
            /* A fire in the grid below a falling block is not the moved
             * block: it stays drawn until the landing puts it out (reference 1-6
             * f5414..f5420). A falling fire is already in its destination cell in the grid. */
            draw_bg_cell_at(p, g, dst, OD_CELL_EMPTY, v->ticks, 0);
        }
    }
    for (unsigned i = 0; i < v->fall_count; i++) {
        unsigned wait = v->fall[i].wait;
        if (!wait) continue;
        unsigned dst = v->fall[i].cell + OD_GRID_W;
        unsigned dy = (dst / OD_GRID_W) * 2, dx = (dst % OD_GRID_W) * 2;
        uint8_t keep0 = 0, keep1 = 0;
        int keep = wait <= 4 && dst < OD_GRID_W * OD_GRID_H &&
                   v->fall[i].type != OD_CELL_FIRE &&
                   (g->cells[dst / OD_GRID_W][dst % OD_GRID_W] & 15) == OD_CELL_FIRE;
        if (keep) {
            keep0 = p->palettes[dy][dx];
            keep1 = p->palettes[dy][dx + 1];
        }
        draw_bg_cell_nb(p, g, v->fall[i].cell, v->fall[i].type,
                        v->fall[i].left, v->fall[i].right, v->ticks,
                        wait > 4 ? 0 : 8);
        if (keep) {
            /* The half that entered the fire's cell takes that cell's
             * attribute: the image is rewritten, the attribute only at the
             * landing (reference 1-6 f5418..f5421: the lower half in pink). */
            p->palettes[dy][dx] = keep0;
            p->palettes[dy][dx + 1] = keep1;
        }
    }
}

/* Room sprite banks are restored before the room raster. The cartridge bank
 * at the end of the frame instead describes the HUD, which is NOT the boss's
 * visible bank. Sprite tile bytes remain unchanged. */
/* A span is redrawn as the right column of each cell and the left column of the
 * next one, so a cell the arena artwork still shows gets a new edge column when
 * the neighbour it faces changes and the joint tiles depend on that neighbour: a
 * wall next to ice that arrived shows its ice joint. Tile column `x` of row `y`:
 * is it such a rewritten edge? */
static int arena_edge_rewritten(const od_game *g, const od_visual_state *v,
                                const od_world_gfx *wg, unsigned x, unsigned y)
{
    unsigned cx = x / 2, cy = y / 2, side = x & 1;
    if (cy >= OD_GRID_H || (side ? cx == OD_GRID_W - 1 : cx == 0)) return 0;
    unsigned ncx = side ? cx + 1 : cx - 1;
    /* The neighbour is what the background still shows for it: a cell the wand changed keeps its
     * previous image until the redraw (`magic`), and so does the edge that faces it. */
    int now = g->cells[cy][ncx] & 15;
    for (unsigned i = 0; i < v->magic_count && i < 3; i++)
        if (v->magic[i].cell == cy * 16 + ncx) now = v->magic[i].type;
    int was = g_arena_grid[cy * 16 + ncx] & 15;
    if (now == was) return 0;
    uint8_t type = (uint8_t)(g->cells[cy][cx] & 15);
    od_gfx_tiles a, b;
    if (od_world_gfx_resolve(wg, type, side ? 0 : now, side ? now : 0, &a) ||
        od_world_gfx_resolve(wg, type, side ? 0 : was, side ? was : 0, &b))
        return 0;
    unsigned k = (y & 1) * 2 + side;
    return (a.tiles[k] & 63) != (b.tiles[k] & 63);
}

static uint16_t boss_chr_tile(uint8_t tile)
{
    return (uint16_t)((g_arena_sprite_banks[tile / 128] & 0xfe) * 64 + tile % 128);
}

/* Append the boss's sprite entries after the hero and the steam, so
 * od_order_sprites() places them in the rotating object band exactly like the
 * original object loop. */
static void compose_boss(od_presentation *p, const od_boss *boss)
{
    if (!boss || p->sprite_count >= 64) return;
    od_boss_sprite parts[64];
    unsigned room = 64 - p->sprite_count;
    unsigned n = od_boss_pose(parts, room, boss);
    for (unsigned i = 0; i < n; ++i) {
        const od_boss_sprite *s = &parts[i];
        p->sprites[p->sprite_count++] = (od_sprite){
            s->x, s->y + 1, boss_chr_tile(s->tile), s->attr};
    }
}

static void present_game_visual_impl(od_presentation *p, const od_game *g,
                                     const od_visual_state *v,
                                     const od_palette_ram *pal,
                                     const od_boss *boss,
                                     const od_boss_attack *attack)
{
    present_game(p, g, pal, &v->ticks, 0);
    restore_pending_fire(p, g, v);
    restore_falling_tiles(p, g, v);
    restore_magic_cells(p, g, v);
    if (boss) {
        /* The robe and walls are authored background artwork, not world-10 terrain.
         * Keep that artwork wherever gameplay has not rewritten a cell. Resolve
         * changed cells from the live grid/video queue above, using the arena's
         * terrain bank and attributes. */
        memcpy(p->palette.ram, g_arena_palette, 32);
        /* The room clock starts on the arena's first frame, one frame before
         * `od_combat_init` (g->frame 0), so the room age is g->frame + 1. On the idle
         * arena route the age is max(2, f - 25748) and the two lower banks change on
         * (f - 25748)/2 and (f - 25748)/4; counted from g->frame every fade step and
         * bank change landed one frame late. The fade brightness lookup runs from 2 on
         * entry up to 15. */
        unsigned age = g->frame < 0 ? 1 : (unsigned)g->frame + 1;
        unsigned brightness = age < 2 ? 2 : age > 15 ? 15 : age;
        for (unsigned i = 0; i < 32; i++) {
            unsigned base = g_arena_base_palette[i];
            p->palette.ram[i] = (uint8_t)((base & 15) | g_arena_fade_lut[(base & 48) + brightness]);
        }
        uint8_t banks[4];
        memcpy(banks, g_arena_banks, sizeof banks);
        banks[0] = (uint8_t)od_background_bank(2, 10, age);
        /* The lower room bank steps every four room ticks; the app's presentation
         * latch delays this prepared frame until the next video update. */
        banks[3] = g_arena_lower_banks[(age / 4) % 3];
        uint8_t attack_cells[256];
        od_boss_attack_graphics(attack_cells, attack);
        /* The logged column writes, oldest first, over what the grid and the attack drew. */
        uint8_t forced[30][32];
        memset(forced, 0, sizeof forced);
        for (unsigned i = 0; i < v->colw_count; i++) {
            unsigned cell = v->colw[i].cell, side = v->colw[i].side;
            unsigned src = v->colw[i].src;
            if (cell >= OD_GRID_W * OD_GRID_H || src >= OD_GRID_W * OD_GRID_H ||
                colw_grid(g, cell) != v->colw[i].mraw ||
                colw_grid(g, src) != v->colw[i].sraw ||
                colw_fall_sig(v, cell) != v->colw[i].fsig)
                continue;
            if (side)
                draw_bg_half(p, g, cell, v->colw[i].type, 0, v->colw[i].nb, 0, v->ticks);
            else
                draw_bg_half(p, g, cell, v->colw[i].type, v->colw[i].nb, 0, 1, v->ticks);
            unsigned tx = (cell & 15) * 2 + side, ty = (cell >> 4) * 2;
            forced[ty][tx] = forced[ty + 1][tx] = 1;
        }
        /* Both stone metatiles: plain and joined to ice. */
        od_gfx_tiles stone[2];
        const od_world_gfx *wg = od_world_gfx_for_level(g->level_index);
        for (int s = 0; s < 2; s++)
            if (!wg || od_world_gfx_resolve(wg, 0x0C, s ? 2 : 0, s ? 2 : 0, &stone[s]))
                memset(&stone[s], 0xFF, sizeof stone[s]);
        for (unsigned y = 0; y < 30; y++) for (unsigned x = 0; x < 32; x++) {
            unsigned cell = (y / 2) * 16 + x / 2;
            unsigned attr = g_arena_nt[960 + (y / 4) * 8 + x / 4];
            unsigned shift = (y & 2) * 2 + (x & 2);
            unsigned tile = g_arena_nt[y * 32 + x];
            unsigned shown = g->cells[y / 2][x / 2] & 15;
            for (unsigned i = 0; i < v->magic_count && i < 3; i++)
                if (v->magic[i].cell == cell)
                    shown = v->magic[i].type;   /* held until the redraw */
            int changed = shown != (g_arena_grid[cell] & 15);
            for (unsigned i = 0; i < v->fall_count; i++)
                if (v->fall[i].wait && (v->fall[i].cell == cell ||
                    v->fall[i].cell + 16 == cell)) changed = 1;
            for (unsigned i = 0; i < v->queued; i++)
                if (v->queue[i] == cell) changed = 1;
            if (!changed && wg)
                changed = arena_edge_rewritten(g, v, wg, x, y);
            if (forced[y][x]) changed = 1;
            if (changed) {
                unsigned bank = p->tiles[y][x] / 64;
                unsigned slot = bank == od_background_bank(2, od_game_world(g), v->ticks) ? 0 :
                                bank == od_background_bank(4, od_game_world(g), v->ticks) ? 2 : 1;
                /* The stone's metatile lives in the first pattern slot, the one the arena's own
                 * (9,1) stone uses. Its tiles are not animated, so the bank tests above cannot
                 * place it once it falls off its background cell. */
                for (unsigned k = 0; k < 8; k++)
                    if (p->tiles[y][x] == stone[k / 4].tiles[k % 4]) slot = 0;
                p->tiles[y][x] = (uint16_t)(banks[slot] * 64 + (p->tiles[y][x] & 63));
            } else {
                p->tiles[y][x] = (uint16_t)(banks[tile / 64] * 64 + tile % 64);
            }
            p->palettes[y][x] = (uint8_t)((attr >> shift) & 3);
            /* Explicit background writes, column-major 2x2 tiles, palette 3 (flame) or 1
             * (erase). The grid only stores the hazard type, losing both graphic phase and
             * direction. */
            unsigned graphic = attack_cells[cell];
            if (graphic < 23 && !forced[y][x]) {
                tile = g_arena_attack_tiles[graphic * 4 + (x & 1) * 2 + (y & 1)];
                p->tiles[y][x] = (uint16_t)(banks[tile / 64] * 64 + tile % 64);
                p->palettes[y][x] = graphic ? 3 : 1;
            }
        }
    }
    od_sprite hero[64];
    unsigned hero_count = hero_sprites(hero, 64, g);
    unsigned world = g->level_index >= 0 && g->level_index < 100
                   ? (unsigned)g->level_index / 10 : 0;
    unsigned bank = (g_world_sprite_banks[world] & 0xFE) * 64;
    for (unsigned i = 0; i < 16; i++) {
        if (!v->steam[i].active) continue;
        unsigned phase = v->steam[i].phase;
        int x = (v->steam[i].cell & 15) * 16 + 8;
        /* the object's field line with its low nibble forced to 8, under the scroll. */
        int y = (int)scrolled_object_y((v->steam[i].cell & 0xF0u) | 8u, g->scroll_y);
        for (unsigned j = 0; j < g_steam_phases[phase].count && p->sprite_count < 64; j++) {
            const od_pose_part *s = &g_steam_parts[g_steam_phases[phase].first + j];
            p->sprites[p->sprite_count++] = (od_sprite){
                (uint8_t)(x + s->x), (uint8_t)(y + s->y) + 1,
                (uint16_t)(bank + (s->tile & 127)), s->attr};
        }
    }
    /* The object loop emits the steam before the flames (C6.74, castle 8-10 f21165:
     * the flame draws over the cloud in the frames the rotation puts the steam first). */
    present_flames(p, g);
    compose_boss(p, boss);
    od_order_sprites(p, hero_count, (v->oam_mode + 3) & 3);
}

void od_present_game_visual(od_presentation *p, const od_game *g,
                             const od_visual_state *v, const od_palette_ram *pal)
{
    present_game_visual_impl(p, g, v, pal, NULL, NULL);
}

void od_present_game_boss(od_presentation *p, const od_game *g,
                           const od_visual_state *v, const od_palette_ram *pal,
                           const od_boss *boss)
{
    present_game_visual_impl(p, g, v, pal, boss, NULL);
}

void od_present_game_combat(od_presentation *p, const od_game *g,
                             const od_visual_state *v, const od_palette_ram *pal,
                             const od_boss *boss, const od_boss_attack *attack)
{
    present_game_visual_impl(p, g, v, pal, boss, attack);
}

void od_present_latch_init(od_present_latch *latch)
{
    memset(latch, 0, sizeof(*latch));
}

const od_presentation *od_present_latch_build(od_present_latch *latch,
                                                const od_game *game,
                                                const od_visual_state *visual,
                                                const od_palette_ram *pal)
{
    unsigned show = latch->primed ? (latch->next ^ 1u) : 0;
    od_present_game_visual(&latch->frame[latch->next], game, visual, pal);
    const od_presentation *previous = latch->primed ? &latch->frame[show] : NULL;
    latch->next ^= 1u;
    latch->primed = 1;
    return previous;
}

void od_render_player(od_index_frame *fb, const od_level *lv,
                       const uint8_t *chr, const od_palette_ram *pal)
{
    od_render_hero(fb, od_player_x(lv), od_player_y(lv), 1, chr, pal);
}

void od_render_level(od_index_frame *fb, const od_level *lv,
                      const uint8_t *chr, const od_palette_ram *pal)
{
    od_render_level_field(fb, lv, chr, pal);
    od_render_player(fb, lv, chr, pal);
}

void od_render_level_index(od_index_frame *fb, const od_level *lv, int level_index,
                            const uint8_t *chr, const od_palette_ram *pal)
{
    if (!render_cells_world(fb, lv->grid, level_index, chr, pal))
        od_render_level_field(fb, lv, chr, pal);
    od_render_player(fb, lv, chr, pal);
}

void od_render_game(od_index_frame *fb, const od_game *g,
                     const uint8_t *chr, const od_palette_ram *pal)
{
    od_presentation p;
    od_present_game(&p, g, pal);
    od_compose_present(fb, &p, chr);
}

void od_frame_to_argb(const od_index_frame *fb, const od_master_palette *mpal, uint32_t *out)
{
    /* The app's historic default now resolves to the frozen PAL reference's RGB.
     * Explicit custom palettes retain their original conversion semantics. */
    if (!mpal || mpal == &od_master_palette_ntsc)
        mpal = &od_master_palette_pal;
    for (int i = 0; i < OD_SCREEN_W * OD_SCREEN_H; i++) {
        uint8_t rgb[3];
        od_master_lookup(mpal, fb->index[i], rgb);
        out[i] = 0xFF000000u | ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2];
    }
}
