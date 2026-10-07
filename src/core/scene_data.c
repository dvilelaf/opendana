#include "opendana/scene_data.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static od_scene_data installed;
static int have_data, reported_missing;

static int data_valid(const od_scene_data *d)
{
    for (unsigned i = 0; i < sizeof d->cycle_block; ++i)
        if (d->cycle_block[i] > 63) return 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (d->cycle_bg[i] >= 16) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (d->cycle_bg[i] == d->cycle_bg[j]) return 0;
        /* NES OAM attribute bits 2..4 are unused; layer priority is represented
         * by the explicit body/skin draw order, not background emulation. */
        if ((d->cursor_body[i][1] | d->cursor_skin[i][1]) & 0x3c) return 0;
    }
    for (unsigned i = 0; i < 10; ++i)
        if ((d->map_world_up[i] | d->map_world_down[i]) > 15) return 0;
    return 1;
}

static od_status fail(char *err, size_t cap, od_status st, const char *message)
{
    if (err && cap) snprintf(err, cap, "%s", message);
    return st;
}

static int bytes_parse(const cJSON *doc, const char *key, uint8_t *out, size_t size)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(doc, key);
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != (int)size) return 0;
    for (size_t i = 0; i < size; ++i) {
        const cJSON *v = cJSON_GetArrayItem(array, (int)i);
        if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= 255)) return 0;
        unsigned n = (unsigned)v->valuedouble;
        if (v->valuedouble != (double)n) return 0;
        out[i] = (uint8_t)n;
    }
    return 1;
}

od_status od_scene_data_parse(od_scene_data *out, const od_content_vfs *vfs,
                               char *err, size_t cap)
{
    if (err && cap) err[0] = '\0';
    if (!out || !vfs) return fail(err, cap, OD_ERR_FORMAT, "invalid scene-data arguments");
    char *text = NULL;
    size_t size = 0;
    od_status st = od_content_vfs_read(vfs, OD_SCENE_DATA_PATH, &text, &size);
    if (st != OD_OK) return fail(err, cap, st, "cannot read " OD_SCENE_DATA_PATH);
    cJSON *doc = cJSON_ParseWithLengthOpts(text, size + 1, NULL, 1);
    free(text);
    if (!doc) return fail(err, cap, OD_ERR_FORMAT, "invalid scene-data JSON");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "schema_version");
    od_scene_data tmp = {0};
#define READ(member) bytes_parse(doc, #member, (uint8_t *)&tmp + offsetof(od_scene_data, member), sizeof tmp.member)
    int valid = cJSON_IsObject(doc) && cJSON_IsNumber(version) && version->valuedouble == 1 &&
        READ(cycle_block) && READ(cycle_bg) && READ(map_world_up) && READ(map_world_down) &&
        READ(cursor_body) && READ(cursor_skin) && data_valid(&tmp);
#undef READ
    cJSON_Delete(doc);
    if (!valid) return fail(err, cap, OD_ERR_FORMAT, "invalid scene-data tables/version");
    *out = tmp;
    return OD_OK;
}

od_status od_scene_data_install(const od_scene_data *data)
{
    if (!data) {
        memset(&installed, 0, sizeof installed);
        have_data = reported_missing = 0;
        return OD_OK;
    }
    if (!data_valid(data)) return OD_ERR_FORMAT;
    installed = *data;
    have_data = 1;
    reported_missing = 0;
    return OD_OK;
}

const od_scene_data *od_scene_data_get(void)
{
    if (have_data) return &installed;
    char base[1024];
    if (od_content_resolve_base(base, sizeof base, NULL) != OD_OK) return NULL;
    od_content_vfs vfs = {0};
    snprintf(vfs.layers[0].root, sizeof vfs.layers[0].root, "%s", base);
    vfs.count = 1;
    od_scene_data tmp;
    if (od_scene_data_parse(&tmp, &vfs, NULL, 0) != OD_OK ||
        od_scene_data_install(&tmp) != OD_OK) return NULL;
    return &installed;
}

const od_scene_data *od_scene_data_require(void)
{
    const od_scene_data *d = od_scene_data_get();
    if (!d && !reported_missing) {
        fprintf(stderr, "opendana: missing or invalid " OD_SCENE_DATA_PATH
                        "; load/install the content VFS before simulation\n");
        reported_missing = 1;
    }
    return d;
}
