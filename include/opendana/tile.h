/* tile.h - CHR tile decoding.
 *
 * The committed assets/tiles.chr is a raw 2-bit tile image: 8192 tiles of
 * 8x8 pixels, 16 bytes each, 2 bits per pixel, stored as two bit-planes
 * (bytes 0..7 = plane 0, bytes 8..15 = plane 1, MSB = leftmost pixel).
 */
#ifndef OD_TILE_H
#define OD_TILE_H

#include "opendana/common.h"

/* Decode tile `index` into 8x8 values in 0..3 (0 = background/transparent). */
void od_tile_decode(const uint8_t *chr, uint16_t index, uint8_t out[8][8]);

/* Decode an indexed 4-colour CHR sheet (8x8 tiles, row-major) into raw 2bpp
 * NES CHR bytes with the vendored stb_image (C2.2). The sheet is a PNG whose
 * four palette levels are the greys 0, 85, 170 and 255. `out_cap` must hold at
 * least (W/8)*(H/8)*OD_TILE_BYTES bytes. Returns the number of tiles written,
 * or -1 when the image is not a valid sheet. */
int od_tile_chr_from_png(const void *data, int size, uint8_t *out, size_t out_cap);

/* Convenience: decode and index a palette of four NES colour indices. */
static inline uint8_t od_tile_pixel(const uint8_t chr[OD_TILE_BYTES], int x, int y)
{
    uint8_t lo = chr[y];
    uint8_t hi = chr[8 + y];
    int bit = 7 - x;
    return (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
}

#endif /* OD_TILE_H */
