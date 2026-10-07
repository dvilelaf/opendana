/* leveled.c - model of the modern level editor (C5.4, C11 pure).
 *
 * See include/opendana/ext/leveled.h for the
 * contract. No SDL, no cJSON: the document is a simple, fixed-shape JSON, so
 * this file carries a small hand-written parser and writer, which keeps the
 * module self-contained (it compiles and is tested on its own).
 */
#include "opendana/ext/leveled.h"
#include "opendana/userdir.h"   /* UTF-8 file access on Windows (C6.36) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================= */
/* Small text helpers                                                        */
/* ========================================================================= */

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0)
        return;
    if (src == NULL)
        src = "";
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int cell_passable(uint8_t c)
{
    return c == 0x00 || c == 0x0E;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ========================================================================= */
/* Undo / redo                                                               */
/* ========================================================================= */

static size_t led_command_size(const od_led_doc *d)
{
    size_t n = 1; /* terminator */
    for (int i = 0; i < d->n_objects; i++) {
        if (d->objects[i].kind == OD_LED_SPECIAL)
            n += 2;
        else
            n += 2 + (size_t)d->objects[i].count;
    }
    return n;
}

static int led_pack_commands(const od_led_doc *d, uint8_t *buf, size_t cap,
                             uint16_t *out_len)
{
    size_t n = led_command_size(d);
    if (n > cap || n > OD_LED_CMD_MAX)
        return 0;
    size_t i = 0;
    for (int k = 0; k < d->n_objects; k++) {
        const od_led_object *o = &d->objects[k];
        if (o->kind == OD_LED_SPECIAL) {
            buf[i++] = o->type;
            buf[i++] = o->argument;
        } else {
            buf[i++] = o->type;
            buf[i++] = (uint8_t)o->count;
            for (int j = 0; j < o->count; j++)
                buf[i++] = o->cells[j];
        }
    }
    buf[i++] = 0x00;
    if (out_len != NULL)
        *out_len = (uint16_t)i;
    return 1;
}

/* Rebuild the object list from a package command stream. */
static int led_unpack_commands(od_led_doc *d, const uint8_t *buf, size_t len)
{
    d->n_objects = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t b = buf[i++];
        if (b == 0x00)
            return 1;
        if (d->n_objects >= OD_LED_MAX_OBJECTS)
            return 0;
        od_led_object *o = &d->objects[d->n_objects];
        memset(o, 0, sizeof *o);
        if (b >= 0x80) {
            if (i >= len)
                return 0;
            o->kind = OD_LED_SPECIAL;
            o->type = b;
            o->argument = buf[i++];
        } else {
            if (i >= len)
                return 0;
            int n = buf[i++];
            if (n < 1 || i + (size_t)n > len)
                return 0;
            o->kind = OD_LED_RUN;
            o->type = b;
            o->count = n;
            memcpy(o->cells, buf + i, (size_t)n);
            i += (size_t)n;
        }
        d->n_objects++;
    }
    return 0; /* no terminator */
}

static void led_snapshot(const od_led_doc *d, od_led_snapshot *s)
{
    memcpy(s->grid, d->grid, sizeof s->grid);
    s->player = d->player;
    s->metadata = d->metadata;
    (void)led_pack_commands(d, s->commands, sizeof s->commands, &s->command_len);
}

static void led_restore(od_led_doc *d, const od_led_snapshot *s)
{
    memcpy(d->grid, s->grid, sizeof d->grid);
    d->player = s->player;
    d->metadata = s->metadata;
    (void)led_unpack_commands(d, s->commands, s->command_len);
}

static void led_push_stack(od_led_snapshot **arr, int *n, const od_led_doc *d)
{
    if (*arr == NULL) {
        *arr = (od_led_snapshot *)calloc(OD_LED_HISTORY, sizeof **arr);
        if (*arr == NULL)
            return;
        *n = 0;
    }
    if (*n == OD_LED_HISTORY) {
        memmove(&(*arr)[0], &(*arr)[1],
                (OD_LED_HISTORY - 1) * sizeof **arr);
        (*n)--;
    }
    led_snapshot(d, &(*arr)[(*n)++]);
}

static void led_clear_redo(od_led_doc *d)
{
    free(d->redo);
    d->redo = NULL;
    d->n_redo = 0;
}

static void led_push_undo(od_led_doc *d)
{
    led_push_stack(&d->undo, &d->n_undo, d);
    led_clear_redo(d);
}

/* ========================================================================= */
/* Document                                                                  */
/* ========================================================================= */

void od_led_free(od_led_doc *d)
{
    if (d == NULL)
        return;
    free(d->undo);
    free(d->redo);
    d->undo = NULL;
    d->redo = NULL;
    d->n_undo = 0;
    d->n_redo = 0;
}

