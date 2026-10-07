/* ui_catalog.h - custom-level catalogue browser (C5.5).
 *
 * Pure C11, SDL-free. Lists the catalogue directory of C5.4
 * (`src/ext/catalog.c`), lets the pointer pick an entry and exposes requests
 * (play, edit, duplicate, delete, import, export) that the host services with
 * the same `--play-level` path the game uses. */
#ifndef OD_UI_CATALOG_H
#define OD_UI_CATALOG_H

#include "opendana/ext/catalog.h"

#include <stddef.h>
#include <stdint.h>

/* C6.24: the catalogue draws into the editor's 384x360 canvas (ui_editor.h),
 * presented by the host the same way. */
#define OD_UCAT_W 384
#define OD_UCAT_H 360
#define OD_UCAT_MAX OD_CATALOG_MAX_ENTRIES
#define OD_UCAT_THUMB_SIZE 32
#define OD_UCAT_THUMB_SLOTS 7       /* visible list rows */

typedef struct {
    char file[256];
    int attempted, w, h;
    uint32_t pixels[OD_UCAT_THUMB_SIZE * OD_UCAT_THUMB_SIZE];
} od_ucat_thumbnail;

enum {
    OD_UCAT_BTN_PLAY = 0,
    OD_UCAT_BTN_EDIT,
    OD_UCAT_BTN_DUP,
    OD_UCAT_BTN_DELETE,
    OD_UCAT_BTN_IMPORT,
    OD_UCAT_BTN_EXPORT,
    OD_UCAT_BTN_SORT,
    OD_UCAT_BTN_BACK,
    OD_UCAT_BTN_COUNT
};

enum {
    OD_UCAT_REQ_NONE = 0,
    OD_UCAT_REQ_PLAY,
    OD_UCAT_REQ_EDIT,
    OD_UCAT_REQ_DUP,
    OD_UCAT_REQ_DELETE,
    OD_UCAT_REQ_IMPORT,
    OD_UCAT_REQ_EXPORT,
    OD_UCAT_REQ_BACK
};

typedef struct od_ui_catalog {
    int open;
    char dir[1024];
    char io_path[1200];          /* import source / export destination      */
    int editing_path;            /* the path field has keyboard focus       */
    int path_cursor;             /* append position in io_path              */

    od_catalog_entry entries[OD_UCAT_MAX];
    int n;
    int focus;
    int scroll;
    int order;                   /* od_catalog_order                       */

    int request;                 /* OD_UCAT_REQ_*                          */
    char request_id[OD_LED_ID_MAX];

    char status[160];
    int status_error;

    /* C6.32: where Back goes (the editor that opened it, else the menu), and
     * a Delete waiting for its confirmation. */
    int return_to_editor;
    int confirm_delete;
    char confirm_id[OD_LED_ID_MAX];

    /* Host-composed 256x240 preview of the focused level (optional). */
    const uint32_t *preview;
    /* Owned, bounded visible-row cache. Refresh/free invalidate failed loads too. */
    od_ucat_thumbnail thumbnails[OD_UCAT_THUMB_SLOTS];
} od_ui_catalog;

/* Default catalogue directory: $XDG_DATA_HOME/opendana/levels, else the
 * HOME fallback, else "./levels". */
void od_ucat_default_dir(char *out, size_t cap);

void od_ucat_init(od_ui_catalog *c, const char *dir);
void od_ucat_free(od_ui_catalog *c);

/* Re-scan the directory (regenerates catalog.json through the model). */
int od_ucat_refresh(od_ui_catalog *c);

void od_ucat_open(od_ui_catalog *c);

/* Pointer sample (canvas pixels, button bitmask, click flag, wheel). A click
 * selects a row; a click on the row already selected plays it (C6.32). */
int od_ucat_pointer(od_ui_catalog *c, int x, int y, int buttons, int click,
                     int wheel);
/* Logical keys: up/down select, Enter play, Delete delete, Esc back. */
enum {
    OD_UCAT_KEY_NONE = 0,
    OD_UCAT_KEY_UP,
    OD_UCAT_KEY_DOWN,
    OD_UCAT_KEY_PLAY,
    OD_UCAT_KEY_DELETE,
    OD_UCAT_KEY_BACK,
    OD_UCAT_KEY_CHAR,     /* a printable character for the path field */
    OD_UCAT_KEY_BACKSPACE
};
int od_ucat_key(od_ui_catalog *c, int key, int ch);
/* Typed text for the path field (UTF-8, from the system's text input, so
 * Shift, the keyboard layout and accents work). Ignored unless editing. */
void od_ucat_text(od_ui_catalog *c, const char *utf8);
/* Rectangles of the delete confirmation's Yes (1) and No (0) buttons. */
void od_ucat_confirm_button_rect(int yes, int *x, int *y, int *w, int *h);
/* Default import/export path: the "exports" folder next to the catalogue, so
 * an export never lands in the catalogue itself (C6.32, UI-5). */
void od_ucat_default_io_path(const char *dir, char *out, size_t cap);

void od_ucat_draw(od_ui_catalog *c, uint32_t *canvas, int cw, int ch);

void od_ucat_button_rect(int button, int *x, int *y, int *w, int *h);
void od_ucat_row_rect(int index, int *x, int *y, int *w, int *h);
/* Entry under a canvas point, or -1. */
int od_ucat_entry_at(const od_ui_catalog *c, int x, int y);

#endif /* OD_UI_CATALOG_H */
