#ifndef OD_OBJECTS_H
#define OD_OBJECTS_H

#include "opendana/common.h"

/* Partial semantic catalogue: knowledge != complete implementation.
 * No unknown entry is silently classified as decoration or inert terrain. */
typedef enum {
    OD_OBJECT_UNKNOWN, OD_OBJECT_EMPTY, OD_OBJECT_WALL,
    OD_OBJECT_ICE, OD_OBJECT_PIPE, OD_OBJECT_STONE,
    OD_OBJECT_REACTIVE, OD_OBJECT_FIRE, OD_OBJECT_HAZARD,
    OD_OBJECT_SCROLL, OD_OBJECT_FLAME
} od_object_class;

typedef struct {
    const char *name;
    od_object_class class_id;
    uint8_t magic_removable;
    uint8_t pipe_entries; /* right=1, left=2, up=4, down=8 */
} od_object_info;

/* NULL means unknown/unsupported. Does not infer behavior from nearby IDs. */
const od_object_info *od_object_type(uint8_t type);

/* Scroll-object controller. state[0..3] is the object's working bytes. */
typedef struct {
    uint8_t enabled, interval;
    uint8_t state[4];
    uint8_t scroll_y, frame_counter;
} od_object_scroll;

/* sound is an output event: 0=no request, 0x46=start scrolling. */
od_status od_object_scroll_tick(od_object_scroll *s, uint8_t game_flags,
                                 uint8_t *sound);

/* Boss main controller only; the attack controller is a separate object and
 * attack_active is its liveness flag. The ice engine sets state[1] to 0x80 when
 * the hero's staff really touches the boss from the right. state[] holds the
 * boss object bytes and poses[] its eight pose words.
 * Events are outputs for this call, cleared at the beginning of each tick.
 * 'defeated' asks the caller to clear the boss object bytes and to decrement the
 * boss counter once.
 * The full encounter (idle, attack spawn, 31 scripts, five hits and the final
 * hit) is verified per tick in tests/test_objects.c. */
typedef struct {
    uint8_t state[16];
    uint16_t poses[8];
    uint8_t spawn_attack, sound, defeated;
} od_boss;

od_status od_boss_tick(od_boss *boss, uint8_t game_flags,
                        uint8_t attack_active);

/* One sprite entry expanded from a boss pose word.
 * `x`/`y` are sprite coordinates (y is the sprite Y byte; the compositor adds one
 * to reach the screen row), `tile` the sprite-pattern tile byte and `attr` the
 * sprite attribute byte. Entries come out in emission order, which is sprite
 * priority order when the builder runs forward. */
typedef struct {
    int x, y;
    uint8_t tile, attr;
} od_boss_sprite;

/* Expand the current pose words into sprite entries: the eight little-endian
 * pose pointers are walked in order and each metasprite is interpreted (8x8 tile
 * entries, signed x/y offsets and nested sub-metasprites up to a terminator).
 * Returns the number written, zero for no poses or no room. The arena does not
 * scroll, so the camera term is zero. */
unsigned od_boss_pose(od_boss_sprite *out, unsigned capacity,
                       const od_boss *boss);

/* Boss attack controller (object slot 1).
 * It walks the table of attack scripts one script every six ticks and carves
 * hazard cells into the grid. `state[]` is the attack object's bytes and `alive`
 * its liveness flag: a spawn stays alive until a null script pointer ends the
 * sequence, which clears both.
 *
 * The tables are content, shipped in the content package
 * (content/base/engine/boss-tables.json), never read from a ROM at run time. The
 * controller and its background graphics are checked over all 31 scripts. */
typedef struct {
    uint8_t state[4];
    uint8_t alive;
} od_boss_attack;

/* Attack graphics are background metatiles, not sprites. Project the scripts
 * already executed (state[3]) into per-cell graphic IDs.
 * 0xff means untouched, 0 is the explicit erase; no animation clock here. */
void od_boss_attack_graphics(uint8_t cells[256], const od_boss_attack *attack);

/* Spawn the attack with argument 2. */
void od_boss_attack_spawn(od_boss_attack *attack);

/* One tick. `grid` is the live 256-byte cell map (row 15 may be indexed past the
 * end, so the last cell is reachable). `hero_x`/`hero_y` are the hero's pixel
 * coordinates. Bit 4 of `*hero_flags` is raised when an end marker finds the
 * hero's cell holding a hazard. `sound` receives the attack sound ID on the spawn
 * tick and 0 otherwise. Returns OD_OK, or OD_ERR_FORMAT on bad arguments. */
od_status od_boss_attack_tick(od_boss_attack *attack, uint8_t grid[256],
                               uint8_t hero_x, uint8_t hero_y,
                               uint8_t *hero_flags, uint8_t *sound);

/* When the attack retires it appends five fixed support spans to the cell-update
 * queue (whatever cells the attack touched) and raises the round-update flag.
 * Span `i` (0..4, queue order) is `cells[i]` with `widths[i]` cells. */
#define OD_BOSS_RETIRE_SPANS 5
void od_boss_attack_retire_spans(uint8_t cells[OD_BOSS_RETIRE_SPANS],
                                  uint8_t widths[OD_BOSS_RETIRE_SPANS]);

#endif
