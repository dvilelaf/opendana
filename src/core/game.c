/* game.c - Hero/ice phase machines. No ROM, CPU interpreter or renderer
 * dependency. */
#include "opendana/game.h"
#include "opendana/editor_data.h"

#include "opendana/content_tables.h"

#include <stdio.h>
#include <string.h>

static void hero_reset(od_game *g);
static void hero_entry_start(od_game *g);
static unsigned hero_root(unsigned variant, unsigned action);
static void emit_sound(od_game *g, unsigned id);

/* The idle countdown reload: 1800 frames (36 s PAL) without a taken input
 * before the idle program. */
#define OD_IDLE_FRAMES 0x0708u
#define OD_ACTION_IDLE 0x1Bu

/* ------------------------------------------------------------------------- */
/* Compatibility index lookup from the JSON package, never legacy dumps      */
/* ------------------------------------------------------------------------- */

static od_content_pack g_index_pack;
static int g_index_pack_state; /* 0 = not tried, 1 = loaded, -1 = failed */
static char g_index_pack_base[1024];

void od_game_set_enemy_asset(const char *path)
{
    /* Deprecated compatibility ABI: never open the supplied dump. Reset only
     * the optional package index cache; known-index callers do not use it. */
    (void)path;
    if (g_index_pack_state == 1) od_content_pack_free(&g_index_pack);
    g_index_pack_state = 0;
    g_index_pack_base[0] = '\0';
}

static const od_levelset *ensure_level_set(void)
{
    char base[1024];
    if (od_content_resolve_base(base, sizeof base, NULL) != OD_OK) return NULL;
    if (strcmp(base, g_index_pack_base) != 0) od_game_set_enemy_asset(NULL);
    if (g_index_pack_state == 0) {
        od_content_vfs vfs = {0};
        snprintf(vfs.layers[0].root, sizeof vfs.layers[0].root, "%s", base);
        vfs.count = 1;
        snprintf(g_index_pack_base, sizeof g_index_pack_base, "%s", base);
        g_index_pack_state = od_content_pack_load(&g_index_pack, &vfs) == OD_OK ? 1 : -1;
        if (g_index_pack_state < 0)
            fprintf(stderr, "opendana: cannot load JSON package for legacy level-index lookup: %s\n", base);
    }
    return g_index_pack_state == 1 ? &g_index_pack.levels : NULL;
}

/* Streams alone are not identities: static and user levels can share FF.
 * Anonymous/editor boards need no index lookup at all. For old callers with
 * commands, require one exact grid/start/metadata/stream match in real JSON
 * level structures; never guess the first shared stream's index. */
static int find_level_index(const od_level *lv)
{
    if (!lv || !lv->commands || lv->command_len <= 1) return -1;
    const od_levelset *set = ensure_level_set();
    if (!set) return -1;
    int found = -1;
    for (int i = 0; i < set->count; i++) {
        const od_level *r = &set->levels[i];
        if (r->player_position == lv->player_position && r->metadata == lv->metadata &&
            r->command_len == lv->command_len && r->commands &&
            memcmp(r->grid, lv->grid, sizeof r->grid) == 0 &&
            memcmp(r->commands, lv->commands, r->command_len) == 0) {
            if (found >= 0) return -1; /* ambiguous, never invent an index */
            found = i;
        }
    }
    return found;
}

int od_game_world(const od_game *g)
{
    return (g->level_index >= 0) ? g->level_index / 10 + 1 : 1;
}

/* ------------------------------------------------------------------------- */
/* Grid helpers                                                               */
/* ------------------------------------------------------------------------- */

static int in_bounds(int x, int y)
{
    return x >= 0 && x < OD_GRID_W && y >= 0 && y < OD_GRID_H;
}

int od_game_cell(const od_game *g, int x, int y)
{
    if (!in_bounds(x, y))
        return OD_GAME_SOLID; /* outside the grid is a wall (and the floor is row 15) */
    return g->cells[y][x];
}

static int count_alive_enemies(const od_game *g)
{
    int n = 0;
    for (int i = 0; i < g->enemies.count; i++)
        if (g->enemies.items[i].alive)
            n++;
    return n;
}

/* `fires` is the level objective: the static 0x0E cells plus the mobile flame
 * entities (0x81/0x82/0x83). In ROUND 2-10 and friends every fire is a moving
 * entity, so counting only the grid would declare the level cleared at once. */
static void recite_fires(od_game *g)
{
    int n = count_alive_enemies(g);
    for (int y = 0; y < OD_GRID_H; y++)
        for (int x = 0; x < OD_GRID_W; x++)
            if (g->cells[y][x] == OD_GAME_FIRE)
                n++;
    g->fires = n;
}

void od_game_update_clear(od_game *g)
{
    g->cleared = (g->fires == 0);
    g->exit_open = g->cleared;
}

void od_game_recount_fires(od_game *g)
{
    recite_fires(g);
    od_game_update_clear(g);
}

/* --- moving flames, death and retry ---------------------------------------- */

/* Sets the death flag, raised by the death-program opcode (and by the lethal
 * contact path). */
static void hero_die(od_game *g)
{
    if (g->dead || g->game_over)
        return;
    g->dead = 1;
    g->deaths++;
    if (g->lives > 0)
        g->lives--;
}

/* The flame controllers only ever test a 256-byte grid: a flame's
 * destination may be the row beyond the 16x15 play field, and the original
 * reads it. That row is empty, so the native field (15 rows) is padded with
 * 16 empty cells, never invented. */
static void flame_grid(const od_game *g, uint8_t grid[256])
{
    memcpy(grid, g->cells, OD_GRID_CELLS);
    memset(grid + OD_GRID_CELLS, 0, 256 - OD_GRID_CELLS);
}

/* Mirror a controller slot into the rendered/objective enemy. The integer pixel
 * centre is state[4] (Y) / state[7] (X); the cell is its high nibbles. */
static void flame_sync_enemy(od_game *g, int i)
{
    od_enemy *e = &g->enemies.items[i];
    const od_flame *f = &g->flames[i];
    if (f->type == 0) {
        e->alive = 0;
        return;
    }
    e->alive = 1;
    e->x = f->state[7] >> 4;
    e->y = f->state[4] >> 4;
    e->dir = 1;
}

/* One exact flame step per tick. `game_flags` is the live freeze/effect bit
 * set; with it set the controller still animates but does not translate or
 * test contact. The controller publishes a lethal-contact bit when it
 * touches Hero, which is the death trigger (not a cell equality). */
int od_game_tick_enemies(od_game *g)
{
    if (g->game_over || g->flame_count <= 0)
        return 0;
    /* The death bit stops translation and contact like an input freeze; the
     * animation keeps running. */
    uint8_t game_flags = (g->freeze || g->dead || g->paused) ? 1u : 0u;
    uint8_t grid[256];
    flame_grid(g, grid);
    unsigned world = (unsigned)(od_game_world(g) - 1); /* 0-based */
    /* Registration order, updated like the original walks the slots (reverse). */
    for (int i = g->flame_count - 1; i >= 0; i--) {
        od_flame *f = &g->flames[i];
        uint8_t flags = (uint8_t)g->hero_flags;
        od_flame_tick(f, world, game_flags, grid,
                       (uint8_t)g->hero_px, (uint8_t)g->hero_py, 0, &flags);
        g->hero_flags = flags;
    }
    for (int i = 0; i < g->flame_count; i++)
        flame_sync_enemy(g, i);
    /* The lethal-contact bit is the original death trigger; the decision task
     * of the next tick consumes it. */
    if (g->hero_flags & 0x10) {
        g->lethal = 1;
        return 1;
    }
    return 0;
}

/* With the action counter nonzero and no freeze bit but the lowest set, a
 * raised lethal-contact bit sets the death flag and starts the death program
 * from its first byte. The program set stays the one selected by the last
 * support check and the held offsets are kept: Hero dies where the contact
 * caught him. */
static void hero_death_start(od_game *g)
{
    g->hero_action = 0x1f;
    g->hero_active = 1;
    g->hero_phase = g->hero_wait = 0;
    g->hero_seq = hero_root(g->hero_set, 0x1f);
    hero_die(g);
}

void od_game_retry(od_game *g)
{
    memcpy(g->cells, g->initial_cells, sizeof(g->cells));
    g->hero_x = g->initial_x;
    g->hero_y = g->initial_y;
    g->facing = 1;
    g->enemies = g->initial_enemies;
    memcpy(g->flames, g->initial_flames, sizeof g->flames);
    g->flame_count = g->initial_flame_count;
    g->hero_flags = 0;
    g->lethal = 0;
    g->fires = 0;
    g->cleared = 0;
    g->exit_open = 0;
    g->cooldown = 0;
    g->pending = 0;
    g->pending_wait = 0;
    g->pending_x = g->pending_y = g->pending_dx = 0;
    g->last_slide_cells = 0;
    g->last_ice_drops = 0;
    g->enemy_clock = 0;
    g->dead = 0;
    g->game_over = 0;
    /* The reload clears the scroll state: the auto-scroll restarts from the
     * top with the round. */
    g->scroll_state = g->scroll_timer = g->scroll_y = 0;
    g->frame = 0; /* a retry restarts the clock at zero */
    hero_reset(g);
    recite_fires(g);
    g->start_fires = g->fires;
    od_game_update_clear(g);
    g->retries++;
    /* The reload re-arms the entry materialization like a level load. */
    hero_entry_start(g);
}

/* Shared by every entry point: `set`/`set_index` select the object record to
 * spawn, `level_index` is the level's place in the 150 (world graphics, flame
 * world, castle). */
