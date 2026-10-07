/* scene_password.c - the "Enter your magicword." screen as logic (C3.3).
 *
 * The dynamic part of the screen (the 24 field glyphs and the two cursors) is
 * built here from the entry state machine; the layout, the colour and the tile
 * alphabet come from the extracted reference screen and the imported codec. */
#include "opendana/scene_password.h"
#include "opendana/game.h"

#include <string.h>

/* The field cells are nametable row 4, columns 3..10 / 12..19 / 21..28 (H5).
 * A cell i starts one column later every eight cells, skipping the box gap. */
#define PASSWORD_NT_BASE 32u
#define PASSWORD_FIELD_ROW 4u
#define PASSWORD_EMPTY_TILE 0xadu  /* the empty cell the reference draws */
#define PASSWORD_FIELD_CURSOR_TILE 0xedu
#define PASSWORD_PALETTE_CURSOR_TILE 0xffu
#define PASSWORD_SPRITE_BASE 2124u

static unsigned field_column(unsigned i)
{
    return 3u + i + i / 8u;
}

void od_scene_password_reset(od_scene_password *p)
{
    memset(p, 0, sizeof *p);
}

void od_scene_password_begin(od_scene_password *p)
{
    od_scene_password_reset(p);
    od_password_ui_reset(&p->ui);
    p->shown = p->ui;
    p->active = 1;
}

od_password_result od_scene_password_tick(od_scene_password *p,
                                            unsigned buttons)
{
    /* The CPU reads the buttons of its own tick (H5 D5); the drawing latch is
     * the previous state, one PPU frame behind. */
    p->shown = p->ui;
    return od_password_ui_tick(&p->ui, buttons);
}

void od_scene_password_field_tiles(const od_scene_password *p,
                                    uint8_t tiles[OD_PASSWORD_FIELD])
{
    for (unsigned i = 0; i < OD_PASSWORD_FIELD; ++i) {
        unsigned code = p->shown.field[i];
        tiles[i] = code == OD_PASSWORD_EMPTY
                 ? (uint8_t)PASSWORD_EMPTY_TILE
                 : od_password_glyph[code & 31u];
    }
}

void od_scene_password_cursor(unsigned cursor, int *x, int *y)
{
    cursor %= OD_PASSWORD_CURSORS;
    if (cursor < 10)       { *x = 44 + 16 * (int)cursor;      *y = 120; }
    else if (cursor < 20)  { *x = 44 + 16 * (int)(cursor-10); *y = 136; }
    else if (cursor < 30)  { *x = 44 + 16 * (int)(cursor-20); *y = 152; }
    else if (cursor == 30) { *x = 44;  *y = 168; }
    else if (cursor == 31) { *x = 60;  *y = 168; }
    else if (cursor == 32) { *x = 76;  *y = 168; }  /* forth */
    else if (cursor == 33) { *x = 124; *y = 168; }  /* back  */
    else                   { *x = 172; *y = 168; }  /* END   */
}

void od_scene_password_panel(const uint8_t tiles[OD_PASSWORD_FIELD],
                              unsigned position, unsigned cursor,
                              uint16_t *display, unsigned display_words)
{
    if (!display)
        return;
    for (unsigned i = 0; i < OD_PASSWORD_FIELD; ++i) {
        unsigned idx = PASSWORD_NT_BASE + PASSWORD_FIELD_ROW * 32u +
                       field_column(i);
        if (idx < display_words)
            display[idx] = tiles ? tiles[i] : PASSWORD_EMPTY_TILE;
    }
    if (PASSWORD_SPRITE_BASE + 8u >= display_words)
        return;
    int px, py;
    od_scene_password_cursor(cursor, &px, &py);
    display[PASSWORD_SPRITE_BASE] = 2;   /* field cursor + palette cursor */
    display[PASSWORD_SPRITE_BASE + 1] =
        (uint16_t)(field_column(position % OD_PASSWORD_FIELD) * 8u);
    display[PASSWORD_SPRITE_BASE + 2] = 40;
    display[PASSWORD_SPRITE_BASE + 3] = PASSWORD_FIELD_CURSOR_TILE;
    display[PASSWORD_SPRITE_BASE + 4] = 0;
    display[PASSWORD_SPRITE_BASE + 5] = (uint16_t)px;
    display[PASSWORD_SPRITE_BASE + 6] = (uint16_t)py;
    display[PASSWORD_SPRITE_BASE + 7] = PASSWORD_PALETTE_CURSOR_TILE;
    display[PASSWORD_SPRITE_BASE + 8] = 0;
}

void od_scene_password_no_cursors(uint16_t *display, unsigned display_words)
{
    if (display && PASSWORD_SPRITE_BASE < display_words)
        display[PASSWORD_SPRITE_BASE] = 0;   /* no sprite this frame */
}
