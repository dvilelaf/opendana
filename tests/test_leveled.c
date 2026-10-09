/* Unit tests for the level-editor model (src/ext/leveled.c): own synthetic documents. */
#include "test.h"
#include "opendana/ext/leveled.h"
#include "opendana/content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OD_REPO_ROOT
#define OD_REPO_ROOT "."
#endif

#define TMP_LEVEL     OD_REPO_ROOT "/build/c54-leveled.json"
#define TMP_LEVEL2    OD_REPO_ROOT "/build/c54-leveled-2.json"

static const char *tmp_path(const char *name)
{
    static char buf[512];
    const char *dir = getenv("TMPDIR");
    snprintf(buf, sizeof buf, "%s/opendana-%s", dir && dir[0] ? dir : "/tmp", name);
    return buf;
}

/* A minimal valid document: solid border (from od_led_new), Hero at (1,1) on
 * an empty cell and a fire cell at (5,5). */
static void make_valid(od_led_doc *d, const char *name)
{
    memset(d, 0, sizeof *d);
    od_led_new(d, 1);
    od_led_paint(d, 5, 5, 0x0E);
    od_led_place_hero(d, 1, 1);
    snprintf(d->name, sizeof d->name, "%s", name);
    snprintf(d->author, sizeof d->author, "%s", "author");
    d->created = 100;
    d->modified = 200;
}

static void test_led_new_case(void)
{
    od_led_doc d;
    memset(&d, 0, sizeof d);
    od_led_new(&d, 7);
    CHECK_EQ_U32(d.theme_world, 7);
    CHECK_EQ_U32(d.editor_version, OD_LED_EDITOR_VERSION);
    CHECK(!od_led_has_hero(&d));
    CHECK_EQ_U32(d.n_objects, 0);
    CHECK_EQ_U32(d.dirty, 0);
    CHECK_EQ_U32(d.grid[0][0], 0x01);       /* solid border */
    CHECK_EQ_U32(d.grid[14][15], 0x01);
    CHECK_EQ_U32(d.grid[7][7], 0x00);       /* empty interior */
    CHECK(strcmp(d.name, "untitled") == 0);
    /* The theme clamps into 1..15. */
    od_led_new(&d, 99);
    CHECK_EQ_U32(d.theme_world, 15);
    od_led_new(&d, 0);
    CHECK_EQ_U32(d.theme_world, 1);
    od_led_free(&d);
}

static void test_led_edit_history_case(void)
{
    od_led_doc d;
    make_valid(&d, "history");
    CHECK_EQ_U32(d.grid[5][5], 0x0E);
    CHECK_EQ_U32(d.dirty, 1);

    /* paint + undo + redo */
    od_led_paint(&d, 6, 6, 0x02);
    CHECK_EQ_U32(d.grid[6][6], 0x02);
    CHECK(od_led_can_undo(&d));
    od_led_undo(&d);
    CHECK_EQ_U32(d.grid[6][6], 0x00);
    CHECK(od_led_can_redo(&d));
    od_led_redo(&d);
    CHECK_EQ_U32(d.grid[6][6], 0x02);

    /* fill a rectangle */
    od_led_fill(&d, 2, 2, 4, 3, 0x03);
    for (int y = 2; y <= 3; y++)
        for (int x = 2; x <= 4; x++)
            CHECK_EQ_U32(d.grid[y][x], 0x03);
    CHECK_EQ_U32(d.grid[1][1], 0x00);   /* outside the rectangle */

    /* A new edit clears the redo stack. */
    od_led_undo(&d);                   /* rectangle gone */
    CHECK(od_led_can_redo(&d));
    od_led_paint(&d, 9, 9, 0x0D);
    CHECK(!od_led_can_redo(&d));

    /* Undo everything: the document returns to the blank new doc. */
    while (od_led_can_undo(&d))
        od_led_undo(&d);
    CHECK_EQ_U32(d.grid[1][1], 0x00);
    CHECK(!od_led_has_hero(&d));
    od_led_free(&d);
}