void od_led_new(od_led_doc *d, int theme_world)
{
    if (d == NULL)
        return;
    od_led_free(d);
    memset(d, 0, sizeof *d);

    for (int y = 0; y < OD_GRID_H; y++)
        for (int x = 0; x < OD_GRID_W; x++)
            d->grid[y][x] = (y == 0 || y == OD_GRID_H - 1 ||
                             x == 0 || x == OD_GRID_W - 1) ? 0x01 : 0x00;

    d->player = OD_LED_NO_PLAYER;
    d->theme_world = clamp_int(theme_world, 1, 15);
    d->editor_version = OD_LED_EDITOR_VERSION;
    d->index = 0;
    d->number = 1;
    d->world = d->theme_world;
    d->round = 1;
    copy_str(d->name, sizeof d->name, "untitled");
    d->created = (int64_t)time(NULL);
    d->modified = d->created;
}

/* ========================================================================= */
/* Edits                                                                     */
/* ========================================================================= */

void od_led_paint(od_led_doc *d, int x, int y, uint8_t cell)
{
    if (d == NULL || x < 0 || x >= OD_GRID_W || y < 0 || y >= OD_GRID_H)
        return;
    if (d->grid[y][x] == cell)
        return;
    led_push_undo(d);
    d->grid[y][x] = cell;
    d->dirty = 1;
    d->modified = (int64_t)time(NULL);
}

void od_led_fill(od_led_doc *d, int x0, int y0, int x1, int y1, uint8_t cell)
{
    if (d == NULL)
        return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= OD_GRID_W) x1 = OD_GRID_W - 1;
    if (y1 >= OD_GRID_H) y1 = OD_GRID_H - 1;
    int changed = 0;
    for (int y = y0; y <= y1 && !changed; y++)
        for (int x = x0; x <= x1; x++)
            if (d->grid[y][x] != cell) { changed = 1; break; }
    if (!changed)
        return;
    led_push_undo(d);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            d->grid[y][x] = cell;
    d->dirty = 1;
    d->modified = (int64_t)time(NULL);
}

void od_led_place_hero(od_led_doc *d, int x, int y)
{
    if (d == NULL || x < 0 || x >= OD_GRID_W || y < 0 || y >= OD_GRID_H)
        return;
    uint8_t cell = (uint8_t)(y * OD_GRID_W + x);
    if (d->player == cell && cell_passable(d->grid[y][x]))
        return;
    led_push_undo(d);
    d->player = cell;
    d->dirty = 1;
    d->modified = (int64_t)time(NULL);
}

static int led_object_size(const od_led_object *o)
{
    if (o->kind == OD_LED_SPECIAL)
        return 2;
    if (o->kind == OD_LED_RUN)
        return 2 + o->count;
    return -1;
}

int od_led_add_object(od_led_doc *d, const od_led_object *o)
{
    if (d == NULL || o == NULL)
        return 0;
    if (d->n_objects >= OD_LED_MAX_OBJECTS)
        return 0;
    int osize = led_object_size(o);
    if (osize < 0)
        return 0;
    if (o->kind == OD_LED_RUN) {
        if (o->type < 0x01 || o->type > 0x7F || o->count < 1 ||
            o->count > OD_LED_MAX_POSITIONS)
            return 0;
        for (int i = 0; i < o->count; i++)
            if (o->cells[i] >= OD_GRID_CELLS)
                return 0;
    } else {
        if (o->type < 0x80 || o->type > 0x9F)
            return 0;
    }
    if (led_command_size(d) + (size_t)osize > OD_LED_CMD_MAX)
        return 0;
    led_push_undo(d);
    d->objects[d->n_objects++] = *o;
    d->dirty = 1;
    d->modified = (int64_t)time(NULL);
    return 1;
}

int od_led_remove_object(od_led_doc *d, int index)
{
    if (d == NULL || index < 0 || index >= d->n_objects)
        return 0;
    led_push_undo(d);
    for (int i = index; i + 1 < d->n_objects; i++)
        d->objects[i] = d->objects[i + 1];
    d->n_objects--;
    d->dirty = 1;
    d->modified = (int64_t)time(NULL);
    return 1;
}

int od_led_object_at(const od_led_doc *d, int x, int y, uint8_t type)
{
    if (d == NULL || x < 0 || x >= OD_GRID_W || y < 0 || y >= OD_GRID_H)
        return -1;
    uint8_t cell = (uint8_t)(y * OD_GRID_W + x);
    for (int i = 0; i < d->n_objects; i++) {
        const od_led_object *o = &d->objects[i];
        if (o->kind != OD_LED_RUN || o->type != type)
            continue;
        for (int k = 0; k < o->count; k++)
            if (o->cells[k] == cell)
                return i;
    }
    return -1;
}

