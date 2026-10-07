/* editor.h - the level editor (EDIT / PLAY / EXIT) state machine.
 *
 * Pure C11, no SDL and no ROM dependency.
 *
 * The edited area is a 14x12 nibble field starting at grid index 0x11 (row 1,
 * column 1 of the 16x15 play grid). It is stored as 84 packed bytes (two
 * horizontal cells per byte, high nibble first). PLAY runs the user level;
 * EXIT leaves to the "What next?" slot.
 *
 * The draw geometry (the cell->tile table, both cursor tables and the A/B
 * tool markers) comes from the content package, and a level can be exported
 * in the package format. */
#ifndef OD_EDITOR_H
#define OD_EDITOR_H

#include "opendana/common.h"
#include "opendana/editor_data.h"

#define OD_EDITOR_AREA_X   1
#define OD_EDITOR_AREA_Y   1
#define OD_EDITOR_AREA_W   14
#define OD_EDITOR_AREA_H   12
#define OD_EDITOR_AREA_CELLS (OD_EDITOR_AREA_W * OD_EDITOR_AREA_H)
#define OD_EDITOR_PACKED     (OD_EDITOR_AREA_CELLS / 2) /* 84 */

/* Tool codes: the low nibble is a grid cell type; 0x10 places the player start
 * (0x10). 0x80 marks an empty palette cell; 0xFF is the palette "back". */
#define OD_EDITOR_TOOL_START 0x10
#define OD_EDITOR_PALETTE_EMPTY 0x80
#define OD_EDITOR_PALETTE_BACK  0xFF

#define OD_EDITOR_PALETTE_ROWS 3
#define OD_EDITOR_PALETTE_COLS 12

/* Palette: three rows of 12 slots. 0x80 = empty slot, 0xFF = the "back"
 * action. */
#define od_editor_palette (od_editor_data_view()->palette)

/* ---- draw tables (from engine/editor-data.json) ------------------------- */

/* Cell type -> character tile the board draws. The 0x10 start cell is a separate
 * marker (0x9e) written when packing, not a cell type. */
#define od_editor_cell_tile (od_editor_data_view()->cell_tile)

/* Toolbar cursor X for EDIT/PLAY/EXIT. */
#define od_editor_toolbar_x (od_editor_data_view()->toolbar_x)

/* Grid cursor Y (indexed by row 0..13) and X (column 0..14). The sprite Y is
 * one scanline above the drawn row, so callers draw at Y+1. */
#define od_editor_grid_y (od_editor_data_view()->grid_y)
#define od_editor_grid_x (od_editor_data_view()->grid_x)

/* Palette cursor (Y,X) per row/column. 0xF0 marks a hidden slot (no cursor). */
#define od_editor_pal_y (od_editor_data_view()->pal_y)
#define od_editor_pal_x (od_editor_data_view()->pal_x)

/* For each tool, the little-endian video address of its legend slot. 0xC1
 * draws the A marker, 0xC2 the B marker. */
#define od_editor_marker_addr (od_editor_data_view()->marker_addr)

#define OD_EDITOR_MARKER_TILE_A 0xC1u
#define OD_EDITOR_MARKER_TILE_B 0xC2u
#define OD_EDITOR_MARKER_BLANK  0x80u /* written over the old slot when moving */
#define OD_EDITOR_TOOLBAR_STALL 4u    /* blocked loop frames            */
#define OD_EDITOR_CURSOR_TILE   0x7Eu /* toolbar + palette */
#define OD_EDITOR_GRID_TILE     0x76u /* grid cursor        */
#define OD_EDITOR_START_TILE    0x9Eu /* Hero start marker  */
#define OD_EDITOR_BOARD_TILE_ROW 8u   /* nametable row 1 of the board  */
#define OD_EDITOR_BOARD_TILE_COL 2u   /* nametable col 1 of the board  */

typedef enum {
    OD_EDITOR_TOOLBAR = 0, /* EDIT / PLAY / EXIT selection          */
    OD_EDITOR_EDIT_ENTER,  /* two-frame ramp                         */
    OD_EDITOR_PALETTE,     /* choose the A/B tools                   */
    OD_EDITOR_PLAY_ENTER,  /* two-frame ramp                         */
    OD_EDITOR_EXIT_ENTER,  /* the frame after the confirmation       */
    OD_EDITOR_GRID         /* move and paint on the 14x12 board       */
} od_editor_mode;

typedef enum {
    OD_EDITOR_EDIT = 0,
    OD_EDITOR_PLAY = 1,
    OD_EDITOR_EXIT = 2
} od_editor_toolbar;

typedef enum {
    OD_EDITOR_EVENT_NONE = 0,
    OD_EDITOR_EVENT_PLAY, /* host must start the user level          */
    OD_EDITOR_EVENT_EXIT, /* host returns to the "What next?" slot   */
    OD_EDITOR_EVENT_DATA_ERROR /* package unavailable; no tick mutation */
} od_editor_event;

/* Executed actions, not sound IDs: GRID_MOVE maps to sound request 34,
 * PLAY_ENTER to request 1. No init/upload readiness is implied. */
typedef enum {
    OD_EDITOR_ACTION_GRID_MOVE = 1,
    OD_EDITOR_ACTION_PLAY_ENTER = 2
} od_editor_action;
#define OD_EDITOR_ACTION_CAPACITY 8u

