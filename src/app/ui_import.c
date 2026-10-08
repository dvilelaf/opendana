/* ui_import.c - first-run screen for the ROM import (C5.8). See ui_import.h. */
#include "opendana/ui_import.h"

#include "opendana/import.h"
#include "opendana/ui_font.h"
#include "opendana/ui_strings.h"

#include <stdio.h>
#include <string.h>

#define CANVAS_W 256
#define CANVAS_H 240
#define ARGB(a, r, g, b) \
    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static const uint32_t C_BG = 0xFF101820u;      /* dark slate          */
static const uint32_t C_PANEL = 0xFF1C2838u;
static const uint32_t C_TEXT = 0xFFE8EEF4u;
static const uint32_t C_DIM = 0xFF9AA7B4u;
static const uint32_t C_ACCENT = 0xFF6FC3FFu;
static const uint32_t C_ERROR = 0xFFFF6B6Bu;
static const uint32_t C_OK = 0xFF7DE08Au;


void od_ui_import_init(od_ui_import *st, const char *content_dir)
{
    if (st == NULL)
        return;
    memset(st, 0, sizeof(*st));
    if (content_dir != NULL)
        snprintf(st->content_dir, sizeof(st->content_dir), "%s", content_dir);
    snprintf(st->status, sizeof(st->status), "%s",
             od_str(OD_STR_FIRST_RUN_NO_CONTENT));
}

void od_ui_import_set_stale(od_ui_import *st, int stale)
{
    if (st == NULL)
        return;
    st->stale = stale ? 1 : 0;
    snprintf(st->status, sizeof(st->status), "%s",
             st->stale ? od_str(OD_STR_FR_STALE_STATUS) : od_str(OD_STR_FIRST_RUN_NO_CONTENT));
    st->status_error = 0;
}

void od_ui_import_append(od_ui_import *st, const char *utf8)
{
    if (st == NULL || utf8 == NULL)
        return;
    size_t n = strlen(utf8);
    if (st->path_len + (int)n >= (int)sizeof(st->path) - 1) {
        snprintf(st->status, sizeof(st->status), "%s",
                 od_str(OD_STR_FIRST_RUN_TOO_LONG));
        st->status_error = 1;
        return;
    }
    st->typing = 1;
    memcpy(st->path + st->path_len, utf8, n);
    st->path_len += (int)n;
    st->path[st->path_len] = '\0';
}

int od_ui_import_status_idle(const od_ui_import *st)
{
    if (st == NULL || st->status[0] == '\0')
        return 1;
    return strcmp(st->status, od_str(OD_STR_FIRST_RUN_NO_CONTENT)) == 0 ||
           strcmp(st->status, od_str(OD_STR_FR_STALE_STATUS)) == 0;
}

void od_ui_import_backspace(od_ui_import *st)
{
    if (st == NULL || st->path_len == 0)
        return;
    st->path_len--;
    st->path[st->path_len] = '\0';
}

/* Map the importer's status and message to the human line (C6.48). */
static od_string import_error_id(od_status rc, const char *err)
{
    static const struct { const char *prefix; od_string id; } MAP[] = {
        {"wrong file size", OD_STR_FR_ERR_NOT_ROM},
        {"not an iNES", OD_STR_FR_ERR_NOT_ROM},
        {"truncated iNES", OD_STR_FR_ERR_NOT_ROM},
        {"the chosen path is a folder", OD_STR_FR_ERR_NOT_ROM},
        {"not the EU ROM", OD_STR_FR_ERR_OTHER_VERSION},
        {"cannot open the ROM", OD_STR_FR_ERR_READ},
        {"cannot read the ROM", OD_STR_FR_ERR_READ},
        {"the ROM file changed", OD_STR_FR_ERR_READ},
        {"cannot create", OD_STR_FR_ERR_SAVE},
        {"cannot write", OD_STR_FR_ERR_SAVE},
        {"cannot flush", OD_STR_FR_ERR_SAVE},
        {"cannot move", OD_STR_FR_ERR_SAVE},
        {"cannot replace", OD_STR_FR_ERR_SAVE},
    };
    for (size_t i = 0; err != NULL && i < sizeof MAP / sizeof MAP[0]; i++)
        if (strncmp(err, MAP[i].prefix, strlen(MAP[i].prefix)) == 0)
            return MAP[i].id;
    (void)rc;
    return OD_STR_FR_ERR_GENERIC;
}

