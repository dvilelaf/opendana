/* rom.c - iNES ROM loading and EU identity check. See rom.h. */
#include "rom.h"
#include "fsutil.h"
#include "sha256.h"

#include "opendana/import.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OD_ROM_PRG_UNIT 16384
#define OD_ROM_CHR_UNIT 8192

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

od_status od_rom_load(od_rom *rom, const char *path, char *err, size_t err_cap)
{
    if (rom == NULL)
        return OD_ERR_FORMAT;
    memset(rom, 0, sizeof(*rom));

    if (path == NULL || path[0] == '\0') {
        set_err(err, err_cap, "no ROM file given");
        return OD_ERR_IO;
    }
    /* C6.29 (C6.22 PUB-18): look at the size before reading anything, so a
     * directory, a 4 GB video or a truncated download is refused at once and
     * the player is told what was expected. */
    unsigned long long on_disk = 0;
    int probe = od_fs_file_size(path, &on_disk);
    if (probe == -2) {
        set_err(err, err_cap, "the chosen path is a folder, not a ROM file "
                "(choose the .nes file itself)");
        return OD_ERR_IO;
    }
    if (probe != 0) {
        set_err(err, err_cap, "cannot open the ROM file (it does not exist or "
                "is not readable)");
        return OD_ERR_IO;
    }
    if (on_disk != OD_ROM_EU_SIZE) {
        if (err != NULL && err_cap > 0)
            snprintf(err, err_cap, "wrong file size: %llu bytes; the EU ROM of "
                     "OpenDana is %u bytes (16-byte iNES header + 128 KiB "
                     "PRG + 128 KiB CHR)", on_disk, (unsigned)OD_ROM_EU_SIZE);
        return OD_ERR_FORMAT;
    }

    size_t size = 0;
    char *raw = od_fs_read(path, &size);
    if (raw == NULL) {
        set_err(err, err_cap, "cannot read the ROM file");
        return OD_ERR_IO;
    }
    if (size != OD_ROM_EU_SIZE) {
        free(raw);
        set_err(err, err_cap, "the ROM file changed while it was being read");
        return OD_ERR_IO;
    }
    if (size < 16 || memcmp(raw, "NES\x1a", 4) != 0) {
        free(raw);
        set_err(err, err_cap, "not an iNES ROM (missing the NES\\x1a header)");
        return OD_ERR_FORMAT;
    }

    char digest[65];
    uint8_t sha[32];
    od_sha256(raw, size, sha);
    od_sha256_hex(sha, digest);
    if (strcmp(digest, OD_ROM_SHA256) != 0) {
        set_err(err, err_cap, "not the EU ROM of Solomon's Key 2 (SHA-256 mismatch)");
        free(raw);
        return OD_ERR_FORMAT;
    }

    size_t trainer = (raw[6] & 0x04) ? 512 : 0;
    size_t prg_size = (size_t)raw[4] * OD_ROM_PRG_UNIT;
    size_t chr_size = (size_t)raw[5] * OD_ROM_CHR_UNIT;
    size_t off = 16 + trainer;
    if (off + prg_size + chr_size > size) {
        free(raw);
        set_err(err, err_cap, "truncated iNES image (header sizes exceed the file)");
        return OD_ERR_FORMAT;
    }

    rom->raw = (uint8_t *)raw;
    rom->raw_size = size;
    rom->prg = rom->raw + off;
    rom->prg_size = prg_size;
    rom->chr = rom->prg + prg_size;
    rom->chr_size = chr_size;
    return OD_OK;
}

void od_rom_free(od_rom *rom)
{
    if (rom == NULL)
        return;
    free(rom->raw);
    memset(rom, 0, sizeof(*rom));
}