/* Drop every listed cell of the rectangle whose grid type no longer matches
 * its run, and runs left empty. Part of the edit that just changed the grid:
 * the undo snapshot that edit pushed already holds the objects. */
static void led_prune_runs(od_led_doc *d, int x0, int y0, int x1, int y1)
{
    for (int i = 0; i < d->n_objects;) {
        od_led_object *o = &d->objects[i];
        if (o->kind == OD_LED_RUN) {
            int w = 0;
            for (int k = 0; k < o->count; k++) {
                int cx = o->cells[k] & 0x0F, cy = o->cells[k] >> 4;
                int inside = cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1;
                if (!inside || cy >= OD_GRID_H || d->grid[cy][cx] == o->type)
                    o->cells[w++] = o->cells[k];
            }
            if (w != o->count) {
                o->count = w;
                d->dirty = 1;
            }
            if (w == 0) {
                for (int j = i; j + 1 < d->n_objects; j++)
                    d->objects[j] = d->objects[j + 1];
                d->n_objects--;
                continue;
            }
        }
        i++;
    }
}

void od_led_paint_clear(od_led_doc *d, int x, int y, uint8_t cell)
{
    if (d == NULL || x < 0 || x >= OD_GRID_W || y < 0 || y >= OD_GRID_H)
        return;
    od_led_paint(d, x, y, cell);
    led_prune_runs(d, x, y, x, y);
}

void od_led_fill_clear(od_led_doc *d, int x0, int y0, int x1, int y1,
                        uint8_t cell)
{
    if (d == NULL)
        return;
    od_led_fill(d, x0, y0, x1, y1, cell);
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    led_prune_runs(d, x0, y0, x1, y1);
}

int od_led_can_undo(const od_led_doc *d)
{
    return d != NULL && d->n_undo > 0;
}

int od_led_can_redo(const od_led_doc *d)
{
    return d != NULL && d->n_redo > 0;
}

void od_led_undo(od_led_doc *d)
{
    if (d == NULL || d->n_undo <= 0)
        return;
    led_push_stack(&d->redo, &d->n_redo, d);
    led_restore(d, &d->undo[d->n_undo - 1]);
    d->n_undo--;
    d->dirty = 1;
    d->modified = (int64_t)time(NULL);
}

void od_led_redo(od_led_doc *d)
{
    if (d == NULL || d->n_redo <= 0)
        return;
    led_push_stack(&d->undo, &d->n_undo, d);
    led_restore(d, &d->redo[d->n_redo - 1]);
    d->n_redo--;
    d->dirty = 1;
    d->modified = (int64_t)time(NULL);
}

/* ========================================================================= */
/* Validation                                                                */
/* ========================================================================= */

int od_led_has_hero(const od_led_doc *d)
{
    return d != NULL && d->player != OD_LED_NO_PLAYER;
}

static void issue_add(od_led_issue *out, int max, int *n, int sev,
                      int x, int y, const char *msg, int code, int value)
{
    if (out == NULL || *n >= max)
        return;
    od_led_issue *it = &out[*n];
    it->severity = sev;
    it->x = x;
    it->y = y;
    copy_str(it->message, sizeof it->message, msg);
    it->code = code;
    it->value = value;
    (*n)++;
}

