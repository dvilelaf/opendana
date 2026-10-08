/* Import front door: the SHA-256 implementation against standard vectors and
 * the rejection of a file that is not the supported ROM. Needs no ROM. */
#include "test.h"

#include "sha256.h"
#include "opendana/common.h"
#include "opendana/content.h"
#include "opendana/import.h"
#include "opendana/tile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
/* mkdtemp is POSIX; od_fs_mkdtemp is the portable one (same template rule). */
#include "fsutil.h"
#define mkdtemp(t) (od_fs_mkdtemp(t) == 0 ? (t) : NULL)
#endif


static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void test_import_sha256(void)
{
    static const struct {
        const char *text;
        const char *hex;
    } vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint8_t digest[32];
        char hex[65];
        od_sha256(vectors[i].text, strlen(vectors[i].text), digest);
        od_sha256_hex(digest, hex);
        CHECK(strcmp(hex, vectors[i].hex) == 0);
    }
}

static void test_import_rejects_wrong_rom(void)
{
    char dir[] = "/tmp/opendana-import-bad-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char rom[1024];
    snprintf(rom, sizeof(rom), "%s/wrong.bin", dir);
    FILE *f = fopen(rom, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("not the supported file", f);
        fclose(f);
    }
    char dest[1200];
    snprintf(dest, sizeof(dest), "%s/content", dir);
    char err[256] = {0};
    od_status st = od_import_rom(rom, dest, NULL, err, sizeof(err));
    CHECK(st != OD_OK);
    CHECK(!file_exists(dest));
    CHECK(err[0] != '\0');
}

OD_TEST(test_import_sha256)
OD_TEST(test_import_rejects_wrong_rom)