static void game_init_core(od_game *g, const od_level *lv,
                           const od_enemyset *set, int set_index, int level_index)
{
    memset(g, 0, sizeof(*g));
    memcpy(g->cells, lv->grid, sizeof(g->cells));
    g->hero_x = od_player_x(lv);
    g->hero_y = od_player_y(lv);
    g->facing = 1;
    g->action_period = OD_ACTION_PERIOD;
    g->level_index = level_index;
    g->lives = OD_HERO_UNLIMITED;

    /* Spawn the level's enemies before counting the objectives. */
    if (set != NULL && set_index >= 0 && set_index < set->count) {
        if (od_enemy_spawn_level(&g->enemies, set, set_index) < 0)
            g->enemies.count = 0;
    }
    g->initial_enemies = g->enemies;

    /* Instantiate the exact flame controller slots from the flame commands
     * (types 0x81..0x83), in registration order, with `command & 31` and the
     * spawn cell. Slots with no command stay zero (type 0 = dead). */
    g->flame_count = 0;
    for (int i = 0; i < g->enemies.count && g->flame_count < OD_FLAME_MAX; i++) {
        const od_enemy *e = &g->enemies.items[i];
        if (e->type < 0x81 || e->type > 0x83)
            continue;
        uint8_t cell = (uint8_t)(e->x + e->y * OD_GRID_W);
        if (od_flame_init(&g->flames[g->flame_count],
                           (uint8_t)(e->type - 0x80), cell) == OD_OK)
            g->flame_count++;
    }
    g->initial_flame_count = g->flame_count;
    memcpy(g->initial_flames, g->flames, sizeof g->flames);

    /* The scroll command (0x80) enables the auto-scroll and carries its
     * period argument. It is read from the same object record the enemies
     * come from (the level's own command stream in the product). */
    if (set != NULL && set_index >= 0 && set_index < set->count) {
        od_enemy_cursor cur;
        od_enemy_command cmd;
        od_enemy_cursor_init(&cur, &set->levels[set_index]);
        while (od_enemy_cursor_next(&cur, &cmd) == 1)
            if (cmd.kind == 0x80) {
                g->scroll_enabled = 1;
                g->scroll_period = cmd.argument;
            }
    }

    recite_fires(g);
    g->start_fires = g->fires;
    od_game_update_clear(g);

    /* Snapshot the start state: it is what a retry restores. */
    memcpy(g->initial_cells, g->cells, sizeof(g->cells));
    g->initial_x = g->hero_x;
    g->initial_y = g->hero_y;
    hero_reset(g);
    /* A real level load arms the entry materialization. */
    hero_entry_start(g);
}

void od_game_init_ex(od_game *g, const od_level *lv,
                      const od_enemyset *set, int level_index)
{
    game_init_core(g, lv, set, level_index, level_index);
}

/* A level's objects are its own command stream — the package rebuilds it
 * from the level's "objects" — so a mod or an editor level that changes them
 * is honoured. For the vanilla levels it is byte-identical to the imported
 * object record. */
void od_game_init_index(od_game *g, const od_level *lv, int level_index)
{
    od_enemyset own;
    memset(&own, 0, sizeof own);
    if (lv->commands != NULL && lv->command_len > 0) {
        own.count = 1;
        own.levels[0].len = lv->command_len;
        own.levels[0].data = lv->commands;
    }
    game_init_core(g, lv, own.count ? &own : NULL, 0, level_index);
}

void od_game_init(od_game *g, const od_level *lv)
{
    od_game_init_index(g, lv, find_level_index(lv));
}

/* --- entity phases and support tasks --------------------------------------- */

static void hero_pixels(od_game *g)
{
    /* The projected position is not written during the level-entry
     * introduction: the entry sequence is armed but its program only starts
     * after `entry_hold` ticks, and until its first pose the projected
     * position stays zero. */
    if (g->entry_hold || g->hero_pose == 0) {
        g->hero_px = g->hero_py = 0;
        return;
    }
    /* The right-facing sprite offset keeps one extra pixel, even at offset zero. */
    g->hero_px = g->hero_x * 16 + 8 + g->facing * g->hero_ox + (g->facing > 0);
    /* The screen row: the vertical scroll of the auto-scrolling rounds is
     * subtracted modulo the 240-line field. */
    g->hero_py = ((g->hero_y * 16 + 8 - g->hero_oy - (int)g->scroll_y) % 240 + 240) % 240;
    /* In auto-scrolling rounds, once Hero's drawn row reaches 200 he has left
     * the visible field and the next decision step starts the death program. */
    if (g->scroll_enabled && g->hero_py >= 0xC8)
        g->lethal = 1;
}

/* The auto-scroll task, the first of the frame. State 0 waits: on a
 * scrolling round a toggle bit flips every call and on the calls that clear
 * it the timer counts down; at zero it requests its sound and enters state 1.
 * State 1 moves the scroll up one pixel per call (mod 240) and reloads the
 * timer and state 0 when a whole row has passed. Nothing runs while the game
 * is paused or Hero is dead. */
static void scroll_task(od_game *g)
{
    if (g->paused || g->dead)
        return;
    switch (g->scroll_state & 3u) {
    case 0:
        if (!g->scroll_enabled)
            return;
        g->scroll_state ^= 0x80u;
        if (g->scroll_state & 0x80u)
            return;
        if (g->scroll_timer && --g->scroll_timer)
            return;
        g->scroll_state = 1;
        emit_sound(g, 0x46);
        return;
    case 1: {
        if (g->scroll_timer && --g->scroll_timer)
            return;
        unsigned y = (g->scroll_y - 1u) & 0xFFu;
        if (y >= 0xF0u)
            y -= 0x10u;
        g->scroll_y = y;
        if ((y & 0x0Fu) == 0) {
            g->scroll_timer = g->scroll_period;
            g->scroll_state = 0;
        } else {
            g->scroll_timer = 1;
        }
        return;
    }
    default:
        return;
    }
}

static void hero_reset(od_game *g)
{
    g->hero_target_x = g->hero_x;
    g->hero_target_y = g->hero_y;
    g->hero_action = g->hero_phase = g->hero_wait = g->hero_active = 0;
    g->hero_ox = g->hero_oy = 0;
    g->hero_set = 4;
    g->hero_pose = 43460u;
    g->hero_seq = 0;
    g->pipe_last = g->pipe_exit = 0;
    g->pipe_cell = g->pipe_dir = g->pipe_kind = g->pipe_prev = 0;
    g->pipe_active = 0;
    g->celebration_done = g->magic_pending = 0;
    g->exit_frames = 0;
    g->level_exit = 0;
    g->support_count = g->drop_count = g->cascade_task = g->cascade_state = 0;
    g->magic_lateral = 0;
    g->effect_count = g->effect_active = 0;
    g->effect_wait = 4;
    g->ice_state = g->ice_wait = g->pending = 0;
    g->pending_material = 0;
    memset(&g->stone, 0, sizeof g->stone);
    g->entry_hold = 0;
    /* A level (re)start reloads the idle countdown; the entry program reloads
     * it again on the tick the play loop really starts (hero_animate). */
    g->idle_timer = OD_IDLE_FRAMES;
    g->jar_cell = g->jar_pending = 0;
    g->ice_px = g->ice_py = 0;
    g->cascade_cell = g->cascade_width = g->cascade_phase = 0;
    g->magic_cell = g->sound_request = 0;
    g->magic_redraw = 0;
    g->sound_count = 0;
    g->freeze = 0;
    hero_pixels(g);
}

/* Append a request to the sound ring. `sound_request` keeps the existing
 * single-value contract; the ordered queue preserves a tick that emits more
 * than one event (e.g. a cascade landing plus an extinction effect). */
static void emit_sound(od_game *g, unsigned id)
{
    if (g->sound_count < 8)
        g->sound_queue[g->sound_count++] = (uint8_t)id;
    g->sound_request = id;
}

/* Public wrappers for controllers that run outside `od_game_tick` (the boss
 * combat owns its own attack/main objects). */
void od_game_sound_event(od_game *g, unsigned id)
{
    if (g == NULL || id == 0)
        return;
    if (g->sound_count < 8)
        g->sound_queue[g->sound_count++] = (uint8_t)id;
    g->sound_request = id;
}

void od_game_lethal_contact(od_game *g)
{
    if (g == NULL)
        return;
    g->lethal = 1; /* consumed by the next decision step */
}

static unsigned cell_index(int x, int y) { return (unsigned)(y * 16 + x); }
static unsigned below_cell(unsigned p) { return (p + 16) % 240; }
static unsigned above_cell(unsigned p) { return (p + 224) % 240; }
static int cell_at(const od_game *g, unsigned p) { return g->cells[p / 16][p % 16]; }
static void put_cell(od_game *g, unsigned p, unsigned value)
{
    g->cells[p / 16][p % 16] = (uint8_t)value;
}
static int passable(int c) { return c == 0 || c == 14; }

/* Auto-scrolling rounds only: a moving ice block (or a falling ice span)
 * whose cell has dropped below the visible play field is lost. The cell's
 * centre row in pixels, moved by the scroll (240 minus the scroll offset, or 0
 * at rest), is wrapped to 0..239 and compared with the field's bottom row;
 * at or beyond it the block is consumed like a hazard. Stones never test it. */
static int scroll_off_screen(const od_game *g, unsigned cell)
{
    if (!g->scroll_enabled)
        return 0;
    unsigned a = (0xF0u - g->scroll_y) & 0xFFu;
    if (a == 0xF0u)
        a = 0;
    unsigned sum = a + ((cell & 0xF0u) | 8u);
    sum = (sum > 0xFFu) ? (((sum & 0xFFu) + 0x10u) & 0xFFu) : sum;
    if (sum >= 0xF0u)
        sum &= 0x0Fu;
    return sum >= 0xC8u;
}

/* These are stacks of changed support spans, not a global scan. */
static void support_changed(od_game *g, unsigned p, unsigned width)
{
    if (g->support_count < OD_SUPPORT_SPANS) {
        unsigned i = g->support_count++;
        g->support_cells[i] = (uint8_t)p;
        g->support_widths[i] = (uint8_t)width;
    }
    if (!g->cascade_task)
        g->cascade_task = 1;
}

/* Public entry for controllers outside `od_game_tick` (the boss attack). */
void od_game_support_changed(od_game *g, unsigned cell, unsigned width)
{
    if (g == NULL)
        return;
    support_changed(g, cell, width);
}