int od_led_validate(const od_led_doc *d, od_led_issue *out, int max)
{
    if (d == NULL)
        return 1;
    int n = 0;
    int errors = 0;

    /* Invalid cell types (a "malformed grid" in the package format). */
    for (int y = 0; y < OD_GRID_H; y++) {
        for (int x = 0; x < OD_GRID_W; x++) {
            if (d->grid[y][x] > 0x0F) {
                char msg[64];
                snprintf(msg, sizeof msg, "Tipo de celda invalido 0x%02X",
                         (unsigned)d->grid[y][x]);
                if (n < max) issue_add(out, max, &n, OD_LED_ERROR, x, y, msg,
                                     OD_LED_ISSUE_INVALID_CELL, d->grid[y][x]);
                errors++;
            }
        }
    }

    if (!od_led_has_hero(d)) {
        if (n < max) issue_add(out, max, &n, OD_LED_ERROR, -1, -1, "Falta el jugador",
                                 OD_LED_ISSUE_MISSING_HERO, 0);
        errors++;
    } else {
        int px = d->player & 0x0F;
        int py = (d->player >> 4) & 0x0F;
        if (py < OD_GRID_H && !cell_passable(d->grid[py][px])) {
            if (n < max)
                issue_add(out, max, &n, OD_LED_ERROR, px, py,
                          "El jugador empieza dentro de algo solido",
                          OD_LED_ISSUE_HERO_SOLID, 0);
            errors++;
        }
    }

    if (d->theme_world < 1 || d->theme_world > 15) {
        if (n < max)
            issue_add(out, max, &n, OD_LED_ERROR, -1, -1,
                      "Mundo de tema fuera de 1..15",
                      OD_LED_ISSUE_INVALID_WORLD, d->theme_world);
        errors++;
    }

    /* Objects, in package order. */
    for (int i = 0; i < d->n_objects; i++) {
        const od_led_object *o = &d->objects[i];
        if (o->kind == OD_LED_SPECIAL) {
            if (o->type < 0x80 || o->type > 0x9F) {
                if (n < max)
                    issue_add(out, max, &n, OD_LED_ERROR, -1, -1,
                              "Objeto especial invalido",
                              OD_LED_ISSUE_INVALID_SPECIAL, o->type);
                errors++;
            }
            continue;
        }
        if (o->kind != OD_LED_RUN || o->type < 0x01 || o->type > 0x7F ||
            o->count < 1 || o->count > OD_LED_MAX_POSITIONS) {
            if (n < max)
                issue_add(out, max, &n, OD_LED_ERROR, -1, -1,
                          "Objeto (run) invalido",
                          OD_LED_ISSUE_INVALID_RUN, o->type);
            errors++;
            continue;
        }
        for (int k = 0; k < o->count; k++) {
            uint8_t cell = o->cells[k];
            if (cell >= OD_GRID_CELLS) {
                if (n < max)
                    issue_add(out, max, &n, OD_LED_ERROR, -1, -1,
                              "Posicion de objeto fuera de la rejilla",
                              OD_LED_ISSUE_OBJECT_POSITION, cell);
                errors++;
                continue;
            }
            int x = cell & 0x0F;
            int y = (cell >> 4) & 0x0F;
            if (d->grid[y][x] != o->type) {
                if (n < max)
                    issue_add(out, max, &n, OD_LED_ERROR, x, y,
                              "El objeto no coincide con la rejilla",
                              OD_LED_ISSUE_OBJECT_GRID, o->type);
                errors++;
            }
        }
    }

    /* No fire: not an error here (the level resolves at start), but the
     * content validator rejects it, so the editor shows an advisory. */
    {
        int fire = 0;
        for (int y = 0; y < OD_GRID_H && !fire; y++)
            for (int x = 0; x < OD_GRID_W; x++)
                if (d->grid[y][x] == 0x0E) { fire = 1; break; }
        for (int i = 0; i < d->n_objects && !fire; i++) {
            const od_led_object *o = &d->objects[i];
            if (o->kind == OD_LED_SPECIAL && o->type >= 0x81 && o->type <= 0x83)
                fire = 1;
        }
        if (!fire && n < max)
            issue_add(out, max, &n, OD_LED_WARNING, -1, -1,
                      "Sin fuegos: el nivel se resuelve al empezar",
                      OD_LED_ISSUE_NO_FIRES, 0);
    }

    return errors;
}

/* ========================================================================= */
/* Package view                                                              */
/* ========================================================================= */

od_status od_led_to_level(const od_led_doc *d, od_level *out,
                            uint8_t *cmd, size_t cap)
{
    if (d == NULL || out == NULL || cmd == NULL)
        return OD_ERR_RANGE;
    memset(out, 0, sizeof *out);
    memcpy(out->grid, d->grid, sizeof out->grid);
    out->player_position = d->player;
    out->metadata = d->metadata;
    out->commands = cmd;
    uint16_t len = 0;
    if (!led_pack_commands(d, cmd, cap, &len))
        return OD_ERR_RANGE;
    out->command_len = len;
    return OD_OK;
}

/* ========================================================================= */
/* Minimal JSON reader (only the value forms the format uses)                */
/* ========================================================================= */

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } jv_kind;

typedef struct jv jv;
struct jv {
    jv_kind kind;
    double num;
    int boolean;
    char *str;
    jv **items;
    int n;
    char **keys;
    jv **vals;
    int nk;
};

typedef struct {
    const char *p;
    const char *end;
    int err;
    int depth;
} jparse;

static void jv_free(jv *v)
{
    if (v == NULL)
        return;
    free(v->str);
    for (int i = 0; i < v->n; i++)
        jv_free(v->items[i]);
    free(v->items);
    for (int i = 0; i < v->nk; i++) {
        free(v->keys[i]);
        jv_free(v->vals[i]);
    }
    free(v->keys);
    free(v->vals);
    free(v);
}

static void jskip(jparse *j)
{
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            j->p++;
        else
            break;
    }
}