void od_ui_import_run(od_ui_import *st)
{
    if (st == NULL)
        return;
    if (st->path_len == 0) {
        snprintf(st->status, sizeof(st->status), "%s",
                 od_str(OD_STR_FIRST_RUN_CHOOSE));
        st->status_error = 1;
        return;
    }
    st->busy = 1;
    char err[256] = {0};
    od_status rc = od_import_rom(st->path, st->content_dir,
                                   st->manifest[0] ? st->manifest : NULL,
                                   err, sizeof(err));
    st->busy = 0;
    if (rc != OD_OK) {
        /* C6.48: the screen says it in one human line; the importer's own
         * wording (sizes, hashes, paths) goes to stderr only. */
        fprintf(stderr, "import: %s\n", err[0] ? err : od_status_str(rc));
        snprintf(st->status, sizeof(st->status), "%s", od_str(import_error_id(rc, err)));
        st->status_error = 1;
        return;
    }
    snprintf(st->status, sizeof(st->status), "%s",
             od_str(OD_STR_FIRST_RUN_DONE));
    st->status_error = 0;
    st->done = 1;
}

void od_ui_import_drop(od_ui_import *st, const char *path)
{
    if (st == NULL || path == NULL || path[0] == '\0')
        return;
    snprintf(st->path, sizeof(st->path), "%s", path);
    st->path_len = (int)strlen(st->path);
    od_ui_import_run(st);
}

void od_ui_import_set_path(od_ui_import *st, const char *path)
{
    if (st == NULL || path == NULL)
        return;
    snprintf(st->path, sizeof(st->path), "%s", path);
    st->path_len = (int)strlen(st->path);
}

void od_ui_import_set_status(od_ui_import *st, const char *msg, int is_error)
{
    if (st == NULL)
        return;
    snprintf(st->status, sizeof(st->status), "%s", msg != NULL ? msg : "");
    st->status_error = is_error ? 1 : 0;
}

/* "Open ROM…" button geometry in canvas coordinates (C6.21). Kept in one
 * place so the pure hit test and the drawing cannot drift apart. */
#define BTN_X 12
#define BTN_Y 148
#define BTN_W 150
#define BTN_H 24

int od_ui_import_button_hit(int x, int y)
{
    return x >= BTN_X && x < BTN_X + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H;
}

void od_ui_import_pointer(od_ui_import *st, int x, int y)
{
    if (st != NULL)
        st->hot = od_ui_import_button_hit(x, y);
}

/* Text cut to `w` pixels with "..." (C6.32: nothing runs off the screen). */
static void text_in(uint32_t *canvas, int x, int y, int w, const char *s,
                    uint32_t argb, int scale)
{
    char buf[200];
    od_ui_font_text(canvas, CANVAS_W, CANVAS_H, x, y,
                     od_ui_font_fit(s, w, scale, buf, sizeof buf), argb, scale);
}