/* The boss attack retires after the frame's support task and leaves its
 * spans with the cascade armed and the freeze bit raised: the next cascade
 * step is the half that clears the arming state, and the one after collects
 * and drops the first candidate. Called after `od_game_tick`, so `freeze` (the
 * freeze bits at the end of the frame) is raised here too. */
void od_game_support_changed_now(od_game *g, unsigned cell, unsigned width)
{
    if (g == NULL)
        return;
    support_changed(g, cell, width);
    g->freeze = 1;
    g->cascade_task = 2;
    g->cascade_state = 0x80;
}

/* Reads the board with an 8-bit index: the row beyond the 15 play rows is
 * empty in the original (see flame_grid). */
static int ram_cell(const od_game *g, unsigned x)
{
    x &= 0xFF;
    return x < OD_GRID_CELLS ? g->cells[x / 16][x % 16] : 0;
}

/* A run falls when every cell below it is empty or fire, and for ice none of
 * them is Hero's destination. The candidate goes on the candidate stack
 * (LIFO); fire uses the width flag. */
static void drop_candidate(od_game *g, unsigned left, unsigned w, int fire)
{
    unsigned q = (left + 16) & 0xFF;
    if (q >= 0xF0) q &= 0x0F;
    unsigned hero = cell_index(g->hero_target_x, g->hero_target_y);
    for (unsigned i = 0; i < w; ++i, q = (q + 1) & 0xFF) {
        int c = ram_cell(g, q);
        if ((fire ? c != 0 : !passable(c)) || (!fire && q == hero))
            return;
    }
    if (g->drop_count >= 16)
        return;
    unsigned n = g->drop_count++;
    g->drop_cells[n] = (uint8_t)left;
    g->drop_widths[n] = (uint8_t)(w | (fire ? 128 : 0));
    for (unsigned k = 0; k < w && k < 16; ++k)
        g->drop_tiles_q[n][k] = (uint8_t)ram_cell(g, left + k);
}

/* Walk left through interior ice (type 2) to a left end (type 3). A stone
 * inside a run is crossed; when no ice continues past it, the stone itself
 * is the run's left end. Any other cell rejects the run and is left in *x. */
static int run_left(const od_game *g, unsigned *x, unsigned *w)
{
    for (;;) {
        ++*w;
        *x = (*x - 1) & 0xFF;
        int c = ram_cell(g, *x);
        if (c == 0x03) return 1;
        if (c == 0x02) continue;
        if (c != 0x0C) return 0;
        ++*w;
        *x = (*x - 1) & 0xFF;
        c = ram_cell(g, *x);
        if (c == 0x03) return 1;
        if (c == 0x02) continue;
        --*w;
        *x = (*x + 1) & 0xFF;
        return 1;
    }
}

/* The mirrored walk to a right end (type 4). */
static int run_right(const od_game *g, unsigned *x, unsigned *w)
{
    for (;;) {
        ++*w;
        *x = (*x + 1) & 0xFF;
        int c = ram_cell(g, *x);
        if (c == 0x04) return 1;
        if (c == 0x02) continue;
        if (c != 0x0C) return 0;
        ++*w;
        *x = (*x + 1) & 0xFF;
        c = ram_cell(g, *x);
        if (c == 0x04) return 1;
        if (c == 0x02) continue;
        --*w;
        *x = (*x - 1) & 0xFF;
        return 1;
    }
}

/* One handler for the cell `p` above a changed span, by cell type: ice types
 * 2, 3, 4 and 5, the stone and fire. Each one leaves the run's left cell in
 * `*left_end` and its right cell in `*right_end` (or the cell that rejected the run).
 * Returns 0 for any other cell, which has no handler and leaves both
 * untouched. */
static int support_handler(od_game *g, unsigned p, unsigned *left_end, unsigned *right_end)
{
    int c = ram_cell(g, p);
    unsigned x = p, w = 1;
    switch (c) {
    case 0x02:
        if (!run_left(g, &x, &w)) { *left_end = x; return 1; }
        *left_end = x;
        x = p;
        if (!run_right(g, &x, &w)) { *right_end = x; return 1; }
        *right_end = x;
        break;
    case 0x03:
        *left_end = p;
        if (!run_right(g, &x, &w)) { *right_end = x; return 1; }
        *right_end = x;
        break;
    case 0x04:
        *right_end = p;
        if (!run_left(g, &x, &w)) { *left_end = x; return 1; }
        *left_end = x;
        break;
    case 0x05:
        *left_end = *right_end = p;
        break;
    case 0x0C:
        /* The stone joins the ice run it touches; with no left end (or
         * interior) on the left, or no right end on the right, it is that end
         * of the run itself, so a lone stone is a one-cell run. */
        ++w;
        x = (x - 1) & 0xFF;
        if (ram_cell(g, x) == 0x02) {
            if (!run_left(g, &x, &w)) { *left_end = x; return 1; }
        } else if (ram_cell(g, x) != 0x03) {
            --w;
            x = p;
        }
        *left_end = x;
        x = p;
        ++w;
        x = (x + 1) & 0xFF;
        if (ram_cell(g, x) == 0x02) {
            if (!run_right(g, &x, &w)) { *right_end = x; return 1; }
        } else if (ram_cell(g, x) != 0x04) {
            --w;
            x = p;
        }
        *right_end = x;
        break;
    case 0x0E:
        *left_end = *right_end = p;
        drop_candidate(g, p, 1, 1);
        return 1;
    default:
        return 0;
    }
    drop_candidate(g, *left_end, w, 0);
    return 1;
}

/* Pop every changed support span (LIFO) and test the cells right above it,
 * right to left, for runs that lost their support. After a handler the scan
 * resumes left of the run. The left/right ends survive across spans (they
 * are cleared once per call): a span on the same row that overlaps the run
 * just tested only scans the part left of it, so one run is never queued
 * twice. */
static void collect_drops(od_game *g)
{
    unsigned left_end = 0, right_end = 0;
    while (g->support_count) {
        unsigned i = --g->support_count;
        unsigned base = above_cell(g->support_cells[i]); /* first cell above */
        unsigned width = g->support_widths[i];          /* span width */
        int y = (int)width;
        if (left_end) {
            unsigned last = (base + width - 1) & 0xFF;
            if ((left_end & 0xF0) == (last & 0xF0) && last >= left_end && right_end >= base) {
                if (left_end < base)
                    continue;
                y = (int)(left_end - base);
            }
        }
        while (--y >= 0) {
            if (!support_handler(g, (base + (unsigned)y) & 0xFF, &left_end, &right_end))
                continue;
            if (left_end < base)
                break;
            y = (int)(left_end - base);
        }
    }
}

/* Queue one extinction effect. The drain plays its sound after four ticks
 * and the freeze flag holds a fragmented landing. */
static void queue_effect(od_game *g)
{
    if (g->effect_count < 255) ++g->effect_count;
    g->effect_active = 1;
    if (!g->effect_wait) g->effect_wait = 4;
}

static void quench(od_game *g, unsigned p)
{
    put_cell(g, p, 0);
    --g->fires;
    queue_effect(g); /* drained one every 4 ticks */
    od_game_update_clear(g);
}

/* Canonical end types of a contiguous horizontal ice run (level format: left
 * end 3, interior 2, right end 4, isolated 5). Applied to the runs produced
 * when a falling group fragments, so the remaining pieces keep usable ends.
 * A stone that joined the run keeps its type and still counts as a run
 * position, like the arena's stone-plus-right-end pair (a stone that put a
 * fire out stays a stone, measured on round 8-5). */
static void ice_normalize(uint8_t *tiles, unsigned width)
{
    if (width == 1) {
        if (tiles[0] != 0x0c)
            tiles[0] = OD_GAME_ICE;
        return;
    }
    for (unsigned i = 0; i < width; ++i)
        if (tiles[i] != 0x0c)
            tiles[i] = (i == 0) ? 3 : (i == width - 1 ? 4 : 2);
}

/* The board is scanned linearly and an index may cross row ends. The board
 * is exactly 240 bytes, so a wrap keeps the read in range; the magic target
 * never sits on an edge cell on the verified routes. */
static int grid_byte(const od_game *g, unsigned p)
{
    p %= 240;
    return g->cells[p / 16][p % 16];
}

/* The connected horizontal ice run immediately to the right of the
 * just-emptied cell `r`. Returns 1 and sets `*cell`/`*width` for a run that
 * has a usable right end (3 ... 4, or an isolated 5). The scan has an ice
 * loop and a stone branch, and a run that does not end correctly rejects
 * (the caller then tries the left side). */
static int magic_run_right(const od_game *g, unsigned r, unsigned *cell, unsigned *width)
{
    unsigned x = (r + 1) % 240, w = 1;
    int c = grid_byte(g, x);
    if (c == 0x03) {
        /* fall through to the ice loop */
    } else if (c == 0x05) {
        goto found; /* isolated block, width 1 */
    } else if (c == 0x0c) {
        goto stone;
    } else {
        return 0; /* left scan next */
    }
ice_loop:
    ++w; x = (x + 1) % 240;
    c = grid_byte(g, x);
    if (c == 0x04) goto found;     /* right end */
    if (c == 0x02) goto ice_loop;  /* interior */
    if (c == 0x0c) goto stone;
    return 0;
stone:
    ++w; x = (x + 1) % 240;
    c = grid_byte(g, x);
    if (c == 0x04) goto found;
    if (c == 0x02) goto ice_loop;
    --w; x = (x + 239) % 240; /* exclude the trailing stone cell */
found:
    *cell = (r + 1) % 240;
    *width = w;
    return 1;
}

/* The mirrored scan to the left; the run must end in a left end (type 4 ...
 * type 3) or be an isolated type 5. `*cell` is the leftmost cell. */
