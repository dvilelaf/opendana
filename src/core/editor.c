/* editor.c - the level editor (EDIT / PLAY / EXIT) state machine.
 *
 * Pure C11. The movement rules and constants are those of the
 * game's editor. The draw tables come from the content package, and a level
 * can be exported in the package format. */
#include "opendana/editor.h"
#include "opendana/game.h"

#include <stdio.h>
#include <string.h>

/* The draw and palette tables are loaded from engine/editor-data.json
 * (array views: editor.h). */

/* The area starts at grid index 0x11 (row 1, column 1). */
#define AREA_BASE 0x11
#define AREA_STRIDE 16

static void clear_actions(od_editor *ed, od_status status)
{
    memset(ed->actions, 0, sizeof ed->actions);
    ed->action_count = 0;
    ed->action_status = (uint8_t)status;
}

od_status od_editor_action_status(const od_editor *ed)
{
    if (!ed || ed->action_count > OD_EDITOR_ACTION_CAPACITY ||
        (ed->action_status != OD_OK && ed->action_status != OD_ERR_RANGE &&
         ed->action_status != OD_ERR_FORMAT))
        return OD_ERR_FORMAT;
    if (ed->action_status != OD_OK && ed->action_count)
        return OD_ERR_FORMAT;
    for (size_t i = 0; i < OD_EDITOR_ACTION_CAPACITY; ++i) {
        if (i < ed->action_count) {
            if (ed->actions[i] != OD_EDITOR_ACTION_GRID_MOVE &&
                ed->actions[i] != OD_EDITOR_ACTION_PLAY_ENTER)
                return OD_ERR_FORMAT;
        } else if (ed->actions[i]) {
            return OD_ERR_FORMAT;
        }
    }
    return (od_status)ed->action_status;
}

size_t od_editor_action_count(const od_editor *ed)
{
    return od_editor_action_status(ed) == OD_OK ? ed->action_count : 0;
}

static void publish_action(od_editor *ed, od_editor_action action)
{
    od_status status = od_editor_action_status(ed);
    if (status != OD_OK) {
        clear_actions(ed, status);
        return;
    }
    if (ed->action_count == OD_EDITOR_ACTION_CAPACITY) {
        clear_actions(ed, OD_ERR_RANGE); /* never publish a truncated batch */
        return;
    }
    ed->actions[ed->action_count++] = (uint8_t)action;
}

od_status od_editor_take_actions(od_editor *ed, uint8_t *out,
                                  size_t capacity, size_t *count)
{
    if (!count) return OD_ERR_FORMAT;
    *count = 0;
    od_status status = od_editor_action_status(ed);
    if (status != OD_OK) {
        if (ed) clear_actions(ed, status);
        return status;
    }
    if (!out && ed->action_count) return OD_ERR_FORMAT;
    if (capacity < ed->action_count) return OD_ERR_RANGE;
    if (ed->action_count) memcpy(out, ed->actions, ed->action_count);
    *count = ed->action_count;
    clear_actions(ed, OD_OK);
    return OD_OK;
}

static uint8_t *area_cell(od_editor *ed, unsigned row, unsigned col)
{
    return &ed->cells[OD_EDITOR_AREA_Y + row][OD_EDITOR_AREA_X + col];
}

unsigned od_editor_cursor_col(const od_editor *ed)
{
    return ed->cell & 0x0Fu;
}

unsigned od_editor_cursor_row(const od_editor *ed)
{
    return (ed->cell >> 4) & 0x0Fu;
}

uint8_t od_editor_palette_at(const od_editor *ed)
{
    if (!od_editor_data_get()) {
        (void)od_editor_data_view();
        return OD_EDITOR_PALETTE_EMPTY;
    }
    unsigned r = ed->pal_row % OD_EDITOR_PALETTE_ROWS;
    unsigned c = ed->pal_col % OD_EDITOR_PALETTE_COLS;
    return od_editor_palette[r][c];
}

