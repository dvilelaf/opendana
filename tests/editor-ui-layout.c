/* editor-ui-layout.c - layout gate of the modern editor and catalogue.
 *
 * Pure C, no SDL, no game content. Built by tests/editor-ui-layout-run.sh with
 * -Wl,--wrap=od_ui_font_text, so every text the two screens draw is recorded
 * with its rectangle while it is still drawn for real. The gate asserts what
 * the owner reported broken (2026-10-06):
 *
 *   - no text leaves the canvas, overlaps another text or crosses the border
 *     of its button/row; fixed labels are drawn in full;
 *   - no interface element covers the 256x240 level, and every cell of the
 *     16x15 grid (rows 0 and 14, columns 0 and 15 included) is under the
 *     pointer at its own place;
 *   - the presentation is a whole multiple of the canvas at window scales 2,
 *     3, 4 and maximised (1920x1080), so containment holds on screen too;
 *   - the test-play view keeps all 240 level rows and puts the game's status
 *     bar below them.
 *
 * Positive control: compiled against the earlier sources (OD_UI_SRC, see
 * the runner) it must fail; the overlay layout covered the level. */
#include "opendana/ui_editor.h"
#include "opendana/ui_catalog.h"
#include "opendana/ui_font.h"
#include "opendana/ui_strings.h"
#include "opendana/ext/catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OD_UED_LEVEL_X          /* earlier header: the overlay layout */
#define OLD_LAYOUT 1
#define OD_UED_LEVEL_X 0
#define OD_UED_LEVEL_Y 0
#endif

static int checks, failures;
#define EXPECT(cond, ...)                                                     \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            if (failures <= 5000) {                                             \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
                printf(__VA_ARGS__);                                          \
                printf("\n");                                                 \
            }                                                                 \
        }                                                                     \
    } while (0)

/* ---- recorded text runs ------------------------------------------------- */

typedef struct { int x, y, w, h, scale; uint32_t argb; char text[200]; } run;
static run runs[1024];
static int nruns;

int __real_od_ui_font_text(uint32_t *canvas, int cw, int ch, int x, int y,
                            const char *text, uint32_t argb, int scale);
int __wrap_od_ui_font_text(uint32_t *canvas, int cw, int ch, int x, int y,
                            const char *text, uint32_t argb, int scale)
{
    if (text != NULL && text[0] != '\0' && strspn(text, " ") != strlen(text) &&
        nruns < (int)(sizeof runs / sizeof runs[0])) {
        run *r = &runs[nruns++];
        r->x = x;
        r->y = y;
        r->w = (int)strlen(text) * OD_UI_FONT_W * scale;
        r->h = OD_UI_FONT_H * scale;
        r->scale = scale;
        r->argb = argb;
        snprintf(r->text, sizeof r->text, "%s", text);
    }
    return __real_od_ui_font_text(canvas, cw, ch, x, y, text, argb, scale);
}

/* Every glyph pixel of every recorded text is still on the final canvas:
 * nothing drawn later (a panel, another box) hides a label. */
static void check_visible(const char *screen, const uint32_t *final, int cw, int ch)
{
    static uint32_t probe[OD_UED_W * OD_UED_H > OD_UCAT_W * OD_UCAT_H ?
                          OD_UED_W * OD_UED_H : OD_UCAT_W * OD_UCAT_H];
    for (int i = 0; i < nruns; i++) {
        const run *r = &runs[i];
        if ((r->argb >> 24) != 0xFFu)
            continue;
        memset(probe, 0, sizeof probe);
        __real_od_ui_font_text(probe, cw, ch, r->x, r->y, r->text, r->argb,
                                r->scale);
        int ink = 0, hidden = 0;
        for (int p = 0; p < cw * ch; p++)
            if (probe[p] != 0) {
                ink++;
                hidden += final[p] != probe[p];
            }
        EXPECT(ink > 0 && hidden == 0,
               "%s: text \"%s\" at %d,%d is hidden (%d of %d glyph pixels "
               "overdrawn)", screen, r->text, r->x, r->y, hidden, ink);
    }
}

static int overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static int inside(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax >= bx && ay >= by && ax + aw <= bx + bw && ay + ah <= by + bh;
}