static int magic_run_left(const od_game *g, unsigned r, unsigned *cell, unsigned *width)
{
    unsigned x = (r + 239) % 240, w = 1;
    int c = grid_byte(g, x);
    if (c == 0x04) {
        /* fall through to the interior loop */
    } else if (c == 0x05) {
        goto found;
    } else if (c == 0x0c) {
        goto stone;
    } else {
        return 0;
    }
ice_loop:
    ++w; x = (x + 239) % 240;
    c = grid_byte(g, x);
    if (c == 0x03) goto found;     /* left end */
    if (c == 0x02) goto ice_loop;  /* interior */
    if (c == 0x0c) goto stone;
    return 0;
stone:
    ++w; x = (x + 239) % 240;
    c = grid_byte(g, x);
    if (c == 0x03) goto found;
    if (c == 0x02) goto ice_loop;
    --w; x = (x + 1) % 240; /* exclude the trailing stone cell */
found:
    *cell = x;
    *width = w;
    return 1;
}

/* Every cell below the run must be empty or fire. */
static int magic_below_clear(const od_game *g, unsigned cell, unsigned width)
{
    unsigned b = below_cell(cell);
    for (unsigned i = 0; i < width; ++i) {
        int c = grid_byte(g, b + i);
        if (c != 0x00 && c != 0x0e)
            return 0;
    }
    return 1;
}

/* Push a released span straight into the candidate queue (the native drop
 * stack) with its tile image, and arm the cascade task. */
static void magic_enqueue(od_game *g, unsigned cell, unsigned width)
{
    if (g->drop_count >= 16)
        return;
    unsigned n = g->drop_count++;
    g->drop_cells[n] = (uint8_t)cell;
    g->drop_widths[n] = (uint8_t)width;
    for (unsigned i = 0; i < width && i < 16; ++i)
        g->drop_tiles_q[n][i] = (uint8_t)grid_byte(g, cell + i);
    g->cascade_task = 1;
}

/* Called when the magic update leaves cell `r` empty: release the connected
 * lateral ice runs that just lost their support. Each side is scanned
 * independently; a run is a candidate only when everything below it is empty
 * or fire. The candidates go straight into the queue (bypassing the
 * above-cell support scan). Only when the candidate queue is still empty is
 * the removed cell re-queued as a support span. Either way the freeze bit is
 * raised so the cascade runs before new decisions. */
static void magic_release(od_game *g, unsigned r)
{
    unsigned cell, width;
    if (magic_run_right(g, r, &cell, &width) &&
        magic_below_clear(g, cell, width)) {
        magic_enqueue(g, cell, width);
        g->magic_lateral = 1; /* right candidate */
    }
    if (magic_run_left(g, r, &cell, &width) &&
        magic_below_clear(g, cell, width)) {
        magic_enqueue(g, cell, width);
        if (!g->magic_lateral) g->magic_lateral = 0x80; /* left candidate */
    }
    if (g->drop_count == 0) {
        support_changed(g, r, 1); /* re-queue the removed cell */
        g->magic_lateral = 0;
    } else
        g->cascade_task = 1;
}

/* One task step. Phase += 32 gives eight ticks per falling cell. Fire
 * occupancy leads its presentation by a cell. */
static void cascade_tick(od_game *g)
{
    if (!g->cascade_task) return;
    if (g->cascade_task == 1) {
        g->cascade_task = 2;
        g->cascade_state = 0x80;
        return; /* then yield */
    }
    if (!(g->cascade_state & 0x80)) {
        collect_drops(g);
        if (!g->drop_count) { g->cascade_task = 0; return; }
        g->cascade_state = 0x80;
    }
    unsigned state = g->cascade_state & 3;
    if (state == 0) {
        if (!g->drop_count) { g->cascade_state = 0; return; }
        unsigned n = --g->drop_count;
        unsigned p = g->drop_cells[n], w = g->drop_widths[n];
        g->cascade_cell = p;
        g->cascade_width = w;
        g->cascade_phase = 0;
        if (w & 128) {
            put_cell(g, p, 0);
            put_cell(g, below_cell(p), 14);
            g->cascade_state = 0x82;
            support_changed(g, p, 1);
        } else {
            for (unsigned i = 0; i < w; ++i) {
                g->drop_tiles[i] = g->drop_tiles_q[n][i];
                put_cell(g, p + i, 0);
            }
            /* The first lateral candidate's support span ALSO covers the cell
             * removed by the wand. Otherwise ice above that cell is never
             * revisited after the lateral fall. The candidates are LIFO: with
             * both sides, the left one pops first. */
            unsigned support = p, width = w;
            if (g->magic_lateral) {
                if (g->magic_lateral == 1 && !g->drop_count)
                    support = (p + 239) % 240;
                ++width;
                g->magic_lateral = 0;
            }
            support_changed(g, support, width);
            g->cascade_state = 0x81;
            /* The ice block starts falling (its sound). A fire drop (width
             * bit 7) is silent. */
            emit_sound(g, 0x58);
        }
        return;
    }
    if (state == 3) {
        if (!g->effect_active) g->cascade_state = 0x80;
        return;
    }
    g->cascade_phase += 32;
    if (g->cascade_phase < 256) return;
    g->cascade_phase = 0;
    unsigned p = below_cell(g->cascade_cell);
    g->cascade_cell = p;
    if (state == 2) {
        unsigned q = below_cell(p);
        int underneath = cell_at(g, q);
        if (!underneath) {
            put_cell(g, p, 0);
            put_cell(g, q, 14);
        } else {
            /* A falling fire reaching a jar cell stores that cell so a later
             * task can turn it into the hazard. */
            if ((underneath & 15) == 0x0D) {
                g->jar_cell = q;
                g->jar_pending = 1;
            }
            g->cascade_state = 0x80;
        }
        return;
    }
    unsigned w = g->cascade_width;
    /* The span already advanced to row `p`. A column stops only when the
     * destination row holds fire (which is extinguished) or the cell below
     * is a hazard or Hero's destination. With no stopped column a solid below
     * lands the WHOLE span here; otherwise the span fragments: stopped
     * columns are destroyed, the continuing runs are written at this row with
     * their run end types and re-queued as continuations. A destroyed column
     * queues its effect and holds the landing in the effect-wait state. */
    unsigned stop[16], fire[16];
    int stopped = 0, destroyed = 0;
    int off_screen = scroll_off_screen(g, p);
    for (unsigned i = 0; i < w; ++i) {
        stop[i] = fire[i] = 0;
        /* Flame slots, walked from the last, whose cell is this column's
         * destination are put out and the fire count drops; the resistant
         * type survives ice but still stops the column. Every stone kills
         * every type. The effect is queued by the fragmentation below like a
         * hazard column, not here. */
        int flame_hit = 0;
        for (int s = g->flame_count - 1; s >= 0; s--) {
            od_flame *f = &g->flames[s];
            if (!f->type || (unsigned)f->cell != p + i)
                continue;
            if (g->drop_tiles[i] == 0x0c || f->type != 3) {
                f->type = 0;
                f->cell = 0;
                if (s < g->enemies.count) {
                    g->enemies.items[s].alive = 0;
                    g->enemies_extinguished++;
                }
                if (g->fires > 0)
                    --g->fires;
                ++g->flame_kills;
                od_game_update_clear(g);
            }
            flame_hit = 1;
        }
        if (flame_hit) {
            stop[i] = 1;
            stopped = 1;
            g->cascade_flame_mask |= 1u << i;
        }
        if (cell_at(g, p + i) == 14) { /* fire in the destination row */
            /* Puts the fire out and queues the column's one effect entry; the
             * stone keeps its tile, so the fragmentation below lets it go
             * on. Measured on the owner's routes: a ten-cell bar on ten fires
             * queues ten cells (round 1-6); a stone on a fire stays in that
             * cell with one entry (round 8-5). */
            quench(g, p + i);
            if (g->drop_tiles[i] != 0x0c)
                g->drop_tiles[i] = 0;
            stop[i] = fire[i] = 1;
            stopped = 1;
            destroyed = 1; /* the queued entry holds the landing too */
            continue;
        }
        unsigned q = below_cell(p + i);
        int below = cell_at(g, q);
        if (below == 15 || below == 16) {
            stop[i] = 1;
            stopped = 1;
        } else if (off_screen && g->drop_tiles[i] != 0x0c) {
            /* The span left the play field of a scrolling round. */
            stop[i] = 1;
            stopped = 1;
        }
    }
    if (!stopped) {
        /* Only a solid (or Hero's cell) in the row below lands the span. */
        int blocked = 0;
        for (unsigned i = 0; i < w; ++i) {
            unsigned q = below_cell(p + i);
            int below = cell_at(g, q);
            if ((below != 0 && below != 14) ||
                (int)q == cell_index(g->hero_target_x, g->hero_target_y)) {
                blocked = 1;
                break;
            }
        }
        if (!blocked) { /* still falling: nothing is written to the board */
            g->cascade_phase = 0;
            g->last_ice_drops += (int)w;
            return;
        }
        for (unsigned i = 0; i < w; ++i)
            put_cell(g, p + i, g->drop_tiles[i]);
        g->cascade_state = 0x80;
        emit_sound(g, 0x59);     /* the span lands on support */
        g->last_ice_drops += (int)w;
        return;
    }
    /* Fragmentation, right to left. A stopped column whose tile is stone is
     * treated as continuing. Stopped columns are destroyed and queue one
     * effect each; every contiguous continuing run is written at this row
     * (canonical end types) and re-queued when its row below is clear. */
    for (unsigned i = 0; i < w; ++i) {
        if (stop[i] && g->drop_tiles[i] == 0x0c)
            stop[i] = 0;
    }
    for (unsigned i = 0; i < w; ++i) {
        if (!stop[i])
            continue;
        /* A column stopped by the hazard queues its cell; one stopped by
         * fire already queued it when the fire was put out. */
        if (!fire[i])
            queue_effect(g);
        destroyed = 1;
        put_cell(g, p + i, 0);
    }
    for (unsigned start = 0; start < w;) {
        if (stop[start]) { ++start; continue; }
        unsigned end = start;
        while (end < w && !stop[end]) ++end;
        unsigned len = end - start;
        uint8_t norm[16];
        for (unsigned i = 0; i < len; ++i) norm[i] = g->drop_tiles[start + i];
        ice_normalize(norm, len);
        int clear = 1;
        unsigned b = below_cell(p + start);
        for (unsigned i = 0; i < len; ++i) {
            int c = cell_at(g, b + i);
            if ((c != 0 && c != 14) ||
                (int)(b + i) == cell_index(g->hero_target_x, g->hero_target_y)) {
                clear = 0;
                break;
            }
        }
        for (unsigned i = 0; i < len; ++i)
            put_cell(g, p + start + i, norm[i]);
        if (clear && g->drop_count < 16) {
            unsigned n = g->drop_count++;
            g->drop_cells[n] = (uint8_t)(p + start);
            g->drop_widths[n] = (uint8_t)len;
            for (unsigned i = 0; i < len; ++i) g->drop_tiles_q[n][i] = norm[i];
        }
        start = end;
    }
    /* A destroyed column raises the effect freeze bit and holds in the
     * effect-wait state until the drain clears it; otherwise the task pops
     * the next candidate. */
    g->cascade_state = destroyed ? 0x83 : 0x80;
    g->last_ice_drops += (int)w;
}

