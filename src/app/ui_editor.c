/* ui_editor.c - modern mouse level editor screen (C5.5).
 *
 * Pure C11, no SDL: input in (canvas pixels, button mask, wheel), pixels out
 * into the overlay canvas. The document and the catalogue are the C5.4 model
 * (`src/ext/leveled.c`, `src/ext/catalog.c`); the level picture is composed by
 * the host with the core compositor and arrives through `preview`. */
#if !defined(_WIN32)
#define _XOPEN_SOURCE 700      /* realpath (C6.32: catalogue file identity) */
#endif
#include "opendana/ui_editor.h"
#include "opendana/ext/catalog.h"
#include "opendana/ui_font.h"
#include "opendana/ui_strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- palette ------------------------------------------------------------ */

static od_ued_palette_entry k_palette[] = {
    { 0x00, NULL }, { 0x01, NULL }, { 0x02, NULL }, { 0x03, NULL },
    { 0x04, NULL }, { 0x05, NULL }, { 0x06, NULL }, { 0x07, NULL },
    { 0x08, NULL }, { 0x09, NULL }, { 0x0A, NULL }, { 0x0B, NULL },
    { 0x0C, NULL }, { 0x0D, NULL }, { 0x0E, NULL }, { 0x0F, NULL },
    { OD_UED_TOOL_HERO, NULL },
};
static const od_string k_palette_labels[] = {
    OD_STR_EMPTY_CELL, OD_STR_BLOCK, OD_STR_ICE, OD_STR_ICE_LEFT,
    OD_STR_ICE_RIGHT, OD_STR_LOOSE_ICE, OD_STR_TUBE_1, OD_STR_TUBE_2,
    OD_STR_TUBE_3, OD_STR_TUBE_4, OD_STR_TUBE_5, OD_STR_TUBE_6,
    OD_STR_STONE, OD_STR_JAR, OD_STR_FIRE, OD_STR_BURNING, OD_STR_HERO
};
#define PALETTE_N ((int)(sizeof k_palette / sizeof k_palette[0]))

int od_ued_palette_count(void) { return PALETTE_N; }
const od_ued_palette_entry *od_ued_palette(void)
{
    for (int i = 0; i < PALETTE_N; i++)
        k_palette[i].name = od_str(k_palette_labels[i]);
    return k_palette;
}

const char *od_ued_cell_name(uint8_t cell)
{
    for (int i = 0; i < PALETTE_N; i++)
        if (k_palette[i].cell == cell)
            return od_str(k_palette_labels[i]);
    return od_str(OD_STR_UNKNOWN_CELL);
}

int od_ued_brush_cell(const od_ui_editor *e, int *flame)
{
    if (flame) *flame = e->flame_brush;
    if (e->flame_brush)
        return 0x0E;
    if (e->brush < 0 || e->brush >= PALETTE_N)
        return 0x00;
    return k_palette[e->brush].cell;
}

/* ---- layout ------------------------------------------------------------- */

/* C6.24: the layout in the editor's own 384x360 canvas. Every
 * box is sized from the 8x8 font: a label is 8*n pixels plus 4 on each side,
 * a box is 14 pixels high (border, 2 of padding, 8 of glyph, 2, border). */
#define LX OD_UED_LEVEL_X
#define LY OD_UED_LEVEL_Y
#define BOX_H 14
#define TB_Y 2                          /* toolbar above the level         */
/* Palette panel to the right of the level, the full canvas height. */
#define PAL_X 264
#define PAL_Y LY
#define PAL_W 116
#define PAL_H 338
#define PAL_LIST_Y (PAL_Y + 14)         /* under the "PALETTE" title       */
#define PAL_ROW_H 18                    /* 16x16 swatch at 1:1 + 2         */
/* Under the level: the world selector and cell readout, the status (two
 * lines) and the help lines. */
#define WORLD_Y (LY + 240 + 4)
#define STATUS_X LX
#define STATUS_Y (WORLD_Y + BOX_H + 2)
#define STATUS_W 256
#define STATUS_H 28
#define HELP_Y (STATUS_Y + STATUS_H + 4)
#define HELP_LINE_H 10

#define C_BG        0xFF101018u
#define C_STATUS_BG 0xFF080810u
#define C_PANEL_BG  0xFF202030u
#define C_BORDER    0xFFB0B0D0u
#define C_TEXT      0xFFE8E8F0u
#define C_SEL_BG    0xFF2E4E9Eu
#define C_SEL_TX    0xFFFFFFFFu
#define C_ERR       0xFFFF8080u
#define C_OK        0xFF90E090u
#define C_MUTED     0xFF9090A0u
#define C_TITLE     0xFFFFE080u
#define C_BTN       0xFF303048u
#define C_BTN_TX    0xFFE8E8F0u

/* Width of a button whose label has `n` characters. */
#define BTN_W(n) ((n) * 8 + 8)

