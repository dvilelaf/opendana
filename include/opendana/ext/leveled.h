/* leveled.h - model of the modern level editor (C11, no SDL).
 *
 * This is the pure document behind the mouse
 * editor of C5.5: a 16x15 grid of cell types (0x00..0x0F), one Hero cell, the
 * object stream of the package format (moving-flame runs and 0x80..0x9F
 * specials), named metadata and a full-snapshot undo/redo history.
 *
 * The document serialises to the package world-file format plus the
 * custom-level metadata block (`schema/custom-level.schema.json`), so a level
 * saved here is accepted by `opendana --validate` and played by
 * `opendana --play-level FILE --round 1` with no new loader.
 *
 * Everything is pure C11 (stdio, stdlib, string, time) and unit tested without
 * a window. */
#ifndef OD_EXT_LEVELED_H
#define OD_EXT_LEVELED_H

#include "opendana/common.h"
#include "opendana/level.h"

#include <stddef.h>
#include <stdint.h>

#define OD_LED_ID_MAX       40
#define OD_LED_NAME_MAX     64
#define OD_LED_AUTHOR_MAX   64
#define OD_LED_DATE_MAX     24
#define OD_LED_MAX_OBJECTS  256
#define OD_LED_MAX_POSITIONS 255
/* The package loader's CONTENT_MAX_COMMANDS; a level that serialises above
 * this is not a playable package level, so the model caps it here too. */
#define OD_LED_CMD_MAX      512
#define OD_LED_HISTORY      256
#define OD_LED_NO_PLAYER    0xFFu   /* sentinel: the document has no Hero */

/* Object severity used by od_led_validate(). */
enum { OD_LED_WARNING = 0, OD_LED_ERROR = 1 };

typedef enum {
    OD_LED_RUN = 0,       /* object run: type + N cell positions        */
    OD_LED_SPECIAL = 1    /* 0x80..0x9F opcode with one byte argument   */
} od_led_object_kind;

typedef struct {
    int kind;                                /* od_led_object_kind       */
    uint8_t type;                            /* RUN: 0x01..0x7F           */
    uint8_t argument;                        /* SPECIAL: 0x00..0xFF       */
    int count;                               /* RUN: 1..255               */
    uint8_t cells[OD_LED_MAX_POSITIONS];    /* RUN: cell = y*16 + x      */
} od_led_object;

/* What an issue is about (C6.32): interfaces translate this code, never the
 * diagnostic text, which stays the model's own (and its tests'). */
typedef enum {
    OD_LED_ISSUE_NONE = 0,
    OD_LED_ISSUE_INVALID_CELL,      /* `value` is the cell type          */
    OD_LED_ISSUE_MISSING_HERO,
    OD_LED_ISSUE_HERO_SOLID,
    OD_LED_ISSUE_INVALID_WORLD,
    OD_LED_ISSUE_INVALID_SPECIAL,
    OD_LED_ISSUE_INVALID_RUN,
    OD_LED_ISSUE_OBJECT_POSITION,
    OD_LED_ISSUE_OBJECT_GRID,
    OD_LED_ISSUE_NO_FIRES
} od_led_issue_code;

typedef struct {
    int severity;                            /* OD_LED_WARNING / ERROR   */
    int x, y;                                /* cell, -1 when not a cell  */
    char message[96];
    int code;                                /* od_led_issue_code        */
    int value;                               /* code-specific argument    */
} od_led_issue;

/* Full-snapshot history entry: the grid, Hero, metadata and the packed object
 * stream (the canonical form of the objects). Heap allocated so the document
 * itself stays cheap to put on the stack. */
typedef struct {
    uint8_t grid[OD_GRID_H][OD_GRID_W];
    uint8_t player;
    uint8_t metadata;
    uint16_t command_len;
    uint8_t commands[OD_LED_CMD_MAX];
} od_led_snapshot;

