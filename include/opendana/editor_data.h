/* C6.4: exact EU legacy ice/editor content loaded at runtime. */
#ifndef OD_EDITOR_DATA_H
#define OD_EDITOR_DATA_H

#include "opendana/content.h"

#define OD_EDITOR_DATA_PATH "engine/editor-data.json"

typedef struct {
    uint8_t ice_connections[4][16];
    uint8_t palette[3][12];
    uint8_t cell_tile[16];
    uint8_t toolbar_x[3];
    uint8_t grid_y[14];
    uint8_t grid_x[15];
    uint8_t pal_y[3][12];
    uint8_t pal_x[3][12];
    uint8_t marker_addr[17][2];
} od_editor_data;

/* Layered read, strict format/semantic validation. Does not install or alter
 * *out on failure. err is optional; no ROM access or compiled vanilla data. */
od_status od_editor_data_parse(od_editor_data *out, const od_content_vfs *vfs,
                                char *err, size_t cap);

/* Copy/validate before publishing; invalid input retains previous tables.
 * NULL clears. Installation must be serialized with simulation. */
od_status od_editor_data_install(const od_editor_data *data);

/* Installed data, lazy default-package load, or NULL. Startup MUST parse and
 * install with the actual VFS including mods; lazy access is for old runners. */
const od_editor_data *od_editor_data_get(void);

/* Source-compatible legacy array views. On missing data, diagnoses to stderr
 * and returns empty storage for safe drawing; never usable vanilla defaults.
 * Simulation rejects missing data separately, and startup must fail. */
const od_editor_data *od_editor_data_view(void);

#endif
