/* test_catalog.c - unit tests for the catalogue (src/ext/catalog.c). */
#define _XOPEN_SOURCE 700   /* mkdtemp, dirent */
#include "test.h"
#include "opendana/ext/catalog.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_dir[512];

static void dir_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", g_dir, name);
}

static int file_has_bytes(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n > 0;
}

static void fill_doc(od_led_doc *d, const char *name, const char *author,
                     int theme, int64_t created, int64_t modified)
{
    memset(d, 0, sizeof *d);
    od_led_new(d, theme);
    snprintf(d->name, sizeof d->name, "%s", name);
    snprintf(d->author, sizeof d->author, "%s", author);
    od_led_paint(d, 5, 5, 0x0E);      /* so the level validates */
    od_led_place_hero(d, 1, 1);
    /* The edits above bump `modified`; the test pins both stamps last. */
    d->created = created;
    d->modified = modified;
}

static void cleanup_dir(void)
{
    DIR *dir = opendir(g_dir);
    if (dir != NULL) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            char p[1200];
            dir_path(p, sizeof p, e->d_name);
            remove(p);
        }
        closedir(dir);
    }
    rmdir(g_dir);
}

static void test_catalog_ids_case(void)
{
    char id[OD_LED_ID_MAX];
    od_catalog_make_id("My Level 2", id, sizeof id);
    CHECK(strcmp(id, "my-level-2") == 0);
    od_catalog_make_id("!!!", id, sizeof id);
    CHECK(strcmp(id, "level") == 0);
    od_catalog_make_id("--x--", id, sizeof id);
    CHECK(strcmp(id, "x") == 0);
    od_catalog_make_id("1-2 Save", id, sizeof id);
    CHECK(strcmp(id, "1-2-save") == 0);
}

