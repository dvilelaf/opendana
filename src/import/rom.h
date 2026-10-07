/* rom.h - the importer's view of an iNES ROM. */
#ifndef OD_IMPORT_ROM_H
#define OD_IMPORT_ROM_H

#include "opendana/common.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *raw;          /* the whole iNES image */
    size_t raw_size;
    const uint8_t *prg;    /* PRG ROM (after the 16-byte header + trainer) */
    size_t prg_size;
    const uint8_t *chr;    /* CHR ROM */
    size_t chr_size;
} od_rom;

/* Read and parse an iNES ROM. Verifies the SHA-256 is the pinned EU image.
 * `err` receives a reason on failure. */
od_status od_rom_load(od_rom *rom, const char *path, char *err, size_t err_cap);
void od_rom_free(od_rom *rom);

#endif /* OD_IMPORT_ROM_H */