typedef struct { int btn, x, y, w; od_string label; } btn_rect;
static const btn_rect k_buttons[OD_UED_BTN_COUNT] = {
    { OD_UED_BTN_NEW,          4, TB_Y, BTN_W(3), OD_STR_NEW },
    { OD_UED_BTN_OPEN,        40, TB_Y, BTN_W(4), OD_STR_OPEN },
    { OD_UED_BTN_SAVE,        84, TB_Y, BTN_W(4), OD_STR_SAVE },
    { OD_UED_BTN_SAVEAS,     128, TB_Y, BTN_W(7), OD_STR_SAVE_AS },
    { OD_UED_BTN_TEST,       200, TB_Y, BTN_W(4), OD_STR_TEST },
    { OD_UED_BTN_UNDO,       248, TB_Y, BTN_W(4), OD_STR_UNDO },
    { OD_UED_BTN_REDO,       292, TB_Y, BTN_W(4), OD_STR_REDO },
    { OD_UED_BTN_WORLD_DOWN, LX, WORLD_Y, BTN_W(2), OD_STR_WORLD_DOWN },
    { OD_UED_BTN_WORLD_UP,  100, WORLD_Y, BTN_W(2), OD_STR_WORLD_UP },
    { OD_UED_BTN_FLAME,  PAL_X + 2, PAL_Y + PAL_H - BOX_H - 2, PAL_W - 4,
      OD_STR_FLAME },
    { OD_UED_BTN_BACK,       340, TB_Y, BTN_W(4), OD_STR_BACK },
};
/* "World 15" between W- and W+, the hovered cell to its right. */
#define WORLD_LABEL_X 30
#define WORLD_LABEL_W 68
#define CELL_INFO_X 130
#define CELL_INFO_W (LX + 256 - CELL_INFO_X)

void od_ued_button_rect(int button, int *x, int *y, int *w, int *h)
{
    for (int i = 0; i < OD_UED_BTN_COUNT; i++) {
        if (k_buttons[i].btn == button) {
            if (x) *x = k_buttons[i].x;
            if (y) *y = k_buttons[i].y;
            if (w) *w = k_buttons[i].w;
            if (h) *h = BOX_H;
            return;
        }
    }
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    if (h) *h = 0;
}

void od_ued_palette_rect(int index, int *x, int *y, int *w, int *h)
{
    if (x) *x = PAL_X + 2;
    if (y) *y = PAL_LIST_Y + index * PAL_ROW_H;
    if (w) *w = PAL_W - 4;
    if (h) *h = PAL_ROW_H;
}

void od_ued_cell_rect(int cx, int cy, int *x, int *y, int *w, int *h)
{
    if (x) *x = LX + cx * 16;
    if (y) *y = LY + cy * 16;
    if (w) *w = 16;
    if (h) *h = 16;
}

