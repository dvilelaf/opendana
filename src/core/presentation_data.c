#include "opendana/presentation_data.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static od_presentation_data installed;
static int loaded, attempted;

static od_status failure(char *err, size_t cap, od_status st, const char *why)
{
    if (err && cap) snprintf(err, cap, "presentation-data: %s", why);
    return st;
}

static int valid(const od_presentation_data *d)
{
    for (unsigned i = 0; i < 6; ++i) {
        if (d->cell_palettes[i] > 3) return 0;
        for (unsigned j = 0; j < 4; ++j)
            if (d->cell_tiles[i][j] >= 8192) return 0;
    }
    for (unsigned l = 0; l < 2; ++l)
        for (unsigned r = 0; r < 3; ++r)
            for (unsigned c = 0; c < 2; ++c)
                if (d->player_tiles[l][r][c] >= 8192) return 0;
    return 1;
}

od_status od_presentation_data_parse_json(od_presentation_data *out,
    const char *json, size_t size, char *err, size_t errcap)
{
    if (err && errcap) err[0] = 0;
    if (!out || !json || size == 0)
        return failure(err, errcap, OD_ERR_FORMAT, "missing input");
    const char *end = NULL;
    cJSON *doc = cJSON_ParseWithLengthOpts(json, size, &end, 0);
    if (!doc) return failure(err, errcap, OD_ERR_FORMAT, "invalid JSON");
    while (end < json + size && (*end == ' ' || *end == '\t' ||
                                *end == '\r' || *end == '\n')) ++end;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "schema_version");
    int ok = end == json + size && cJSON_IsObject(doc) &&
             cJSON_IsNumber(version) && version->valuedouble == 1;
    /* Reject duplicate or unknown keys, keeping runtime and schema identical. */
    unsigned keys = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, doc) {
        unsigned bit = 0;
        if (item->string && !strcmp(item->string, "schema_version")) bit = 1;
        if (item->string && !strcmp(item->string, "cell_tiles")) bit = 2;
        if (item->string && !strcmp(item->string, "cell_palettes")) bit = 4;
        if (item->string && !strcmp(item->string, "player_tiles")) bit = 8;
        if (!bit || (keys & bit)) ok = 0;
        keys |= bit;
    }
    if (keys != 15) ok = 0;
    od_presentation_data temp;
    memset(&temp, 0, sizeof temp);
    const char *names[] = {"cell_tiles", "cell_palettes", "player_tiles"};
    const unsigned counts[] = {24, 6, 12};
    for (unsigned k = 0; ok && k < 3; ++k) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(doc, names[k]);
        if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != (int)counts[k]) {
            ok = 0; break;
        }
        for (unsigned i = 0; i < counts[k]; ++i) {
            const cJSON *v = cJSON_GetArrayItem(a, (int)i);
            unsigned max = k == 1 ? 3 : 8191;
            if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= max)
                || v->valuedouble != (unsigned)v->valuedouble) {
                ok = 0; break;
            }
            if (k == 0) temp.cell_tiles[i / 4][i % 4] = (uint16_t)v->valuedouble;
            if (k == 1) temp.cell_palettes[i] = (uint8_t)v->valuedouble;
            if (k == 2) temp.player_tiles[i / 6][i % 6 / 2][i % 2] = (uint16_t)v->valuedouble;
        }
    }
    cJSON_Delete(doc);
    if (!ok) return failure(err, errcap, OD_ERR_FORMAT, "invalid version, fields, length or integer range");
    *out = temp;
    return OD_OK;
}

od_status od_presentation_data_parse(od_presentation_data *out,
    const od_content_vfs *vfs, char *err, size_t errcap)
{
    if (!out || !vfs || vfs->count < 1 || vfs->count > OD_CONTENT_MAX_LAYERS)
        return failure(err, errcap, OD_ERR_FORMAT, "invalid output or VFS");
    char *text = NULL;
    size_t size = 0;
    od_status st = od_content_vfs_read(vfs, "engine/presentation-data.json", &text, &size);
    if (st != OD_OK) return failure(err, errcap, st, "cannot read engine/presentation-data.json");
    st = od_presentation_data_parse_json(out, text, size, err, errcap);
    free(text);
    return st;
}

od_status od_presentation_data_install(const od_presentation_data *d)
{
    if (!d) {
        memset(&installed, 0, sizeof installed);
        loaded = attempted = 0;
        return OD_OK;
    }
    if (!valid(d)) return OD_ERR_FORMAT;
    installed = *d;
    loaded = attempted = 1;
    return OD_OK;
}

const od_presentation_data *od_presentation_data_get(void)
{
    if (!loaded && !attempted) {
        attempted = 1;
        od_content_vfs vfs = {0};
        od_presentation_data temp;
        char err[200];
        od_status st = od_content_resolve_base(vfs.layers[0].root,
                                                sizeof vfs.layers[0].root, NULL);
        vfs.count = 1;
        if (st == OD_OK) st = od_presentation_data_parse(&temp, &vfs, err, sizeof err);
        else snprintf(err, sizeof err, "presentation-data: cannot resolve default content");
        if (st == OD_OK) (void)od_presentation_data_install(&temp);
        else fprintf(stderr, "%s\n", err);
    }
    return loaded ? &installed : NULL;
}
