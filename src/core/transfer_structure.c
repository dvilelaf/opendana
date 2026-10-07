#include "opendana/transfer_structure.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

struct od_transfer_structure_stage { od_transfer_structure art; };
static od_transfer_structure g_art;
static int g_loaded;
static const cJSON *get(const cJSON *doc, const char *key) { return cJSON_GetObjectItemCaseSensitive(doc, key); }
static int shape(const cJSON *doc, const char *const *keys, unsigned count)
{
    if (!cJSON_IsObject(doc)) return 0;
    unsigned seen = 0;
    for (const cJSON *v = doc->child; v; v = v->next) {
        unsigned i;
        for (i = 0; i < count; ++i) if (v->string && !strcmp(v->string, keys[i])) break;
        if (i == count || (seen & (1u<<i))) return 0;
        seen |= 1u<<i;
    }
    return seen == (1u<<count)-1;
}
static int number(const cJSON *v, unsigned max, unsigned *out)
{
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= max)) return 0;
    unsigned n = (unsigned)v->valuedouble;
    if (v->valuedouble != n) return 0;
    *out = n; return 1;
}
static int byte_field(const cJSON *doc, const char *key, uint8_t *out, unsigned max)
{
    unsigned n;
    if (!number(get(doc, key), max, &n)) return 0;
    *out = (uint8_t)n; return 1;
}
static int bytes(const cJSON *doc, uint8_t *out, unsigned count, unsigned max)
{
    if (!cJSON_IsArray(doc) || cJSON_GetArraySize(doc) != (int)count) return 0;
    unsigned i = 0;
    for (const cJSON *v = doc->child; v; v = v->next) {
        unsigned n;
        if (!number(v, max, &n)) return 0;
        out[i++] = (uint8_t)n;
    }
    return 1;
}
od_status od_transfer_structure_prepare_doc(const cJSON *doc, od_transfer_structure_stage **out)
{
    static const char *const root_keys[] = {"schema_version", "id", "destinations", "atlases", "images"};
    static const char *const atlas_keys[] = {"tile_storage_low", "attribute_storage_low", "tiles", "attributes"};
    static const char *const image_keys[] = {"atlas_id", "columns", "rows", "header_storage_low", "metatiles"};
    const cJSON *id = get(doc, "id"), *atlases = get(doc, "atlases"), *images = get(doc, "images"), *dest = get(doc, "destinations");
    unsigned version;
    if (!out || !shape(doc, root_keys, 5) || !number(get(doc, "schema_version"), 1, &version) || version != 1 ||
        !cJSON_IsString(id) || strcmp(id->valuestring, "transfer-structure") ||
        !cJSON_IsArray(atlases) || cJSON_GetArraySize(atlases) != 2 ||
        !cJSON_IsArray(images) || cJSON_GetArraySize(images) != 31 ||
        !cJSON_IsArray(dest) || cJSON_GetArraySize(dest) != 12) return OD_ERR_FORMAT;
    od_transfer_structure_stage *s = calloc(1, sizeof *s);
    if (!s) return OD_ERR_NOMEM;
    int ok = 1;
    for (unsigned i = 0; i < 12 && ok; ++i) {
        unsigned n;
        ok = number(cJSON_GetArrayItem(dest, (int)i), 0x2fff, &n) && (!n || n >= 0x2000);
        if (ok) s->art.destinations[i] = (uint16_t)n;
    }
    for (unsigned i = 0; i < 2 && ok; ++i) {
        od_transfer_atlas *a = &s->art.atlases[i]; a->count = i ? 132 : 239;
        const cJSON *d = cJSON_GetArrayItem(atlases, (int)i), *tiles = get(d, "tiles");
        ok = shape(d, atlas_keys, 4) && byte_field(d, "tile_storage_low", &a->tile_storage_low, 255) &&
            byte_field(d, "attribute_storage_low", &a->attribute_storage_low, 255) &&
            bytes(get(d, "attributes"), a->attributes, a->count, 255) &&
            cJSON_IsArray(tiles) && cJSON_GetArraySize(tiles) == (int)a->count;
        for (unsigned k = 0; k < a->count && ok; ++k)
            ok = bytes(cJSON_GetArrayItem(tiles, (int)k), a->tiles[k], 16, 255);
    }
    for (unsigned i = 0; i < 31 && ok; ++i) {
        od_transfer_image *m = &s->art.images[i]; const cJSON *d = cJSON_GetArrayItem(images, (int)i);
        ok = shape(d, image_keys, 5) && byte_field(d, "atlas_id", &m->atlas_id, 1) &&
            byte_field(d, "columns", &m->columns, 8) && m->columns &&
            byte_field(d, "rows", &m->rows, 8) && m->rows &&
            byte_field(d, "header_storage_low", &m->header_storage_low, 255);
        if (ok) ok = bytes(get(d, "metatiles"), m->metatiles, m->columns*m->rows, s->art.atlases[m->atlas_id].count-1);
    }
    if (!ok) { free(s); return OD_ERR_FORMAT; }
    *out = s; return OD_OK;
}
void od_transfer_structure_commit(od_transfer_structure_stage *s)
{
    if (!s) return;
    g_art = s->art; g_loaded = 1; free(s);
}
void od_transfer_structure_discard(od_transfer_structure_stage *s) { free(s); }
const od_transfer_structure *od_transfer_structure_get(void) { return g_loaded ? &g_art : NULL; }
void od_transfer_structure_reset(void) { memset(&g_art, 0, sizeof g_art); g_loaded = 0; }
