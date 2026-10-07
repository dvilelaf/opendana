/* test_ui_catalog.c - unit tests for the catalogue browser. */
#include "test.h"
#include "opendana/ui_catalog.h"
#include "opendana/ext/catalog.h"
#include "opendana/png.h"
#include "opendana/ui_strings.h"
#include "opendana/ui_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *tmp_dir(void)
{
    static char buf[512];
    const char *dir = getenv("TMPDIR");
    snprintf(buf, sizeof buf, "%s/opendana-c55-catalog", dir && dir[0] ? dir : "/tmp");
    return buf;
}

static void make_level(od_led_doc *d, const char *name)
{
    memset(d, 0, sizeof *d);
    od_led_new(d, 1);
    od_led_paint(d, 5, 5, 0x0E);
    od_led_place_hero(d, 1, 1);
    snprintf(d->name, sizeof d->name, "%s", name);
    snprintf(d->author, sizeof d->author, "%s", "tester");
}

static void test_ucat_list_sort_select(void)
{
    const char *dir = tmp_dir();
    od_led_doc d;
    char id[OD_LED_ID_MAX];
    make_level(&d, "Beta");
    od_catalog_save(dir, &d, id);
    od_led_free(&d);
    make_level(&d, "Alpha");
    od_catalog_save(dir, &d, id);
    od_led_free(&d);

    od_ui_catalog c;
    od_ucat_init(&c, dir);
    CHECK_EQ_U32(od_ucat_refresh(&c), OD_OK);
    CHECK(c.n >= 2);
    CHECK(strcmp(c.entries[0].name, "Alpha") == 0);   /* sorted by name */

    /* Pointer over the first row selects it and asks to play it. */
    int rx, ry, rw, rh;
    od_ucat_row_rect(0, &rx, &ry, &rw, &rh);
    c.request = OD_UCAT_REQ_NONE;
    od_ucat_pointer(&c, rx + 4, ry + 4, 1, 1, 0);
    CHECK_EQ_U32(c.focus, 0);
    CHECK_EQ_U32(c.request, OD_UCAT_REQ_PLAY);
    CHECK(strcmp(c.request_id, c.entries[0].id) == 0);

    /* Keyboard moves the focus around the list. */
    od_ucat_key(&c, OD_UCAT_KEY_DOWN, 0);
    CHECK_EQ_U32(c.focus, 1);
    od_ucat_key(&c, OD_UCAT_KEY_UP, 0);
    CHECK_EQ_U32(c.focus, 0);

    /* The Sort button flips the order. */
    int bx, by, bw, bh;
    od_ucat_button_rect(OD_UCAT_BTN_SORT, &bx, &by, &bw, &bh);
    int before = c.order;
    od_ucat_pointer(&c, bx + 2, by + 2, 1, 1, 0);
    CHECK(c.order != before);
}

static void test_ucat_delete_import_export_requests(void)
{
    const char *dir = tmp_dir();
    od_led_doc d;
    char id[OD_LED_ID_MAX];
    make_level(&d, "ToDelete");
    od_catalog_save(dir, &d, id);
    od_led_free(&d);

    od_ui_catalog c;
    od_ucat_init(&c, dir);
    od_ucat_refresh(&c);
    int n_before = c.n;
    /* Focus the entry to delete and use the Delete button. */
    for (int i = 0; i < c.n; i++)
        if (strcmp(c.entries[i].id, id) == 0)
            c.focus = i;
    int bx, by, bw, bh;
    od_ucat_button_rect(OD_UCAT_BTN_DELETE, &bx, &by, &bw, &bh);
    od_ucat_pointer(&c, bx + 2, by + 2, 1, 1, 0);
    /* Delete asks first; Yes deletes. */
    CHECK(c.confirm_delete);
    CHECK_EQ_U32(c.n, n_before);
    od_ucat_confirm_button_rect(1, &bx, &by, &bw, &bh);
    od_ucat_pointer(&c, bx + 2, by + 2, 1, 1, 0);
    CHECK_EQ_U32(c.n, n_before - 1);

    /* Import takes the source from io_path; Export takes the focused id.
     * both run in the catalogue and report their outcome. */
    snprintf(c.io_path, sizeof c.io_path, "%s/source.json", dir);
    od_ucat_button_rect(OD_UCAT_BTN_IMPORT, &bx, &by, &bw, &bh);
    c.request = OD_UCAT_REQ_NONE;
    od_ucat_pointer(&c, bx + 2, by + 2, 1, 1, 0);
    CHECK(c.status_error && strstr(c.status, "Import failed") != NULL);

    if (c.n > 0) {
        char out[700];
        snprintf(c.io_path, sizeof c.io_path, "%s-exports/", dir);
        snprintf(out, sizeof out, "%s-exports/%s.json", dir, c.entries[c.focus].id);
        remove(out);
        od_ucat_button_rect(OD_UCAT_BTN_EXPORT, &bx, &by, &bw, &bh);
        c.request = OD_UCAT_REQ_NONE;
        od_ucat_pointer(&c, bx + 2, by + 2, 1, 1, 0);
        CHECK(!c.status_error && strstr(c.status, "Exported to") != NULL);
        FILE *f = fopen(out, "rb");
        CHECK(f != NULL);
        if (f) fclose(f);
        remove(out);
    }
}

