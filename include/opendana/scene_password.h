/* scene_password.h - the "Enter your magicword." screen as logic (C3.3).
 *
 * The codec and the entry state machine live in password.c (H5). This module
 * owns the *scene*: the one-tick PPU latch between the CPU field and what the
 * screen draws, the 24 field glyphs and the two cursors, drawn from the imported
 * layout (the old reference capture `assets/screens/password.track` is private
 * since C6.18). Pure C11, no SDL. */
#ifndef OD_SCENE_PASSWORD_H
#define OD_SCENE_PASSWORD_H

#include "opendana/common.h"
#include "opendana/password.h"

/* Password-entry scene state. `ui` is the CPU state (field, cursors,
 * validation); `shown` is the PPU latch the renderer draws, one tick behind,
 * exactly as the reference's nametable update reaches the screen one display
 * pass later (H5 §8.3, D5). */
typedef struct {
    od_password_ui ui;
    od_password_ui shown;
    int active;
} od_scene_password;

void od_scene_password_reset(od_scene_password *p);
/* Enter the screen: empty field, palette cursor on the first symbol. */
void od_scene_password_begin(od_scene_password *p);
/* Advance one CPU tick with the native OD_BTN_* mask and publish the view
 * latch. Returns the UI result (EDITING/ACCEPTED/REJECTED). */
od_password_result od_scene_password_tick(od_scene_password *p,
                                            unsigned buttons);

/* The 24 nametable tiles of the drawn field (`shown`): a filled code resolves
 * through `od_password_glyph`, an empty cell is the blank tile. */
void od_scene_password_field_tiles(const od_scene_password *p,
                                    uint8_t tiles[OD_PASSWORD_FIELD]);

/* Screen geometry of the extracted display: the 24 cells are nametable row 4,
 * columns 3..10 / 12..19 / 21..28; the field cursor and the
 * palette cursor are two sprite tiles. Returns the pixel position of palette cursor
 * `cursor` (0..34). */
void od_scene_password_cursor(unsigned cursor, int *x, int *y);

/* Patch the drawn field and both cursors into an OD_SCENE_WORDS vector
 * (`tiles` from od_scene_password_field_tiles, `position` the 0..23 field
 * cursor, `cursor` the 0..34 palette cursor). */
void od_scene_password_panel(const uint8_t tiles[OD_PASSWORD_FIELD],
                              unsigned position, unsigned cursor,
                              uint16_t *display, unsigned display_words);

/* Drop both cursor sprites from a vector patched by od_scene_password_panel:
 * the frame the "*Error! try again." message reverts carries no cursor in OAM
 * (reference f1880 -> shown f1881). */
void od_scene_password_no_cursors(uint16_t *display, unsigned display_words);

#endif /* OD_SCENE_PASSWORD_H */