static int jhex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *jparse_string_raw(jparse *j)
{
    if (j->p >= j->end || *j->p != '"') {
        j->err = 1;
        return NULL;
    }
    j->p++;
    const char *start = j->p;
    size_t len = 0;
    int esc = 0;
    while (j->p < j->end) {
        char c = *j->p;
        if (esc) {
            if (c == 'u') {
                if (j->p + 4 >= j->end) { j->err = 1; return NULL; }
                j->p += 5;
                len += 4;
            } else {
                j->p++;
                len++;
            }
            esc = 0;
            continue;
        }
        if (c == '\\') { esc = 1; j->p++; continue; }
        if (c == '"') break;
        j->p++;
        len++;
    }
    if (j->p >= j->end) { j->err = 1; return NULL; }

    char *out = (char *)malloc(len + 1);
    if (out == NULL) { j->err = 1; return NULL; }
    size_t o = 0;
    const char *q = start;
    esc = 0;
    while (q < j->p) {
        char c = *q;
        if (esc) {
            switch (c) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': out[o++] = '\r'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case '/': out[o++] = '/'; break;
            case '\\': out[o++] = '\\'; break;
            case '"': out[o++] = '"'; break;
            case 'u': {
                int h0 = jhex(q[1]), h1 = jhex(q[2]);
                int h2 = jhex(q[3]), h3 = jhex(q[4]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) { free(out); j->err = 1; return NULL; }
                unsigned cp = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                /* BMP only; valid UTF-8 encoding. */
                if (cp < 0x80) {
                    out[o++] = (char)cp;
                } else if (cp < 0x800) {
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
                q += 4;
                break;
            }
            default: out[o++] = c; break;
            }
            esc = 0;
            q++;
            continue;
        }
        if (c == '\\') { esc = 1; q++; continue; }
        out[o++] = c;
        q++;
    }
    out[o] = '\0';
    j->p = q + 1; /* past the closing quote */
    return out;
}

static jv *jparse_value(jparse *j);

static jv *jv_new(jv_kind k)
{
    jv *v = (jv *)calloc(1, sizeof *v);
    if (v != NULL)
        v->kind = k;
    return v;
}

static jv *jparse_object(jparse *j)
{
    jv *v = jv_new(JV_OBJ);
    if (v == NULL) { j->err = 1; return NULL; }
    j->p++; /* '{' */
    jskip(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return v; }
    for (;;) {
        jskip(j);
        char *key = jparse_string_raw(j);
        if (key == NULL) { jv_free(v); return NULL; }
        jskip(j);
        if (j->p >= j->end || *j->p != ':') { free(key); jv_free(v); j->err = 1; return NULL; }
        j->p++;
        jv *val = jparse_value(j);
        if (val == NULL) { free(key); jv_free(v); return NULL; }
        char **nk = (char **)realloc(v->keys, (size_t)(v->nk + 1) * sizeof(char *));
        if (nk == NULL) {
            free(key);
            jv_free(val);
            jv_free(v);
            j->err = 1;
            return NULL;
        }
        v->keys = nk;
        jv **nv = (jv **)realloc(v->vals, (size_t)(v->nk + 1) * sizeof(jv *));
        if (nv == NULL) {
            free(key);
            jv_free(val);
            jv_free(v);
            j->err = 1;
            return NULL;
        }
        v->vals = nv;
        v->keys[v->nk] = key;
        v->vals[v->nk] = val;
        v->nk++;
        jskip(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == '}') { j->p++; break; }
        j->err = 1;
        jv_free(v);
        return NULL;
    }
    return v;
}

static jv *jparse_array(jparse *j)
{
    jv *v = jv_new(JV_ARR);
    if (v == NULL) { j->err = 1; return NULL; }
    j->p++; /* '[' */
    jskip(j);
    if (j->p < j->end && *j->p == ']') { j->p++; return v; }
    for (;;) {
        jv *item = jparse_value(j);
        if (item == NULL) { jv_free(v); return NULL; }
        jv **ni = (jv **)realloc(v->items, (size_t)(v->n + 1) * sizeof(jv *));
        if (ni == NULL) { jv_free(item); jv_free(v); j->err = 1; return NULL; }
        v->items = ni;
        v->items[v->n++] = item;
        jskip(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == ']') { j->p++; break; }
        j->err = 1;
        jv_free(v);
        return NULL;
    }
    return v;
}

