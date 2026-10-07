/* scene_editor.c - the level editor screen as logic (C3.7).
 *
 * The editor draws its dynamic state through a small display list and the OAM:
 * the board cells are nametable writes, the A/B tool markers are single-tile
 * nametable writes at their legend slots, and the toolbar/palette/grid cursors
 * are one OAM sprite each. This module patches the same things directly into the
 * scene display vector that the renderer composes. Pure C11, no SDL. */
#include "opendana/scene_editor.h"
#include "opendana/scene_data.h"

/* Layout offsets inside the OD_SCENE_WORDS display vector (screens.c). */
#define EDITOR_NT_BASE     32u    /* 32 palette words, then the nametable    */
#define EDITOR_SPRITE_BASE 2124u  /* count, then 4 words per sprite          */

void od_scene_editor_board_rows(const od_editor *ed, uint16_t *display,
                                 unsigned display_words, unsigned rows)
{
    if (!ed || !display)
        return;
    /* The cell types come from the legend table, with no start marker; the 13th
     * step draws the start marker at the hero's cell (od_editor_cell_draw). */
    unsigned board = rows > OD_EDITOR_AREA_H ? OD_EDITOR_AREA_H : rows;
    for (unsigned r = 0; r < board; ++r)
        for (unsigned c = 0; c < OD_EDITOR_AREA_W; ++c) {
            unsigned idx = EDITOR_NT_BASE +
                           (OD_EDITOR_BOARD_TILE_ROW + r) * 32u +
                           (OD_EDITOR_BOARD_TILE_COL + c);
            unsigned y = OD_EDITOR_AREA_Y + r, x = OD_EDITOR_AREA_X + c;
            if (idx < display_words)
                display[idx] = rows > OD_EDITOR_AREA_H
                                   ? od_editor_cell_draw(ed, y, x)
                                   : od_editor_cell_tile[ed->cells[y][x] & 0x0Fu];
        }
}

void od_scene_editor_panel(const od_editor *ed, uint16_t *display,
                            unsigned display_words)
{
    if (!ed || !display || display_words < EDITOR_SPRITE_BASE + 5u)
        return;

    /* Board: the 14x12 area at nametable rows 8..19, columns 2..15. */
    for (unsigned r = 0; r < OD_EDITOR_AREA_H; ++r) {
        for (unsigned c = 0; c < OD_EDITOR_AREA_W; ++c) {
            unsigned idx = EDITOR_NT_BASE +
                           (OD_EDITOR_BOARD_TILE_ROW + r) * 32u +
                           (OD_EDITOR_BOARD_TILE_COL + c);
            if (idx < display_words)
                display[idx] = od_editor_cell_draw(ed,
                                  OD_EDITOR_AREA_Y + r,
                                  OD_EDITOR_AREA_X + c);
        }
    }

    /* A/B tool markers: every legend slot a choice rewrote keeps the last tile
     * that was queued there (the old slot is blanked, the new one gets the A or B
     * tile); untouched slots keep the screen's tile. */
    for (unsigned t = 0; t < 17; ++t) {
        if (!ed->legend[t])
            continue;
        unsigned addr = (unsigned)od_editor_marker_addr[t][0] |
                        ((unsigned)od_editor_marker_addr[t][1] << 8);
        unsigned idx = EDITOR_NT_BASE + (addr - 0x2000u);
        if (idx < display_words)
            display[idx] = ed->legend[t];
    }

    /* Cursors: exactly one sprite. The entry ramp frames (no cursor yet) clear the
     * toolbar sprite the extracted track carries. */
    int x = 0, y = 0, visible = 0;
    unsigned tile = OD_EDITOR_CURSOR_TILE;
    switch (ed->mode) {
    case OD_EDITOR_TOOLBAR:
        visible = od_editor_toolbar_cursor_pos(ed, &x, &y);
        break;
    case OD_EDITOR_PALETTE:
        visible = od_editor_palette_cursor_pos(ed, &x, &y);
        break;
    case OD_EDITOR_GRID:
        visible = od_editor_grid_cursor_pos(ed, &x, &y);
        tile = OD_EDITOR_GRID_TILE;
        break;
    case OD_EDITOR_EDIT_ENTER:
    case OD_EDITOR_PLAY_ENTER:
    default:
        visible = 0;
        break;
    }
    if (visible) {
        display[EDITOR_SPRITE_BASE] = 1;
        display[EDITOR_SPRITE_BASE + 1] = (uint16_t)x;
        display[EDITOR_SPRITE_BASE + 2] = (uint16_t)y;
        display[EDITOR_SPRITE_BASE + 3] = (uint16_t)tile;
        display[EDITOR_SPRITE_BASE + 4] = 0;
    } else {
        display[EDITOR_SPRITE_BASE] = 0;
        display[EDITOR_SPRITE_BASE + 1] = 0;
        display[EDITOR_SPRITE_BASE + 2] = 0;
        display[EDITOR_SPRITE_BASE + 3] = 0;
        display[EDITOR_SPRITE_BASE + 4] = 0;
    }
}