/* Every run inside the canvas, off the level (editor only) and apart. */
static void check_runs(const char *screen, int cw, int ch, int level)
{
    for (int i = 0; i < nruns; i++) {
        const run *r = &runs[i];
        EXPECT(inside(r->x, r->y, r->w, r->h, 0, 0, cw, ch),
               "%s: text \"%s\" at %d,%d %dx%d leaves the %dx%d canvas",
               screen, r->text, r->x, r->y, r->w, r->h, cw, ch);
        if (level)
            EXPECT(!overlap(r->x, r->y, r->w, r->h, OD_UED_LEVEL_X,
                            OD_UED_LEVEL_Y, 256, 240),
                   "%s: text \"%s\" at %d,%d covers the level", screen,
                   r->text, r->x, r->y);
        for (int j = i + 1; j < nruns; j++) {
            const run *s = &runs[j];
            EXPECT(!overlap(r->x, r->y, r->w, r->h, s->x, s->y, s->w, s->h),
                   "%s: text \"%s\" at %d,%d overlaps \"%s\" at %d,%d",
                   screen, r->text, r->x, r->y, s->text, s->x, s->y);
        }
    }
}

/* Each text crossing a box lies inside its border; `label` must be one of
 * them, drawn in full. */
static void check_box(const char *screen, const char *what, int x, int y,
                      int w, int h, const char *label)
{
    int found = label == NULL;
    for (int i = 0; i < nruns; i++) {
        const run *r = &runs[i];
        if (!overlap(r->x, r->y, r->w, r->h, x, y, w, h))
            continue;
        EXPECT(inside(r->x, r->y, r->w, r->h, x + 1, y + 1, w - 2, h - 2),
               "%s: \"%s\" at %d,%d %dx%d crosses the border of %s %d,%d %dx%d",
               screen, r->text, r->x, r->y, r->w, r->h, what, x, y, w, h);
        if (label != NULL && strcmp(r->text, label) == 0)
            found = 1;
    }
    EXPECT(found, "%s: %s does not show its full label \"%s\"", screen, what,
           label ? label : "");
}

/* ---- editor ------------------------------------------------------------- */

static uint32_t preview[256 * 240];
static uint32_t canvas[OD_UED_W * OD_UED_H];

static const char *const k_button_labels[OD_UED_BTN_COUNT] = {
    [OD_UED_BTN_NEW] = "New", [OD_UED_BTN_OPEN] = "Open",
    [OD_UED_BTN_SAVE] = "Save", [OD_UED_BTN_SAVEAS] = "Save as",
    [OD_UED_BTN_TEST] = "Test", [OD_UED_BTN_UNDO] = "Undo",
    [OD_UED_BTN_REDO] = "Redo", [OD_UED_BTN_WORLD_DOWN] = "W-",
    [OD_UED_BTN_WORLD_UP] = "W+", [OD_UED_BTN_FLAME] = "Flame",
    [OD_UED_BTN_BACK] = "Back",
};

static void draw_editor(od_ui_editor *e)
{
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 256; x++)
            preview[y * 256 + x] = 0xFF000000u | (uint32_t)(y << 9) | (uint32_t)x;
    e->preview = preview;
    e->preview_w = 256;
    e->preview_h = 240;
    e->hover_cell_x = e->hover_cell_y = -1;
    memset(canvas, 0, sizeof canvas);
    nruns = 0;
    od_ued_draw(e, canvas, OD_UED_W, OD_UED_H);
}