void od_ui_import_draw(const od_ui_import *st, uint32_t *canvas)
{
    if (canvas == NULL)
        return;
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++)
        canvas[i] = C_BG;

    const int w = CANVAS_W - 24;
    /* The title at 2x needs 240 px: start at 8 so it is never cut. */
    text_in(canvas, 8, 20, CANVAS_W - 16, od_str(OD_STR_FIRST_RUN_TITLE),
            C_ACCENT, 2);
    int y = 72;
    if (st->stale) {
        /* C6.33: the package is from another version. Say so and ask for the
         * ROM again; the drop/click hints stay, 4 px higher. */
        text_in(canvas, 16, 44, CANVAS_W - 32, od_str(OD_STR_FR_STALE_HEADLINE), C_ERROR, 1);
        char body[3][64];
        int n = od_ui_font_wrap(od_str(OD_STR_FR_STALE_BODY), w, 1, &body[0][0], sizeof body[0], 3);
        for (int i = 0; i < n; i++)
            od_ui_font_text(canvas, CANVAS_W, CANVAS_H, 12, 58 + i * 10, body[i],
                             C_TEXT, 1);
        y = 96;
    } else {
        text_in(canvas, 16, 44, CANVAS_W - 32, od_str(OD_STR_FIRST_RUN_SUBTITLE),
                C_TEXT, 1);
        text_in(canvas, 12, y, w, od_str(OD_STR_FIRST_RUN_NEED), C_TEXT, 1);
        y += 16;
    }
    text_in(canvas, 12, y, w, od_str(OD_STR_FIRST_RUN_CLICK), C_DIM, 1);
    y += 12;
    text_in(canvas, 12, y, w, od_str(OD_STR_FIRST_RUN_DROP), C_DIM, 1);

    /* Path box. */
    int by = 122;
    for (int yy = by; yy < by + 20; yy++)
        for (int xx = 12; xx < CANVAS_W - 12; xx++)
            canvas[yy * CANVAS_W + xx] = C_PANEL;
    const char *shown = st->path_len ? st->path : od_str(OD_STR_FIRST_RUN_PATH);
    uint32_t col = st->path_len ? C_TEXT : C_DIM;
    /* Show the tail when the path is long ("..." and its last characters). */
    char tail[200];
    int max_chars = (CANVAS_W - 32) / 8;
    int len = (int)strlen(shown);
    if (len > max_chars)
        snprintf(tail, sizeof tail, "...%s", shown + (len - (max_chars - 3)));
    else {
        size_t n = (size_t)len < sizeof tail - 1 ? (size_t)len : sizeof tail - 1;
        memcpy(tail, shown, n);
        tail[n] = '\0';
    }
    text_in(canvas, 16, by + 6, CANVAS_W - 32, tail, col, 1);

    /* Status: two lines, cut with "..." beyond that. */
    uint32_t scol = st->status_error ? C_ERROR : (st->done ? C_OK : C_DIM);
    char lines[2][64];
    int n = od_ui_font_wrap(st->status, w, 1, &lines[0][0], sizeof lines[0], 2);
    for (int i = 0; i < n; i++)
        od_ui_font_text(canvas, CANVAS_W, CANVAS_H, 12, 177 + i * 10, lines[i],
                         scol, 1);

    /* C6.21: the "Open ROM..." button (its rectangle is the pure hit test). */
    uint32_t bg = st->hot ? 0xFF2A456Bu : C_PANEL;
    for (int yy = BTN_Y; yy < BTN_Y + BTN_H; yy++) {
        for (int xx = BTN_X; xx < BTN_X + BTN_W; xx++) {
            int edge = xx == BTN_X || xx == BTN_X + BTN_W - 1 ||
                       yy == BTN_Y || yy == BTN_Y + BTN_H - 1;
            canvas[yy * CANVAS_W + xx] = edge ? C_ACCENT : bg;
        }
    }
    char label[32];
    od_ui_font_fit(od_str(OD_STR_FIRST_RUN_OPEN), BTN_W - 4, 1, label,
                    sizeof label);
    int lx = BTN_X + (BTN_W - od_ui_font_width(label, 1)) / 2;
    od_ui_font_text(canvas, CANVAS_W, CANVAS_H, lx, BTN_Y + 8, label,
                     st->hot ? C_ACCENT : C_TEXT, 1);

    text_in(canvas, 12, 200, w, od_str(OD_STR_FIRST_RUN_ENTER), C_DIM, 1);
    text_in(canvas, 12, 212, w, od_str(OD_STR_FIRST_RUN_KEYS), C_DIM, 1);
}