/* One moving-block machine step. Both slots (ice and stone) share this
 * shape; only the restored tile (ice vs stone), the fire landing (ice is
 * consumed; the stone survives and lands) and the hazard below (ice is
 * destroyed; the stone is not) differ, selected by `b->material`. Init and
 * termination are distinct phases. */
/* Destroying an ice block: the cell is cleared, the machine terminates and
 * the cell enters the effect queue with the effect freeze bit set, without
 * the fire-count decrement of a quenched fire. The drain pops it four ticks
 * later and requests the effect sound. */
static void hazard_destroy(od_game *g, od_block *b, unsigned p)
{
    put_cell(g, p, 0);
    b->state = 4;
    b->wait = 2;
    queue_effect(g);
    g->hazard_effect = p + 1u;
}

static unsigned abs_diff8(unsigned a, unsigned b)
{
    return (a > b ? a - b : b - a) & 0xff;
}

/* Contact of a moving block with the flame slots, called by the moving-block
 * machines on every 2-pixel step, BEFORE the cell-centre test: the flame
 * slots are walked from the last to the first and compared with the block's
 * pixel centre.
 *   sliding: |blockY - flameY| < 8 and |blockX - flameX| < 2
 *   falling: |blockX - flameX| < 8 and |blockY - flameY| < 2
 * The first slot in range is cleared and its cell is cleared, the fire count
 * is decremented and the cell is queued for the effect drain. Ice is consumed
 * (it enters its end state) and the handler is abandoned; the stone carries
 * on. A falling ice block that meets the resistant flame type only dies
 * itself and the flame survives. The cell cleared is the flame's own for a
 * sliding block and the block's own for a falling one. Returns 1 when the ice
 * machine must stop this step. */
static int flame_contact(od_game *g, od_block *b, int falling)
{
    int stone = b->material == 0x0C;
    for (int i = g->flame_count - 1; i >= 0; i--) {
        od_flame *f = &g->flames[i];
        if (!f->type)
            continue;
        unsigned fy = f->state[4], fx = f->state[7];
        int hit = falling
            ? abs_diff8((unsigned)b->px, fx) < 8 && abs_diff8((unsigned)b->py, fy) < 2
            : abs_diff8((unsigned)b->py, fy) < 8 && abs_diff8((unsigned)b->px, fx) < 2;
        if (!hit)
            continue;
        unsigned own = ((unsigned)b->py & 0xf0) | ((unsigned)b->px >> 4);
        unsigned cell = falling ? own : ((fy & 0xf0) | (fx >> 4));
        if (falling && !stone && f->type == 3) {
            hazard_destroy(g, b, own % 240u); /* the block dies, the flame resists */
            return 1;
        }
        f->type = 0;
        f->cell = 0;
        if (i < g->enemies.count) {
            g->enemies.items[i].alive = 0;
            g->enemies_extinguished++;
        }
        if (cell < OD_GRID_CELLS)
            put_cell(g, cell, 0);
        if (g->fires > 0)
            --g->fires;
        ++g->flame_kills;
        queue_effect(g);
        g->hazard_effect = (cell % 240u) + 1u;
        od_game_update_clear(g);
        if (stone)
            return 0;
        b->state = 4;
        b->wait = 2;
        return 1;
    }
    return 0;
}

static void block_step(od_game *g, od_block *b)
{
    if (!b->active) return;
    if (!b->state) {
        b->state = b->dx > 0 ? 1 : 2;
        b->wait = 4;
        b->px = b->cell_x * 16 + 8;
        b->py = b->cell_y * 16 + 8;
        return;
    }
    if (b->wait && --b->wait) return;
    if (b->state == 4) {
        b->active = b->state = 0;
        b->px = b->py = 0; /* the block leaves the field */
        support_changed(g, cell_index(b->cell_x, b->cell_y), 1);
        return;
    }
    int falling = b->state == 3;
    if (falling) b->py = (b->py + 2) % 240;
    else b->px += 2 * b->dx;
    if (flame_contact(g, b, falling)) return;
    if (((falling ? b->py : b->px) & 15) != 8) return;
    unsigned p = cell_index(b->px / 16, b->py / 16);
    ++g->last_slide_cells;
    int stone = b->material == 0x0C;
    unsigned material = (unsigned)b->material;
    if (cell_at(g, p) == 14) {
        quench(g, p);
        /* Ice falling onto a fire is consumed (no cell write). A stone
         * quenches and keeps falling into the support. */
        if (!falling || !stone) {
            if (!falling) support_changed(g, p, 1);
            b->state = 4;
            b->wait = 2;
            return;
        }
    }
    /* Ice only: on a scrolling round a block whose cell is below the play
     * field is consumed before the rest of the cell test. */
    if (!stone && scroll_off_screen(g, p)) {
        hazard_destroy(g, b, p); /* consumed outside the field */
        return;
    }
    /* Ice machine only: a leftward block whose left neighbour is the boss
     * hazard registers the boss's hit and is consumed. The stone machine has
     * no such test, and the rightward ice path never looks left. */
    if (!stone && !falling && b->dx < 0) {
        unsigned left = p ? p - 1u : 239u;
        if (cell_at(g, left) == 0x10) {
            g->boss_hit = 1;
            hazard_destroy(g, b, p); /* the hit consumes the block */
            return;
        }
    }
    int below = cell_at(g, below_cell(p));
    /* Only ice is destroyed by a hazard below; the stone has no hazard case
     * and always lands as stone. */
    if (!stone && (below == 15 || below == 16)) {
        hazard_destroy(g, b, p);
        return;
    }
    if (passable(below)) {
        if (!falling) {
            b->state = 3;
            b->wait = 4;
            emit_sound(g, 0x58); /* the pushed block falls */
        }
    } else if (falling || !passable(od_game_cell(g, b->px / 16 + b->dx,
                                                b->py / 16))) {
        put_cell(g, p, material); /* the block lands as its own material */
        b->state = 4;
        b->wait = 2;
        /* The fall handler's landing requests its sound. The non-falling
         * slide-in writes the cell silently. */
        if (falling) emit_sound(g, 0x59);
    }
}

/* The ice slot: the scattered public fields are loaded into the generic
 * record, stepped, and stored back, so the public `pending*`/`ice_*`
 * observables and the presentation layer's `ice_state` transition are
 * unchanged. */
static void ice_tick(od_game *g)
{
    if (!g->pending) return;
    od_block b;
    b.active = g->pending;
    b.state = g->ice_state;
    b.wait = g->ice_wait;
    b.px = g->ice_px;
    b.py = g->ice_py;
    b.dx = g->pending_dx;
    b.cell_x = g->pending_x;
    b.cell_y = g->pending_y;
    /* The injection path can put a stone in this slot; a real push of ice
     * leaves this at 0 and defaults to ice. */
    b.material = g->pending_material ? g->pending_material : 5;
    block_step(g, &b);
    g->pending = b.active;
    g->ice_state = b.state;
    g->ice_wait = b.wait;
    g->ice_px = b.px;
    g->ice_py = b.py;
    g->pending_material = b.material;
}

/* The stone slot: a second, independent machine, so an ice block and a stone
 * can be in flight simultaneously. While the ice slot is free a lone stone
 * also publishes into the public `ice_*` fields, which is exactly the
 * single-slot behaviour the presentation layer already consumed; when both
 * run, each keeps its own record. */
static void stone_tick(od_game *g)
{
    block_step(g, &g->stone);
    if (!g->pending) {
        g->ice_state = g->stone.state;
        g->ice_wait = g->stone.wait;
        g->ice_px = g->stone.px;
        g->ice_py = g->stone.py;
    }
}

/* Hero's action roots (40 entries, two bytes each) and the action programs
 * come from the content package (engine tables). The interpreter in
 * `hero_animate` executes them opcode by opcode. Program pointers are stored
 * relative to this origin. */
#define OD_HERO_BASE 40960u

static unsigned hero_word(unsigned variant, unsigned addr)
{
    const od_engine_tables *t = od_engine_tables_get();
    if (t == NULL)
        return 0;
    const uint8_t *p = (variant == 5) ? t->hero_prog_covered : t->hero_prog_clear;
    unsigned i = addr - OD_HERO_BASE;
    return p[i] | ((unsigned)p[i + 1] << 8);
}

static unsigned hero_byte(unsigned variant, unsigned addr)
{
    const od_engine_tables *t = od_engine_tables_get();
    if (t == NULL)
        return 0;
    const uint8_t *p = (variant == 5) ? t->hero_prog_covered : t->hero_prog_clear;
    return p[addr - OD_HERO_BASE];
}

/* The base of that action's program. */
static unsigned hero_root(unsigned variant, unsigned action)
{
    if (action > 0x27) return 0;
    return hero_word(variant, OD_HERO_BASE + action * 2u);
}

/* Sign-extended operands of the 5-bit offset opcodes and the 8-bit pair opcodes. */
static int hero_sgn8(unsigned v) { return (v & 0x80) ? (int)v - 0x100 : (int)v; }

/* Chooses the program set: the covered set is used when the cell above Hero
 * is not empty OR a live flame slot stands in that very cell. */