void od_editor_pack(od_editor *ed)
{
    unsigned fires = 0;
    for (unsigned r = 0; r < OD_EDITOR_AREA_H; ++r) {
        for (unsigned k = 0; k < OD_EDITOR_AREA_W / 2; ++k) {
            uint8_t hi = *area_cell(ed, r, 2 * k)     & 0x0Fu;
            uint8_t lo = *area_cell(ed, r, 2 * k + 1) & 0x0Fu;
            ed->packed[r * (OD_EDITOR_AREA_W / 2) + k] = (uint8_t)((hi << 4) | lo);
        }
        for (unsigned c = 0; c < OD_EDITOR_AREA_W; ++c)
            if ((*area_cell(ed, r, c) & 0x0Fu) == 0x0E)
                fires++;
    }
    ed->fires = fires;
    ed->dirty = 0;
}

void od_editor_unpack(od_editor *ed, const uint8_t packed[OD_EDITOR_PACKED])
{
    for (unsigned r = 0; r < OD_EDITOR_AREA_H; ++r) {
        for (unsigned k = 0; k < OD_EDITOR_AREA_W / 2; ++k) {
            uint8_t byte = packed[r * (OD_EDITOR_AREA_W / 2) + k];
            *area_cell(ed, r, 2 * k)     = (uint8_t)((byte >> 4) & 0x0Fu);
            *area_cell(ed, r, 2 * k + 1) = (uint8_t)(byte & 0x0Fu);
        }
    }
    od_editor_pack(ed); /* refresh the fire count */
}

void od_editor_init(od_editor *ed)
{
    /* The packed buffer starts filled with 0x11, so the default board is cell
     * type 1 everywhere (the brick the editor screen draws), not 0. The whole
     * 16x15 grid starts solid, outside the 14x12 area too, and the PLAY level
     * uses it. */
    for (unsigned y = 0; y < OD_GRID_H; ++y)
        for (unsigned x = 0; x < OD_GRID_W; ++x)
            ed->cells[y][x] = 0x01;
    ed->toolbar = OD_EDITOR_EDIT;
    ed->entered = 0;
    ed->mode = OD_EDITOR_TOOLBAR;
    ed->phase = 0;
    ed->pal_row = 0;
    ed->pal_col = 0;
    ed->tool = 0x00;   /* starts on FIELD  */
    ed->erase = 0x01;  /* starts on BLOCK  */
    ed->tool_prev = ed->tool;
    ed->erase_prev = ed->erase;
    /* The initial markers, A at FIELD and B at BLOCK, are the screen's own
     * tiles; no slot has been rewritten yet. */
    memset(ed->legend, 0, sizeof ed->legend);
    ed->stall = 0;
    ed->cell = AREA_BASE;
    ed->start = 0;
    ed->fires = 0;
    ed->previous = 0;
    ed->dirty = 0;
    ed->playing = 0;
    ed->last_event = OD_EDITOR_EVENT_NONE;
    clear_actions(ed, OD_OK);
    od_editor_pack(ed);
}

/* ---------------------------------------------------------- toolbar ------- */

static void to_toolbar(od_editor *ed)
{
    od_editor_pack(ed); /* the board is packed before returning */
    ed->toolbar = OD_EDITOR_EDIT;
    ed->entered = 0;
    ed->mode = OD_EDITOR_TOOLBAR;
    ed->phase = 0;
    ed->cell = 0; /* the cursor goes back to cell 0 */
    /* The toolbar strings are queued for drawing and the queue has to have
     * room: the main loop stays blocked until the 4th following frame. */
    ed->stall = OD_EDITOR_TOOLBAR_STALL;
}

/* The old slot gets the empty glyph (0x80), then the new one the A (0xC1) or
 * B (0xC2) glyph, in that order. */
static void legend_move(od_editor *ed, int old_slot, int new_slot, uint8_t glyph)
{
    if (old_slot >= 0 && old_slot < 17)
        ed->legend[old_slot] = OD_EDITOR_MARKER_BLANK;
    if (new_slot >= 0 && new_slot < 17)
        ed->legend[new_slot] = glyph;
}

/* ---------------------------------------------------------- palette ------- */

/* Returns 1 when the cursor must stop (and publishes `slot`), 0 to keep
 * scanning. `slot` is the palette value (0xFE means hidden/back). */
