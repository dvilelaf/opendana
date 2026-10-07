/* extract_graphics.c - CHR sheets of the content package.
 *
 * The 128 KiB CHR ROM is split into eight 256x256 sheets of 1024 tiles each,
 * written as grayscale PNGs whose four levels (0/85/170/255) decode back to the same
 * 2-bit tiles through `od_tile_chr_from_png` (stb_image). `stb_image_write` is
 * vendored in external/stb.
 */
#include "fsutil.h"
#include "rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image_write.h"

#define CHR_BYTES (8192 * 16)
#define TILES_PER_FILE 1024
#define TILES_PER_ROW 32
#define SHEET_PX (TILES_PER_ROW * 8)

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

od_status od_import_graphics(const od_rom *rom, const char *dir, char *err, size_t err_cap)
{
    if (rom->chr_size != CHR_BYTES) {
        set_err(err, err_cap, "CHR ROM is not 128 KiB");
        return OD_ERR_FORMAT;
    }
    char sheet_dir[4096];
    od_fs_join(sheet_dir, sizeof(sheet_dir), dir, "graphics/chr");
    if (od_fs_mkdir_p(sheet_dir) != 0) {
        set_err(err, err_cap, "cannot create graphics/chr");
        return OD_ERR_IO;
    }
    const uint8_t *chr = rom->chr;
    int files = CHR_BYTES / (TILES_PER_FILE * 16);
    uint8_t *pixels = (uint8_t *)malloc((size_t)SHEET_PX * SHEET_PX);
    if (pixels == NULL) {
        set_err(err, err_cap, "out of memory building the CHR sheets");
        return OD_ERR_NOMEM;
    }
    for (int f = 0; f < files; f++) {
        for (int t = 0; t < TILES_PER_FILE; t++) {
            int tile_index = f * TILES_PER_FILE + t;
            const uint8_t *base = chr + (size_t)tile_index * 16;
            int tx = (t % TILES_PER_ROW) * 8;
            int ty = (t / TILES_PER_ROW) * 8;
            for (int y = 0; y < 8; y++) {
                uint8_t lo = base[y];
                uint8_t hi = base[8 + y];
                for (int x = 0; x < 8; x++) {
                    int bit = 7 - x;
                    int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                    pixels[(size_t)(ty + y) * SHEET_PX + tx + x] = (uint8_t)(v * 85);
                }
            }
        }
        char path[4096];
        char rel[64];
        snprintf(rel, sizeof(rel), "graphics/chr/chr-%02d.png", f);
        od_fs_join(path, sizeof(path), dir, rel);
        if (!stbi_write_png(path, SHEET_PX, SHEET_PX, 1, pixels, SHEET_PX)) {
            free(pixels);
            set_err(err, err_cap, "cannot write a CHR sheet");
            return OD_ERR_IO;
        }
    }
    free(pixels);
    return OD_OK;
}
