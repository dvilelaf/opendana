/* C6.6: legacy presentation IDs, loaded from the final layered VFS.
 * No ROM or reference access at runtime. */
#ifndef OD_PRESENTATION_DATA_H
#define OD_PRESENTATION_DATA_H

#include "opendana/content.h"

typedef struct {
    /* empty, solid, left edge, right edge, ice, fire; TL,TR,BL,BR */
    uint16_t cell_tiles[6][4];
    uint8_t cell_palettes[6];
    /* idle pose only, palette layer / row / column */
    uint16_t player_tiles[2][3][2];
} od_presentation_data;

/* Parse into a temporary; failures leave *out unchanged and explain why.
 * parse does NOT install. The caller publishes all package components only
 * after every component validated. The last VFS layer wins, even if invalid. */
od_status od_presentation_data_parse_json(od_presentation_data *out,
    const char *json, size_t size, char *err, size_t errcap);
od_status od_presentation_data_parse(od_presentation_data *out,
    const od_content_vfs *vfs, char *err, size_t errcap);
/* Validated copy; failure preserves installed data. NULL clears installation.
 * Serialize install/reset with rendering (same lifetime as other engine data). */
od_status od_presentation_data_install(const od_presentation_data *data);
/* Compatibility for existing callers: lazily read the default package once.
 * Real --content/mod callers MUST install their final VFS data explicitly.
 * No content/zero-table fallback: unavailable returns NULL, diagnoses error. */
const od_presentation_data *od_presentation_data_get(void);

#endif