static int inside(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

int od_ued_cell_at(const od_ui_editor *e, int x, int y, int *cx, int *cy)
{
    (void)e;                            /* nothing is drawn over the level */
    if (!inside(x, y, LX, LY, OD_GRID_W * 16, OD_GRID_H * 16))
        return 0;
    if (cx) *cx = (x - LX) / 16;
    if (cy) *cy = (y - LY) / 16;
    return 1;
}

/* ---- presentation geometry (host, tests) -------------------------------- */

static int widen87(int v, int aspect_87)
{
    return aspect_87 ? (v * 8 + 3) / 7 : v;
}

void od_ued_fit(int win_w, int win_h, int cw, int ch, int integer,
                 int aspect_87, od_ued_rect *out)
{
    if (out == NULL)
        return;
    if (win_w < 1) win_w = 1;
    if (win_h < 1) win_h = 1;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    int w = 0, h = 0;
    if (integer) {
        for (int k = win_h / ch + 1; k >= 1; k--) {
            int dw = widen87(cw * k, aspect_87), dh = ch * k;
            if (dw <= win_w && dh <= win_h) {
                w = dw;
                h = dh;
                break;
            }
        }
    }
    if (w == 0) {
        /* Largest fit keeping the aspect: limited by the height, else by the
         * width. */
        h = win_h;
        int sw = (h * cw + ch / 2) / ch;
        w = widen87(sw, aspect_87);
        if (w > win_w) {
            w = win_w;
            sw = aspect_87 ? w * 7 / 8 : w;
            h = (sw * ch + cw / 2) / cw;
            if (h > win_h) h = win_h;
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }
    out->w = w;
    out->h = h;
    out->x = (win_w - w) / 2;
    out->y = (win_h - h) / 2;
}

void od_ued_test_view(const uint32_t *frame, const uint32_t *level,
                       uint32_t *out)
{
    if (frame == NULL || level == NULL || out == NULL)
        return;
    const size_t row = OD_UED_TEST_W;
    const size_t bar = (size_t)OD_UED_BAR_TOP * row;
    int drawn = memcmp(frame + bar, level + bar,
                       (size_t)OD_UED_BAR_H * row * sizeof *frame) != 0;
    memcpy(out, drawn ? level : frame, 240 * row * sizeof *out);
    if (drawn) {
        memcpy(out + 240 * row, frame + bar,
               (size_t)OD_UED_BAR_H * row * sizeof *out);
    } else {
        for (size_t i = 240 * row; i < (size_t)OD_UED_TEST_H * row; i++)
            out[i] = 0xFF000000u;
    }
}

/* ---- document ----------------------------------------------------------- */

void od_ued_init(od_ui_editor *e, const char *catalog_dir)
{
    if (e == NULL)
        return;
    memset(e, 0, sizeof *e);
    if (catalog_dir != NULL)
        snprintf(e->catalog_dir, sizeof e->catalog_dir, "%s", catalog_dir);
    e->hover_cell_x = e->hover_cell_y = -1;
    od_led_new(&e->doc, 1);
    e->doc.id[0] = '\0';
    snprintf(e->doc.name, sizeof e->doc.name, "%s", od_str(OD_STR_UNTITLED));
    e->brush = 1;   /* the solid block, not the empty field */
    e->open = 1;
    snprintf(e->status, sizeof e->status, "%s", od_str(OD_STR_EDITOR_NEW_HINT));
}

void od_ued_free(od_ui_editor *e)
{
    if (e != NULL) {
        od_led_free(&e->doc);
        memset(&e->thumbnail_doc,0,sizeof e->thumbnail_doc);
        e->thumbnail_pending=0;
    }
}

static void update_status(od_ui_editor *e)
{
    char msg[120];
    int errors = od_ued_validate(e, msg, sizeof msg);
    if (errors > 0) {
        snprintf(e->status, sizeof e->status, "! %s", msg);
        e->status_error = 1;
    } else if (msg[0] != '\0') {
        snprintf(e->status, sizeof e->status, "%s", msg);
        e->status_error = 0;
    } else {
        int solid = 0;
        for (int y = 0; y < OD_GRID_H; y++)
            for (int x = 0; x < OD_GRID_W; x++)
                if ((e->doc.grid[y][x] & 0x0F) != 0x00)
                    solid++;
        snprintf(e->status, sizeof e->status,
                 od_str(OD_STR_EDITOR_COUNTS), solid, e->doc.n_objects);
        e->status_error = 0;
    }
}

void od_ued_new(od_ui_editor *e, int theme_world)
{
    od_led_new(&e->doc, theme_world);
    e->doc.id[0] = '\0';
    e->file_path[0] = '\0';
    e->doc.number = (theme_world - 1) * 10 + 1;  /* world graphics match theme */
    e->doc.world = theme_world;
    e->brush = k_palette[0].cell == 0x00 ? 1 : 0; /* a block, not empty */
    e->flame_brush = 0;
    e->painting = e->erasing = e->filling = 0;
    e->doc.dirty = 0;
    e->confirm = OD_UED_CONFIRM_NONE;
    update_status(e);
}

/* 1 when `path` names a file directly inside directory `dir` (both resolved,
 * so an aliased data folder still matches). */
static int file_in_dir(const char *path, const char *dir)
{
    if (path == NULL || dir == NULL || dir[0] == '\0')
        return 0;
    char parent[1200];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
#if defined(_WIN32)
    char *bslash = strrchr(parent, '\\');
    if (bslash != NULL && (slash == NULL || bslash > slash))
        slash = bslash;
#endif
    if (slash == NULL)
        snprintf(parent, sizeof parent, ".");
    else if (slash == parent)
        parent[1] = '\0';
    else
        *slash = '\0';
#if defined(_WIN32)
    char a[1200], b[1200];
    if (!_fullpath(a, parent, sizeof a) || !_fullpath(b, dir, sizeof b))
        return 0;
    return _stricmp(a, b) == 0;
#else
    char *a = realpath(parent, NULL), *b = realpath(dir, NULL);
    int same = a != NULL && b != NULL && strcmp(a, b) == 0;
    free(a);
    free(b);
    return same;
#endif
}

/* The catalogue id of a file of the catalogue: its stem, when it is one. */
static int stem_id(const char *path, char *out, size_t cap)
{
    const char *base = strrchr(path, '/');
#if defined(_WIN32)
    const char *bbase = strrchr(path, '\\');
    if (bbase != NULL && (base == NULL || bbase > base))
        base = bbase;
#endif
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if (n <= 5 || strcmp(base + n - 5, ".json") != 0 || n - 5 >= cap ||
        n - 5 >= OD_LED_ID_MAX)
        return 0;
    for (size_t i = 0; i < n - 5; i++) {
        char c = base[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '_'))
            return 0;
    }
    memcpy(out, base, n - 5);
    out[n - 5] = '\0';
    return 1;
}

int od_ued_load(od_ui_editor *e, const char *path)
{
    if (e == NULL || path == NULL)
        return OD_ERR_FORMAT;
    od_status st = od_led_load_json(&e->doc, path);
    if (st != OD_OK) {
        snprintf(e->status, sizeof e->status, "%s", od_str(OD_STR_LEVEL_OPEN_ERROR));
        e->status_error = 1;
        return (int)st;
    }
    snprintf(e->file_path, sizeof e->file_path, "%s", path);
    /* C6.32 (UI-3): the id that Save writes to is the file's own stem when
     * the file is in the catalogue, never the "id" written inside the JSON;
     * a file from anywhere else becomes a new catalogue entry. */
    if (!file_in_dir(path, e->catalog_dir) ||
        !stem_id(path, e->doc.id, sizeof e->doc.id))
        e->doc.id[0] = '\0';
    e->doc.dirty = 0;
    e->confirm = OD_UED_CONFIRM_NONE;
    e->brush = 1;   /* C6.24: the block, as for a new document (was "Empty",
                     * so a left click on a reopened level erased) */
    e->flame_brush = 0;
    update_status(e);
    return OD_OK;
}

static void catalog_level_path(const char *dir, const char *id, char *out,
                               size_t cap)
{
    if (dir != NULL && dir[0] != '\0')
        snprintf(out, cap, "%s/%s.json", dir, id);
    else
        snprintf(out, cap, "%s.json", id);
}

int od_ued_save(od_ui_editor *e, int as_new)
{
    if (e == NULL)
        return OD_ERR_RANGE;
    if (e->doc.name[0] == '\0' ||
        strcmp(e->doc.name, od_str(OD_STR_UNTITLED)) == 0) {
        /* Owner decision (2026-10-06): no name entry; a level is named after
         * the local date and time it was first saved, and keeps that name. */
        time_t now = time(NULL);
        struct tm *tmv = localtime(&now);
        if (tmv == NULL ||
            strftime(e->doc.name, sizeof e->doc.name, "%Y-%m-%d %H:%M", tmv) == 0)
            snprintf(e->doc.name, sizeof e->doc.name, "%s", od_str(OD_STR_UNTITLED));
    }
    if (as_new || e->doc.id[0] == '\0')
        e->doc.id[0] = '\0';
    char id[OD_LED_ID_MAX] = "";
    od_status st = od_catalog_save(e->catalog_dir, &e->doc, id);
    if (st != OD_OK) {
        snprintf(e->status, sizeof e->status, "%s", od_str(OD_STR_LEVEL_SAVE_ERROR));
        e->status_error = 1;
        return (int)st;
    }
    snprintf(e->doc.id, sizeof e->doc.id, "%s", id);
    e->doc.dirty = 0;
    snprintf(e->last_saved_id, sizeof e->last_saved_id, "%s", id);
    catalog_level_path(e->catalog_dir, id, e->last_saved_path,
                       sizeof e->last_saved_path);
    snprintf(e->status, sizeof e->status, od_str(OD_STR_LEVEL_SAVED), id);
    e->status_error = 0;
    e->thumbnail_doc=e->doc;
    e->thumbnail_doc.undo=e->thumbnail_doc.redo=NULL;
    e->thumbnail_doc.n_undo=e->thumbnail_doc.n_redo=0;
    e->thumbnail_pending=1;
    return OD_OK;
}

/* The model reports a code; the interface words it (C6.32, UI-18: no
 * matching on the model's own diagnostic text). */
static void issue_text(const od_led_issue *issue, char *out, size_t cap)
{
    static const struct { int code; od_string label; } labels[] = {
        { OD_LED_ISSUE_MISSING_HERO, OD_STR_MISSING_HERO },
        { OD_LED_ISSUE_HERO_SOLID, OD_STR_HERO_SOLID },
        { OD_LED_ISSUE_INVALID_WORLD, OD_STR_INVALID_WORLD },
        { OD_LED_ISSUE_INVALID_SPECIAL, OD_STR_INVALID_SPECIAL },
        { OD_LED_ISSUE_INVALID_RUN, OD_STR_INVALID_RUN },
        { OD_LED_ISSUE_OBJECT_POSITION, OD_STR_OBJECT_POSITION },
        { OD_LED_ISSUE_OBJECT_GRID, OD_STR_OBJECT_GRID },
        { OD_LED_ISSUE_NO_FIRES, OD_STR_NO_FIRES }
    };
    if (issue->code == OD_LED_ISSUE_INVALID_CELL) {
        snprintf(out, cap, od_str(OD_STR_INVALID_CELL), (unsigned)issue->value);
        return;
    }
    for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++)
        if (issue->code == labels[i].code) {
            snprintf(out, cap, "%s", od_str(labels[i].label));
            return;
        }
    snprintf(out, cap, "%s", od_str(OD_STR_INVALID_LEVEL));
}