static int palette_slot(const od_editor *ed, uint8_t *slot)
{
    uint8_t v = od_editor_palette_at(ed);
    if (v & 0x80) {
        if (v == OD_EDITOR_PALETTE_BACK) { *slot = v; return 1; }
        return 0; /* 0x80: empty */
    }
    if ((int)v == ed->tool || (int)v == ed->erase)
        return 0; /* already bound to A or B */
    *slot = v;
    return 1;
}

/* LEFT/RIGHT move the palette row, skipping empty and A/B cells. */
static void palette_move_row(od_editor *ed, int dir)
{
    for (unsigned i = 0; i < OD_EDITOR_PALETTE_ROWS; ++i) {
        ed->pal_row = (ed->pal_row + OD_EDITOR_PALETTE_ROWS + (unsigned)dir)
                      % OD_EDITOR_PALETTE_ROWS;
        uint8_t slot;
        if (palette_slot(ed, &slot))
            return;
    }
}

/* UP/DOWN move the palette column. */
static void palette_move_col(od_editor *ed, int dir)
{
    for (unsigned i = 0; i < OD_EDITOR_PALETTE_COLS; ++i) {
        ed->pal_col = (ed->pal_col + OD_EDITOR_PALETTE_COLS + (unsigned)dir)
                      % OD_EDITOR_PALETTE_COLS;
        uint8_t slot;
        if (palette_slot(ed, &slot))
            return;
    }
}

/* Idle path: advance the column until a real slot appears. */
static void palette_idle_scan(od_editor *ed)
{
    uint8_t slot;
    while (!palette_slot(ed, &slot))
        ed->pal_col = (ed->pal_col + 1u) % OD_EDITOR_PALETTE_COLS;
}

/* ------------------------------------------------------------- grid ------ */

/* One step right in the grid, wrapping to the next row. */
static unsigned move_right(unsigned c)
{
    unsigned col = c & 0x0F;
    unsigned next = (col + 1u < 0x0Fu) ? c + 1u : (c & 0xF0u) + 0x11u;
    return next >= 0xCFu ? 0x11u : next;
}

/* One step left in the grid, wrapping to the previous row. */
static unsigned move_left(unsigned c)
{
    unsigned col = c & 0x0F;
    unsigned next = (col - 1u == 0u) ? (c & 0xF0u) - 0x10u + 0x0Eu : c - 1u;
    return next < 0x11u ? 0xCEu : next;
}

/* One step down in the grid, wrapping to the top. */
static unsigned move_down(unsigned c)
{
    unsigned next = c + 0x10u;
    return next >= 0xD0u ? (c & 0x0Fu) + 0x10u : next;
}

/* One step up in the grid, wrapping to the bottom. */
static unsigned move_up(unsigned c)
{
    unsigned next = c - 0x10u;
    return next < 0x10u ? (c & 0x0Fu) | 0xC0u : next;
}

/* A tool below 0x10 paints the cell; 0x10 records the start. */
static void place(od_editor *ed, int tool)
{
    if (tool < 0)
        return;
    unsigned row = od_editor_cursor_row(ed);
    unsigned col = od_editor_cursor_col(ed);
    if (row >= OD_GRID_H || col >= OD_GRID_W)
        return;
    if (tool == OD_EDITOR_TOOL_START) {
        if (ed->start) {
            unsigned sr = (ed->start >> 4) & 0x0F, sc = ed->start & 0x0F;
            ed->cells[sr][sc] = 0;
        }
        ed->start = ed->cell;
        ed->cells[row][col] = 0;
    } else {
        if (ed->cell == ed->start)
            ed->start = 0;
        ed->cells[row][col] = (uint8_t)(tool & 0x0F);
    }
    ed->dirty = 1;
}

/* ------------------------------------------------------------- tick ------ */