static void check_editor(const char *state, od_ui_editor *e)
{
    draw_editor(e);
    /* The level is intact: no panel, bar or text over any of its pixels. */
    int covered = 0;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 256; x++) {
            int cx = OD_UED_LEVEL_X + x, cy = OD_UED_LEVEL_Y + y;
            if (cx >= OD_UED_W || cy >= OD_UED_H ||
                canvas[(size_t)cy * OD_UED_W + cx] != preview[y * 256 + x])
                covered++;
        }
    EXPECT(covered == 0, "editor %s: %d of 61440 level pixels are covered by the "
                         "interface", state, covered);
    check_runs(state, OD_UED_W, OD_UED_H, 1);
    check_visible(state, canvas, OD_UED_W, OD_UED_H);

    for (int b = 0; b < OD_UED_BTN_COUNT; b++) {
        int x, y, w, h;
        od_ued_button_rect(b, &x, &y, &w, &h);
        EXPECT(w > 0 && h > 0 && inside(x, y, w, h, 0, 0, OD_UED_W, OD_UED_H),
               "editor %s: button %s outside the canvas", state, k_button_labels[b]);
        EXPECT(!overlap(x, y, w, h, OD_UED_LEVEL_X, OD_UED_LEVEL_Y, 256, 240),
               "editor %s: button %s covers the level", state, k_button_labels[b]);
        EXPECT(h >= OD_UI_FONT_H + 2,
               "editor %s: button %s is %d px high, its 8 px label does not fit "
               "inside the border", state, k_button_labels[b], h);
        check_box(state, k_button_labels[b], x, y, w, h, k_button_labels[b]);
        for (int o = b + 1; o < OD_UED_BTN_COUNT; o++) {
            int ox, oy, ow, oh;
            od_ued_button_rect(o, &ox, &oy, &ow, &oh);
            EXPECT(!overlap(x, y, w, h, ox, oy, ow, oh),
                   "editor %s: buttons %s and %s overlap", state,
                   k_button_labels[b], k_button_labels[o]);
        }
    }
    if (!e->palette_hidden) {
        const od_ued_palette_entry *pal = od_ued_palette();
        for (int i = 0; i < od_ued_palette_count(); i++) {
            int x, y, w, h;
            od_ued_palette_rect(i, &x, &y, &w, &h);
            EXPECT(inside(x, y, w, h, 0, 0, OD_UED_W, OD_UED_H),
                   "editor %s: palette row %d outside the canvas", state, i);
            EXPECT(!overlap(x, y, w, h, OD_UED_LEVEL_X, OD_UED_LEVEL_Y, 256, 240),
                   "editor %s: palette row %s covers the level", state, pal[i].name);
            check_box(state, "palette row", x, y, w, h, pal[i].name);
        }
    }
}

static void test_editor(void)
{
    od_ui_editor e;
    char dir[600];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/editor-ui-layout", tmp && tmp[0] ? tmp : "/tmp");
    od_ued_init(&e, dir);

    check_editor("new document", &e);

    /* Every cell of the grid answers at its own place. */
    int wrong = 0;
    for (int cy = 0; cy < OD_GRID_H; cy++)
        for (int cx = 0; cx < OD_GRID_W; cx++) {
            int gx = -1, gy = -1;
            int hit = od_ued_cell_at(&e, OD_UED_LEVEL_X + cx * 16 + 8,
                                      OD_UED_LEVEL_Y + cy * 16 + 8, &gx, &gy);
            if (!hit || gx != cx || gy != cy) {
                if (wrong < 4)
                    printf("  cell %d,%d -> hit=%d %d,%d\n", cx, cy, hit, gx, gy);
                wrong++;
            }
        }
    EXPECT(wrong == 0, "editor: %d of 240 cells are not editable at their place "
                       "(bottom row 14 / columns under the palette)", wrong);

    /* Every state message the editor can show, plus a long user name. */
    static const od_string messages[] = {
        OD_STR_EDITOR_NEW_HINT, OD_STR_NO_FIRES, OD_STR_HERO_SOLID,
        OD_STR_OBJECT_POSITION, OD_STR_INVALID_WORLD, OD_STR_TEST_FINISHED,
        OD_STR_LEVEL_OPEN_ERROR, OD_STR_MISSING_HERO,
    };
    for (size_t i = 0; i < sizeof messages / sizeof messages[0]; i++) {
        snprintf(e.status, sizeof e.status, "! %s", od_str(messages[i]));
        e.status_error = 1;
        check_editor(od_str(messages[i]), &e);
    }
    snprintf(e.status, sizeof e.status, od_str(OD_STR_LEVEL_SAVED),
             "a-very-long-level-identifier-made-of-forty");
    check_editor("saved long id", &e);
    memset(e.status, 'W', sizeof e.status - 1);
    e.status[sizeof e.status - 1] = '\0';
    check_editor("159-character status", &e);
    e.flame_brush = 1;
    e.brush = od_ued_palette_count() - 1;
    e.doc.theme_world = 15;
    check_editor("flame brush, world 15", &e);
    od_ued_free(&e);
}

/* ---- presentation at window scales -------------------------------------- */