typedef struct od_led_doc {
    /* Level state (the package level). */
    uint8_t grid[OD_GRID_H][OD_GRID_W];
    uint8_t player;                          /* OD_LED_NO_PLAYER = none  */
    uint8_t metadata;
    /* Metadata block of the custom-level schema. */
    char id[OD_LED_ID_MAX];
    char name[OD_LED_NAME_MAX];
    char author[OD_LED_AUTHOR_MAX];
    int64_t created, modified;               /* Unix seconds              */
    int theme_world;                         /* 1..15                     */
    int editor_version;                      /* OD_LED_EDITOR_VERSION    */
    int index, number, world, round;         /* package level metadata    */
    /* Objects, kept in package order. */
    int n_objects;
    od_led_object objects[OD_LED_MAX_OBJECTS];
    /* Undo/redo, heap allocated on the first edit. */
    int dirty;
    od_led_snapshot *undo;
    int n_undo;
    od_led_snapshot *redo;
    int n_redo;
} od_led_doc;

#define OD_LED_EDITOR_VERSION 1

/* ---- document ----------------------------------------------------------- */

/* Reset `d` to a new blank document: solid border, empty interior, no Hero,
 * no objects, `theme_world` clamped to 1..15, name "untitled", timestamps at
 * the current time. Frees any history the document already owned; call it on a
 * zero-initialised document or on one already freed. */
void od_led_new(od_led_doc *d, int theme_world);

/* Release the undo/redo history. Safe to call twice. */
void od_led_free(od_led_doc *d);

/* ---- edits (each one pushes an undo snapshot) --------------------------- */

void od_led_paint(od_led_doc *d, int x, int y, uint8_t cell);
void od_led_fill(od_led_doc *d, int x0, int y0, int x1, int y1, uint8_t cell);
void od_led_place_hero(od_led_doc *d, int x, int y);   /* unique        */

/* Append an object copy. Returns 1 on success, 0 when the list is full or the
 * object is inconsistent (bad kind/type/count/position). For a RUN the listed
 * cells are left to the caller (paint them; the validator checks the match). */
int od_led_add_object(od_led_doc *d, const od_led_object *o);
int od_led_remove_object(od_led_doc *d, int index);

/* C6.32: index of the first RUN object of `type` that lists cell (x, y), or
 * -1. */
int od_led_object_at(const od_led_doc *d, int x, int y, uint8_t type);
/* Paint `cell` at (x, y) and drop the cell from every RUN object of another
 * type that lists it (a run left with no cell is removed), in the same undo
 * step. Painting over a moving flame with anything else must not leave an
 * orphan object (C6.32, UI-15). */
void od_led_paint_clear(od_led_doc *d, int x, int y, uint8_t cell);
/* The same for a rectangle (od_led_fill plus the run cleanup). */
void od_led_fill_clear(od_led_doc *d, int x0, int y0, int x1, int y1,
                        uint8_t cell);

void od_led_undo(od_led_doc *d);
void od_led_redo(od_led_doc *d);
int od_led_can_undo(const od_led_doc *d);
int od_led_can_redo(const od_led_doc *d);

/* ---- query / validation ------------------------------------------------- */

int od_led_has_hero(const od_led_doc *d);

/* Validate the document against the loader rules plus the playable rules.
 * Fills up to `max` issues in `out` (errors first) and
 * returns the number of ERRORS found. Warnings (for example a level with no
 * fire, which resolves at start) are also reported in `out` but do not count. */
int od_led_validate(const od_led_doc *d, od_led_issue *out, int max);

/* ---- package view ------------------------------------------------------- */

/* Build the `od_level` of the document (grid, Hero, metadata and the packed
 * object stream) into `out`. `cmd`/`cap` is the caller-owned command buffer;
 * out->commands points into it. Returns OD_ERR_RANGE when the stream does not
 * fit OD_LED_CMD_MAX. */
od_status od_led_to_level(const od_led_doc *d, od_level *out,
                            uint8_t *cmd, size_t cap);

/* ---- JSON round trip ---------------------------------------------------- */

/* Write the custom-level JSON (atomic: temporary + rename). */
od_status od_led_save_json(const od_led_doc *d, const char *path);

/* Parse a custom-level file, or the first level of a package world file. A
 * malformed grid, a missing Hero cell or a bad object is OD_ERR_FORMAT. */
od_status od_led_load_json(od_led_doc *d, const char *path);

#endif /* OD_EXT_LEVELED_H */
