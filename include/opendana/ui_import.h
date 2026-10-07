/* ui_import.h - first-run screen for the one-time ROM import (C5.8).
 *
 * When no content package is found the product does not need a terminal: it
 * shows this screen ("drag your ROM here, or press Enter to type its path"),
 * accepts an SDL_DROPFILE or a typed path, runs the same C importer as
 * `--import-rom` and reports the result. The drawing is pure and SDL-free so
 * it can be unit-tested and captured headlessly. */
#ifndef OD_UI_IMPORT_H
#define OD_UI_IMPORT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char content_dir[1024];   /* destination of the import (XDG)            */
    char manifest[1024];      /* optional reference manifest ("" = none)    */
    char path[1024];          /* ROM path, typed or dropped                 */
    int path_len;
    char status[160];         /* message line shown under the box           */
    int status_error;         /* status is an error (drawn in red)          */
    int busy;                 /* import running                             */
    int done;                 /* import succeeded: the host may start       */
    int quit;                 /* the user asked to leave                    */
    int hot;                  /* the pointer is over the "Open ROM…" button */
    int stale;                /* C6.33: the package there is from another version */
    /* C6.48 (native view, ui_import_view.h) */
    int pressed;              /* the button is held down                    */
    int typing;               /* the player started typing a path           */
    int no_chooser;           /* the system file chooser is not available   */
    int kbd;                  /* keyboard focus ring visible                */
} od_ui_import;

/* C6.48: 1 when the status line has nothing to say (the idle first-run or
 * stale text, or an empty message), so the modern view leaves it out. */
int od_ui_import_status_idle(const od_ui_import *st);

/* Initialise the screen for a destination package directory. */
void od_ui_import_init(od_ui_import *st, const char *content_dir);

/* C6.33: the package in `content_dir` exists but is not this version's import
 * (an older importer wrote it, or it was edited). The screen then explains
 * that and asks for the ROM again instead of the first-run text; the existing
 * files stay until a new import succeeds (od_import_rom swaps atomically). */
#define OD_UI_IMPORT_STALE 1
void od_ui_import_set_stale(od_ui_import *st, int stale);

/* Replace the ROM path (e.g. from an SDL_DROPFILE event) and import it. */
void od_ui_import_drop(od_ui_import *st, const char *path);

/* Set the ROM path without importing (used after the system file chooser). */
void od_ui_import_set_path(od_ui_import *st, const char *path);

/* Replace the status line (e.g. "no file chooser available"). */
void od_ui_import_set_status(od_ui_import *st, const char *msg, int is_error);

/* "Open ROM…" button: the pure hit test and the hover state, both in canvas
 * coordinates. The host maps the pointer through the displayed viewport and
 * calls od_ui_import_pointer() on motion and the hit test on a left click. */
int od_ui_import_button_hit(int x, int y);
void od_ui_import_pointer(od_ui_import *st, int x, int y);

/* Path typing: append UTF-8 bytes, erase the last byte, or clear. */
void od_ui_import_append(od_ui_import *st, const char *utf8);
void od_ui_import_backspace(od_ui_import *st);

/* Run the importer on `st->path` now, filling status/done. */
void od_ui_import_run(od_ui_import *st);

/* Rasterise the screen into a 256x240 ARGB canvas (opaque background). */
void od_ui_import_draw(const od_ui_import *st, uint32_t *canvas);

#endif /* OD_UI_IMPORT_H */
