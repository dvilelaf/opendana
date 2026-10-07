/* sha256.h - small SHA-256 (FIPS 180-4) used by the ROM importer.
 *
 * The importer hashes the user's ROM to verify it is the pinned EU image and
 * hashes every produced file for `content-manifest.sha256`. Pure C11, no
 * dependency on a crypto library.
 */
#ifndef OD_IMPORT_SHA256_H
#define OD_IMPORT_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} od_sha256_ctx;

void od_sha256_init(od_sha256_ctx *ctx);
void od_sha256_update(od_sha256_ctx *ctx, const void *data, size_t len);
void od_sha256_final(od_sha256_ctx *ctx, uint8_t out[32]);

/* One-shot convenience: hash `len` bytes into `out[32]`. */
void od_sha256(const void *data, size_t len, uint8_t out[32]);

/* Lower-case hex of a 32-byte digest, NUL-terminated (`out` needs 65 bytes). */
void od_sha256_hex(const uint8_t digest[32], char out[65]);

#endif /* OD_IMPORT_SHA256_H */
