/* ui_import_view.h - the first-run screen at the window's native resolution
 * (C6.48): one centred column (icon, title, one sentence, a primary button, a
 * hint, and a status line only when there is something to say), drawn with
 * Inter. The layout is computed into a list of boxes that the drawing, the
 * pointer hit test and tests/first-run-layout.c all use, so they cannot
 * drift apart. SDL-free. When the fonts are missing od_ui_import_view_open()
 * returns NULL and the caller keeps the 8x8 canvas screen of ui_import.h. */
#ifndef OD_UI_IMPORT_VIEW_H
#define OD_UI_IMPORT_VIEW_H

#include "opendana/ui_import.h"

#include <stdint.h>

typedef struct od_ui_import_view od_ui_import_view;

typedef struct { int x, y, w, h; } od_ui_box;

enum {
    OD_IV_ICON,
    OD_IV_TEXT,     /* one centred line of text in `box`                      */
    OD_IV_BUTTON,   /* the primary button; `text` is its label                */
    OD_IV_FIELD,    /* the typed-path box; `text` is the visible tail          */
    OD_IV_CARET
};

typedef struct {
    int kind;
    od_ui_box box;
    char text[1100];
    int face;          /* od_ttf face                                          */
    int px;            /* em size in pixels                                    */
    uint32_t color;
    int left;          /* text starts at the box's left padding, not centred   */
} od_iv_item;

#define OD_IV_MAX_ITEMS 24

typedef struct {
    int w, h;
    int n;
    od_iv_item item[OD_IV_MAX_ITEMS];
    int button;        /* index of the button item                              */
} od_iv_layout;

/* Load Inter and the icon. NULL (with one stderr warning) when the fonts are
 * missing. `font_dir` / `icon_path` NULL = the packaged/dev locations. */
od_ui_import_view *od_ui_import_view_open(const char *font_dir, const char *icon_path);
void od_ui_import_view_close(od_ui_import_view *v);

/* Lay the screen out for a w x h pixel drawable. */
void od_ui_import_view_layout(od_ui_import_view *v, const od_ui_import *st,
                              int w, int h, od_iv_layout *out);

/* Width of `text` at the item's face and size (for the layout test). */
int od_ui_import_view_text_width(od_ui_import_view *v, const od_iv_item *it);

/* Draw into a w x h ARGB buffer (opaque result). */
void od_ui_import_view_draw(od_ui_import_view *v, const od_ui_import *st,
                            uint32_t *buf, int w, int h);

/* Pointer hit test of the button, in drawable pixels. */
int od_ui_import_view_button_hit(od_ui_import_view *v, const od_ui_import *st,
                                 int w, int h, int x, int y);

#endif /* OD_UI_IMPORT_VIEW_H */