static od_editor_event tick_toolbar(od_editor *ed, unsigned pressed)
{
    if (pressed & OD_BTN_RIGHT) ed->toolbar = (ed->toolbar + 1u) % 3u;
    if (pressed & OD_BTN_LEFT)  ed->toolbar = (ed->toolbar + 2u) % 3u;
    if (pressed & OD_BTN_A) {
        ed->entered = 1;
        if (ed->toolbar == OD_EDITOR_EDIT) {
            ed->mode = OD_EDITOR_EDIT_ENTER;
            ed->phase = 0;
        } else if (ed->toolbar == OD_EDITOR_PLAY) {
            ed->mode = OD_EDITOR_PLAY_ENTER;
            ed->phase = 0;
        } else {
            ed->mode = OD_EDITOR_EXIT_ENTER;
        }
    }
    return OD_EDITOR_EVENT_NONE;
}

/* The frame after the EXIT confirmation. */
static od_editor_event tick_exit_enter(od_editor *ed)
{
    (void)ed;
    return OD_EDITOR_EVENT_EXIT;
}

/* Two frames between the EDIT confirmation and the palette. The idle scan runs
 * on the following palette tick. Entering resets the row and the cell but
 * leaves the column alone, so a second EDIT draws the cursor once in row 0 of
 * the kept column (the BLOCK slot) before the scan moves it. */
static void tick_edit_enter(od_editor *ed)
{
    if (ed->phase == 0) {
        ed->phase = 1;
        return;
    }
    ed->mode = OD_EDITOR_PALETTE;
    ed->pal_row = 0;
    ed->cell = AREA_BASE;
}

/* Two frames between PLAY and the running level. */
static od_editor_event tick_play_enter(od_editor *ed)
{
    if (ed->phase == 0) {
        ed->phase = 1;
        return OD_EDITOR_EVENT_NONE;
    }
    ed->playing = 1;
    /* The sound request comes with the PLAY transition, not the confirmation. */
    publish_action(ed, OD_EDITOR_ACTION_PLAY_ENTER);
    return OD_EDITOR_EVENT_PLAY;
}

static void tick_palette(od_editor *ed, unsigned pressed)
{
    if (pressed & OD_BTN_START) {   /* START -> toolbar */
        to_toolbar(ed);
        return;
    }
    if (pressed & OD_BTN_SELECT) {  /* SELECT -> grid */
        ed->mode = OD_EDITOR_GRID;
        return;
    }
    unsigned dir = 0;
    if (pressed & OD_BTN_RIGHT) dir |= 1u;
    if (pressed & OD_BTN_LEFT)  dir |= 2u;
    if (pressed & OD_BTN_DOWN)  dir |= 4u;
    if (pressed & OD_BTN_UP)    dir |= 8u;
    if (dir & 1u) palette_move_row(ed, +1);
    else if (dir & 2u) palette_move_row(ed, -1);
    else if (dir & 4u) palette_move_col(ed, +1);
    else if (dir & 8u) palette_move_col(ed, -1);
    else palette_idle_scan(ed);

    if (pressed & (OD_BTN_A | OD_BTN_B)) {
        uint8_t sel = od_editor_palette_at(ed);
        if (sel == OD_EDITOR_PALETTE_BACK) {
            to_toolbar(ed);
        } else if (pressed & OD_BTN_A) {
            ed->tool_prev = ed->tool;
            ed->tool = sel;
            legend_move(ed, ed->tool_prev, ed->tool, OD_EDITOR_MARKER_TILE_A);
        } else {
            ed->erase_prev = ed->erase;
            ed->erase = sel;
            legend_move(ed, ed->erase_prev, ed->erase, OD_EDITOR_MARKER_TILE_B);
        }
    }
}