int od_ued_validate(const od_ui_editor *e, char *out, size_t cap)
{
    od_led_issue issues[16];
    memset(issues, 0, sizeof issues);
    for (int i = 0; i < 16; i++) issues[i].severity = -1;
    int n = od_led_validate(&e->doc, issues, 16);
    if (out != NULL && cap > 0) {
        out[0] = '\0';
        if (n > 0) {
            for (int i = 0; i < 16; i++)
                if (issues[i].severity == OD_LED_ERROR) {
                    issue_text(&issues[i], out, cap);
                    break;
                }
        } else {
            for (int i = 0; i < 16; i++)
                if (issues[i].severity == OD_LED_WARNING) {
                    issue_text(&issues[i], out, cap);
                    break;
                }
        }
    }
    return n;
}

/* ---- tool actions ------------------------------------------------------- */

static void set_world(od_ui_editor *e, int delta)
{
    int w = e->doc.theme_world + delta;
    if (w < 1) w = 1;
    if (w > 15) w = 15;
    if (w == e->doc.theme_world)
        return;
    e->doc.theme_world = w;
    e->doc.world = w;
    e->doc.number = (w - 1) * 10 + (e->doc.round > 0 ? e->doc.round : 1);
    e->doc.dirty = 1;
    char msg[64];
    snprintf(msg, sizeof msg, od_str(OD_STR_THEME_WORLD), w);
    snprintf(e->status, sizeof e->status, "%s", msg);
    e->status_error = 0;
}

static void add_flame(od_ui_editor *e, int x, int y)
{
    /* C6.32 (UI-15): one run per cell; dragging over a cell that already has
     * its moving flame adds nothing. */
    if (od_led_object_at(&e->doc, x, y, 0x0E) >= 0)
        return;
    od_led_paint_clear(&e->doc, x, y, 0x0E);
    od_led_object o;
    memset(&o, 0, sizeof o);
    o.kind = OD_LED_RUN;
    o.type = 0x0E;
    o.count = 1;
    o.cells[0] = (uint8_t)(y * OD_GRID_W + x);
    od_led_add_object(&e->doc, &o);
}