static void test_led_hero_case(void)
{
    od_led_doc d;
    memset(&d, 0, sizeof d);
    od_led_new(&d, 1);
    od_led_place_hero(&d, 1, 1);
    CHECK(od_led_has_hero(&d));
    CHECK_EQ_U32(d.player, 1 * OD_GRID_W + 1);
    od_led_place_hero(&d, 4, 6);
    CHECK_EQ_U32(d.player, 6 * OD_GRID_W + 4);
    /* Out of range is ignored. */
    od_led_place_hero(&d, 99, 0);
    CHECK_EQ_U32(d.player, 6 * OD_GRID_W + 4);
    od_led_free(&d);
}

static void test_led_objects_case(void)
{
    od_led_doc d;
    make_valid(&d, "objects");
    od_led_paint(&d, 2, 3, 0x05);
    od_led_paint(&d, 3, 3, 0x05);

    od_led_object run;
    memset(&run, 0, sizeof run);
    run.kind = OD_LED_RUN;
    run.type = 0x05;
    run.count = 2;
    run.cells[0] = 3 * OD_GRID_W + 2;
    run.cells[1] = 3 * OD_GRID_W + 3;
    CHECK_EQ_U32(od_led_add_object(&d, &run), 1);

    od_led_object sp;
    memset(&sp, 0, sizeof sp);
    sp.kind = OD_LED_SPECIAL;
    sp.type = 0x81;
    sp.argument = 87;
    CHECK_EQ_U32(od_led_add_object(&d, &sp), 1);
    CHECK_EQ_U32(d.n_objects, 2);

    /* A run whose cells do not match the grid still stores, but the validator
     * reports it. */
    od_led_object bad;
    memset(&bad, 0, sizeof bad);
    bad.kind = OD_LED_RUN;
    bad.type = 0x05;
    bad.count = 1;
    bad.cells[0] = 10 * OD_GRID_W + 10;   /* grid there is empty */
    CHECK_EQ_U32(od_led_add_object(&d, &bad), 1);
    od_led_issue issues[8];
    CHECK(od_led_validate(&d, issues, 8) >= 1);
    CHECK_EQ_U32(od_led_remove_object(&d, 2), 1);
    CHECK_EQ_U32(d.n_objects, 2);

    /* A run with an out-of-range type is refused. */
    od_led_object junk = run;
    junk.type = 0x00;
    CHECK_EQ_U32(od_led_add_object(&d, &junk), 0);

    /* The packed command stream is the package order: run, special, 0. */
    od_level lv;
    uint8_t cmd[OD_LED_CMD_MAX];
    CHECK_EQ_U32(od_led_to_level(&d, &lv, cmd, sizeof cmd), OD_OK);
    CHECK_EQ_U32(lv.command_len, 7);
    CHECK_EQ_U32(cmd[0], 0x05);
    CHECK_EQ_U32(cmd[1], 2);
    CHECK_EQ_U32(cmd[2], 3 * OD_GRID_W + 2);
    CHECK_EQ_U32(cmd[3], 3 * OD_GRID_W + 3);
    CHECK_EQ_U32(cmd[4], 0x81);
    CHECK_EQ_U32(cmd[5], 87);
    CHECK_EQ_U32(cmd[6], 0x00);
    CHECK_EQ_U32(lv.grid[5][5], 0x0E);
    CHECK_EQ_U32(od_player_x(&lv), 1);
    CHECK_EQ_U32(od_player_y(&lv), 1);

    od_led_free(&d);
}