static void tick_grid(od_editor *ed, unsigned pressed, unsigned held)
{
    if (pressed & OD_BTN_START) {   /* START -> toolbar */
        to_toolbar(ed);
        return;
    }
    if (pressed & OD_BTN_SELECT) {  /* SELECT -> palette */
        ed->mode = OD_EDITOR_PALETTE;
        return;
    }
    /* Exactly one direction executes, priority RIGHT > LEFT > DOWN > UP. */
    if (pressed & OD_BTN_RIGHT) {
        ed->cell = move_right(ed->cell);
        publish_action(ed, OD_EDITOR_ACTION_GRID_MOVE);
    } else if (pressed & OD_BTN_LEFT) {
        ed->cell = move_left(ed->cell);
        publish_action(ed, OD_EDITOR_ACTION_GRID_MOVE);
    } else if (pressed & OD_BTN_DOWN) {
        ed->cell = move_down(ed->cell);
        publish_action(ed, OD_EDITOR_ACTION_GRID_MOVE);
    } else if (pressed & OD_BTN_UP) {
        ed->cell = move_up(ed->cell);
        publish_action(ed, OD_EDITOR_ACTION_GRID_MOVE);
    }

    /* Painting uses the held buttons, not the press edge. */
    if (held & OD_BTN_A)      place(ed, ed->tool);
    else if (held & OD_BTN_B) place(ed, ed->erase);
}

od_editor_event od_editor_tick(od_editor *ed, unsigned buttons)
{
    if (!od_editor_data_get()) {
        (void)od_editor_data_view();
        return OD_EDITOR_EVENT_DATA_ERROR;
    }
    /* The press edge of the current frame drives A/B/SELECT/START and the
     * directions; painting uses the held buttons. The draw latch lives in the
     * caller (flow.c), so the state here is immediate. */
    if (ed->playing) /* the host owns the running level */
        return OD_EDITOR_EVENT_NONE;
    /* Blocked frames: no input is read until the loop resumes, so the press
     * edge is later taken against the last read. */
    if (ed->stall) {
        --ed->stall;
        return OD_EDITOR_EVENT_NONE;
    }
    unsigned pressed = buttons & ~ed->previous;
    unsigned held = buttons;
    ed->previous = buttons;

    od_editor_event event = OD_EDITOR_EVENT_NONE;
    switch (ed->mode) {
    case OD_EDITOR_TOOLBAR:
        event = tick_toolbar(ed, pressed);
        break;
    case OD_EDITOR_EDIT_ENTER:
        tick_edit_enter(ed);
        break;
    case OD_EDITOR_PLAY_ENTER:
        event = tick_play_enter(ed);
        break;
    case OD_EDITOR_EXIT_ENTER:
        event = tick_exit_enter(ed);
        break;
    case OD_EDITOR_PALETTE:
        tick_palette(ed, pressed);
        break;
    case OD_EDITOR_GRID:
    default:
        tick_grid(ed, pressed, held);
        break;
    }
    ed->last_event = (unsigned)event;
    return event;
}

void od_editor_resume(od_editor *ed)
{
    /* The level ended (solved, or NEW GAME): the editor starts again. The
     * board comes back from the packed buffer, the palette cursor is kept, and
     * the toolbar returns to the first entry with A = FIELD and B = BLOCK and
     * their markers on a freshly drawn legend. */
    ed->playing = 0;
    ed->entered = 0;
    ed->toolbar = OD_EDITOR_EDIT;
    ed->mode = OD_EDITOR_TOOLBAR;
    ed->phase = 0;
    ed->cell = 0;
    ed->previous = 0;
    ed->tool = 0x00;
    ed->erase = 0x01;
    ed->tool_prev = ed->tool;
    ed->erase_prev = ed->erase;
    memset(ed->legend, 0, sizeof ed->legend);
    ed->stall = 0;
    clear_actions(ed, OD_OK);
}

void od_editor_discard(od_editor *ed)
{
    /* This edition does not persist user levels. */
    od_editor_init(ed);
}

/* ------------------------------------------------- draw geometry (C3.7) -- */

int od_editor_toolbar_cursor_pos(const od_editor *ed, int *x, int *y)
{
    if (!od_editor_data_get()) { (void)od_editor_data_view(); return 0; }
    unsigned t = ed->toolbar % 3u;
    if (x) *x = od_editor_toolbar_x[t];
    if (y) *y = 0x17 + 1; /* the drawn row is one below 0x17 */
    return 1;
}

int od_editor_palette_cursor_pos(const od_editor *ed, int *x, int *y)
{
    if (!od_editor_data_get()) { (void)od_editor_data_view(); return 0; }
    unsigned r = ed->pal_row % OD_EDITOR_PALETTE_ROWS;
    unsigned c = ed->pal_col % OD_EDITOR_PALETTE_COLS;
    if (od_editor_pal_y[r][c] == 0xF0)
        return 0;
    if (x) *x = od_editor_pal_x[r][c];
    if (y) *y = od_editor_pal_y[r][c] + 1;
    return 1;
}