static void test_scales(void)
{
#ifdef OLD_LAYOUT
    EXPECT(0, "no editor viewport: the editor is the 256x240 game overlay");
    EXPECT(0, "no test-play view: the 24-row status bar covers level rows "
              "216..239 (row 14 and half of row 13)");
#else
    static const int win[][2] = {
        { 512, 480 }, { 768, 720 }, { 1024, 960 }, { 1920, 1080 }, { 3840, 2160 },
    };
    for (size_t i = 0; i < sizeof win / sizeof win[0]; i++) {
        int ww = win[i][0], wh = win[i][1];
        od_ued_rect r;
        od_ued_fit(ww, wh, OD_UED_W, OD_UED_H, 1, 0, &r);
        int k = r.w / OD_UED_W;
        EXPECT(k >= 1 && r.w == OD_UED_W * k && r.h == OD_UED_H * k,
               "editor at %dx%d: %dx%d is not a whole multiple of %dx%d", ww, wh,
               r.w, r.h, OD_UED_W, OD_UED_H);
        EXPECT(inside(r.x, r.y, r.w, r.h, 0, 0, ww, wh),
               "editor at %dx%d: canvas %d,%d %dx%d leaves the window", ww, wh,
               r.x, r.y, r.w, r.h);
        od_ued_fit(ww, wh, OD_UED_TEST_W, OD_UED_TEST_H, 1, 0, &r);
        k = r.w / OD_UED_TEST_W;
        EXPECT(k >= 1 && r.h == OD_UED_TEST_H * k &&
               inside(r.x, r.y, r.w, r.h, 0, 0, ww, wh),
               "test-play at %dx%d: %d,%d %dx%d does not show the whole 256x264 "
               "view", ww, wh, r.x, r.y, r.w, r.h);
        /* 8:7 pixels keep the full height. */
        od_ued_fit(ww, wh, OD_UED_TEST_W, OD_UED_TEST_H, 1, 1, &r);
        EXPECT(r.h % OD_UED_TEST_H == 0 && inside(r.x, r.y, r.w, r.h, 0, 0, ww, wh),
               "test-play 8:7 at %dx%d: %dx%d", ww, wh, r.w, r.h);
        /* Integer scaling off: the largest fit, still whole. */
        od_ued_fit(ww, wh, OD_UED_TEST_W, OD_UED_TEST_H, 0, 0, &r);
        EXPECT(inside(r.x, r.y, r.w, r.h, 0, 0, ww, wh) &&
               (r.w == ww || r.h == wh),
               "test-play fit at %dx%d: %d,%d %dx%d", ww, wh, r.x, r.y, r.w, r.h);
    }
    /* A window smaller than the canvas still shows all of it. */
    od_ued_rect r;
    od_ued_fit(300, 200, OD_UED_W, OD_UED_H, 1, 0, &r);
    EXPECT(inside(r.x, r.y, r.w, r.h, 0, 0, 300, 200) && r.w > 0 && r.h > 0,
           "editor in a 300x200 window: %d,%d %dx%d", r.x, r.y, r.w, r.h);

    /* Test-play view: the full level on top, the game's bar below it. */
    static uint32_t frame[256 * 240], level[256 * 240];
    static uint32_t out[OD_UED_TEST_W * OD_UED_TEST_H];
    for (int i = 0; i < 256 * 240; i++) {
        level[i] = 0xFF000000u | (uint32_t)i;
        frame[i] = level[i];
    }
    for (int y = OD_UED_BAR_TOP; y < 240; y++)
        for (int x = 0; x < 256; x++)
            frame[y * 256 + x] = 0xFF800000u | (uint32_t)(y * 256 + x);
    od_ued_test_view(frame, level, out);
    int bad_level = 0, bad_bar = 0;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 256; x++)
            bad_level += out[y * 256 + x] != level[y * 256 + x];
    for (int y = 0; y < OD_UED_BAR_H; y++)
        for (int x = 0; x < 256; x++)
            bad_bar += out[(240 + y) * 256 + x] !=
                       frame[(OD_UED_BAR_TOP + y) * 256 + x];
    EXPECT(bad_level == 0, "test-play: %d level pixels missing", bad_level);
    EXPECT(bad_bar == 0, "test-play: %d status bar pixels missing", bad_bar);
    /* No bar drawn (another scene, or the pause band the original draws
     * there): the frame as presented, nothing invented below it. */
    od_ued_test_view(level, level, out);
    bad_level = bad_bar = 0;
    for (int i = 0; i < 256 * 240; i++)
        bad_level += out[i] != level[i];
    for (int i = 256 * 240; i < OD_UED_TEST_W * OD_UED_TEST_H; i++)
        bad_bar += out[i] != 0xFF000000u;
    EXPECT(bad_level == 0 && bad_bar == 0,
           "test-play without bar: %d frame / %d bar pixels differ", bad_level,
           bad_bar);