static void test_led_validate_case(void)
{
    od_led_doc d;
    od_led_issue issues[16];

    make_valid(&d, "valid");
    CHECK_EQ_U32(od_led_validate(&d, issues, 16), 0);

    /* Invalid cell type. */
    od_led_paint(&d, 2, 2, 0x1F);
    CHECK_EQ_U32(od_led_validate(&d, issues, 16), 1);
    CHECK(strstr(issues[0].message, "Invalid cell type") != NULL);
    od_led_undo(&d);
    CHECK_EQ_U32(od_led_validate(&d, issues, 16), 0);

    /* No Hero. */
    od_led_doc nohero;
    memset(&nohero, 0, sizeof nohero);
    od_led_new(&nohero, 1);
    od_led_paint(&nohero, 5, 5, 0x0E);
    int errs = od_led_validate(&nohero, issues, 16);
    CHECK_EQ_U32(errs, 1);
    CHECK(strstr(issues[0].message, "Missing player start") != NULL);
    od_led_free(&nohero);

    /* Hero inside a solid cell. */
    od_led_place_hero(&d, 0, 0);   /* border block */
    errs = od_led_validate(&d, issues, 16);
    CHECK_EQ_U32(errs, 1);
    CHECK(strstr(issues[0].message, "solid") != NULL);

    /* No fire: not an error, but the advisory is reported. */
    od_led_doc nofire;
    memset(&nofire, 0, sizeof nofire);
    od_led_new(&nofire, 1);
    od_led_place_hero(&nofire, 1, 1);
    int n = od_led_validate(&nofire, issues, 16);
    CHECK_EQ_U32(n, 0);
    CHECK_EQ_U32(issues[0].severity, OD_LED_WARNING);
    CHECK(strstr(issues[0].message, "No flames") != NULL);
    od_led_free(&nofire);

    od_led_free(&d);
}

static void test_led_roundtrip_case(void)
{
    od_led_doc a;
    make_valid(&a, "round-trip");
    snprintf(a.author, sizeof a.author, "C5.4");
    snprintf(a.id, sizeof a.id, "round-trip");
    a.theme_world = 4;
    a.world = 4;
    a.metadata = 5;
    od_led_paint(&a, 2, 3, 0x05);
    od_led_object run;
    memset(&run, 0, sizeof run);
    run.kind = OD_LED_RUN;
    run.type = 0x05;
    run.count = 1;
    run.cells[0] = 3 * OD_GRID_W + 2;
    CHECK_EQ_U32(od_led_add_object(&a, &run), 1);
    od_led_object sp;
    memset(&sp, 0, sizeof sp);
    sp.kind = OD_LED_SPECIAL;
    sp.type = 0x82;
    sp.argument = 7;
    CHECK_EQ_U32(od_led_add_object(&a, &sp), 1);
    /* The edits above bump `modified`; pin both stamps before saving. */
    a.created = 100;
    a.modified = 200;

    remove(TMP_LEVEL);
    remove(TMP_LEVEL2);
    CHECK_EQ_U32(od_led_save_json(&a, TMP_LEVEL), OD_OK);

    od_led_doc b;
    memset(&b, 0, sizeof b);
    CHECK_EQ_U32(od_led_load_json(&b, TMP_LEVEL), OD_OK);
    CHECK_EQ_U32(b.theme_world, 4);
    CHECK_EQ_U32(b.world, 4);
    CHECK_EQ_U32(b.metadata, 5);
    CHECK_EQ_U32(b.player, a.player);
    CHECK_EQ_U32(b.n_objects, 2);
    CHECK(strcmp(b.name, "round-trip") == 0);
    CHECK(strcmp(b.author, "C5.4") == 0);
    CHECK_EQ_U32(b.created, 100);
    CHECK_EQ_U32(b.modified, 200);
    CHECK_EQ_MEM(b.grid, a.grid, sizeof a.grid);
    CHECK_EQ_U32(b.objects[0].kind, OD_LED_RUN);
    CHECK_EQ_U32(b.objects[0].type, 0x05);
    CHECK_EQ_U32(b.objects[0].cells[0], 3 * OD_GRID_W + 2);
    CHECK_EQ_U32(b.objects[1].kind, OD_LED_SPECIAL);
    CHECK_EQ_U32(b.objects[1].type, 0x82);
    CHECK_EQ_U32(b.objects[1].argument, 7);

    /* Byte-for-byte ida y vuelta: save the loaded document again. */
    CHECK_EQ_U32(od_led_save_json(&b, TMP_LEVEL2), OD_OK);
    FILE *f1 = fopen(TMP_LEVEL, "rb");
    FILE *f2 = fopen(TMP_LEVEL2, "rb");
    CHECK(f1 != NULL && f2 != NULL);
    if (f1 && f2) {
        char buf1[4096], buf2[4096];
        size_t n1 = fread(buf1, 1, sizeof buf1, f1);
        size_t n2 = fread(buf2, 1, sizeof buf2, f2);
        CHECK_EQ_U32(n1, n2);
        CHECK_EQ_MEM(buf1, buf2, n1);
    }
    if (f1) fclose(f1);
    if (f2) fclose(f2);

    /* The same loader `--play-level` uses accepts it. */
    od_content_level cl;
    memset(&cl, 0, sizeof cl);
    CHECK_EQ_U32(od_content_level_load(&cl, TMP_LEVEL, 1), OD_OK);
    CHECK_EQ_U32(od_player_x(&cl.level), 1);
    CHECK_EQ_U32(cl.level.grid[5][5], 0x0E);
    CHECK_EQ_U32(cl.level.command_len, b.objects[0].count + 2 + 2 + 1);
    od_content_level_free(&cl);

    od_validate_report rep;
    CHECK_EQ_U32(od_content_validate(TMP_LEVEL, &rep), OD_OK);
    CHECK_EQ_U32(rep.errors, 0);
    CHECK_EQ_U32(rep.levels, 1);

    od_led_free(&a);
    od_led_free(&b);
    remove(TMP_LEVEL);
    remove(TMP_LEVEL2);
}

