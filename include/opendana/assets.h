/* assets.h - runtime bundle reconstructed from the layered content package.
 * No legacy dumps or ROM are read by this loader. */
#ifndef OD_ASSETS_H
#define OD_ASSETS_H

#include "opendana/common.h"
#include "opendana/level.h"
#include "opendana/palette.h"
#include "opendana/content.h"

typedef struct {
    od_levelset levels;
    uint8_t *chr;        /* OD_CHR_BYTES, owned */
    size_t chr_size;
    od_palette_ram palette_ram;
} od_assets;

/* Compatibility entry point for older module callers: dir is unused; resolves
 * the default content package. New callers must pass their selected VFS. */
od_status od_assets_load(od_assets *out, const char *dir);
od_status od_assets_load_content(od_assets *out, const od_content_vfs *vfs);
void od_assets_free(od_assets *assets);

void od_path_join(char *out, size_t cap, const char *dir, const char *name);

#endif /* OD_ASSETS_H */