static void test_catalog_case(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/opendana-c54-cat-XXXXXX",
             tmp && tmp[0] ? tmp : "/tmp");
    if (mkdtemp(g_dir) == NULL) {
        CHECK(0 && "mkdtemp failed");
        return;
    }

    od_led_doc zeta, alpha, mid;
    fill_doc(&zeta, "zeta", "Z", 1, 100, 300);
    fill_doc(&alpha, "alpha", "A", 2, 200, 100);
    fill_doc(&mid, "mid", "M", 3, 300, 200);

    char idz[OD_LED_ID_MAX], ida[OD_LED_ID_MAX], idm[OD_LED_ID_MAX];
    CHECK_EQ_U32(od_catalog_save(g_dir, &zeta, idz), OD_OK);
    CHECK_EQ_U32(od_catalog_save(g_dir, &alpha, ida), OD_OK);
    CHECK_EQ_U32(od_catalog_save(g_dir, &mid, idm), OD_OK);
    CHECK(strcmp(idz, "zeta") == 0);
    CHECK(strcmp(ida, "alpha") == 0);
    CHECK(strcmp(idm, "mid") == 0);
    char p[1200];
    dir_path(p, sizeof p, "zeta.json");
    CHECK(file_has_bytes(p));
    dir_path(p, sizeof p, "alpha.json");
    CHECK(file_has_bytes(p));
    dir_path(p, sizeof p, "mid.json");
    CHECK(file_has_bytes(p));
    dir_path(p, sizeof p, "catalog.json");
    CHECK(file_has_bytes(p));

    /* Scan lists every entry, sorted by name, with coherent metadata. */
    od_catalog_entry list[16];
    int n = 0;
    CHECK_EQ_U32(od_catalog_scan(g_dir, list, 16, &n), OD_OK);
    CHECK_EQ_U32(n, 3);
    CHECK(strcmp(list[0].id, "alpha") == 0);
    CHECK(strcmp(list[1].id, "mid") == 0);
    CHECK(strcmp(list[2].id, "zeta") == 0);
    CHECK(strcmp(list[0].file, "alpha.json") == 0);
    CHECK(strcmp(list[0].author, "A") == 0);
    CHECK_EQ_U32(list[0].created, 200);
    CHECK_EQ_U32(list[0].modified, 100);
    CHECK_EQ_U32(list[0].theme_world, 2);
    CHECK(strcmp(list[2].file, "zeta.json") == 0);
    CHECK_EQ_U32(list[2].theme_world, 1);

    /* find() */
    CHECK_EQ_U32(od_catalog_find(list, n, "mid"), 1);
    CHECK(od_catalog_find(list, n, "nope") < 0);

    /* Sort by modified, most recent first. */
    od_catalog_sort(list, n, OD_CATALOG_SORT_MODIFIED);
    CHECK(strcmp(list[0].id, "zeta") == 0);
    CHECK(strcmp(list[1].id, "mid") == 0);
    CHECK(strcmp(list[2].id, "alpha") == 0);

    /* Delete one entry; the others stay and the index shrinks. */
    CHECK_EQ_U32(od_catalog_delete(g_dir, "mid"), OD_OK);
    dir_path(p, sizeof p, "mid.json");
    CHECK(!file_has_bytes(p));
    CHECK_EQ_U32(od_catalog_scan(g_dir, list, 16, &n), OD_OK);
    CHECK_EQ_U32(n, 2);
    CHECK(od_catalog_find(list, n, "mid") < 0);
    CHECK(od_catalog_find(list, n, "alpha") >= 0);
    CHECK(od_catalog_find(list, n, "zeta") >= 0);

    /* Losing catalog.json loses nothing: scan rebuilds it. */
    dir_path(p, sizeof p, "catalog.json");
    remove(p);
    CHECK(!file_has_bytes(p));
    CHECK_EQ_U32(od_catalog_scan(g_dir, list, 16, &n), OD_OK);
    CHECK_EQ_U32(n, 2);
    CHECK(file_has_bytes(p));

    /* Import an external file under a fresh id. */
    od_led_doc ext;
    fill_doc(&ext, "Imported Level", "X", 5, 400, 500);
    char ext_path[1200];
    dir_path(ext_path, sizeof ext_path, "outside.json");
    CHECK_EQ_U32(od_led_save_json(&ext, ext_path), OD_OK);
    char imported[OD_LED_ID_MAX];
    CHECK_EQ_U32(od_catalog_import(g_dir, ext_path, imported), OD_OK);
    CHECK(strcmp(imported, "imported-level") == 0);
    od_led_free(&ext);
    (void)remove(ext_path);
    /* The imported level now lives inside the catalogue; scan sees all three. */
    CHECK_EQ_U32(od_catalog_scan(g_dir, list, 16, &n), OD_OK);
    CHECK_EQ_U32(n, 3);
    CHECK(od_catalog_find(list, n, imported) >= 0);

    /* Export is a byte copy of the entry file. */
    char out[1200], src[1200];
    dir_path(out, sizeof out, "exported.json");
    dir_path(src, sizeof src, "alpha.json");
    CHECK_EQ_U32(od_catalog_export(g_dir, "alpha", out), OD_OK);
    FILE *a = fopen(src, "rb");
    FILE *b = fopen(out, "rb");
    CHECK(a != NULL && b != NULL);
    if (a && b) {
        char ba[4096], bb[4096];
        size_t na = fread(ba, 1, sizeof ba, a);
        size_t nb = fread(bb, 1, sizeof bb, b);
        CHECK_EQ_U32(na, nb);
        CHECK_EQ_MEM(ba, bb, na);
    }
    if (a) fclose(a);
    if (b) fclose(b);
    remove(out);

    /* Saving over the same id replaces the entry, it does not duplicate it. */
    od_led_doc again = alpha;
    snprintf(again.id, sizeof again.id, "alpha");
    again.modified = 999;
    char idagain[OD_LED_ID_MAX];
    CHECK_EQ_U32(od_catalog_save(g_dir, &again, idagain), OD_OK);
    CHECK(strcmp(idagain, "alpha") == 0);
    CHECK_EQ_U32(od_catalog_scan(g_dir, list, 16, &n), OD_OK);
    CHECK_EQ_U32(n, 3);
    int idx = od_catalog_find(list, n, "alpha");
    CHECK(idx >= 0);
    if (idx >= 0)
        CHECK_EQ_U32(list[idx].modified, 999);

    od_led_free(&zeta);
    od_led_free(&alpha);
    od_led_free(&mid);
    cleanup_dir();
}

OD_TEST(test_catalog_ids_case)
OD_TEST(test_catalog_case)
