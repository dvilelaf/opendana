#include "opendana/level.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ODLV_MAGIC "ODLV"
#define ODLV_VERSION 1u
#define ODLV_HEADER_SIZE 24u /* magic(4)+ver(1)+gw(1)+gh(1)+rsv(1)+count(2)+rsv(2)+h32(4)+h64(8) */

od_status od_levelset_parse(od_levelset *out, const uint8_t *data, size_t size)
{
    if (out == NULL || data == NULL)
        return OD_ERR_FORMAT;
    memset(out, 0, sizeof(*out));

    if (size < ODLV_HEADER_SIZE || memcmp(data, ODLV_MAGIC, 4) != 0)
        return OD_ERR_FORMAT;
    if (data[4] != ODLV_VERSION || data[5] != OD_GRID_W || data[6] != OD_GRID_H)
        return OD_ERR_FORMAT;

    uint16_t count = od_rd16(data + 8);
    if (count > OD_LEVEL_COUNT)
        return OD_ERR_RANGE;
    out->grid_fnv1a32 = od_rd32(data + 12);
    out->grid_fnv1a64 = (uint64_t)od_rd32(data + 16) | ((uint64_t)od_rd32(data + 16 + 4) << 32);

    /* The level records borrow the command bytes, so keep an owned copy. */
    uint8_t *buffer = (uint8_t *)malloc(size);
    if (buffer == NULL)
        return OD_ERR_NOMEM;
    memcpy(buffer, data, size);
    out->buffer = buffer;
    out->buffer_size = size;

    size_t pos = ODLV_HEADER_SIZE;
    for (int i = 0; i < count; i++) {
        if (pos + 4 > size) {
            od_levelset_free(out);
            return OD_ERR_FORMAT;
        }
        od_level *lv = &out->levels[i];
        lv->player_position = buffer[pos + 0];
        lv->metadata = buffer[pos + 1];
        lv->command_len = od_rd16(buffer + pos + 2);
        pos += 4;
        if (pos + OD_GRID_CELLS > size) {
            od_levelset_free(out);
            return OD_ERR_FORMAT;
        }
        for (int y = 0; y < OD_GRID_H; y++)
            for (int x = 0; x < OD_GRID_W; x++)
                lv->grid[y][x] = buffer[pos + y * OD_GRID_W + x];
        pos += OD_GRID_CELLS;
        if (pos + lv->command_len > size) {
            od_levelset_free(out);
            return OD_ERR_FORMAT;
        }
        lv->commands = buffer + pos;
        pos += lv->command_len;
    }
    out->count = count;
    return OD_OK;
}

od_status od_levelset_load_file(od_levelset *out, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return OD_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return OD_ERR_IO;
    }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return OD_ERR_IO;
    }
    uint8_t *buffer = (uint8_t *)malloc((size_t)len);
    if (buffer == NULL) {
        fclose(f);
        return OD_ERR_NOMEM;
    }
    size_t got = fread(buffer, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buffer);
        return OD_ERR_IO;
    }
    od_status st = od_levelset_parse(out, buffer, (size_t)len);
    free(buffer);
    return st;
}

void od_levelset_free(od_levelset *set)
{
    if (set == NULL)
        return;
    free(set->buffer);
    set->buffer = NULL;
    set->buffer_size = 0;
}

uint32_t od_levelset_grid_hash32(const od_levelset *set)
{
    uint32_t h = OD_FNV1A32_INIT;
    for (int i = 0; i < set->count; i++)
        h = od_fnv1a32(set->levels[i].grid, sizeof(set->levels[i].grid), h);
    return h;
}

uint64_t od_levelset_grid_hash64(const od_levelset *set)
{
    uint64_t h = OD_FNV1A64_INIT;
    for (int i = 0; i < set->count; i++)
        h = od_fnv1a64(set->levels[i].grid, sizeof(set->levels[i].grid), h);
    return h;
}
