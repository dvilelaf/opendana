/* test_ui_editor.c - unit tests for the editor screen (pure, no SDL). */
#include "test.h"
#include "opendana/ui_editor.h"
#include "opendana/ext/catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *tmp_dir(void)
{
    static char buf[512];
    const char *dir = getenv("TMPDIR");
    snprintf(buf, sizeof buf, "%s/opendana-c55-editor", dir && dir[0] ? dir : "/tmp");
    return buf;
}

/* Click the centre of grid cell (x,y) with `buttons`/`click`. */
static void click_cell(od_ui_editor *e, int x, int y, int buttons, int click)
{
    od_ued_pointer(e, OD_UED_LEVEL_X + x * 16 + 8, OD_UED_LEVEL_Y + y * 16 + 8,
                    buttons, click, 0, 0, 0);
}

static void test_ued_new_and_paint(void)
{
    od_ui_editor e;
    od_ued_init(&e, tmp_dir());
    CHECK(e.open == 1);
    CHECK_EQ_U32(e.doc.theme_world, 1);
    CHECK_EQ_U32(e.doc.grid[0][0], 0x01);
    CHECK(!od_led_has_hero(&e.doc));

    /* Default brush is a solid block. */
    click_cell(&e, 3, 3, OD_UED_BTN_LEFT, 1);
    CHECK_EQ_U32(e.doc.grid[3][3], 0x01);
    click_cell(&e, 4, 3, OD_UED_BTN_LEFT, 1);
    CHECK_EQ_U32(e.doc.grid[3][4], 0x01);

    /* Right click erases. */
    click_cell(&e, 3, 3, OD_UED_BTN_RIGHT, 1);
    CHECK_EQ_U32(e.doc.grid[3][3], 0x00);

    od_ued_free(&e);
}

static void test_ued_palette_and_hero(void)
{
    od_ui_editor e;
    od_ued_init(&e, tmp_dir());

    /* Click palette slot of 0x0E (fire is index 14) and paint. */
    int px, py, pw, ph;
    od_ued_palette_rect(14, &px, &py, &pw, &ph);
    od_ued_pointer(&e, px + 2, py + 3, OD_UED_BTN_LEFT, 1, 0, 0, 0);
    CHECK_EQ_U32(od_ued_brush_cell(&e, NULL), 0x0E);
    click_cell(&e, 5, 5, OD_UED_BTN_LEFT, 1);
    CHECK_EQ_U32(e.doc.grid[5][5], 0x0E);

    /* Hero brush (last palette slot) places the unique start cell. */
    int n = od_ued_palette_count();
    od_ued_palette_rect(n - 1, &px, &py, &pw, &ph);
    od_ued_pointer(&e, px + 2, py + 3, OD_UED_BTN_LEFT, 1, 0, 0, 0);
    CHECK_EQ_U32(od_ued_brush_cell(&e, NULL), OD_UED_TOOL_HERO);
    click_cell(&e, 2, 2, OD_UED_BTN_LEFT, 1);
    CHECK(od_led_has_hero(&e.doc));
    CHECK_EQ_U32(e.doc.player & 0x0F, 2);
    CHECK_EQ_U32(e.doc.player >> 4, 2);

    /* Eyedropper: middle click on the fire cell selects fire again. */
    od_ued_pointer(&e, OD_UED_LEVEL_X + 4 * 16 + 8, OD_UED_LEVEL_Y + 3 * 16 + 8, 0, 0, 0, 0, 0);
    e.doc.grid[3][4] = 0x0E;   /* seed a cell to sample */
    od_ued_pointer(&e, OD_UED_LEVEL_X + 4 * 16 + 8, OD_UED_LEVEL_Y + 3 * 16 + 8, OD_UED_BTN_MIDDLE, 1, 0, 0, 0);
    CHECK_EQ_U32(od_ued_brush_cell(&e, NULL), 0x0E);

    od_ued_free(&e);
}

static void test_ued_fill_undo(void)
{
    od_ui_editor e;
    od_ued_init(&e, tmp_dir());
    /* Shift+left drag from (1,1) to (3,2) fills with the block brush. */
    od_ued_pointer(&e, OD_UED_LEVEL_X + 1 * 16 + 8, OD_UED_LEVEL_Y + 1 * 16 + 8, OD_UED_BTN_LEFT, 1, 0, 1, 0);
    od_ued_pointer(&e, OD_UED_LEVEL_X + 3 * 16 + 8, OD_UED_LEVEL_Y + 2 * 16 + 8, OD_UED_BTN_LEFT, 0, 0, 1, 0);
    od_ued_pointer(&e, OD_UED_LEVEL_X + 3 * 16 + 8, OD_UED_LEVEL_Y + 2 * 16 + 8, 0, 0, 0, 0, 0);
    for (int y = 1; y <= 2; y++)
        for (int x = 1; x <= 3; x++)
            CHECK_EQ_U32(e.doc.grid[y][x], 0x01);

    od_ued_key(&e, OD_UED_KEY_UNDO);
    CHECK_EQ_U32(e.doc.grid[1][1], 0x00);
    od_ued_key(&e, OD_UED_KEY_REDO);
    CHECK_EQ_U32(e.doc.grid[1][1], 0x01);
    od_ued_free(&e);
}