static jv *jparse_value(jparse *j)
{
    if (j->depth > 64) { j->err = 1; return NULL; }
    jskip(j);
    if (j->p >= j->end) { j->err = 1; return NULL; }
    char c = *j->p;
    if (c == '{') {
        j->depth++;
        jv *v = jparse_object(j);
        j->depth--;
        return v;
    }
    if (c == '[') {
        j->depth++;
        jv *v = jparse_array(j);
        j->depth--;
        return v;
    }
    if (c == '"') {
        jv *v = jv_new(JV_STR);
        if (v == NULL) { j->err = 1; return NULL; }
        v->str = jparse_string_raw(j);
        if (v->str == NULL) { jv_free(v); return NULL; }
        return v;
    }
    if (j->end - j->p >= 4 && strncmp(j->p, "true", 4) == 0) {
        jv *v = jv_new(JV_BOOL);
        if (v == NULL) { j->err = 1; return NULL; }
        v->boolean = 1;
        j->p += 4;
        return v;
    }
    if (j->end - j->p >= 5 && strncmp(j->p, "false", 5) == 0) {
        jv *v = jv_new(JV_BOOL);
        if (v == NULL) { j->err = 1; return NULL; }
        v->boolean = 0;
        j->p += 5;
        return v;
    }
    if (j->end - j->p >= 4 && strncmp(j->p, "null", 4) == 0) {
        jv *v = jv_new(JV_NULL);
        if (v == NULL) { j->err = 1; return NULL; }
        j->p += 4;
        return v;
    }
    {
        char *endp = NULL;
        double d = strtod(j->p, &endp);
        if (endp == j->p || endp > j->end) { j->err = 1; return NULL; }
        jv *v = jv_new(JV_NUM);
        if (v == NULL) { j->err = 1; return NULL; }
        v->num = d;
        j->p = endp;
        return v;
    }
}

static jv *jv_get(const jv *obj, const char *key)
{
    if (obj == NULL || obj->kind != JV_OBJ)
        return NULL;
    for (int i = 0; i < obj->nk; i++)
        if (strcmp(obj->keys[i], key) == 0)
            return obj->vals[i];
    return NULL;
}

static int jv_int(const jv *v, long *out)
{
    if (v == NULL || v->kind != JV_NUM)
        return 0;
    *out = (long)v->num;
    return 1;
}

/* ========================================================================= */
/* JSON writer and save                                                      */
/* ========================================================================= */

static void write_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        case '\b': fputs("\\b", f); break;
        case '\f': fputs("\\f", f); break;
        default:
            if (*p < 0x20)
                fprintf(f, "\\u%04x", (unsigned)*p);
            else
                fputc(*p, f);
            break;
        }
    }
    fputc('"', f);
}

static int write_grid(FILE *f, const od_led_doc *d)
{
    static const char hex[] = "0123456789abcdef";
    for (int y = 0; y < OD_GRID_H; y++) {
        char row[OD_GRID_W + 1];
        for (int x = 0; x < OD_GRID_W; x++)
            row[x] = hex[d->grid[y][x] & 0x0F];
        row[OD_GRID_W] = '\0';
        if (fprintf(f, "        \"%s\"%s\n", row,
                    y + 1 < OD_GRID_H ? "," : "") < 0)
            return 0;
    }
    return 1;
}

static int write_objects(FILE *f, const od_led_doc *d)
{
    if (fprintf(f, "      \"objects\": [") < 0)
        return 0;
    if (d->n_objects == 0) {
        return fputs("]\n", f) >= 0;
    }
    fputc('\n', f);
    for (int i = 0; i < d->n_objects; i++) {
        const od_led_object *o = &d->objects[i];
        if (o->kind == OD_LED_SPECIAL) {
            if (fprintf(f, "        { \"special\": %u, \"argument\": %u }%s\n",
                        (unsigned)o->type, (unsigned)o->argument,
                        i + 1 < d->n_objects ? "," : "") < 0)
                return 0;
        } else {
            if (fprintf(f, "        { \"type\": %u, \"positions\": [",
                        (unsigned)o->type) < 0)
                return 0;
            for (int k = 0; k < o->count; k++) {
                unsigned x = o->cells[k] & 0x0F;
                unsigned y = (o->cells[k] >> 4) & 0x0F;
                if (fprintf(f, "%s[%u, %u]", k ? ", " : "", x, y) < 0)
                    return 0;
            }
            if (fputs("] }", f) < 0)
                return 0;
            if (i + 1 < d->n_objects)
                if (fputc(',', f) < 0) return 0;
            if (fputc('\n', f) < 0) return 0;
        }
    }
    return fputs("      ]\n", f) >= 0;
}

