/* png.c - minimal PNG encoder.
 *
 * Emits a truecolour (RGB8) PNG using zlib "stored" (uncompressed) deflate
 * blocks. PNG allows that, so no compression library is needed and the output
 * is a perfectly valid PNG. This keeps the project free of SDL_image/libpng.
 */
#include "opendana/png.h"
#include "opendana/userdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- CRC-32 (PNG chunk checksum) ---- */
static uint32_t crc_table[256];
static int crc_ready = 0;

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    if (!crc_ready)
        crc_init();
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static uint32_t adler32(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void be32(uint8_t out[4], uint32_t v)
{
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[4];
    be32(hdr, len);
    if (fwrite(hdr, 1, 4, f) != 4)
        return -1;
    if (fwrite(type, 1, 4, f) != 4)
        return -1;
    if (len && fwrite(data, 1, len, f) != len)
        return -1;
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const uint8_t *)type, 4);
    if (len)
        crc = crc32_update(crc, data, len);
    be32(hdr, crc ^ 0xFFFFFFFFu);
    return fwrite(hdr, 1, 4, f) == 4 ? 0 : -1;
}

int od_png_write_rgb(const char *path, int width, int height, const uint8_t *rgb)
{
    if (width <= 0 || height <= 0 || rgb == NULL)
        return -1;

    /* Raw scanlines with a filter byte (0 = None) in front of each row. */
    size_t stride = (size_t)width * 3 + 1;
    size_t raw_len = stride * (size_t)height;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (raw == NULL)
        return -1;
    for (int y = 0; y < height; y++) {
        uint8_t *row = raw + (size_t)y * stride;
        row[0] = 0;
        memcpy(row + 1, rgb + (size_t)y * width * 3, (size_t)width * 3);
    }

    /* zlib stream: 2-byte header + stored deflate blocks + adler32. */
    size_t z_len = 2 + raw_len + (raw_len / 65535 + 1) * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(z_len);
    if (z == NULL) {
        free(raw);
        return -1;
    }
    size_t zp = 0;
    z[zp++] = 0x78; /* CMF: deflate, 32K window */
    z[zp++] = 0x01; /* FLG */
    size_t off = 0;
    while (off < raw_len) {
        size_t n = raw_len - off;
        if (n > 65535)
            n = 65535;
        int last = (off + n >= raw_len);
        z[zp++] = (uint8_t)(last ? 1 : 0);
        z[zp++] = (uint8_t)(n & 0xFF);
        z[zp++] = (uint8_t)(n >> 8);
        z[zp++] = (uint8_t)(~n & 0xFF);
        z[zp++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zp, raw + off, n);
        zp += n;
        off += n;
    }
    uint32_t ad = adler32(raw, raw_len);
    be32(z + zp, ad);
    zp += 4;

    FILE *f = od_userdir_fopen(path, "wb");   /* UTF-8 on Windows (C6.36) */
    if (f == NULL) {
        free(raw);
        free(z);
        return -1;
    }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    int ok = fwrite(sig, 1, 8, f) == 8;

    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)width);
    be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* colour type: truecolour */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* adaptive filtering */
    ihdr[12] = 0; /* no interlace */
    if (ok)
        ok = write_chunk(f, "IHDR", ihdr, 13) == 0;
    if (ok)
        ok = write_chunk(f, "IDAT", z, (uint32_t)zp) == 0;
    if (ok)
        ok = write_chunk(f, "IEND", NULL, 0) == 0;

    fclose(f);
    free(raw);
    free(z);
    return ok ? 0 : -1;
}
