/* enemy.h - object/enemy model for the native OpenDana rewrite.
 *
 * Pure C11, no SDL. This module is self-contained: it parses the raw object
 * records of the 150 levels (from the content package) and offers a small
 * entity list with a per-type legacy cell-step update function plus the
 * separate exact flame engine.
 *
 * ============================================================================
 * SCOPE
 * ----------------------------------------------------------------------------
 * The record stream is the original's object list (type, count, positions,
 * terminated by a zero byte). What each type DOES in the old API was
 * approximated from observation. That API is retained for source
 * compatibility, not certified. The exact flame controller is separate.
 * Grid-owned objects are NOT necessarily static: ice, stones and fire can
 * fall. Use objects.h for the semantic catalogue.
 * ============================================================================
 */
#ifndef OD_ENEMY_H
#define OD_ENEMY_H

#include "opendana/common.h"

#define OD_ENEMY_MAGIC "ODEN"
#define OD_ENEMY_VERSION 1u
#define OD_ENEMY_HEADER_SIZE 24u
#define OD_ENEMY_MAX 64          /* spawned entities per level */

/* Object-record marker types, in the same encoding as the level grid. */
#define OD_OBJ_EMPTY  0x00
#define OD_OBJ_SOLID  0x01
#define OD_OBJ_ICE    0x05
#define OD_OBJ_FIRE   0x0E

/* --- raw asset: one object-record stream per level ----------------------- */

typedef struct {
    uint16_t len;                  /* bytes, includes the 00 terminator */
    const uint8_t *data;           /* borrowed from the enemyset buffer */
} od_enemy_record;

typedef struct {
    uint8_t *buffer;               /* owned copy of enemies.bin */
    size_t buffer_size;
    uint32_t record_fnv1a32;       /* digests stored in the header */
    uint64_t record_fnv1a64;
    int count;
    od_enemy_record levels[OD_LEVEL_COUNT];
} od_enemyset;

od_status od_enemyset_parse(od_enemyset *out, const uint8_t *data, size_t size);
od_status od_enemyset_load_file(od_enemyset *out, const char *path);
void od_enemyset_free(od_enemyset *set);

/* FNV-1a over every record byte, in level order (the header value). */
uint32_t od_enemyset_hash32(const od_enemyset *set);
uint64_t od_enemyset_hash64(const od_enemyset *set);

/* --- record iteration ---------------------------------------------------- */

/* One decoded command of a record. `kind` is 0x00..0x7F for a normal run
 * (then `count` positions follow) or 0x80..0x9F for a special two-byte
 * command (then `argument` is valid and `count` is 0). */
typedef struct {
    uint8_t kind;
    uint8_t count;
    uint8_t argument;
    const uint8_t *positions;      /* `count` bytes, y*16+x */
} od_enemy_command;

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} od_enemy_cursor;

void od_enemy_cursor_init(od_enemy_cursor *c, const od_enemy_record *rec);

/* Advance to the next command. Returns 1 on success, 0 at the terminator,
 * -1 on a malformed stream. */
int od_enemy_cursor_next(od_enemy_cursor *c, od_enemy_command *out);

/* --- entities ------------------------------------------------------------ */

typedef enum {
    OD_EKIND_STATIC = 0,  /* grid-owned; legacy entity list does not update it */
    OD_EKIND_WALKER,      /* walks left/right, turns at a wall or a ledge */
    OD_EKIND_FALLER,      /* falls one cell per tick when unsupported */
    OD_EKIND_PATROL_H,    /* 0x81: floats, patrols horizontally between walls
                            * (evidence: ROUND 2-10) */
    OD_EKIND_PATROL_V,    /* 0x82: floats, patrols vertically between walls
                            * (evidence: ROUND 4-10) */
    OD_EKIND_SPECIAL,     /* 0x83: an instanced enemy whose axis is not yet
                            * characterised; spawned but does not move yet */
    OD_EKIND_KIND_COUNT
} od_enemy_kind;

typedef struct {
    uint8_t type;          /* object id (0x02..0x1F) or special (0x81..0x83) */
    uint8_t kind;          /* od_enemy_kind */
    uint8_t alive;         /* 0 once removed from play */
    int x, y;              /* current cell */
    int home_x, home_y;    /* spawn cell, for tests/reset */
    int dir;               /* -1 or +1 (movement) */
    uint16_t timer;        /* free counter available to the update function */
} od_enemy;

typedef struct {
    int count;
    od_enemy items[OD_ENEMY_MAX];
} od_enemy_list;

/* Static description of an object type (name + how it moves). Never NULL. */
typedef struct {
    const char *name;
    od_enemy_kind kind;
} od_enemy_type_info;

const od_enemy_type_info *od_enemy_type(uint8_t type);

/* Collision callback: returns non-zero if the cell blocks movement. `ctx` is
 * the opaque pointer passed to od_enemy_tick. The default (ctx == NULL)
 * treats out-of-grid as a wall and the OD_OBJ_* cell values provided by a
 * caller as blocking; this module never reads the live game grid itself. */
typedef int (*od_enemy_blocked_fn)(void *ctx, int x, int y);

/* Spawn the entities of level `index`. Only types whose kind is not
 * OD_EKIND_STATIC are spawned; the caller keeps the static terrain in its
 * own grid. Returns the number of entities spawned, or -1 on error.
 * Entities are placed in an empty list (previous contents are cleared). */
int od_enemy_spawn_level(od_enemy_list *list, const od_enemyset *set, int index);

/* Spawn every object record (static and mobile) as an entity. Useful for
 * diagnostics/tests that want the full catalogue of a level. */
int od_enemy_spawn_level_all(od_enemy_list *list, const od_enemyset *set, int index);

/* LEGACY approximation, not the exact tick API. Advance every entity one step.
 * `blocked` may be NULL, in which case only the level bounds are treated as
 * walls. */
void od_enemy_tick(od_enemy_list *list, od_enemy_blocked_fn blocked, void *ctx);

/* Exact flame engine, independent of the legacy cell-step API above.
 * Call once per game tick, NOT every 45 ticks.
 * Unsupported type/world/state is an error, never a stationary fallback.
 * state[] is the slot's working state; type/cell are its kind and cell.
 * Input type is 1..3 (command & 31), world is the 0-based world (0..16).
 */
typedef struct {
    uint8_t type, cell;
    uint8_t state[8];
    uint16_t pose; /* metasprite selected this tick */
} od_flame;

od_status od_flame_init(od_flame *e, uint8_t type, uint8_t cell);
od_status od_flame_tick(od_flame *e, unsigned world, uint8_t game_flags,
                         const uint8_t grid[256], uint8_t hero_x,
                         uint8_t hero_y, uint8_t scroll_x,
                         uint8_t *hero_flags);

#endif /* OD_ENEMY_H */
