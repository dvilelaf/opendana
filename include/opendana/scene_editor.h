/* scene_editor.h - the level editor screen as logic (C3.7).
 *
 * The editor state machine lives in editor.c (H5). This module owns the
 * *scene*: the board cells, the two tool markers, the three cursors and the
 * colour cycle, drawn from the imported layout (the old reference capture
 * `assets/screens/editor.track` is private since C6.18). Pure C11, no SDL. */
#ifndef OD_SCENE_EDITOR_H
#define OD_SCENE_EDITOR_H

#include "opendana/common.h"
#include "opendana/editor.h"

/* Patch the editor's dynamic state into an OD_SCENE_WORDS display vector:
 *  - the 14x12 board area (nametable rows 8..19, columns 2..15) from the cell
 *    types through the legend table, with the Hero-start marker;
 *  - the legend slots the A/B tool choices rewrote (`ed->legend`);
 *  - exactly one cursor sprite (toolbar/palette or grid).
 * The static screen (toolbar text, legends, borders) comes from the extracted
 * track; this only overlays what the original drives from the editor's state. */
void od_scene_editor_panel(const od_editor *ed, uint16_t *display,
                            unsigned display_words);

/* The editor init's board draw: the first `rows` rows of the board area from
 * `ed`, as uploaded one row per frame before the loop. */
void od_scene_editor_board_rows(const od_editor *ed, uint16_t *display,
                                 unsigned display_words, unsigned rows);

/* The editor's colour cycle, called once per pass of the editor loop. On the
 * frames whose frame counter is a multiple of 8 it writes the background colour
 * table entry to the background entries 5, 6, 10 and 14 (the toolbar frame and
 * the board border) and steps the background counter (wrap at 7); on the
 * multiples of 4 it writes the three sprite-palette tables to entries 1..3 of
 * the four sprite palettes (the cursors) and steps the sprite counter (& 3).
 * The background counter is the one the title and the map share
 * (od_screens.frame_cycle), the sprite counter the one "Today's magicword" also
 * steps for its cursor (one byte, od_screens.cycle_6f); neither is reset by a
 * scene. The written values persist in the palette buffer until the screen
 * reloads it. */
typedef struct {
    /* The palette buffer entries the cycle writes (background 5/6/10/14 and the
     * sprite entries 1..3 of the four palettes), valid where `known` is set:
     * captured from the screen's palette when the loop starts
     * (od_scene_editor_cycle_start), then rewritten by each step. */
    uint8_t entry[32];
    uint8_t known[32];
    /* Frames since a return to the toolbar (1..4, 0 = none): its string re-upload,
     * od_scene_editor_toolbar_redraw. */
    uint8_t toolbar_phase;
} od_editor_cycle;

/* The loop's first pass: the palette buffer holds the palette the editor's
 * init loaded (the screen's own palette words `display[0..31]`). */
/* C6.5: start/step require installed scene-data (or the default JSON package).
 * On unavailable/invalid data they diagnose to stderr and leave all outputs
 * unchanged. Startup must propagate parse/install errors, not continue. */
void od_scene_editor_cycle_start(od_editor_cycle *c, const uint16_t *display);

void od_scene_editor_cycle_step(od_editor_cycle *c, unsigned frame_tick,
                                 uint8_t *r6e, uint8_t *r6f);

/* Write the cycle's palette entries into a display vector's 32 palette words
 * (or a 32-entry palette when `words` is a byte palette; see the two
 * helpers). Entries the cycle has not written yet keep their value. */
void od_scene_editor_cycle_apply(const od_editor_cycle *c, uint16_t *display);

/* The toolbar re-upload as seen on the blocked frames: `phase` is the frame
 * after the START/back press (1..4); phase 2 shows the toolbar row without EXIT
 * and phase 4 the round number blanked; other phases change nothing. */
void od_scene_editor_toolbar_redraw(uint16_t *display, unsigned display_words,
                                     unsigned phase);
void od_scene_editor_cycle_apply_ram(const od_editor_cycle *c,
                                      uint8_t palette[32]);

#endif /* OD_SCENE_EDITOR_H */