int od_editor_grid_cursor_pos(const od_editor *ed, int *x, int *y)
{
    if (!od_editor_data_get()) { (void)od_editor_data_view(); return 0; }
    unsigned row = od_editor_cursor_row(ed);
    unsigned col = od_editor_cursor_col(ed);
    if (row == 0 || row > 13 || col == 0 || col > 14)
        return 0;
    if (x) *x = od_editor_grid_x[col];
    if (y) *y = od_editor_grid_y[row] + 1;
    return 1;
}

uint8_t od_editor_cell_draw(const od_editor *ed, unsigned row, unsigned col)
{
    if (!od_editor_data_get()) { (void)od_editor_data_view(); return 0; }
    if (ed->start && ((ed->start >> 4) & 0x0F) == row &&
        (ed->start & 0x0F) == col)
        return OD_EDITOR_START_TILE;
    return od_editor_cell_tile[ed->cells[row][col] & 0x0Fu];
}

/* --------------------------------------------------- package export ------ */

/* Write one hex row of the 16-cell grid. */
static void grid_row(const uint8_t *row, char out[OD_GRID_W + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned x = 0; x < OD_GRID_W; ++x)
        out[x] = hex[row[x] & 0x0Fu];
    out[OD_GRID_W] = '\0';
}

od_status od_editor_export(const od_editor *ed, const char *path,
                             int world, int round, const char *name)
{
    if (!ed || !path)
        return OD_ERR_FORMAT;
    FILE *f = fopen(path, "w");
    if (!f)
        return OD_ERR_IO;

    unsigned start = ed->start ? ed->start : AREA_BASE;
    int px = (int)(start & 0x0F);
    int py = (int)((start >> 4) & 0x0F);
    if (world < 1) world = 1;
    if (round < 1) round = 1;
    if (!name || !name[0]) name = "editor";

    int ok = 1;
    ok &= fprintf(f, "{\n") > 0;
    ok &= fprintf(f, "  \"schema_version\": 1,\n") > 0;
    ok &= fprintf(f, "  \"id\": \"editor-%d-%d\",\n", world, round) > 0;
    ok &= fprintf(f, "  \"world\": %d,\n", world) > 0;
    ok &= fprintf(f, "  \"grid_width\": %d,\n", OD_GRID_W) > 0;
    ok &= fprintf(f, "  \"grid_height\": %d,\n", OD_GRID_H) > 0;
    ok &= fprintf(f, "  \"levels\": [\n") > 0;
    ok &= fprintf(f, "    {\n") > 0;
    ok &= fprintf(f, "      \"index\": 0,\n") > 0;
    ok &= fprintf(f, "      \"number\": %d,\n", round) > 0;
    ok &= fprintf(f, "      \"world\": %d,\n", world) > 0;
    ok &= fprintf(f, "      \"round\": %d,\n", round) > 0;
    ok &= fprintf(f, "      \"name\": \"%s\",\n", name) > 0;
    ok &= fprintf(f, "      \"player\": { \"x\": %d, \"y\": %d },\n", px, py) > 0;
    ok &= fprintf(f, "      \"metadata\": 0,\n") > 0;
    ok &= fprintf(f, "      \"grid\": [\n") > 0;
    for (unsigned y = 0; y < OD_GRID_H; ++y) {
        char row[OD_GRID_W + 1];
        grid_row(ed->cells[y], row);
        ok &= fprintf(f, "        \"%s\"%s\n", row,
                      y + 1 < OD_GRID_H ? "," : "") > 0;
    }
    ok &= fprintf(f, "      ]\n") > 0;
    ok &= fprintf(f, "    }\n") > 0;
    ok &= fprintf(f, "  ]\n") > 0;
    ok &= fprintf(f, "}\n") > 0;
    if (fclose(f) != 0)
        ok = 0;
    return ok ? OD_OK : OD_ERR_IO;
}