static void do_button(od_ui_editor *e, int button)
{
    switch (button) {
    case OD_UED_BTN_NEW:
        if (e->doc.dirty)
            e->confirm = OD_UED_CONFIRM_NEW;
        else
            od_ued_new(e, e->doc.theme_world);
        break;
    case OD_UED_BTN_OPEN:
        if (e->doc.dirty)
            e->confirm = OD_UED_CONFIRM_OPEN;
        else
            e->request_open = 1;
        break;
    case OD_UED_BTN_SAVE:
        od_ued_save(e, 0);
        break;
    case OD_UED_BTN_SAVEAS:
        od_ued_save(e, 1);
        break;
    case OD_UED_BTN_TEST:
        e->request_test = 1;
        break;
    case OD_UED_BTN_UNDO:
        od_led_undo(&e->doc);
        update_status(e);
        break;
    case OD_UED_BTN_REDO:
        od_led_redo(&e->doc);
        update_status(e);
        break;
    case OD_UED_BTN_WORLD_DOWN: set_world(e, -1); break;
    case OD_UED_BTN_WORLD_UP:   set_world(e, +1); break;
    case OD_UED_BTN_FLAME:      e->flame_brush = !e->flame_brush; break;
    case OD_UED_BTN_BACK:
        if (e->doc.dirty)
            e->confirm = OD_UED_CONFIRM_BACK;
        else
            e->request_back = 1;
        break;
    default: break;
    }
}

/* Answer the pending confirmation. */
static void answer_confirm(od_ui_editor *e, int yes)
{
    int what = e->confirm;
    e->confirm = OD_UED_CONFIRM_NONE;
    if (!yes)
        return;
    if (what == OD_UED_CONFIRM_NEW)
        od_ued_new(e, e->doc.theme_world);
    else if (what == OD_UED_CONFIRM_OPEN)
        e->request_open = 1;
    else if (what == OD_UED_CONFIRM_BACK)
        e->request_back = 1;
}

#define CONFIRM_X (LX + 24)
#define CONFIRM_Y (LY + 84)
#define CONFIRM_W 208
#define CONFIRM_H 64

void od_ued_confirm_rect(int *x, int *y, int *w, int *h)
{
    if (x) *x = CONFIRM_X;
    if (y) *y = CONFIRM_Y;
    if (w) *w = CONFIRM_W;
    if (h) *h = CONFIRM_H;
}

void od_ued_confirm_button_rect(int yes, int *x, int *y, int *w, int *h)
{
    int bw = BTN_W(7);
    if (x) *x = yes ? CONFIRM_X + 12 : CONFIRM_X + CONFIRM_W - 12 - bw;
    if (y) *y = CONFIRM_Y + CONFIRM_H - BOX_H - 8;
    if (w) *w = bw;
    if (h) *h = BOX_H;
}

static void pick_palette(od_ui_editor *e, int index)
{
    int n = PALETTE_N;
    if (index < 0 || index >= n)
        return;
    e->brush = index;
    e->flame_brush = 0;
    char msg[64];
    snprintf(msg, sizeof msg, od_str(OD_STR_MATERIAL), od_str(k_palette_labels[index]));
    snprintf(e->status, sizeof e->status, "%s", msg);
    e->status_error = 0;
}

static void apply_brush(od_ui_editor *e, int x, int y)
{
    int flame = 0;
    int cell = od_ued_brush_cell(e, &flame);
    if (flame) {
        add_flame(e, x, y);
        update_status(e);
        return;
    }
    if (cell == OD_UED_TOOL_HERO) {
        od_led_place_hero(&e->doc, x, y);
    } else {
        od_led_paint_clear(&e->doc, x, y, (uint8_t)cell);
    }
    update_status(e);
}

/* ---- input -------------------------------------------------------------- */

