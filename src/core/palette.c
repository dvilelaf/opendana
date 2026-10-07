#include "opendana/palette.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The three master palettes come from the content package (C2.2,
 * `palettes/master.json`). The structs stay `const`; only the colour arrays
 * they point at are filled by the loader, so every existing reference (and the
 * `mpal == &od_master_palette_ntsc` identity check) keeps working. */
static uint32_t g_ntsc[64];
static uint32_t g_pal[64];
static uint32_t g_reference[64];

const od_master_palette od_master_palette_ntsc = {
    .colors = g_ntsc,
    .name = "NTSC 2C02",
};
const od_master_palette od_master_palette_pal = {
    .colors = g_pal,
    .name = "EU PAL 2C07 reference, emphasis 0",
};
const od_master_palette od_master_palette_reference = {
    .colors = g_reference,
    .name = "EU console reference, emphasis 0",
};

void od_master_lookup(const od_master_palette *pal, uint8_t index, uint8_t rgb[3])
{
    uint32_t c = pal->colors[index & 0x3F];
    rgb[0] = (uint8_t)((c >> 16) & 0xFF);
    rgb[1] = (uint8_t)((c >> 8) & 0xFF);
    rgb[2] = (uint8_t)(c & 0xFF);
}

od_status od_palette_ram_load(od_palette_ram *out, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return OD_ERR_IO;
    size_t got = fread(out->ram, 1, sizeof(out->ram), f);
    fclose(f);
    return got == sizeof(out->ram) ? OD_OK : OD_ERR_IO;
}

struct od_palette_content_stage { uint32_t colors[3][64]; };

static int palette_index(const char *id)
{
    if (id != NULL && strcmp(id, "ntsc") == 0)
        return 0;
    if (id != NULL && strcmp(id, "pal") == 0)
        return 1;
    if (id != NULL && strcmp(id, "reference") == 0)
        return 2;
    return -1;
}

static od_status palette_parse(od_palette_content_stage *stage, const char *json, size_t len)
{
    if (json == NULL)
        return OD_ERR_FORMAT;
    cJSON *doc = cJSON_ParseWithLength(json, len);
    if (doc == NULL)
        return OD_ERR_FORMAT;
    const cJSON *master = cJSON_GetObjectItemCaseSensitive(doc, "master");
    if (!cJSON_IsArray(master) || cJSON_GetArraySize(master) != 3) {
        cJSON_Delete(doc);
        return OD_ERR_FORMAT;
    }
    int filled = 0;
    int count = cJSON_GetArraySize(master);
    for (int i = 0; i < count; i++) {
        const cJSON *entry = cJSON_GetArrayItem(master, i);
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(entry, "id");
        const cJSON *colors = cJSON_GetObjectItemCaseSensitive(entry, "colors");
        if (!cJSON_IsString(id) || !cJSON_IsArray(colors) ||
            cJSON_GetArraySize(colors) != 64)
            { cJSON_Delete(doc); return OD_ERR_FORMAT; }
        int index = palette_index(id->valuestring);
        if (index < 0 || (filled & (1 << index))) {
            cJSON_Delete(doc); return OD_ERR_FORMAT;
        }
        uint32_t *target = stage->colors[index];
        for (int k = 0; k < 64; k++) {
            const cJSON *v = cJSON_GetArrayItem(colors, k);
            if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= 0xFFFFFF) ||
                v->valuedouble != (double)(uint32_t)v->valuedouble) {
                cJSON_Delete(doc);
                return OD_ERR_FORMAT;
            }
            target[k] = (uint32_t)v->valuedouble;
        }
        filled |= 1 << index;
    }
    cJSON_Delete(doc);
    return filled == 7 ? OD_OK : OD_ERR_FORMAT;
}

od_status od_palette_content_prepare(od_palette_content_stage **out,
    const char *const *json, const size_t *sizes, size_t count)
{
    if (out == NULL || json == NULL || sizes == NULL || count == 0)
        return OD_ERR_FORMAT;
    od_palette_content_stage *stage = calloc(1, sizeof *stage);
    if (stage == NULL) return OD_ERR_NOMEM;
    for (size_t i = 0; i < count; ++i) {
        od_status st = palette_parse(stage, json[i], sizes[i]);
        if (st != OD_OK) { free(stage); return st; }
    }
    *out = stage;
    return OD_OK;
}

void od_palette_content_commit(od_palette_content_stage *stage)
{
    if (stage == NULL) return;
    memcpy(g_ntsc, stage->colors[0], sizeof g_ntsc);
    memcpy(g_pal, stage->colors[1], sizeof g_pal);
    memcpy(g_reference, stage->colors[2], sizeof g_reference);
    free(stage);
}

void od_palette_content_discard(od_palette_content_stage *stage) { free(stage); }

od_status od_palette_master_load_json(const char *json, size_t len)
{
    od_palette_content_stage *stage = NULL;
    od_status st = od_palette_content_prepare(&stage, &json, &len, 1);
    if (st == OD_OK) od_palette_content_commit(stage);
    return st;
}