static unsigned hero_set_above(const od_game *g)
{
    if (od_game_cell(g, g->hero_x, g->hero_y - 1))
        return 5;
    if (g->hero_y > 0) {
        unsigned above = cell_index(g->hero_x, g->hero_y - 1);
        for (int i = g->flame_count - 1; i >= 0; i--)
            if (g->flames[i].type && g->flames[i].cell == above)
                return 5;
    }
    return 4;
}

static void hero_start(od_game *g, unsigned action, int x, int y)
{
    g->hero_action = action;
    g->hero_active = 1;
    g->hero_phase = g->hero_wait = 0;
    g->hero_target_x = x;
    g->hero_target_y = y;
    /* The program set selected by the support check. */
    g->hero_set = hero_set_above(g);
    g->hero_seq = hero_root(g->hero_set, action);
}

static void hero_finish(od_game *g)
{
    g->hero_x = g->hero_target_x;
    g->hero_y = g->hero_target_y;
    g->hero_ox = g->hero_oy = 0;
    g->hero_active = g->hero_phase = g->hero_wait = 0;
    g->hero_seq = 0;
}

/* Level entry: the level header arms Hero's entry action, but the sequence
 * task does not run it until the intro ends. On the solve route of round 1-1
 * the level materialises with the entry action armed and all cursors zero,
 * and the program's first pose lands 21 ticks later. The level-start routine
 * arms on the materialisation frame, yields for 20 frames, and the play loop
 * adds its per-frame yield, so the first sequence call is the 21st tick. The
 * same zeroed entry state is published for the password routes. */
#define OD_ENTRY_HOLD 21

static void hero_entry_start(od_game *g)
{
    g->hero_action = 0x1D;
    g->hero_active = 1;
    g->hero_phase = g->hero_wait = 0;
    g->hero_pose = 0;
    g->hero_target_x = g->hero_x;
    g->hero_target_y = g->hero_y;
    /* The program set selected by the support check, exactly as hero_start
     * does for every action. */
    g->hero_set = hero_set_above(g);
    g->hero_seq = hero_root(g->hero_set, 0x1D);
    g->hero_ox = g->hero_oy = 0;
    g->entry_hold = OD_ENTRY_HOLD;
    g->hero_px = g->hero_py = 0;
}

void od_game_entry_finish(od_game *g)
{
    if (g->entry_hold == 0 && g->hero_action != 0x1D)
        return;
    g->entry_hold = 0;
    g->idle_timer = OD_IDLE_FRAMES; /* the play loop starts here */
    g->scroll_timer = g->scroll_period;
    g->hero_active = 0;
    g->hero_action = 0;
    g->hero_phase = g->hero_wait = 0;
    g->hero_ox = g->hero_oy = 0;
    g->hero_seq = 0;
    g->hero_pose = 43460u; /* idle rest pose */
    hero_pixels(g);
}

/* From travel direction `dir` (0 right, 1 left, 2 up, 3 down) inside a
 * connector of `type` (6..11), the next cell lies in `*mv` and must be one of
 * the types in `*accept`. Returns 0 when that connector has no continuation.
 * The same tables serve the one-step interaction. */
static int pipe_next(unsigned dir, unsigned type, unsigned *mv, unsigned *accept)
{
    unsigned t6 = 1u << 6, t7 = 1u << 7, t8 = 1u << 8;
    unsigned t9 = 1u << 9, t10 = 1u << 10, t11 = 1u << 11;
    switch (dir) {
    case 0: /* travelling right */
        if (type == 7)  { *mv = 0; *accept = t7 | t9 | t11; return 1; }
        if (type == 9)  { *mv = 3; *accept = t6 | t10 | t11; return 1; }
        if (type == 11) { *mv = 2; *accept = t6 | t8 | t9; return 1; }
        return 0;
    case 1: /* travelling left */
        if (type == 7)  { *mv = 1; *accept = t7 | t8 | t10; return 1; }
        if (type == 8)  { *mv = 3; *accept = t6 | t10 | t11; return 1; }
        if (type == 10) { *mv = 2; *accept = t6 | t8 | t9; return 1; }
        return 0;
    case 2: /* travelling up */
        if (type == 6)  { *mv = 2; *accept = t6 | t8 | t9; return 1; }
        if (type == 8)  { *mv = 0; *accept = t7 | t9 | t11; return 1; }
        if (type == 9)  { *mv = 1; *accept = t7 | t8 | t10; return 1; }
        return 0;
    default: /* 3: travelling down */
        if (type == 6)  { *mv = 3; *accept = t6 | t10 | t11; return 1; }
        if (type == 10) { *mv = 0; *accept = t7 | t9 | t11; return 1; }
        if (type == 11) { *mv = 1; *accept = t7 | t8 | t10; return 1; }
        return 0;
    }
}

/* Neighbour grid index in movement direction `mv`, with the original's
 * vertical wrap (moving down from the last row goes to row 0; moving up from
 * row 0 reaches the bottom). */
static unsigned pipe_neighbour(unsigned p, unsigned mv)
{
    switch (mv) {
    case 0: return p + 1;
    case 1: return p - 1;
    case 2: return (p < 16) ? (p + 224) : (p - 16);
    default: { unsigned q = p + 16; return (q >= 240) ? (q - 240) : q; }
    }
}

/* Walk the connector network from cell `pos` of `type` entered travelling
 * `dir` until the next cell is empty. On success `*dest` is that empty cell,
 * `*exit_dir` the direction just travelled and `*last_pos` the last connector
 * visited. On rejection the caller leaves Hero in place and plays the
 * blocked variant. */
static int pipe_walk(const od_game *g, unsigned pos, unsigned type, unsigned dir,
                     unsigned *dest, unsigned *exit_dir, unsigned *last_pos)
{
    unsigned p = pos, t = type, d = dir;
    for (int guard = 0; guard < 64; ++guard) {
        unsigned mv, accept;
        if (!pipe_next(d, t, &mv, &accept)) return 0;
        unsigned q = pipe_neighbour(p, mv);
        if (q >= 240) return 0;
        int c = cell_at(g, q);
        if (c == 0) { *dest = q; *exit_dir = mv; *last_pos = p; return 1; }
        if (c > 0 && c < 16 && (accept & (1u << c))) {
            p = q; t = (unsigned)c; d = mv; continue;
        }
        return 0;
    }
    return 0;
}

/* One interaction step of the connector opcode. Advances the walk state on a
 * connector; returns 0 when the next cell is not one. */
static int pipe_step(const od_game *g, unsigned *pcell, unsigned *pdir,
                     unsigned *pkind, unsigned *pprev)
{
    unsigned p = *pcell, d = *pdir, t = *pkind, mv, accept;
    if (pipe_next(d, t, &mv, &accept)) {
        unsigned q = pipe_neighbour(p, mv);
        if (q < 240) {
            int c = cell_at(g, q);
            if (c > 0 && c < 16 && (accept & (1u << c))) {
                *pprev = p; *pcell = q; *pdir = mv; *pkind = (unsigned)c;
                return 1;
            }
        }
    }
    *pprev = p;  /* remember the last cell and clear the walk: the opcode finishes */
    *pcell = 0;
    return 0;
}

/* Resolve one connector entry into a destination and action (`base`
 * traverse / `base+1` blocked), then start the sequence. */
static void pipe_entry(od_game *g, int cx, int cy, int type, unsigned dir,
                       unsigned base)
{
    unsigned pos = (unsigned)(cy * 16 + cx);
    unsigned dest, exit_dir, last;
    /* Store the entry cell/direction/type before walking; a rejection clears
     * them. The interaction loop then steps from there. */
    g->pipe_cell = pos;
    g->pipe_dir = dir;
    g->pipe_kind = (unsigned)type;
    g->pipe_prev = 0;
    if (pipe_walk(g, pos, (unsigned)type, dir, &dest, &exit_dir, &last)) {
        g->pipe_last = last;
        g->pipe_exit = exit_dir;
        hero_start(g, base, (int)(dest % 16), (int)(dest / 16));
    } else {
        g->pipe_cell = g->pipe_dir = g->pipe_kind = g->pipe_prev = 0;
        hero_start(g, base + 1, g->hero_x, g->hero_y);
    }
}

/* Held-button mask read by the direction branches and the network, in the
 * original bit layout: Right=1, Left=2, Down=4, Up=8, Start=16, Select=32,
 * B=64, A=128. The native enum order is different, so it is translated
 * before the mask. */
static unsigned hero_held(unsigned buttons)
{
    unsigned nes = 0;
    if (buttons & OD_BTN_A)      nes |= 0x80;
    if (buttons & OD_BTN_B)      nes |= 0x40;
    if (buttons & OD_BTN_SELECT) nes |= 0x20;
    if (buttons & OD_BTN_START)  nes |= 0x10;
    if (buttons & OD_BTN_UP)     nes |= 0x08;
    if (buttons & OD_BTN_DOWN)   nes |= 0x04;
    if (buttons & OD_BTN_LEFT)   nes |= 0x02;
    if (buttons & OD_BTN_RIGHT)  nes |= 0x01;
    return nes;
}

/* The entity sequence VM. `hero_seq` is the program base, `hero_phase` the
 * cursor, `hero_wait` the pose timer, `hero_ox/oy` the held offsets and
 * `hero_pose` the metasprite pointer. Each call runs opcodes until a wait
 * token (0x40-0x7F) stores the cursor and returns; a non-zero wait is
 * decremented first and continues on expiry. */