int od_ued_pointer(od_ui_editor *e, int x, int y, int buttons, int click,
                    int wheel, int shift, int ctrl)
{
    if (e == NULL || !e->open)
        return 0;
    e->ptr_x = x;
    e->ptr_y = y;
    e->shift = shift;
    e->ctrl = ctrl;

    /* A pending confirmation takes every pointer sample. */
    if (e->confirm != OD_UED_CONFIRM_NONE) {
        e->hover_cell_x = e->hover_cell_y = -1;
        e->painting = e->erasing = e->filling = 0;
        for (int yes = 0; yes <= 1; yes++) {
            int bx, by, bw, bh;
            od_ued_confirm_button_rect(yes, &bx, &by, &bw, &bh);
            if (click && (buttons & OD_UED_BTN_LEFT) &&
                inside(x, y, bx, by, bw, bh)) {
                answer_confirm(e, yes);
                break;
            }
        }
        return 1;
    }

    /* Toolbar. */
    for (int i = 0; i < OD_UED_BTN_COUNT; i++) {
        int bx, by, bw, bh;
        od_ued_button_rect(k_buttons[i].btn, &bx, &by, &bw, &bh);
        if (!inside(x, y, bx, by, bw, bh) ||
            (k_buttons[i].btn == OD_UED_BTN_FLAME && e->palette_hidden))
            continue;
        e->hover_cell_x = e->hover_cell_y = -1;
        if (click && (buttons & OD_UED_BTN_LEFT))
            do_button(e, k_buttons[i].btn);
        return 1;
    }

    /* The wheel steps through the palette wherever the pointer is. */
    if (wheel != 0 && PALETTE_N > 0) {
        int b = e->brush < 0 || e->brush >= PALETTE_N ? 0 : e->brush;
        pick_palette(e, (b + (wheel > 0 ? 1 : PALETTE_N - 1)) % PALETTE_N);
        return 1;
    }

    /* Palette panel. */
    if (!e->palette_hidden && inside(x, y, PAL_X, PAL_Y, PAL_W, PAL_H)) {
        for (int i = 0; i < PALETTE_N; i++) {
            int rx, ry, rw, rh;
            od_ued_palette_rect(i, &rx, &ry, &rw, &rh);
            if (inside(x, y, rx, ry, rw, rh)) {
                if (click && (buttons & OD_UED_BTN_LEFT))
                    pick_palette(e, i);
                break;
            }
        }
        e->hover_cell_x = e->hover_cell_y = -1;
        return 1;
    }

    /* Grid. */
    int cx, cy;
    if (!od_ued_cell_at(e, x, y, &cx, &cy)) {
        e->hover_cell_x = e->hover_cell_y = -1;
        return 0;
    }
    e->hover_cell_x = cx;
    e->hover_cell_y = cy;

    int left_down = (buttons & OD_UED_BTN_LEFT) != 0;
    int right_down = (buttons & OD_UED_BTN_RIGHT) != 0;
    int middle_down = (buttons & OD_UED_BTN_MIDDLE) != 0;

    if (click && middle_down) {          /* eyedropper */
        uint8_t cell = e->doc.grid[cy][cx];
        for (int i = 0; i < PALETTE_N; i++)
            if (k_palette[i].cell == cell) { e->brush = i; e->flame_brush = 0; break; }
        char msg[64];
        snprintf(msg, sizeof msg, od_str(OD_STR_EYEDROPPER), od_ued_cell_name(cell));
        snprintf(e->status, sizeof e->status, "%s", msg);
        return 1;
    }
    if (click && left_down && shift) {   /* begin a rectangle fill */
        e->filling = 1;
        e->fill_x0 = e->fill_x1 = cx;
        e->fill_y0 = e->fill_y1 = cy;
        return 1;
    }
    if (e->filling && left_down) {       /* extend the rectangle    */
        e->fill_x1 = cx;
        e->fill_y1 = cy;
        return 1;
    }
    if (e->filling && (!left_down || click)) {
        int cell = od_ued_brush_cell(e, NULL);
        if (cell != OD_UED_TOOL_HERO)
            od_led_fill_clear(&e->doc, e->fill_x0, e->fill_y0, e->fill_x1,
                               e->fill_y1, (uint8_t)cell);
        else
            od_led_place_hero(&e->doc, cx, cy);
        e->filling = 0;
        update_status(e);
        return 1;
    }
    if (click && left_down) {
        e->painting = 1;
        e->erasing = 0;
        apply_brush(e, cx, cy);
        return 1;
    }
    if (e->painting && left_down) {
        apply_brush(e, cx, cy);
        return 1;
    }
    if (click && right_down) {
        e->erasing = 1;
        e->painting = 0;
        od_led_paint_clear(&e->doc, cx, cy, 0x00);
        update_status(e);
        return 1;
    }
    if (e->erasing && right_down) {
        od_led_paint_clear(&e->doc, cx, cy, 0x00);
        update_status(e);
        return 1;
    }
    if (click && !left_down && !right_down && !middle_down)
        e->painting = e->erasing = 0;
    return 1;
}

int od_ued_key(od_ui_editor *e, int key)
{
    if (e == NULL || !e->open)
        return 0;
    if (e->confirm != OD_UED_CONFIRM_NONE) {
        if (key == OD_UED_KEY_ACCEPT || key == OD_UED_KEY_BACK)
            answer_confirm(e, key == OD_UED_KEY_ACCEPT);
        return 1;          /* nothing else while the question is open */
    }
    switch (key) {
    case OD_UED_KEY_UNDO: od_led_undo(&e->doc); update_status(e); return 1;
    case OD_UED_KEY_REDO: od_led_redo(&e->doc); update_status(e); return 1;
    case OD_UED_KEY_SAVE: od_ued_save(e, 0); return 1;
    case OD_UED_KEY_TEST: e->request_test = 1; return 1;
    case OD_UED_KEY_BACK: do_button(e, OD_UED_BTN_BACK); return 1;
    case OD_UED_KEY_ACCEPT: return 1;
    case OD_UED_KEY_TOGGLE_PALETTE: e->palette_hidden = !e->palette_hidden; return 1;
    default: return 0;
    }
}

/* ---- drawing ------------------------------------------------------------ */

static void fill_rect(uint32_t *c, int cw, int ch, int x, int y, int w, int h,
                      uint32_t argb)
{
    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= ch) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= cw) continue;
            c[(size_t)py * cw + px] = argb;
        }
    }
}

static void border(uint32_t *c, int cw, int ch, int x, int y, int w, int h,
                   uint32_t argb)
{
    fill_rect(c, cw, ch, x, y, w, 1, argb);
    fill_rect(c, cw, ch, x, y + h - 1, w, 1, argb);
    fill_rect(c, cw, ch, x, y, 1, h, argb);
    fill_rect(c, cw, ch, x + w - 1, y, 1, h, argb);
}

