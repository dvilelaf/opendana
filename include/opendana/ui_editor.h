/* ui_editor.h - modern mouse level editor screen (C5.5).
 *
 * The pure, SDL-free half of the editor screen. The document itself is the
 * model of C5.4 (`src/ext/leveled.c`); this module turns mouse and keyboard
 * input into model edits and rasterises the screen into the 256x240 ARGB
 * overlay canvas of `src/app/ui.c`. It never touches the simulation: the host
 * freezes the flow while the editor is open, exactly like the modern menu.
 *
 * The level is drawn by the host with the core compositor (the same one the
 * game uses) and handed in through `preview`; the editor only draws the panels
 * and maps the pointer onto grid cells. Cell (x, y) is painted by the core at
 * (16x, 16y), so the editor uses that origin verbatim. */
#ifndef OD_UI_EDITOR_H
#define OD_UI_EDITOR_H

#include "opendana/ext/leveled.h"

#include <stddef.h>
#include <stdint.h>

/* Canvas geometry (C6.24). The editor and the catalogue draw
 * into their own opaque 384x360 canvas, which the host presents at the
 * largest whole multiple that fits the window (od_ued_fit). Nothing overlaps
 * the level: the 256x240 frame sits 1:1 at OD_UED_LEVEL_X/Y with the toolbar
 * above it, the palette to its right and the world selector, the status and
 * the hints below it. (Until C6.24 the panels were drawn over the level in
 * the 256x240 game overlay, which hid rows 0 and 14 and columns 11-15.) */
#define OD_UED_W 384
#define OD_UED_H 360
#define OD_UED_LEVEL_X 4
#define OD_UED_LEVEL_Y 18
#define OD_UED_LEVEL_W 256
#define OD_UED_LEVEL_H 240

/* Test-play view (C6.24): the game's 256x240 frame composed without its
 * status bar, with the 24 rows of the bar (frame rows 216..239, where the
 * original draws ROUND/TIME over the level) appended underneath. Only the
 * editor's Test/Play uses it; the vanilla game is presented as always. */
#define OD_UED_TEST_W 256
#define OD_UED_TEST_H 264
#define OD_UED_BAR_TOP 216
#define OD_UED_BAR_H 24

typedef struct { int x, y, w, h; } od_ued_rect;

/* Destination of a `cw` x `ch` canvas in a `win_w` x `win_h` window: the
 * largest whole multiple that fits when `integer` is set (falling back to the
 * largest fit when not even 1x fits), else the largest aspect-preserving fit;
 * `aspect_87` widens horizontally by 8/7 like the game. Always centred. */
void od_ued_fit(int win_w, int win_h, int cw, int ch, int integer,
                 int aspect_87, od_ued_rect *out);

/* Compose the 256x264 test-play view into `out`. `frame` is the frame the
 * game presented (bar included); `level` is the same tick composed without
 * the bar. When the game drew a bar over rows 216..239 the whole level goes on
 * top and the bar below it; otherwise (a scene without a bar, or the pause
 * band, which the original also draws there) the frame is shown unchanged
 * with an empty bar. */
void od_ued_test_view(const uint32_t *frame, const uint32_t *level,
                       uint32_t *out);

/* Pointer button bits (od_ued_pointer). */
#define OD_UED_BTN_LEFT   1
#define OD_UED_BTN_MIDDLE 2
#define OD_UED_BTN_RIGHT  4

/* Cell type used for the Hero start marker; the model serialises it as the
 * level's `player` position (0x10 is the original editor's start tool). */
#define OD_UED_TOOL_HERO 0x10

/* The palette offered by the editor: every playable cell type
 * the original editor lists plus the Hero start marker. */
#define OD_UED_PALETTE_MAX 20
typedef struct {
    uint8_t cell;          /* 0x00..0x0F, or OD_UED_TOOL_HERO            */
    const char *name;      /* English name shown next to the swatch (D6)  */
} od_ued_palette_entry;

/* Buttons: the toolbar above the level (New .. Back), the world selector
 * under it (W-, W+) and the moving-flame brush under the palette. */
enum {
    OD_UED_BTN_NEW = 0,
    OD_UED_BTN_OPEN,
    OD_UED_BTN_SAVE,
    OD_UED_BTN_SAVEAS,
    OD_UED_BTN_TEST,
    OD_UED_BTN_UNDO,
    OD_UED_BTN_REDO,
    OD_UED_BTN_WORLD_DOWN,
    OD_UED_BTN_WORLD_UP,
    OD_UED_BTN_FLAME,
    OD_UED_BTN_BACK,
    OD_UED_BTN_COUNT
};