od_status od_led_save_json(const od_led_doc *d, const char *path)
{
    if (d == NULL || path == NULL)
        return OD_ERR_RANGE;

    char tmp[1200];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return OD_ERR_RANGE;

    FILE *f = od_userdir_fopen(tmp, "wb");
    if (f == NULL)
        return OD_ERR_IO;

    int ok = 1;
    ok &= fprintf(f,
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": ") > 0;
    if (ok) write_json_string(f, d->id);
    ok &= fprintf(f, ",\n  \"world\": %d,\n", clamp_int(d->theme_world, 1, 15)) > 0;
    ok &= fprintf(f,
        "  \"grid_width\": %d,\n"
        "  \"grid_height\": %d,\n"
        "  \"name\": ", OD_GRID_W, OD_GRID_H) > 0;
    if (ok) write_json_string(f, d->name);
    ok &= fprintf(f, ",\n  \"author\": ") > 0;
    if (ok) write_json_string(f, d->author);
    ok &= fprintf(f,
        ",\n  \"created\": %lld,\n  \"modified\": %lld,\n"
        "  \"theme_world\": %d,\n  \"editor_version\": %d,\n"
        "  \"levels\": [\n"
        "    {\n"
        "      \"index\": %d,\n"
        "      \"number\": %d,\n"
        "      \"world\": %d,\n"
        "      \"round\": %d,\n"
        "      \"name\": ",
        (long long)d->created, (long long)d->modified,
        clamp_int(d->theme_world, 1, 15), d->editor_version,
        d->index, d->number, clamp_int(d->world, 1, 15), d->round) > 0;
    if (ok) write_json_string(f, d->name);
    ok &= fputs(",\n", f) >= 0;

    if (ok && d->player != OD_LED_NO_PLAYER) {
        int px = d->player & 0x0F;
        int py = (d->player >> 4) & 0x0F;
        ok &= fprintf(f, "      \"player\": { \"x\": %d, \"y\": %d },\n",
                      px, py) > 0;
    }
    ok &= fprintf(f, "      \"metadata\": %u,\n", (unsigned)d->metadata) > 0;
    ok &= fputs("      \"grid\": [\n", f) >= 0;
    if (ok) ok = write_grid(f, d);
    ok &= fputs("      ],\n", f) >= 0;
    if (ok) ok = write_objects(f, d);
    ok &= fputs("    }\n  ]\n}\n", f) >= 0;

    if (fclose(f) != 0 || !ok) {
        od_userdir_remove(tmp);
        return OD_ERR_IO;
    }
    if (od_userdir_rename(tmp, path) != 0) {
        od_userdir_remove(path);
        if (od_userdir_rename(tmp, path) != 0) {
            od_userdir_remove(tmp);
            return OD_ERR_IO;
        }
    }
    return OD_OK;
}

/* ========================================================================= */
/* JSON load                                                                 */
/* ========================================================================= */

static int parse_player(const jv *lv, od_led_doc *out)
{
    const jv *player = jv_get(lv, "player");
    if (player == NULL)
        return 1; /* tolerated: the editor flags the missing Hero */
    long x, y;
    if (player->kind != JV_OBJ || !jv_int(jv_get(player, "x"), &x) ||
        !jv_int(jv_get(player, "y"), &y) ||
        x < 0 || x >= OD_GRID_W || y < 0 || y >= OD_GRID_H)
        return 0;
    out->player = (uint8_t)(y * OD_GRID_W + x);
    return 1;
}

static int parse_grid(const jv *lv, od_led_doc *out)
{
    const jv *rows = jv_get(lv, "grid");
    if (rows == NULL || rows->kind != JV_ARR || rows->n != OD_GRID_H)
        return 0;
    for (int y = 0; y < OD_GRID_H; y++) {
        const jv *row = rows->items[y];
        if (row == NULL || row->kind != JV_STR ||
            strlen(row->str) != OD_GRID_W)
            return 0;
        for (int x = 0; x < OD_GRID_W; x++) {
            int v = jhex(row->str[x]);
            if (v < 0 || v > 0x0F)
                return 0;
            out->grid[y][x] = (uint8_t)v;
        }
    }
    return 1;
}

static int parse_objects(const jv *lv, od_led_doc *out)
{
    const jv *objs = jv_get(lv, "objects");
    out->n_objects = 0;
    if (objs == NULL)
        return 1;
    if (objs->kind != JV_ARR)
        return 0;
    for (int i = 0; i < objs->n; i++) {
        const jv *o = objs->items[i];
        if (o == NULL || o->kind != JV_OBJ)
            return 0;
        if (out->n_objects >= OD_LED_MAX_OBJECTS)
            return 0;
        od_led_object *dst = &out->objects[out->n_objects];
        memset(dst, 0, sizeof *dst);
        const jv *special = jv_get(o, "special");
        if (special != NULL) {
            long sp, arg;
            if (!jv_int(special, &sp) || sp < 0x80 || sp > 0x9F ||
                !jv_int(jv_get(o, "argument"), &arg) || arg < 0 || arg > 0xFF)
                return 0;
            dst->kind = OD_LED_SPECIAL;
            dst->type = (uint8_t)sp;
            dst->argument = (uint8_t)arg;
        } else {
            long type;
            const jv *pos = jv_get(o, "positions");
            if (!jv_int(jv_get(o, "type"), &type) || type < 0x01 || type > 0x7F ||
                pos == NULL || pos->kind != JV_ARR || pos->n < 1 ||
                pos->n > OD_LED_MAX_POSITIONS)
                return 0;
            dst->kind = OD_LED_RUN;
            dst->type = (uint8_t)type;
            dst->count = pos->n;
            for (int k = 0; k < pos->n; k++) {
                const jv *p = pos->items[k];
                long x, y;
                if (p == NULL || p->kind != JV_ARR || p->n != 2 ||
                    !jv_int(p->items[0], &x) || !jv_int(p->items[1], &y) ||
                    x < 0 || x >= OD_GRID_W || y < 0 || y >= OD_GRID_H)
                    return 0;
                dst->cells[k] = (uint8_t)(y * OD_GRID_W + x);
            }
        }
        out->n_objects++;
    }
    return 1;
}