typedef struct {
    uint8_t cells[OD_GRID_H][OD_GRID_W]; /* play grid, nibble per cell */
    uint8_t packed[OD_EDITOR_PACKED];

    unsigned toolbar;   /* 0..2: EDIT / PLAY / EXIT                    */
    int entered;        /* toolbar choice committed (A)                */
    unsigned mode;      /* od_editor_mode                            */
    unsigned phase;     /* frames spent in the EDIT/PLAY entry ramp     */

    unsigned pal_row;   /* moved by LEFT/RIGHT                         */
    unsigned pal_col;   /* moved by DOWN/UP                            */
    int tool;           /* A-selected paint tool (-1 = none)           */
    int erase;          /* B-selected erase tool (-1 = none)           */
    int tool_prev;      /* previous A tool, to blank its legend slot    */
    int erase_prev;     /* previous B tool                              */
    /* The legend slots' nametable tile as the writes left it, per tool
     * (indexed by marker slot); 0 = never written (the screen's own tile). A
     * new A/B tool writes 0x80 over the old slot first and then 0xC1/0xC2 at
     * the new one: the writes persist, so a slot keeps a blank (or loses the
     * other marker) after the choice moves on. */
    uint8_t legend[17];
    /* Frames the main loop stays blocked after this tick: going back to the
     * toolbar (START from the palette or the grid, or the palette's back slot)
     * yields while the toolbar strings are queued, so the loop's tail runs
     * only on the 4th following frame and no input is read until the 5th. The
     * host counts it down. */
    unsigned stall;

    unsigned cell;      /* grid cursor (row<<4 | col), starts at 0x11  */
    unsigned start;     /* player start cell, 0 = none                 */
    unsigned fires;     /* count of 0x0e cells in the area             */

    unsigned previous;  /* last frame's buttons, for press edges       */
    int dirty;          /* edits since the last pack                   */
    int playing;        /* the user level is being played              */
    unsigned last_event;
    /* Serializable POD; live actions 1..2, unused slots zero. Error is sticky
     * until init/resume/discard: RANGE overflow, FORMAT invalid queue state. */
    uint8_t actions[OD_EDITOR_ACTION_CAPACITY];
    uint8_t action_count; /* 0..8; errors require zero */
    uint8_t action_status; /* OD_OK / OD_ERR_RANGE / OD_ERR_FORMAT */
} od_editor;

void od_editor_init(od_editor *ed);

/* Advance one tick with native OD_BTN_* buttons. Returns the event raised by
 * this tick (PLAY/EXIT), NONE or DATA_ERROR. Startup must reject missing
 * editor data before simulation; the latter does not mutate the editor. */
od_editor_event od_editor_tick(od_editor *ed, unsigned buttons);

/* Read-only queries; invalid POD returns FORMAT / zero count. Drain preserves
 * queue on insufficient capacity or invalid output arguments; *count=0 on
 * error, never a partial batch. Error queues cannot fall back to other cues.
 * NULL out accepted only for an empty batch. Successful drain zeros storage.
 * Caller drains immediately after tick, BEFORE dispatching PLAY/EXIT/resume. */
size_t od_editor_action_count(const od_editor *ed);
od_status od_editor_action_status(const od_editor *ed);
od_status od_editor_take_actions(od_editor *ed, uint8_t *out,
                                  size_t capacity, size_t *count);

/* Host callbacks for the create -> play -> return -> modify cycle. */
void od_editor_resume(od_editor *ed); /* level ended: keep the board, back to toolbar */
void od_editor_discard(od_editor *ed); /* EXIT: the created level is not persisted     */

/* Pack the 14x12 area into 84 bytes, counting the 0x0e fires.
 * The inverse of od_editor_unpack. */
void od_editor_pack(od_editor *ed);
void od_editor_unpack(od_editor *ed, const uint8_t packed[OD_EDITOR_PACKED]);

/* Palette cell currently under the cursor. */
uint8_t od_editor_palette_at(const od_editor *ed);

/* Grid coordinates of the cursor: column 1..14, row 1..12 of the play grid. */
unsigned od_editor_cursor_col(const od_editor *ed);
unsigned od_editor_cursor_row(const od_editor *ed);

/* Geometry helpers used by the scene module. All return 0 when the cursor is
 * hidden (a 0xF0 palette slot). */
int od_editor_toolbar_cursor_pos(const od_editor *ed, int *x, int *y);
int od_editor_palette_cursor_pos(const od_editor *ed, int *x, int *y);
int od_editor_grid_cursor_pos(const od_editor *ed, int *x, int *y);

/* The nametable tile a board cell draws (the start cell draws 0x9e). */
uint8_t od_editor_cell_draw(const od_editor *ed, unsigned row, unsigned col);

/* Export the edited level in the package JSON format (content/base/levels/
 * *.json: schema_version + a single-entry "levels" array). The level carries
 * Hero at `ed.start` (or the default 0x11) and the exact 16x15 grid.
 * Returns OD_OK or OD_ERR_IO/OD_ERR_FORMAT. */
od_status od_editor_export(const od_editor *ed, const char *path,
                             int world, int round, const char *name);

#endif /* OD_EDITOR_H */