static void test_ued_validate_and_draw(void)
{
    od_ui_editor e;
    od_ued_init(&e, tmp_dir());
    char msg[160];
    CHECK(od_ued_validate(&e, msg, sizeof msg) > 0);   /* no Hero */

    int n = od_ued_palette_count();
    int px, py, pw, ph;
    od_ued_palette_rect(n - 1, &px, &py, &pw, &ph);
    od_ued_pointer(&e, px + 2, py + 3, OD_UED_BTN_LEFT, 1, 0, 0, 0);
    click_cell(&e, 2, 2, OD_UED_BTN_LEFT, 1);          /* Hero */
    od_ued_palette_rect(14, &px, &py, &pw, &ph);
    od_ued_pointer(&e, px + 2, py + 3, OD_UED_BTN_LEFT, 1, 0, 0, 0);
    click_cell(&e, 5, 5, OD_UED_BTN_LEFT, 1);          /* fire */
    CHECK_EQ_U32(od_ued_validate(&e, msg, sizeof msg), 0);

    /* Draw with a preview: the level pixels are copied, the panels differ. */
    static uint32_t preview[OD_UED_W * OD_UED_H];
    static uint32_t canvas[OD_UED_W * OD_UED_H];
    for (int i = 0; i < OD_UED_W * OD_UED_H; i++)
        preview[i] = 0xFF102030u;
    e.preview = preview;
    e.preview_w = OD_UED_W;
    e.preview_h = OD_UED_H;
    od_ued_draw(&e, canvas, OD_UED_W, OD_UED_H);
    CHECK_EQ_U32(canvas[20 * OD_UED_W + 20], 0xFF102030u);   /* level      */
    CHECK(canvas[8 * OD_UED_W + 50] != 0xFF102030u);         /* toolbar    */
    CHECK(canvas[100 * OD_UED_W + 300] != 0xFF102030u);      /* palette    */
    od_ued_free(&e);
}

static void test_ued_catalog_roundtrip(void)
{
    od_ui_editor e;
    od_ued_init(&e, tmp_dir());
    snprintf(e.doc.name, sizeof e.doc.name, "Mouse test");
    od_led_paint(&e.doc, 5, 5, 0x0E);
    od_led_place_hero(&e.doc, 2, 2);
    CHECK_EQ_U32(od_ued_save(&e, 1), OD_OK);
    CHECK(e.last_saved_id[0] != '\0');
    CHECK(e.last_saved_path[0] != '\0');

    od_ui_editor back;
    od_ued_init(&back, tmp_dir());
    CHECK_EQ_U32(od_ued_load(&back, e.last_saved_path), OD_OK);
    CHECK_EQ_U32(back.doc.grid[5][5], 0x0E);
    CHECK(od_led_has_hero(&back.doc));
    CHECK_EQ_U32(back.doc.player & 0x0F, 2);
    CHECK(strcmp(back.doc.name, "Mouse test") == 0);

    od_ued_free(&back);
    od_ued_free(&e);
}

OD_TEST(test_ued_new_and_paint)
OD_TEST(test_ued_palette_and_hero)
OD_TEST(test_ued_fill_undo)
OD_TEST(test_ued_validate_and_draw)
OD_TEST(test_ued_catalog_roundtrip)

static void test_ued_english_labels(void)
{
    od_ui_editor e;
    od_ued_init(&e, tmp_dir());
    CHECK(strcmp(od_ued_cell_name(0x02), "Ice") == 0);
    CHECK(strcmp(od_ued_cell_name(0x0C), "Stone") == 0);
    CHECK(strcmp(od_ued_palette()[0].name, "Empty") == 0);
    CHECK(strstr(e.status, "New level") != NULL);
    char msg[160];
    CHECK(od_ued_validate(&e, msg, sizeof msg) > 0);
    CHECK(strcmp(msg, "Missing Player") == 0);
    od_led_place_hero(&e.doc, 2, 2);
    CHECK_EQ_U32(od_ued_validate(&e, msg, sizeof msg), 0);
    CHECK(strcmp(msg, "No fires: level completes at start") == 0);
    od_led_paint(&e.doc, 5, 5, 0x0E);
    CHECK_EQ_U32(od_ued_validate(&e, msg, sizeof msg), 0);
    CHECK_EQ_U32(msg[0], 0);
    od_ued_free(&e);
}
OD_TEST(test_ued_english_labels)
