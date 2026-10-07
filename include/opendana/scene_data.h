/* C6.5: imported presentation and map navigation data, loaded, not compiled. */
#ifndef OD_SCENE_DATA_H
#define OD_SCENE_DATA_H

#include "opendana/content.h"

#define OD_SCENE_DATA_PATH "engine/scene-data.json"

typedef struct {
    uint8_t cycle_block[19];
    uint8_t cycle_bg[4];
    uint8_t map_world_up[10];
    uint8_t map_world_down[10];
    uint8_t cursor_body[4][2]; /* pose 1 body, (tile, attr) */
    uint8_t cursor_skin[4][2]; /* pose 1 skin, (tile, attr) */
} od_scene_data;

/* Strict layered read. No mutation of *out or installed data on error.
 * err is optional (NULL/cap 0). No ROM access or compiled fallback. */
od_status od_scene_data_parse(od_scene_data *out, const od_content_vfs *vfs,
                               char *err, size_t cap);

/* Validate then publish a COPY; serialize with simulation. NULL clears.
 * Invalid input leaves the old installation intact. */
od_status od_scene_data_install(const od_scene_data *data);

/* Installed data, default-package lazy load, or NULL. Startup must parse and
 * install using its actual layered VFS, including --content and mods. */
const od_scene_data *od_scene_data_get(void);

/* Same as get, but diagnoses missing/invalid data to stderr. Consumers stop
 * without state mutation; the real startup must propagate the parse error. */
const od_scene_data *od_scene_data_require(void);

#endif