/* Flat fallback colour for a cell type (used when the host has no swatches). */
static uint32_t swatch_color(uint8_t cell)
{
    switch (cell) {
    case 0x00: return 0xFF101020u;
    case 0x01: return 0xFF808890u;
    case 0x02: return 0xFF80C8F0u;
    case 0x03: return 0xFF6CA8D8u;
    case 0x04: return 0xFFA0E0FFu;
    case 0x05: return 0xFFB0E8FFu;
    case 0x06: return 0xFF40A040u;
    case 0x07: return 0xFF50B050u;
    case 0x08: return 0xFF60C060u;
    case 0x09: return 0xFF70D070u;
    case 0x0A: return 0xFF40A080u;
    case 0x0B: return 0xFF50B090u;
    case 0x0C: return 0xFF404048u;
    case 0x0D: return 0xFFC8A040u;
    case 0x0E: return 0xFFE05020u;
    case 0x0F: return 0xFFFFB040u;
    case OD_UED_TOOL_HERO: return 0xFFFFF0C0u;
    default: return 0xFF808080u;
    }
}


static void draw_swatch(uint32_t *c, int cw, int ch, int x, int y, int w, int h,
                        const uint32_t *tile, uint32_t fallback)
{
    if (tile == NULL) {
        fill_rect(c, cw, ch, x, y, w, h, fallback);
        return;
    }
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++) {
            int dx = x + px, dy = y + py;
            if (dx < 0 || dx >= cw || dy < 0 || dy >= ch)
                continue;
            int sx = px * 16 / (w > 0 ? w : 1);
            int sy = py * 16 / (h > 0 ? h : 1);
            c[(size_t)dy * cw + dx] = tile[(size_t)sy * 16 + sx];
        }
}

/* Text cut to `w` pixels (ending in "..." when it does not fit). */
static void text_in(uint32_t *c, int cw, int ch, int x, int y, int w,
                    const char *s, uint32_t argb)
{
    char buf[200];
    od_ui_font_text(c, cw, ch, x, y, od_ui_font_fit(s, w, 1, buf, sizeof buf),
                     argb, 1);
}

/* A labelled box: border, fill, label centred inside the border. */
static void draw_button(uint32_t *c, int cw, int ch, int x, int y, int w, int h,
                        const char *label, int active)
{
    fill_rect(c, cw, ch, x, y, w, h, active ? C_SEL_BG : C_BTN);
    border(c, cw, ch, x, y, w, h, C_BORDER);
    char buf[64];
    const char *s = od_ui_font_fit(label, w - 4, 1, buf, sizeof buf);
    int lw = od_ui_font_width(s, 1);
    od_ui_font_text(c, cw, ch, x + (w - lw) / 2, y + (h - OD_UI_FONT_H) / 2,
                     s, active ? C_SEL_TX : C_BTN_TX, 1);
}

static void outline(uint32_t *c, int cw, int ch, int x, int y, int w, int h,
                    uint32_t argb)
{
    border(c, cw, ch, x, y, w, h, argb);
}