static void hero_animate(od_game *g, unsigned buttons)
{
    if (!g->hero_active) return;
    /* Level-entry introduction: hold the entry program for the first 21
     * ticks, then run it on the 21st. */
    if (g->entry_hold) {
        if (--g->entry_hold) {
            /* The idle countdown is reloaded after the 20-frame yield, one
             * tick before the first entry pose. */
            if (g->entry_hold == 1) {
                g->idle_timer = OD_IDLE_FRAMES;
                /* Just before, the auto-scroll timer takes its period. */
                g->scroll_timer = g->scroll_period;
            }
            return;
        }
    }
    if (g->hero_wait && --g->hero_wait) return;
    unsigned base = g->hero_seq, y = g->hero_phase;
    for (int guard = 0; guard < 256; ++guard) {
        unsigned op = hero_byte(g->hero_set, base + y);
        ++y;
        if (op < 0x20) { g->hero_ox = (int)(op & 15) - (int)(op & 16); continue; }
        if (op < 0x40) { op -= 0x20; g->hero_oy = (int)(op & 15) - (int)(op & 16); continue; }
        if (op < 0x80) { g->hero_seq = base; g->hero_phase = y; g->hero_wait = op & 0x3F; return; }
        if (op < 0xF0) { g->hero_pose = (op << 8) | hero_byte(g->hero_set, base + y); ++y; continue; }
        switch ((~op) & 0x0F) {
        case 0x00: /* end: commit the pending action, clear cursor/offsets */
            if (g->hero_action == 0x18) g->celebration_done = 1;
            hero_finish(g);
            return;
        case 0x01: { /* jump lo hi: another program, cursor zero */
            unsigned lo = hero_byte(g->hero_set, base + y);
            unsigned hi = hero_byte(g->hero_set, base + y + 1);
            base = lo | (hi << 8);
            y = 0;
            continue;
        }
        case 0x02: case 0x03: { /* mask state lo hi: direction branch */
            unsigned mask = hero_byte(g->hero_set, base + y);
            ++y;
            unsigned held = hero_held(buttons) & mask;
            unsigned sel = (g->facing > 0) ? (held & 0x01) : (held & 0x02);
            int take = (op == 0xFD) ? (sel != 0) : (sel == 0);
            if (!take) { y += 3; continue; }
            unsigned state = hero_byte(g->hero_set, base + y);
            ++y;
            unsigned lo = hero_byte(g->hero_set, base + y);
            ++y;
            unsigned hi = hero_byte(g->hero_set, base + y);
            g->hero_action = state;
            base = lo | (hi << 8);
            y = 0;
            continue;
        }
        case 0x04: /* x y: set both full 8-bit offsets */
            g->hero_ox = hero_sgn8(hero_byte(g->hero_set, base + y));
            g->hero_oy = hero_sgn8(hero_byte(g->hero_set, base + y + 1));
            y += 2;
            continue;
        case 0x05: /* magic update: previous cell becomes the new one */
            g->magic_pending = 1;
            continue;
        case 0x06: /* enqueue the current cell as a support span */
            support_changed(g, cell_index(g->hero_x, g->hero_y), 1);
            continue;
        case 0x07: { /* rebase on the last connector, branch on the exit direction */
            unsigned exit_dir_sel = g->pipe_exit;
            base += y;
            g->hero_x = (int)(g->pipe_last % 16);
            g->hero_y = (int)(g->pipe_last / 16);
            unsigned lo = hero_byte(g->hero_set, base + exit_dir_sel * 2);
            unsigned hi = hero_byte(g->hero_set, base + exit_dir_sel * 2 + 1);
            base = lo | (hi << 8);
            g->facing = (exit_dir_sel & 1) ? -1 : 1;
            g->pipe_last = g->pipe_exit = 0;
            y = 0;
            continue;
        }
        case 0x08: /* x y: add to both offsets */
            g->hero_ox += hero_sgn8(hero_byte(g->hero_set, base + y));
            g->hero_oy += hero_sgn8(hero_byte(g->hero_set, base + y + 1));
            y += 2;
            continue;
        case 0x09: { /* sound request */
            unsigned id = hero_byte(g->hero_set, base + y);
            /* The level-entry program carries a request for the level-start
             * cue after its sixth pose. The scene agenda already emits that
             * cue from the app at its measured frame; re-emitting it here
             * would land one app tick early and duplicate it, so the core
             * defers this one startup cue to the app. Every other request is
             * unchanged. The arena entry has no app agenda: there
             * `entry_cue` keeps it here. */
            if (!(g->hero_action == 0x1d && id == 0x29) || g->entry_cue)
                emit_sound(g, id);
            ++y;
            continue;
        }
        case 0x0A: /* death hand-off */
            hero_die(g);
            continue;
        case 0x0B: /* world music selection (the audio task owns it) */
            continue;
        case 0x0C: /* connector interaction through the walk state */
            if (!g->pipe_active) g->pipe_active = 1; /* interaction flag */
            if (g->pipe_cell) {
                pipe_step(g, &g->pipe_cell, &g->pipe_dir, &g->pipe_kind, &g->pipe_prev);
                g->hero_seq = base;
                g->hero_phase = y - 1; /* re-read the same opcode next call */
                g->hero_wait = 6;
                return;
            }
            g->pipe_cell = g->pipe_dir = g->pipe_kind = g->pipe_prev = 0;
            g->pipe_active = 0;
            continue;
        case 0x0D: /* invert orientation */
            g->facing = -g->facing;
            continue;
        default: /* reserved opcodes: keep reading */
            continue;
        }
    }
    hero_finish(g);
}

/* The connection transforms for neighbouring ice are loaded data
 * (engine/editor-data.json). */

static void hero_magic(od_game *g)
{
    const od_editor_data *tables = od_editor_data_get();
    if (!tables) { (void)od_editor_data_view(); return; }
    int x = g->hero_x + g->facing, y = (g->hero_y + 1) % 15;
    int c = od_game_cell(g, x, y);
    unsigned action = 0x1c;
    /* A destination cell equal to the cell of a live flame slot rejects the
     * magic like a blocked one (the failure action). */
    int flame_there = 0;
    for (int s = 0; s < g->flame_count; s++)
        if (g->flames[s].type && g->flames[s].cell == cell_index(x, y))
            flame_there = 1;
    if (in_bounds(x, y) && !flame_there && od_game_cell(g, x, (y + 1) % 15) < 15) {
        if (!c) {
            int neighbour = od_game_cell(g, x + g->facing, y);
            g->cells[y][x] = (uint8_t)(passable(neighbour) || neighbour == 16 ?
                                                       (g->facing > 0 ? 4 : 3) : 2);
            ++g->ice_made;
            action = 6;
        } else if (c >= 2 && c <= 5) {
            g->cells[y][x] = 0;
            ++g->ice_removed;
            action = 7;
        }
        if (action != 0x1c) {
            unsigned table = action == 6 ? 2 : 0;
            for (int d = -1; d <= 1; d += 2) {
                int n = od_game_cell(g, x + d, y);
                if (in_bounds(x + d, y) && n < 16)
                    g->cells[y][x + d] = tables->ice_connections[table + (d < 0)][n];
            }
            g->magic_cell = cell_index(x, y);
            g->magic_redraw = 1;
        }
    }
    hero_start(g, action, g->hero_x, g->hero_y);
}

/* New decisions are suppressed while any of three freeze conditions holds: a
 * fire-extinction effect is queued, a tile write is pending (the moving ice
 * block) or a support/candidate span is queued (cascade). The frame clock and
 * an in-flight sequence keep running. */
static int input_frozen(const od_game *g)
{
    return g->pending || g->stone.active || g->cascade_task || g->effect_active;
}