static void test_ucat_draw_and_geometry(void)
{
    od_ui_catalog c;
    od_ucat_init(&c, tmp_dir());
    od_ucat_refresh(&c);

    int x, y, w, h;
    od_ucat_button_rect(OD_UCAT_BTN_PLAY, &x, &y, &w, &h);
    CHECK(w > 0 && h > 0);
    od_ucat_row_rect(0, &x, &y, &w, &h);
    CHECK(y >= 0 && y < OD_UCAT_H);
    if (c.n > 0)
        CHECK(od_ucat_entry_at(&c, 10, y + 2) == 0);
    CHECK(od_ucat_entry_at(&c, 10, 0) < 0);

    static uint32_t canvas[OD_UCAT_W * OD_UCAT_H];
    memset(canvas, 0, sizeof canvas);
    od_ucat_draw(&c, canvas, OD_UCAT_W, OD_UCAT_H);
    CHECK(canvas[20 * OD_UCAT_W + 20] != 0);
    {   /* The title bar is filled. */
        int changed = 0;
        for (int i = 0; i < OD_UCAT_W; i++)
            if (canvas[i] != 0)
                changed = 1;
        CHECK(changed);
    }
    char defdir[1024];
    od_ucat_default_dir(defdir, sizeof defdir);
    CHECK(defdir[0] != '\0');
}

/* Own solid-colour PNG: no game assets and no content/base lookup. */
static void test_ucat_thumbnail_pixels(void)
{
    od_led_doc d;
    char id[OD_LED_ID_MAX];
    char dir[600];
    snprintf(dir, sizeof dir, "%s-thumbnail", tmp_dir());
    make_level(&d, "000 Thumbnail proof");
    snprintf(d.id, sizeof d.id, "proof");
    CHECK_EQ_U32(od_catalog_save(dir, &d, id), OD_OK);
    od_led_free(&d);
    od_ui_catalog c;
    od_ucat_init(&c, dir);
    od_ucat_refresh(&c);
    CHECK(c.n > 0);
    if (c.n <= 0) return;
    char path[1500];
    snprintf(path, sizeof path, "%s/%s.png", c.dir, c.entries[0].id);
    unsigned char rgb[8 * 4 * 3];
    for (size_t i = 0; i < sizeof rgb; i += 3) {
        rgb[i] = 18; rgb[i + 1] = 171; rgb[i + 2] = 52;
    }
    CHECK_EQ_U32(od_png_write_rgb(path, 8, 4, rgb), 0);
    CHECK_EQ_U32(od_ucat_refresh(&c), OD_OK);
    static uint32_t cv[OD_UCAT_W * OD_UCAT_H];
    od_ucat_draw(&c, cv, OD_UCAT_W, OD_UCAT_H);
    int count = 0;
    for (size_t i = 0; i < sizeof cv / sizeof cv[0]; i++)
        if (cv[i] == 0xFF12AB34u) count++;
    CHECK(count > 0);
    CHECK_EQ_U32(count, 32 * 16);
    CHECK_EQ_U32(c.thumbnails[0].w, 32);
    CHECK_EQ_U32(c.thumbnails[0].h, 16);
    /* Refresh invalidates a cached image. Oversized IHDR is rejected before
     * decoding; stb ignores CRC, so these edited dimension bytes exercise
     * our own guard rather than only a generic corrupt-file rejection. */
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL);
    if (f) {
        const unsigned char huge[4] = { 0x7f, 0xff, 0xff, 0xff };
        fseek(f, 16, SEEK_SET);
        fwrite(huge, 1, 4, f);
        fclose(f);
    }
    od_ucat_refresh(&c);
    od_ucat_draw(&c, cv, OD_UCAT_W, OD_UCAT_H);
    CHECK_EQ_U32(c.thumbnails[0].w, 0);
    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { fputs("not a PNG", f); fclose(f); }
    od_ucat_refresh(&c);
    od_ucat_draw(&c, cv, OD_UCAT_W, OD_UCAT_H);
    CHECK_EQ_U32(c.thumbnails[0].w, 0);
    /* Even a valid PNG may not be requested through a traversal path. */
    CHECK_EQ_U32(od_png_write_rgb(path, 8, 4, rgb), 0);
    od_ucat_refresh(&c);
    snprintf(c.entries[0].thumb, sizeof c.entries[0].thumb, "../outside.png");
    od_ucat_draw(&c, cv, OD_UCAT_W, OD_UCAT_H);
    CHECK_EQ_U32(c.thumbnails[0].w, 0);
    /* Missing thumbnail never substitutes pixels from a host preview. */
    static uint32_t preview[OD_UCAT_W * OD_UCAT_H];
    for (size_t i = 0; i < sizeof preview / sizeof preview[0]; i++)
        preview[i] = 0xFF12AB34u;
    c.preview = preview;
    remove(path);
    od_ucat_refresh(&c);
    od_ucat_draw(&c, cv, OD_UCAT_W, OD_UCAT_H);
    count = 0;
    for (size_t i = 0; i < sizeof cv / sizeof cv[0]; i++)
        if (cv[i] == 0xFF12AB34u) count++;
    CHECK_EQ_U32(count, 0);
    /* A smaller host canvas cannot be overrun by thumbnail or text drawing. */
    uint32_t clipped[8 * 8 + 2];
    clipped[0] = clipped[8 * 8 + 1] = 0x12345678u;
    od_ucat_draw(&c, clipped + 1, 8, 8);
    CHECK_EQ_U32(clipped[0], 0x12345678u);
    CHECK_EQ_U32(clipped[8 * 8 + 1], 0x12345678u);
    od_ucat_free(&c);
    CHECK_EQ_U32(c.thumbnails[0].attempted, 0);
    CHECK_EQ_U32(od_catalog_delete(dir, id), OD_OK);
}

