#include "opendana/tile.h"

#include "stb_image.h"

void od_tile_decode(const uint8_t *chr, uint16_t index, uint8_t out[8][8])
{
    const uint8_t *t = chr + (size_t)index * OD_TILE_BYTES;
    for (int y = 0; y < 8; y++) {
        uint8_t lo = t[y];
        uint8_t hi = t[8 + y];
        for (int x = 0; x < 8; x++) {
            int bit = 7 - x;
            out[y][x] = (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
        }
    }
}

/* Map the four greyscale palette levels (0, 85, 170, 255) back to 0..3. The
 * exporter writes exactly those values, so any other level is a bad sheet. */
static int grey_to_2bpp(unsigned v)
{
    switch (v) {
    case 0:   return 0;
    case 85:  return 1;
    case 170: return 2;
    case 255: return 3;
    default:  return -1;
    }
}

int od_tile_chr_from_png(const void *data, int size, uint8_t *out, size_t out_cap)
{
    if (data == NULL || size <= 0 || out == NULL)
        return -1;
    int w = 0, h = 0, channels = 0;
    stbi_uc *img = stbi_load_from_memory((const stbi_uc *)data, size, &w, &h,
                                         &channels, 1);
    if (img == NULL)
        return -1;
    if (w <= 0 || h <= 0 || (w % 8) != 0 || (h % 8) != 0) {
        stbi_image_free(img);
        return -1;
    }
    size_t tiles = (size_t)(w / 8) * (size_t)(h / 8);
    if (tiles == 0 || tiles * OD_TILE_BYTES > out_cap) {
        stbi_image_free(img);
        return -1;
    }
    for (size_t t = 0; t < tiles; t++) {
        size_t tx = (t % (size_t)(w / 8)) * 8;
        size_t ty = (t / (size_t)(w / 8)) * 8;
        uint8_t *tile = out + t * OD_TILE_BYTES;
        for (int y = 0; y < 8; y++) {
            uint8_t lo = 0, hi = 0;
            for (int x = 0; x < 8; x++) {
                int v = grey_to_2bpp(img[(ty + (size_t)y) * (size_t)w + tx + (size_t)x]);
                if (v < 0) {
                    stbi_image_free(img);
                    return -1;
                }
                int bit = 7 - x;
                lo = (uint8_t)(lo | ((v & 1) << bit));
                hi = (uint8_t)(hi | (((v >> 1) & 1) << bit));
            }
            tile[y] = lo;
            tile[8 + y] = hi;
        }
    }
    stbi_image_free(img);
    return (int)tiles;
}