/* ---- the editor's colour cycle ------------------------------------------ */

/* One contiguous block of tables: three sprite-palette tables (indexed by the
 * sprite counter) and the background one (indexed by the background counter).
 * Indexing the block as a whole keeps an out-of-range counter left by another
 * scene exact (it reads the following table). */
static uint8_t cycle_at(const od_scene_data *data, unsigned i)
{
    return i < sizeof data->cycle_block ? data->cycle_block[i] : 0x0F;
}

/* Palette-buffer targets of the cycle: entry 1 of each of the four sprite
 * palettes plus the background slots. */

static unsigned cycle_spr(unsigned p, unsigned k)
{
    return 16u + p * 4u + 1u + k;
}

void od_scene_editor_cycle_start(od_editor_cycle *c, const uint16_t *display)
{
    if (!c || !display)
        return;
    const od_scene_data *data = od_scene_data_require();
    if (!data) return;
    for (unsigned i = 0; i < 4; ++i) {
        c->entry[data->cycle_bg[i]] = (uint8_t)(display[data->cycle_bg[i]] & 0x3Fu);
        c->known[data->cycle_bg[i]] = 1;
    }
    for (unsigned p = 0; p < 4; ++p)
        for (unsigned k = 0; k < 3; ++k) {
            unsigned e = cycle_spr(p, k);
            c->entry[e] = (uint8_t)(display[e] & 0x3Fu);
            c->known[e] = 1;
        }
}

void od_scene_editor_cycle_step(od_editor_cycle *c, unsigned frame_tick,
                                 uint8_t *r6e, uint8_t *r6f)
{
    if (!c || !r6e || !r6f)
        return;
    const od_scene_data *data = od_scene_data_require();
    if (!data) return;
    if ((frame_tick & 7u) == 0) {          /* sprite palette step */
        unsigned x = *r6e;
        uint8_t colour = cycle_at(data, 12u + x);
        for (unsigned i = 0; i < 4; ++i) {
            c->entry[data->cycle_bg[i]] = colour;
            c->known[data->cycle_bg[i]] = 1;
        }
        ++x;
        if (x >= 7u)
            x = 0;
        *r6e = (uint8_t)x;
    }
    if ((frame_tick & 3u) == 0) {          /* background palette step */
        unsigned x = *r6f;
        for (unsigned p = 0; p < 4; ++p)
            for (unsigned k = 0; k < 3; ++k) {
                unsigned e = cycle_spr(p, k);
                c->entry[e] = cycle_at(data, k * 4u + x);
                c->known[e] = 1;
            }
        *r6f = (uint8_t)((x + 1u) & 3u);
    }
}

void od_scene_editor_cycle_apply(const od_editor_cycle *c, uint16_t *display)
{
    if (!c || !display)
        return;
    for (unsigned i = 0; i < 32; ++i)
        if (c->known[i])
            display[i] = c->entry[i];
}

void od_scene_editor_cycle_apply_ram(const od_editor_cycle *c,
                                      uint8_t palette[32])
{
    if (!c || !palette)
        return;
    for (unsigned i = 0; i < 32; ++i)
        if (c->known[i])
            palette[i] = c->entry[i];
}

/* ---- the toolbar re-upload ---------------------------------------------- */

/* The re-upload queues the toolbar strings, waiting for room in the 60-byte
 * VRAM queue. From an empty queue the strings land on the following frames as
 * measured (queue length 50, 33, 55, 19 at f2801..f2804): only two uploads
 * change the picture. The 2nd frame writes nine blanks over the toolbar's EXIT
 * (it disappears until the 3rd frame rewrites the row), and the 4th frame
 * writes "ROUND" plus three blanks, blanking the round number until the loop
 * queues it again. */
void od_scene_editor_toolbar_redraw(uint16_t *display, unsigned display_words,
                                     unsigned phase)
{
    if (!display)
        return;
    unsigned addr = 0, count = 0;
    if (phase == 2) {          /* EXIT blanks */
        addr = 0x2072u;
        count = 9;
    } else if (phase == 4) {   /* the three trailing blanks of "ROUND" */
        addr = 0x20AAu;
        count = 3;
    }
    for (unsigned i = 0; i < count; ++i) {
        unsigned idx = EDITOR_NT_BASE + (addr - 0x2000u) + i;
        if (idx < display_words)
            display[idx] = OD_EDITOR_MARKER_BLANK;
    }
}