#endif
}

/* ---- catalogue ---------------------------------------------------------- */

static uint32_t ccanvas[OD_UCAT_W * OD_UCAT_H];

static void test_catalog(void)
{
    char dir[600];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/editor-ui-layout-catalog", tmp && tmp[0] ? tmp : "/tmp");
    static const char *const names[] = {
        "A level name that is much longer than the row of the catalogue list",
        "Short",
    };
    char ids[2][OD_LED_ID_MAX];
    for (int i = 0; i < 2; i++) {
        od_led_doc d;
        memset(&d, 0, sizeof d);
        od_led_new(&d, 15);
        od_led_place_hero(&d, 1, 1);
        od_led_paint(&d, 5, 5, 0x0E);
        snprintf(d.name, sizeof d.name, "%s", names[i]);
        snprintf(d.author, sizeof d.author, "%s",
                 "An author with a rather long name as well, sixty characters");
        EXPECT(od_catalog_save(dir, &d, ids[i]) == OD_OK, "catalogue: save %d", i);
        od_led_free(&d);
    }
    od_ui_catalog c;
    od_ucat_init(&c, dir);
    od_ucat_refresh(&c);
    EXPECT(c.n == 2, "catalogue: %d entries", c.n);
    snprintf(c.io_path, sizeof c.io_path,
             "/home/someone/.local/share/opendana/levels/and/a/deep/folder/export.json");

    static const struct { int btn; const char *label; } buttons[] = {
        { OD_UCAT_BTN_PLAY, "Play" }, { OD_UCAT_BTN_EDIT, "Edit" },
        { OD_UCAT_BTN_DUP, "Dup" }, { OD_UCAT_BTN_DELETE, "Delete" },
        { OD_UCAT_BTN_IMPORT, "Import" }, { OD_UCAT_BTN_EXPORT, "Export" },
        { OD_UCAT_BTN_SORT, "Sort" }, { OD_UCAT_BTN_BACK, "Back" },
    };
    static const char *const statuses[] = {
        NULL, "! Importacion fallida",
        "Exported to /home/someone/.local/share/opendana/levels/export.json",
    };
    for (size_t s = 0; s < sizeof statuses / sizeof statuses[0]; s++) {
        for (int editing = 0; editing <= 1; editing++) {
            if (statuses[s])
                snprintf(c.status, sizeof c.status, "%s", statuses[s]);
            c.editing_path = editing;
            memset(ccanvas, 0, sizeof ccanvas);
            nruns = 0;
            od_ucat_draw(&c, ccanvas, OD_UCAT_W, OD_UCAT_H);
            char state[64];
            snprintf(state, sizeof state, "catalogue status %zu path %d", s, editing);
            check_runs(state, OD_UCAT_W, OD_UCAT_H, 0);
            check_visible(state, ccanvas, OD_UCAT_W, OD_UCAT_H);
            for (size_t b = 0; b < sizeof buttons / sizeof buttons[0]; b++) {
                int x, y, w, h;
                od_ucat_button_rect(buttons[b].btn, &x, &y, &w, &h);
                EXPECT(h >= OD_UI_FONT_H + 2,
                       "%s: button %s is %d px high", state, buttons[b].label, h);
                check_box(state, buttons[b].label, x, y, w, h, buttons[b].label);
            }
            for (int row = 0; row < c.n; row++) {
                int x, y, w, h;
                od_ucat_row_rect(row, &x, &y, &w, &h);
                check_box(state, "list row", x, y, w, h, NULL);
            }
        }
    }
    od_ucat_free(&c);
    for (int i = 0; i < 2; i++)
        od_catalog_delete(dir, ids[i]);
}

int main(void)
{
    test_editor();
    test_scales();
    test_catalog();
    printf("editor-ui-layout: %d checks, %d failures%s\n", checks, failures,
#ifdef OLD_LAYOUT
           " (earlier overlay layout)"
#else
           ""
#endif
    );
    return failures ? 1 : 0;
}
