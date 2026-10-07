#include "opendana/editor_data.h"
#include "cJSON.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static od_editor_data installed;
static int have_data;
static int reported_missing;

static int data_valid(const od_editor_data *data)
{
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 16; ++col)
            if (data->ice_connections[row][col] >= 16) return 0;
    for (unsigned row = 0; row < 3; ++row) {
        uint32_t tools = 0;
        int back = 0;
        for (unsigned col = 0; col < 12; ++col) {
            unsigned value = data->palette[row][col];
            if (value <= 16) tools |= UINT32_C(1) << value;
            else if (value == 0xff) back = 1;
            else if (value != 0x80) return 0;
        }
        unsigned count = 0;
        for (; tools; tools >>= 1) count += tools & 1;
        /* Every row must scan successfully with two tools already bound. */
        if (!back && count < 3) return 0;
    }
    for (unsigned i = 0; i < 17; ++i)
        if (data->marker_addr[i][1] < 0x20 || data->marker_addr[i][1] > 0x23)
            return 0;
    return 1;
}

static od_status fail(char *err, size_t cap, od_status status, const char *msg)
{
    if (err && cap) snprintf(err, cap, "%s", msg);
    return status;
}

static int bytes_parse(const cJSON *doc, const char *key, uint8_t *out, size_t size)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(doc, key);
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != (int)size) return 0;
    for (size_t i = 0; i < size; ++i) {
        const cJSON *v = cJSON_GetArrayItem(array, (int)i);
        if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= 255))
            return 0;
        unsigned n = (unsigned)v->valuedouble;
        if (v->valuedouble != (double)n) return 0;
        out[i] = (uint8_t)n;
    }
    return 1;
}

od_status od_editor_data_parse(od_editor_data *out, const od_content_vfs *vfs,
                                char *err, size_t cap)
{
    if (err && cap) err[0] = '\0';
    if (!out || !vfs) return fail(err, cap, OD_ERR_FORMAT, "invalid editor-data arguments");
    char *text = NULL;
    size_t size = 0;
    od_status status = od_content_vfs_read(vfs, OD_EDITOR_DATA_PATH, &text, &size);
    if (status != OD_OK)
        return fail(err, cap, status, "cannot read " OD_EDITOR_DATA_PATH);
    cJSON *doc = cJSON_ParseWithLengthOpts(text, size + 1, NULL, 1);
    free(text);
    if (!doc) return fail(err, cap, OD_ERR_FORMAT, "invalid editor-data JSON");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "schema_version");
    od_editor_data tmp = {0};
#define READ(member) bytes_parse(doc, #member, (uint8_t *)&tmp + offsetof(od_editor_data, member), sizeof tmp.member)
    int valid = cJSON_IsObject(doc) && cJSON_IsNumber(version) &&
        version->valuedouble == 1 && READ(ice_connections) && READ(palette) &&
        READ(cell_tile) && READ(toolbar_x) && READ(grid_y) && READ(grid_x) &&
        READ(pal_y) && READ(pal_x) && READ(marker_addr) && data_valid(&tmp);
#undef READ
    cJSON_Delete(doc);
    if (!valid) return fail(err, cap, OD_ERR_FORMAT, "invalid editor-data tables/version");
    *out = tmp;
    return OD_OK;
}

od_status od_editor_data_install(const od_editor_data *data)
{
    if (!data) {
        memset(&installed, 0, sizeof installed);
        have_data = 0;
        reported_missing = 0;
        return OD_OK;
    }
    if (!data_valid(data)) return OD_ERR_FORMAT;
    installed = *data;
    have_data = 1;
    reported_missing = 0;
    return OD_OK;
}

const od_editor_data *od_editor_data_get(void)
{
    if (have_data) return &installed;
    char base[1024];
    if (od_content_resolve_base(base, sizeof base, NULL) != OD_OK) return NULL;
    od_content_vfs vfs = {0};
    snprintf(vfs.layers[0].root, sizeof vfs.layers[0].root, "%s", base);
    vfs.count = 1;
    od_editor_data tmp;
    if (od_editor_data_parse(&tmp, &vfs, NULL, 0) != OD_OK) return NULL;
    if (od_editor_data_install(&tmp) != OD_OK) return NULL;
    return &installed;
}

const od_editor_data *od_editor_data_view(void)
{
    static const od_editor_data empty;
    const od_editor_data *data = od_editor_data_get();
    if (data) return data;
    if (!reported_missing) {
        fprintf(stderr, "opendana: missing or invalid " OD_EDITOR_DATA_PATH
                        "; load/install the content VFS before simulation\n");
        reported_missing = 1;
    }
    return &empty;
}