static void parse_meta_string(const jv *root, const char *key, char *dst, size_t cap)
{
    const jv *v = jv_get(root, key);
    if (v != NULL && v->kind == JV_STR)
        copy_str(dst, cap, v->str);
}

static void parse_meta_int(const jv *root, const char *key, int64_t *dst)
{
    long n;
    if (jv_int(jv_get(root, key), &n))
        *dst = (int64_t)n;
}

od_status od_led_load_json(od_led_doc *d, const char *path)
{
    if (d == NULL || path == NULL)
        return OD_ERR_FORMAT;

    FILE *f = od_userdir_fopen(path, "rb");
    if (f == NULL)
        return OD_ERR_IO;
    size_t cap = 4096, len = 0;
    char *text = (char *)malloc(cap);
    if (text == NULL) { fclose(f); return OD_ERR_NOMEM; }
    size_t got;
    while ((got = fread(text + len, 1, cap - len - 1, f)) > 0) {
        len += got;
        if (len + 1 >= cap) {
            size_t ncap = cap * 2;
            char *grown = (char *)realloc(text, ncap);
            if (grown == NULL) { free(text); fclose(f); return OD_ERR_NOMEM; }
            text = grown;
            cap = ncap;
        }
    }
    int ioerr = ferror(f);
    fclose(f);
    if (ioerr) { free(text); return OD_ERR_IO; }
    text[len] = '\0';

    jparse j;
    j.p = text;
    j.end = text + len;
    j.err = 0;
    j.depth = 0;
    jv *root = jparse_value(&j);
    if (root == NULL || j.err) {
        jv_free(root);
        free(text);
        return OD_ERR_FORMAT;
    }

    const jv *lv = root;
    const jv *levels = jv_get(root, "levels");
    if (levels != NULL) {
        if (levels->kind != JV_ARR || levels->n < 1) {
            jv_free(root);
            free(text);
            return OD_ERR_FORMAT;
        }
        lv = levels->items[0];
    }

    od_led_doc tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.player = OD_LED_NO_PLAYER;
    tmp.theme_world = 1;
    tmp.editor_version = OD_LED_EDITOR_VERSION;

    if (lv == NULL || lv->kind != JV_OBJ ||
        !parse_grid(lv, &tmp) || !parse_player(lv, &tmp) ||
        !parse_objects(lv, &tmp)) {
        jv_free(root);
        free(text);
        return OD_ERR_FORMAT;
    }

    parse_meta_string(root, "id", tmp.id, sizeof tmp.id);
    parse_meta_string(root, "name", tmp.name, sizeof tmp.name);
    parse_meta_string(root, "author", tmp.author, sizeof tmp.author);
    parse_meta_int(root, "created", &tmp.created);
    parse_meta_int(root, "modified", &tmp.modified);

    long v;
    if (jv_int(jv_get(root, "theme_world"), &v))
        tmp.theme_world = (int)v;
    else if (jv_int(jv_get(root, "world"), &v))
        tmp.theme_world = (int)v;
    tmp.theme_world = clamp_int(tmp.theme_world, 1, 15);

    if (jv_int(jv_get(root, "editor_version"), &v) && v >= 1)
        tmp.editor_version = (int)v;
    if (lv->kind == JV_OBJ) {
        if (jv_int(jv_get(lv, "index"), &v)) tmp.index = (int)v;
        if (jv_int(jv_get(lv, "number"), &v)) tmp.number = (int)v;
        if (jv_int(jv_get(lv, "world"), &v)) tmp.world = (int)v;
        if (jv_int(jv_get(lv, "round"), &v)) tmp.round = (int)v;
    }
    if (tmp.world <= 0) tmp.world = tmp.theme_world;
    if (tmp.round <= 0) tmp.round = 1;
    if (tmp.number <= 0) tmp.number = 1;
    if (jv_int(jv_get(lv, "metadata"), &v))
        tmp.metadata = (uint8_t)(v & 0xFF);
    /* The level object's own name wins over the metadata block. */
    parse_meta_string(lv, "name", tmp.name, sizeof tmp.name);

    jv_free(root);
    free(text);

    od_led_free(d);
    *d = tmp;
    d->dirty = 0;
    return OD_OK;
}
