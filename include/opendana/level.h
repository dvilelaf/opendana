/* level.h - the 150 level records of the content package.
 *
 * A level is a 16x15 grid of cell types (0..0x1F) plus the raw object/special
 * command stream the original loader applied on top of the backing mask. The
 * parser is self-contained: the runtime never needs a ROM or any external tool.
 */
#ifndef OD_LEVEL_H
#define OD_LEVEL_H

#include "opendana/common.h"

typedef struct {
    uint8_t player_position;              /* x = pos & 0x0F, y = pos >> 4 */
    uint8_t metadata;                     /* per-level metadata byte */
    uint16_t command_len;                 /* bytes in `commands` */
    const uint8_t *commands;              /* borrowed from the levelset buffer */
    uint8_t grid[OD_GRID_H][OD_GRID_W]; /* final cell types, row-major */
} od_level;

typedef struct {
    uint8_t *buffer;          /* owned copy of the levels.bin file */
    size_t buffer_size;
    uint32_t grid_fnv1a32;    /* digest stored in the file header */
    uint64_t grid_fnv1a64;
    int count;
    od_level levels[OD_LEVEL_COUNT];
} od_levelset;

od_status od_levelset_parse(od_levelset *out, const uint8_t *data, size_t size);
od_status od_levelset_load_file(od_levelset *out, const char *path);
void od_levelset_free(od_levelset *set);

/* FNV-1a over the 150 grids in level order (the value in the header). */
uint32_t od_levelset_grid_hash32(const od_levelset *set);
uint64_t od_levelset_grid_hash64(const od_levelset *set);

static inline int od_player_x(const od_level *lv) { return lv->player_position & 0x0F; }
static inline int od_player_y(const od_level *lv) { return lv->player_position >> 4; }

#endif /* OD_LEVEL_H */