static void hero_decide(od_game *g, unsigned buttons)
{
    /* A running program blocks the decision, except the idle program, which
     * input, a fall or a hazard interrupt. */
    if (g->hero_active && g->hero_action != OD_ACTION_IDLE) return;
    int x = g->hero_x, y = g->hero_y;
    int below = od_game_cell(g, x, (y + 1) % 15);
    /* The fall check comes before input and also runs during selective input
     * freeze. */
    if (passable(below)) {
        int next = od_game_cell(g, x, (y + 2) % 15);
        unsigned a = below == 14 || next >= 15 ? 0x1a : passable(next) ? 10 : 5;
        /* A long fall that is not already running requests its sound. */
        if (a == 10 && g->hero_action != 10) emit_sound(g, 0x53);
        hero_start(g, a, x, (y + 1) % 15);
        ++g->falls;
        return;
    }
    if (below >= 15) { hero_start(g, 0x1f, x, y); return; }
    /* Still before the freeze test: landing from a fall returns the action to
     * 0 and requests the landing sound. */
    if (g->hero_action == 5 || g->hero_action == 10) {
        emit_sound(g, 0x54);
        hero_start(g, 0, x, y);
        return;
    }
    /* The input freeze and the boss's live attack share one path: no new
     * decision. The idle program keeps running there; any other idle Hero
     * restarts program 0. */
    if (input_frozen(g) || g->attack_lock) {
        if (g->hero_action != OD_ACTION_IDLE) hero_start(g, 0, x, y);
        return;
    }
    if (buttons & (OD_BTN_A | OD_BTN_B | OD_BTN_LEFT | OD_BTN_RIGHT |
                   OD_BTN_UP | OD_BTN_DOWN)) {
        /* A taken input reloads the idle countdown and drops the running
         * program (only the idle one can be running here); the current action
         * number is kept, so a walk out of idle continues as a plain walk. */
        g->idle_timer = OD_IDLE_FRAMES;
        g->hero_active = g->hero_phase = g->hero_wait = 0;
        g->hero_seq = 0;
    } else {
        /* No input: the idle program keeps running; otherwise the neutral
         * program starts, or the idle one once the countdown has run out. */
        if (g->hero_action != OD_ACTION_IDLE)
            hero_start(g, g->idle_timer ? 0 : OD_ACTION_IDLE, x, y);
        return;
    }
    if (buttons & (OD_BTN_A | OD_BTN_B)) { hero_magic(g); return; }
    unsigned horizontal = buttons & (OD_BTN_LEFT | OD_BTN_RIGHT);
    if (horizontal == (OD_BTN_LEFT | OD_BTN_RIGHT)) return; /* opposing directions: ignored */
    if (!horizontal) {
        /* UP/DOWN into a connector traverses it; it is a pipe request, not a
         * jump. DOWN is tested first. */
        unsigned vertical = buttons & (OD_BTN_UP | OD_BTN_DOWN);
        if (vertical & OD_BTN_DOWN) {
            int by = (y + 1) % 15, c = od_game_cell(g, x, by);
            if (c == 6 || c == 10 || c == 11) {
                pipe_entry(g, x, by, c, 3, 0x0D);
                return;
            }
        } else if (vertical & OD_BTN_UP) {
            int by = (y + 14) % 15, c = od_game_cell(g, x, by);
            if (c == 6 || c == 8 || c == 9) {
                pipe_entry(g, x, by, c, 2, 0x0F);
                return;
            }
        }
        hero_start(g, 0, x, y);
        return;
    }
    int dir = horizontal == OD_BTN_RIGHT ? 1 : -1;
    if (dir != g->facing) { hero_start(g, 0x27, x, y); return; }
    int tx = x + dir;
    if (!in_bounds(tx, y) || tx == 0) { hero_start(g, 0, x, y); return; }
    int c = od_game_cell(g, tx, y);
    int type = c & 15; /* the lateral dispatch only looks at the low nibble */

    /* Lateral dispatch on the destination cell's low nibble. Each nibble maps
     * to one of four handlers; the three that cannot leave the grid untouched
     * are implemented, the connector walks are handled by pipe_entry. */
    int push2 = type == 2 || type == 3 || type == 4 || type == 5 || type == 0xC;
    /* A blocked push falls back to the climb handler. */
    int ascend = type == 1 || type == 6 || type == 0xD || push2 ||
                 (dir > 0 ? (type == 8 || type == 10) : (type == 9 || type == 11));
    int connector = type == 7 ||
                    (dir > 0 ? (type == 9 || type == 11) : (type == 8 || type == 10));

    if (type == 0) { /* open cell: walk, drop or land on danger */
        int floor = od_game_cell(g, tx, (y + 1) % 15);
        unsigned a = g->hero_action ? 2 : 1;
        if (!floor) {
            int danger = od_game_cell(g, tx, (y + 2) % 15) >= 15;
            a = od_game_cell(g, tx, y - 1) ? (danger ? 0x21 : 0x20) : (danger ? 0x1e : 4);
            hero_start(g, a, tx, (y + 1) % 15);
        } else if (floor == 14) {
            hero_start(g, od_game_cell(g, tx, y - 1) ? 0x21 : 0x1e, tx, (y + 1) % 15);
        } else if (floor >= 15) {
            hero_start(g, 0x19, tx, y);
        } else {
            if (!!od_game_cell(g, x, y - 1) != !!od_game_cell(g, tx, y - 1)) a = 0x16;
            hero_start(g, a, tx, y);
        }
        return;
    }
    /* Push two cells ahead when it is empty or fire, else climb. The stone
     * type goes to the stone machine, the ice types to the ice machine; both
     * are action 8 here and the slot only decides the landed material. */
    if (push2 && in_bounds(tx + dir, y) && tx + dir != 0 &&
        passable(od_game_cell(g, tx + dir, y))) {
        /* Stone and ice are independent slots; a busy slot just idles. */
        if (type == 0xC) {
            if (g->stone.active) { hero_start(g, 0, x, y); return; }
            g->stone.active = 1;
            g->stone.state = 0;
            g->stone.px = g->stone.py = 0;
            g->stone.dx = dir;
            g->stone.cell_x = tx;
            g->stone.cell_y = y;
            g->stone.material = 0x0C;
            g->stone.wait = 0;
        } else {
            if (g->pending) { hero_start(g, 0, x, y); return; }
            g->pending = 1;
            g->pending_x = tx; g->pending_y = y; g->pending_dx = dir;
            g->pending_material = 0x05;
            g->ice_state = 0;
        }
        g->cells[y][tx] = 0;
        g->last_slide_cells = 0;
        ++g->pushes;
        hero_start(g, 8, x, y);
        return;
    }
    /* Climb into the cell above when the way is clear. The cell above Hero
     * must be empty; if the diagonal above in the travel direction is a
     * connector, the climb hands over to the pipe network instead of stepping
     * up. The base sequence is the taller one when the cell two rows above
     * Hero is occupied, else the shorter one; a rejected climb adds one. */
    if (ascend && y > 0 && !od_game_cell(g, x, y - 1)) {
        int diag = od_game_cell(g, tx, y - 1);
        int diag_pipe = dir > 0 ? (diag == 7 || diag == 9 || diag == 11)
                                : (diag == 7 || diag == 8 || diag == 10);
        if (diag_pipe) {
            unsigned base = od_game_cell(g, x, y - 2) ? 0x24 : 0x11;
            pipe_entry(g, tx, y - 1, diag, dir > 0 ? 0u : 1u, base);
            return;
        }
        if (!diag) {
            unsigned a = od_game_cell(g, tx, y - 2) ?
                (od_game_cell(g, x, y - 2) ? 0x26 : 0x17) :
                (od_game_cell(g, x, y - 2) ? 0x23 : 3);
            hero_start(g, a, tx, y - 1);
            return;
        }
    }
    if (type == 14) { hero_start(g, 0x19, tx, y); return; } /* hazard cell */
    if (connector) {
        /* Resolve the connector network, so a pipe is never crossed as air.
         * A rejection keeps Hero in place and plays the blocked variant. */
        pipe_entry(g, tx, y, type, dir > 0 ? 0u : 1u, 0x0B);
        return;
    }
    /* The solid type, and any other nibble, idles in place. */
    hero_start(g, 0, x, y);
}

void od_game_tick(od_game *g, unsigned buttons)
{
    /* The existing void ABI cannot return a load status. Diagnose and leave
     * state untouched; startup must propagate the parser error instead. */
    if (!od_editor_data_get()) { (void)od_editor_data_view(); return; }
    ++g->frame;
    g->sound_request = 0;
    g->sound_count = 0;
    g->hero_flags = 0;
    g->boss_hit = 0;
    g->hazard_effect = 0;
    g->flame_kills = 0;
    g->cascade_flame_mask = 0;
    /* The idle countdown drops once per frame, before the frame's decision. */
    if (g->idle_timer) --g->idle_timer;
    if (g->game_over) return;
    /* The auto-scroll task runs first, once the play loop has started (its
     * timer is loaded on the tick before the first entry pose). */
    if (g->entry_hold <= 1)
        scroll_task(g);
    /* Decisions, then animation, then magic. An in-flight program finishes
     * while the high freeze bits inhibit new decisions; the frame clock never
     * stops. */
    int was_celebrated = g->celebration_done;
    if (g->cleared) {
        if (g->hero_action != 0x18 && !g->celebration_done) {
            hero_start(g, 0x18, g->hero_x, g->hero_y);
            /* Starting the celebration fades the theme, except in the last
             * round of the last world, where the clear and the boss's defeat
             * keep it. */
            if (g->level_index != 99)
                emit_sound(g, 0x02);
        }
    } else if (g->dead) {
        /* With the death flag set the decision task returns at once. The rest
         * of the loop still runs: the death program loops its two poses and
         * the flames animate in place until the failure band resolves it. */
    } else if (g->paused) {
        /* The pause flag is part of the same inhibit mask: an in-flight
         * program finishes, no new decision starts. */
    } else if (g->lethal && !input_frozen(g) && !g->retire_freeze) {
        /* A lethal contact needs every freeze bit but the lowest clear;
         * `retire_freeze` is the bit the arena's retirement raised before
         * this decision. */
        hero_death_start(g);
    } else hero_decide(g, buttons);
    hero_animate(g, buttons);
    /* Once the celebration has ended the level state runs a fixed exit tail
     * (palette fade) before handing over to the map scene. The tail is counted
     * from the tick AFTER the one that set `celebration_done`. */
    if (g->cleared && g->celebration_done && !g->level_exit && was_celebrated) {
        if (++g->exit_frames >= OD_LEVEL_EXIT_FRAMES) g->level_exit = 1;
    }
    hero_pixels(g);
    if (g->magic_pending) {
        /* When the magic update left the cell empty, release it so the
         * lateral ice freed by the removal enters the cascade directly. */
        if (!cell_at(g, g->magic_cell)) magic_release(g, g->magic_cell);
        g->magic_pending = 0;
        g->magic_redraw = 0; /* the cell writer redraws it now */
    }
    /* The support task runs before the pushed-object task: a newly queued
     * span is first seen next tick. */
    cascade_tick(g);
    ice_tick(g);
    stone_tick(g);
    /* Convert a scheduled jar cell into the hazard and request its sound. */
    if (g->jar_pending) {
        put_cell(g, g->jar_cell, 0x0F);
        g->jar_pending = 0;
        emit_sound(g, 0x5a);
    }
    if (g->effect_active) { /* the queue is tested BEFORE its decrement/pop */
        if (!g->effect_count) {
            g->effect_active = 0;
            g->effect_wait = 4;
        } else if (--g->effect_wait == 0) {
            --g->effect_count;
            g->effect_wait = 4;
            /* The effect drain pops one cell, flags it for redraw and
             * requests the matching sound. */
            emit_sound(g, 0x5b);
        }
    }
    g->cooldown = (int)g->hero_wait;
    g->pending_wait = g->ice_wait;
    /* The flame scheduler sees the live freeze bits. With them set the
     * controller still animates but does not translate or test contact, so
     * the call is not gated by an in-flight block. */
    if (!g->cleared) {
        g->freeze = input_frozen(g) ? 1u : 0u;
        od_game_tick_enemies(g);
    }
    /* Expose the input gate: it is the state that will gate the next tick. */
    g->freeze = input_frozen(g) ? 1u : 0u;
}

/* Compatibility convenience: run a single requested action through the same
 * ticks, including a preceding turn and autonomous falling. No alternate physics. */
int od_game_action(od_game *g, unsigned buttons)
{
    if (!od_editor_data_get()) { (void)od_editor_data_view(); return 0; }
    if (!buttons || g->dead || g->game_over) return 0;
    od_game_recount_fires(g);
    od_game_tick(g, buttons);
    int turn = g->hero_action == 0x27;
    do {
        od_game_tick(g, buttons);
    } while (!g->dead && !g->game_over &&
             (g->hero_active || g->pending || g->stone.active ||
              g->cascade_task || g->effect_active));
    if (turn) return od_game_action(g, buttons);
    return 1;
}