static void test_led_load_reject_case(void)
{
    const char *bad_grid = tmp_path("c54-badgrid.json");
    FILE *f = fopen(bad_grid, "wb");
    CHECK(f != NULL);
    if (f) {
        /* 14 grid rows instead of 15. */
        fputs("{\"schema_version\":1,\"id\":\"x\",\"world\":1,"
              "\"grid_width\":16,\"grid_height\":15,\"levels\":[{"
              "\"index\":0,\"number\":1,\"world\":1,\"round\":1,\"name\":\"x\","
              "\"player\":{\"x\":1,\"y\":1},\"metadata\":0,\"grid\":[", f);
        for (int y = 0; y < 14; y++)
            fprintf(f, "\"%s\"%s", "1111111111111111", y < 13 ? "," : "");
        fputs("],\"objects\":[]}]}", f);
        fclose(f);
        od_led_doc d;
        memset(&d, 0, sizeof d);
        CHECK_EQ_U32(od_led_load_json(&d, bad_grid), OD_ERR_FORMAT);
        od_led_free(&d);
        remove(bad_grid);
    }

    const char *bad_type = tmp_path("c54-badtype.json");
    f = fopen(bad_type, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("{\"schema_version\":1,\"id\":\"x\",\"world\":1,"
              "\"grid_width\":16,\"grid_height\":15,\"levels\":[{"
              "\"index\":0,\"number\":1,\"world\":1,\"round\":1,\"name\":\"x\","
              "\"player\":{\"x\":1,\"y\":1},\"metadata\":0,\"grid\":[", f);
        for (int y = 0; y < 15; y++)
            fprintf(f, "\"%s\"%s", "0000000000000000", y < 14 ? "," : "");
        fputs("],\"objects\":[{\"type\":0,\"positions\":[[1,1]]}]}]}", f);
        fclose(f);
        od_led_doc d;
        memset(&d, 0, sizeof d);
        CHECK_EQ_U32(od_led_load_json(&d, bad_type), OD_ERR_FORMAT);
        od_led_free(&d);
        remove(bad_type);
    }

    /* Missing file is an IO error. */
    od_led_doc d;
    memset(&d, 0, sizeof d);
    CHECK_EQ_U32(od_led_load_json(&d, "/nonexistent/opendana-c54.json"), OD_ERR_IO);
    od_led_free(&d);
}

OD_TEST(test_led_new_case)
OD_TEST(test_led_edit_history_case)
OD_TEST(test_led_hero_case)
OD_TEST(test_led_objects_case)
OD_TEST(test_led_validate_case)
OD_TEST(test_led_roundtrip_case)
OD_TEST(test_led_load_reject_case)