typedef struct od_ui_editor {
    od_led_doc doc;
    int open;

    char catalog_dir[1024];       /* where Save/Save as write (XDG levels)  */
    char file_path[1200];         /* file the document came from, may be "" */
    char last_saved_id[OD_LED_ID_MAX];
    char last_saved_path[1200];
    /* Successful JSON save only; host consumes once. Detached value snapshot,
     * never owns undo/redo and never enters the simulation/state wire. */
    int thumbnail_pending;
    od_led_doc thumbnail_doc;

    /* Brush: index into the palette (or the flame-object brush). */
    int brush;
    int palette_scroll;           /* unused since C6.24: every entry shows   */
    int palette_hidden;           /* 'P' hides the palette panel             */
    int flame_brush;              /* the extra "llama" object brush           */

    /* Pointer state (canvas coordinates). */
    int ptr_x, ptr_y;
    int hover_cell_x, hover_cell_y;   /* cell under the pointer, -1 outside */
    int painting;                     /* left drag in progress             */
    int erasing;                      /* right drag in progress            */
    int filling;                      /* shift+left rectangle in progress  */
    int fill_x0, fill_y0, fill_x1, fill_y1;
    int shift, ctrl;

    /* Requests for the host (cleared when serviced). */
    int request_test;             /* Test: play the document             */
    int request_back;             /* leave the editor                    */
    int request_open;             /* open the catalogue in edit mode     */

    /* C6.32: an action that would discard unsaved changes (New, Open, Back)
     * waits for a confirmation (OD_UED_CONFIRM_*); Accept/Discard runs it,
     * Esc/Cancel drops it. */
    int confirm;

    /* Status line. */
    char status[160];
    int status_error;

    /* Preview supplied by the host each frame: the document composed by the
     * core (256x240 ARGB), optional per-cell-type palette swatches
     * (`palette_swatch` points to 16 consecutive 16x16 ARGB tiles, one per
     * cell type 0..15; NULL falls back to flat colour squares) and the Hero
     * swatch. */
    const uint32_t *preview;
    int preview_w, preview_h;
    const uint32_t *palette_swatch;
    const uint32_t *hero_swatch;
} od_ui_editor;

/* ---- document ----------------------------------------------------------- */

void od_ued_init(od_ui_editor *e, const char *catalog_dir);
/* Release the document's undo history. Safe on a zeroed struct. */
void od_ued_free(od_ui_editor *e);

/* Start a new blank document (solid border, no Hero) for `theme_world`. */
void od_ued_new(od_ui_editor *e, int theme_world);
/* Load a package world file or a custom-level file into the document. A file
 * of the catalogue directory keeps its file stem as the id, so Save writes
 * back to that same file; any other file gets no id and Save creates a new
 * catalogue entry (C6.32, UI-3: an id read from the JSON could overwrite
 * another level). */
int od_ued_load(od_ui_editor *e, const char *path);
/* Save the document into the catalogue directory. `as_new` ignores the loaded
 * id and derives a fresh one from the name. A document without a name (or
 * still "untitled") is named after the local date and time of this first save,
 * "YYYY-MM-DD HH:MM", and keeps it (owner decision 2026-10-06, C6.32).
 * Returns OD_OK or an error. */
int od_ued_save(od_ui_editor *e, int as_new);

/* ---- palette / cell helpers (also used by the tests) -------------------- */

int od_ued_palette_count(void);
const od_ued_palette_entry *od_ued_palette(void);
const char *od_ued_cell_name(uint8_t cell);
/* Brushes: a palette index, or -1 and `*flame=1` for the flame object brush. */
int od_ued_brush_cell(const od_ui_editor *e, int *flame);

/* ---- input -------------------------------------------------------------- */

/* Feed a pointer sample. `buttons` is the held button bitmask, `click` is 1 on
 * the frame a button went down, `wheel` a signed notch count. Returns 1 when
 * the editor consumed the sample. */
int od_ued_pointer(od_ui_editor *e, int x, int y, int buttons, int click,
                    int wheel, int shift, int ctrl);
/* Logical keys handled by the editor (Ctrl+Z/Y, Ctrl+S, F5 test, Esc, P). */
enum {
    OD_UED_KEY_NONE = 0,
    OD_UED_KEY_UNDO,
    OD_UED_KEY_REDO,
    OD_UED_KEY_SAVE,
    OD_UED_KEY_TEST,
    OD_UED_KEY_BACK,
    OD_UED_KEY_TOGGLE_PALETTE,
    OD_UED_KEY_ACCEPT            /* Enter: confirms a pending question     */
};
enum { OD_UED_CONFIRM_NONE = 0, OD_UED_CONFIRM_NEW, OD_UED_CONFIRM_OPEN,
       OD_UED_CONFIRM_BACK };
/* Rectangles of the confirmation box and its two buttons (yes = discard). */
void od_ued_confirm_rect(int *x, int *y, int *w, int *h);
void od_ued_confirm_button_rect(int yes, int *x, int *y, int *w, int *h);
int od_ued_key(od_ui_editor *e, int key);

/* ---- drawing ------------------------------------------------------------ */

/* Rasterise the editor into `canvas` (cw x ch ARGB pixels). */
void od_ued_draw(od_ui_editor *e, uint32_t *canvas, int cw, int ch);

/* ---- validation --------------------------------------------------------- */

/* Fill `out` with the first validation message (error or warning) and return
 * the number of errors (like od_led_validate). */
int od_ued_validate(const od_ui_editor *e, char *out, size_t cap);

/* ---- geometry (shared with the tests) ----------------------------------- */

void od_ued_button_rect(int button, int *x, int *y, int *w, int *h);
void od_ued_palette_rect(int index, int *x, int *y, int *w, int *h);
/* Cell under a canvas point inside the level rectangle (every cell of the
 * 16x15 grid, borders included); returns 1 and sets cx/cy. */
int od_ued_cell_at(const od_ui_editor *e, int x, int y, int *cx, int *cy);
/* Canvas rectangle of grid cell (cx, cy). */
void od_ued_cell_rect(int cx, int cy, int *x, int *y, int *w, int *h);

#endif /* OD_UI_EDITOR_H */