OD_TEST(test_ucat_list_sort_select)
OD_TEST(test_ucat_delete_import_export_requests)
OD_TEST(test_ucat_draw_and_geometry)
OD_TEST(test_ucat_thumbnail_pixels)

static void test_ucat_english_status(void)
{
    od_ui_catalog c;
    od_ucat_init(&c, tmp_dir());
    CHECK(strcmp(c.status, "Custom levels") == 0);
    CHECK_EQ_U32(od_ucat_refresh(&c), OD_OK);
    CHECK(strstr(c.status, "custom level(s)") != NULL);
    CHECK_EQ_U32(OD_STR_MENU_TITLE, 0);
    CHECK_EQ_U32(OD_STR_ASPECT_87, 11);
    CHECK_EQ_U32(OD_STR_PAUSED, 32);
    for (int i = OD_STR_CUSTOM_LEVELS; i < OD_STR_COUNT; i++)
        CHECK(od_str((od_string)i)[0] != '\0');
    /* Real raster proof of a failed import's message: no OCR. The
     * message is English where it is made; no translation table. */
    static uint32_t actual[OD_UCAT_W * OD_UCAT_H], expected[OD_UCAT_W * OD_UCAT_H];
    snprintf(c.io_path, sizeof c.io_path, "%s/missing-source.json", tmp_dir());
    int bx, by, bw, bh;
    od_ucat_button_rect(OD_UCAT_BTN_IMPORT, &bx, &by, &bw, &bh);
    od_ucat_pointer(&c, bx + 2, by + 2, 1, 1, 0);
    CHECK_EQ_U32(c.status_error, 1);
    od_ucat_draw(&c, actual, OD_UCAT_W, OD_UCAT_H);
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++)
        expected[i] = 0xFF14142Cu;
    od_ui_font_text(expected, OD_UCAT_W, OD_UCAT_H, 4, 284,
                     od_str(OD_STR_IMPORT_ERROR), 0xFFFF8080u, 1);
    CHECK_EQ_MEM(actual + 284 * OD_UCAT_W, expected + 284 * OD_UCAT_W,
                 8 * OD_UCAT_W * sizeof(uint32_t));
    od_ucat_free(&c);
}
OD_TEST(test_ucat_english_status)