void od_ued_draw(od_ui_editor *e, uint32_t *canvas, int cw, int ch)
{
    if (e == NULL || canvas == NULL)
        return;
    fill_rect(canvas, cw, ch, 0, 0, cw, ch, C_BG);

    /* Level: the core-composed preview at 1:1, cell (x, y) at
     * (LX + 16x, LY + 16y), framed outside its 256x240 rectangle. */
    outline(canvas, cw, ch, LX - 1, LY - 1, 258, 242, C_BORDER);
    if (e->preview != NULL) {
        for (int y = 0; y < 240 && y < e->preview_h; y++)
            for (int x = 0; x < 256 && x < e->preview_w; x++) {
                int dx = LX + x, dy = LY + y;
                if (dx < cw && dy < ch)
                    canvas[(size_t)dy * cw + dx] =
                        e->preview[(size_t)y * e->preview_w + x];
            }
    } else {
        fill_rect(canvas, cw, ch, LX, LY, 256, 240, 0xFF000000u);
    }

    /* Hover cell, and the rectangle of a Shift+drag fill in progress. */
    if (e->filling) {
        int x0 = e->fill_x0 < e->fill_x1 ? e->fill_x0 : e->fill_x1;
        int x1 = e->fill_x0 < e->fill_x1 ? e->fill_x1 : e->fill_x0;
        int y0 = e->fill_y0 < e->fill_y1 ? e->fill_y0 : e->fill_y1;
        int y1 = e->fill_y0 < e->fill_y1 ? e->fill_y1 : e->fill_y0;
        outline(canvas, cw, ch, LX + x0 * 16, LY + y0 * 16, (x1 - x0 + 1) * 16,
                (y1 - y0 + 1) * 16, C_TITLE);
    } else if (e->hover_cell_x >= 0 && e->hover_cell_y >= 0) {
        outline(canvas, cw, ch, LX + e->hover_cell_x * 16,
                LY + e->hover_cell_y * 16, 16, 16, C_SEL_TX);
    }

    /* Toolbar and world selector (the flame brush goes with the palette). */
    for (int i = 0; i < OD_UED_BTN_COUNT; i++) {
        if (k_buttons[i].btn == OD_UED_BTN_FLAME)
            continue;
        int bx, by, bw, bh;
        od_ued_button_rect(k_buttons[i].btn, &bx, &by, &bw, &bh);
        draw_button(canvas, cw, ch, bx, by, bw, bh, od_str(k_buttons[i].label),
                    0);
    }
    {
        char ws[24], fit[24];
        snprintf(ws, sizeof ws, od_str(OD_STR_WORLD), e->doc.theme_world);
        od_ui_font_fit(ws, WORLD_LABEL_W, 1, fit, sizeof fit);
        int lw = od_ui_font_width(fit, 1);
        od_ui_font_text(canvas, cw, ch,
                         WORLD_LABEL_X + (WORLD_LABEL_W - lw) / 2,
                         WORLD_Y + (BOX_H - OD_UI_FONT_H) / 2, fit, C_TEXT, 1);
    }
    if (e->hover_cell_x >= 0 && e->hover_cell_y >= 0 &&
        e->hover_cell_x < OD_GRID_W && e->hover_cell_y < OD_GRID_H) {
        char info[64];
        uint8_t cell = e->doc.grid[e->hover_cell_y][e->hover_cell_x];
        const char *name = od_ued_cell_name(cell);
        if (od_led_has_hero(&e->doc) &&
            e->doc.player == (uint8_t)((e->hover_cell_y << 4) | e->hover_cell_x))
            name = od_str(OD_STR_HERO);
        snprintf(info, sizeof info, od_str(OD_STR_CELL_INFO), e->hover_cell_x,
                 e->hover_cell_y, name);
        text_in(canvas, cw, ch, CELL_INFO_X, WORLD_Y + (BOX_H - OD_UI_FONT_H) / 2,
                CELL_INFO_W, info, C_MUTED);
    }

    /* Status: up to two lines, cut with "..." beyond that. */
    fill_rect(canvas, cw, ch, STATUS_X, STATUS_Y, STATUS_W, STATUS_H, C_STATUS_BG);
    {
        const char *status = e->status;
        char lines[2][64];
        int n = od_ui_font_wrap(status, STATUS_W - 8, 1, &lines[0][0],
                                 sizeof lines[0], 2);
        for (int i = 0; i < n; i++)
            od_ui_font_text(canvas, cw, ch, STATUS_X + 4, STATUS_Y + 3 + i * 11,
                             lines[i], e->status_error ? C_ERR : C_OK, 1);
    }

    /* Help. */
    static const od_string help[] = {
        OD_STR_EDITOR_HELP_1, OD_STR_EDITOR_HELP_2, OD_STR_EDITOR_HELP_3,
        OD_STR_EDITOR_HELP_4, OD_STR_EDITOR_HELP_5,
    };
    for (int i = 0; i < (int)(sizeof help / sizeof help[0]); i++)
        text_in(canvas, cw, ch, LX, HELP_Y + i * HELP_LINE_H, 256,
                od_str(help[i]), C_MUTED);

    /* Palette panel: title, then every material with its real 16x16 tile
     * and its full name. */
    if (!e->palette_hidden) {
        fill_rect(canvas, cw, ch, PAL_X, PAL_Y, PAL_W, PAL_H, C_PANEL_BG);
        border(canvas, cw, ch, PAL_X, PAL_Y, PAL_W, PAL_H, C_BORDER);
        text_in(canvas, cw, ch, PAL_X + 4, PAL_Y + 3, PAL_W - 8,
                od_str(OD_STR_PALETTE), C_TITLE);
        for (int i = 0; i < PALETTE_N; i++) {
            int rx, ry, rw, rh;
            od_ued_palette_rect(i, &rx, &ry, &rw, &rh);
            int selected = (i == e->brush && !e->flame_brush);
            if (selected)
                fill_rect(canvas, cw, ch, rx, ry, rw, rh, C_SEL_BG);
            const uint32_t *tile = NULL;
            uint8_t cell = k_palette[i].cell;
            if (cell == OD_UED_TOOL_HERO)
                tile = e->hero_swatch;
            else if (e->palette_swatch != NULL)
                tile = e->palette_swatch + (size_t)cell * 16 * 16;
            draw_swatch(canvas, cw, ch, rx + 1, ry + 1, 16, 16, tile,
                        swatch_color(cell));
            text_in(canvas, cw, ch, rx + 20, ry + (rh - OD_UI_FONT_H) / 2,
                    rw - 22, od_str(k_palette_labels[i]),
                    selected ? C_SEL_TX : C_TEXT);
        }
        int bx, by, bw, bh;
        od_ued_button_rect(OD_UED_BTN_FLAME, &bx, &by, &bw, &bh);
        draw_button(canvas, cw, ch, bx, by, bw, bh, od_str(OD_STR_FLAME),
                    e->flame_brush);
    }

    /* The pending confirmation, over everything. */
    if (e->confirm != OD_UED_CONFIRM_NONE) {
        fill_rect(canvas, cw, ch, CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H,
                  C_PANEL_BG);
        border(canvas, cw, ch, CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H,
               C_TITLE);
        text_in(canvas, cw, ch, CONFIRM_X + 8, CONFIRM_Y + 8, CONFIRM_W - 16,
                od_str(OD_STR_UNSAVED_TITLE), C_TITLE);
        text_in(canvas, cw, ch, CONFIRM_X + 8, CONFIRM_Y + 20, CONFIRM_W - 16,
                od_str(OD_STR_UNSAVED_QUESTION), C_TEXT);
        int bx, by, bw, bh;
        od_ued_confirm_button_rect(1, &bx, &by, &bw, &bh);
        draw_button(canvas, cw, ch, bx, by, bw, bh, od_str(OD_STR_DISCARD), 0);
        od_ued_confirm_button_rect(0, &bx, &by, &bw, &bh);
        draw_button(canvas, cw, ch, bx, by, bw, bh, od_str(OD_STR_CANCEL), 0);
    }
}
