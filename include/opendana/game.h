/* game.h - deterministic, headless game mechanics for the native rewrite.
 *
 * This is pure C11 with no SDL. It models one level at a time as a 16x15 grid
 * of cell values (the level format's encoding) plus Hero's cell, destination,
 * pixel offsets, pose and timed phase. The tick function is a fixed-step
 * state machine suitable for both scripted replays and the SDL front end.
 *
 * ============================================================================
 * MODEL
 * ----------------------------------------------------------------------------
 * Hero's action programs are executed opcode by opcode from the engine
 * tables (`hero_phase` is the cursor). Cell changes commit at sequence end;
 * offsets are held, never integrated as velocity. Gravity is autonomous. UP
 * requests a connector, not a free hop. Moving ice advances two pixels per
 * tick after its four-tick startup. Support changes enqueue spans; cascades
 * animate one piece/group at a time in LIFO order. A magic removal releases
 * the connected lateral ice runs that just lost their support straight into
 * the candidate queue, and the removed cell is re-queued as a support span
 * only when no lateral candidate exists. A falling group that meets a hazard
 * fragments; its surviving columns are written at the current row and held in
 * a wait state until the effect queue drains. These phases share the game
 * clock, including during input freezes.
 * Connector traversal is reconstructed for the lateral, vertical and
 * climb-into-connector entries; special-object arbitration remains
 * incomplete. The moving flames and the death trigger are exact:
 * `od_flame_tick` runs once per tick and the controller's lethal-contact bit
 * starts the death program.
 *
 * ============================================================================
 * MOVING FLAMES AND DEATH
 * ----------------------------------------------------------------------------
 * The moving flames of the level's object records (0x81 horizontal, 0x82
 * vertical, 0x83 resistant) are spawned on level start and advanced one tick
 * per game tick with the exact controller `od_flame_tick`, not the legacy
 * cell-step approximation. The eight slots keep the record's registration
 * order. The controller's contact test sets bit 4 of `hero_flags`, latched in
 * `lethal`; the next decision step turns it into the death program. The death
 * (`dead`) does not stop the level: Hero's program loops, the flames animate
 * in place and the scene clock runs until the failure band resolves it.
 * ============================================================================
 */
#ifndef OD_GAME_H
#define OD_GAME_H

#include "opendana/common.h"
#include "opendana/enemy.h"
#include "opendana/level.h"

/* Controller bits. */
typedef enum {
    OD_BTN_A      = 1u << 0,
    OD_BTN_B      = 1u << 1,
    OD_BTN_SELECT = 1u << 2,
    OD_BTN_START  = 1u << 3,
    OD_BTN_UP     = 1u << 4,
    OD_BTN_DOWN   = 1u << 5,
    OD_BTN_LEFT   = 1u << 6,
    OD_BTN_RIGHT  = 1u << 7
} od_button;

/* Cell values reuse the level format. */
#define OD_GAME_EMPTY 0x00
#define OD_GAME_SOLID 0x01
#define OD_GAME_ICE   0x05
#define OD_GAME_FIRE  0x0E

/* Legacy source compatibility only. Timing comes from phases, never this field.
 * A continuous walk has 16 one-tick poses plus its dispatch tick. */
#define OD_ACTION_PERIOD 17

/* LEGACY timing, no longer used by the game. It calibrated the horizontal
 * flame of round 2-10 to one cell every 45 frames; that was a documented
 * approximation. The game now advances every mobile flame with the exact
 * controller `od_flame_tick` (one tick per game tick). Kept only because
 * existing callers/tests reference the macro. */
#define OD_ENEMY_STEP_FRAMES 45

/* Slots of the flame controller. One per 0x81/0x82/0x83 command of the level
 * record, in registration order. */
#define OD_FLAME_MAX 8

/* Support-span stack size: none of its pushers checks a bound, so it can grow
 * past the 32 slots modelled here. The original never uses the extra range
 * during play (verified over the whole of round 10-10 and the whole arena
 * fight); further growth would overwrite unrelated buffers and is not
 * modelled. */
#define OD_SUPPORT_SPANS 32

/* `lives < 0` means the level is retried forever (what the original does
 * after a retry; no life counter was observed). A positive value is only a
 * diagnostic counter that each death decrements: the engine never turns it
 * into a game over, because the original has none. */
#define OD_HERO_UNLIMITED (-1)

/* Frames of the exit tail after the solved-level celebration ends (palette
 * fade, state hand-over, then the map scene). Measured on the solve route:
 * `level_exit` latches 18 frames after `celebration_done`. */
#define OD_LEVEL_EXIT_FRAMES 18

/* One moving-block machine record: state, wait and pixel centre plus the
 * originating cell and material. The engine keeps two independent slots, ice
 * and stone, so both can be in flight at once; `od_game` stores the ice slot
 * in its legacy `pending*`/`ice_*` fields and the stone slot here.
 * `material` is the ice value (5) or the stone value (12). */
typedef struct {
    int active;      /* slot in use */
    int state;       /* low 3 bits: 1 right, 2 left, 3 fall, 4 terminate */
    int wait;        /* ticks until the next step */
    int px, py;      /* centre in pixels */
    int dx;          /* push direction: +1 right, -1 left */
    int cell_x, cell_y; /* originating cell, used for the termination span */
    int material;    /* 5 ice, 12 stone */
} od_block;

typedef struct {
    uint8_t cells[OD_GRID_H][OD_GRID_W]; /* live cell values */
    int hero_x, hero_y;
    /* Hero's working state. Offsets are held positions, NOT velocities. No
     * fractional accumulator exists in this path. The cell commits at phase
     * end. */
    int hero_target_x, hero_target_y;
    int hero_px, hero_py, hero_ox, hero_oy;
    unsigned hero_action, hero_phase, hero_wait, hero_active, hero_set;
    unsigned hero_pose, celebration_done;
    /* Level-entry materialization: the level is loaded with Hero's entry
     * program already armed, but the original does not expose the pose and
     * pixels until the level intro ends. `entry_hold` counts those intro
     * ticks down before the entry program starts running. */
    unsigned entry_hold;
    /* Idle countdown: set when the play loop starts and on every decision
     * that takes an input, decremented once per frame. A decision with no
     * input and the countdown at zero starts the idle program. */
    unsigned idle_timer;
    /* Auto-scroll: a level whose object record holds the scroll command sets
     * the enabled flag and the period (the next byte). The play loop loads the
     * timer with the period; the scroll task (first of the frame, skipped
     * while paused or dead) counts it down every other frame, then requests
     * its sound and scrolls one row up, one pixel per frame: the offset goes
     * 0 -> 239..224 -> ... (mod 240). Hero's row and the playfield picture are
     * drawn with that scroll; the grid itself does not move. */
    unsigned scroll_period;   /* meaningful when scroll_enabled */
    int scroll_enabled;
    unsigned scroll_state;    /* bit 7 = the every-other-frame toggle */
    unsigned scroll_timer;
    unsigned scroll_y;        /* 0..239 */
    /* Solved-level exit tail: counted after `celebration_done`, latched in
     * `level_exit`. The app (main.c) uses `level_exit` to hand the frame over
     * to the map scene; the fade of those frames is presentation. */
    unsigned exit_frames;
    int level_exit;
    /* Pipe/connector traversal. The sequence pointer is an address in the
     * action-program window; `hero_phase` is the cursor. `pipe_last` and
     * `pipe_exit` are the continuation selector (last connector cell, exit
     * direction); `pipe_cell`, `pipe_dir`, `pipe_kind` and `pipe_prev` are the
     * one-cell stepper used by the interaction opcode. `pipe_active` mirrors
     * the interaction flag. */
    unsigned hero_seq;
    unsigned pipe_last, pipe_exit;
    unsigned pipe_cell, pipe_dir, pipe_kind, pipe_prev;
    unsigned pipe_active;
    /* Sound requests queued by this tick, in the order the routines called
     * for them. `sound_request` is the last one (kept for the existing
     * callers); `sound_count`/`sound_queue` keep the whole ordered list. Both
     * are reset at the start of every od_game_tick. */
    unsigned sound_request, magic_cell, magic_pending;
    /* The wand's decision commits its cells at once, but the cell writer only
     * redraws the target and its two neighbours when the program's update
     * opcode hands it over: 12 frames later on a creation, 11 on a removal.
     * Set by the wand, cleared by that step; presentation only. */
    unsigned magic_redraw;
    uint8_t sound_queue[8];
    unsigned sound_count;
    /* Moving ice is absent from cells until it lands. This is the ice slot;
     * `stone` below is the independent stone slot, so an ice block and a
     * stone can be in flight simultaneously. A lone stone is also mirrored
     * into these legacy fields (when the ice slot is free) so the
     * presentation layer keeps seeing the active block. */
    int ice_px, ice_py, ice_state, ice_wait;
    od_block stone;
    /* LIFO support/candidate queues and the cascade task. The queued piece
     * keeps its tile types so a group that fragments mid-fall can be
     * re-queued with its own material. */
    uint8_t support_cells[OD_SUPPORT_SPANS], support_widths[OD_SUPPORT_SPANS];
    uint8_t drop_cells[16], drop_widths[16], drop_tiles[16];
    uint8_t drop_tiles_q[16][16];
    unsigned support_count, drop_count, cascade_task, cascade_state;
    unsigned magic_lateral; /* expand the first released support span */
    unsigned cascade_cell, cascade_width, cascade_phase;
    unsigned effect_count, effect_wait, effect_active;
    /* Selective freeze: mirrors the high bits of the original's flag byte
     * that gate new player input. Set while a pushed block is in flight (tile
     * writes), a support/candidate span is queued or a fire extinction effect
     * is pending. The frame clock and sequence stepping never stop; only new
     * decisions do. */
    unsigned freeze;
    /* In the combat scene: the boss's attack is alive. The decision task then
     * takes the same path as the selective freeze: Hero only finishes or
     * falls, no new input; the HUD clock stops too. Published by
     * od_combat_tick right after the attack step, before the gameplay step: in
     * the arena the object pass runs before the decision task. It also covers
     * the retirement frame. Never set outside the combat scene. */
    int attack_lock;
    /* Closing the START pause clears the attack flag for the first of the
     * handler's two frame yields. Set by the flow for that frame only; in
     * the arena it lifts `attack_lock` for the decision and the HUD clock. */
    int r90_resume;
    /* Raised by the attack's retirement in the object pass, which runs before
     * this frame's decision task: the death program only starts with every
     * freeze bit but the lowest clear, so a lethal contact still latched on
     * the retirement frame waits for the cascade to end. The arena sets it for
     * that one gameplay step. */
    int retire_freeze;
    /* The arena's object pass (the boss and its attack) shares the task of the
     * pause handler and runs before it: it sees the pause bit as the frame
     * began, and it does not run while the handler's close sequence yields
     * (the close frame and the two yields). Set by the flow before each
     * gameplay step. */
    int objects_paused;
    int facing;          /* -1 = left, +1 = right */
    int fires;           /* remaining fire cells */
    int start_fires;     /* fires at level start (for HUD/tests) */
    int cleared;         /* 1 once every fire is out */
    int exit_open;       /* same condition as `cleared` for now */
    int cooldown;        /* compatibility mirror of hero_wait, not a scheduler */
    int action_period;   /* legacy, ignored by physics */
    long frame;          /* frames since the level (re)started; reset on a retry */
    /* Animation bookkeeping for the last action (also useful in tests). */
    int last_slide_cells; /* cells the pushed block covered (slide + fall) */
    int last_ice_drops;   /* cells the passive gravity cascade dropped */
    /* Moving-object ownership. Hero's current animation can finish while new
     * decisions are inhibited; cascades and effect queues have their own phases. */
    int pending;
    int pending_wait;
    int pending_x, pending_y, pending_dx;
    /* Cell value the pushed block is restored with when it lands: 0x05 for
     * ice or 0x0C for the stone. A falling stone also survives the fire cell
     * it lands on, where ice is consumed. */
    int pending_material;
    /* A falling fire reaching a jar cell stores that cell; the task writes the
     * hazard into it and requests its sound. */
    unsigned jar_cell, jar_pending;
    /* Counters kept for tests and diagnostics. */
    long pushes;
    long falls;
    long ice_made;
    long ice_removed;

    /* --- moving flames and death ------------------------------------------ */
    /* Exact flame controller slots, one per 0x81/0x82/0x83 command of the
     * level record, in registration order. `enemies.items[i]` is the
     * rendered/objective mirror of `flames[i]`; both are kept in step so the
     * level objective, ice quench and presentation keep working. */
    od_flame flames[OD_FLAME_MAX];
    int flame_count;
    od_flame initial_flames[OD_FLAME_MAX]; /* snapshot restored on a retry */
    int initial_flame_count;
    /* Flags the object controllers publish for Hero. Bit 4 is the lethal
     * contact set by `od_flame_tick` (and, later, the boss attack); it is
     * reset at the start of every `od_game_tick`. */
    unsigned hero_flags;
    /* The lethal-contact bit as the decision task sees it: once raised it
     * stays set until the level restarts, and the next decision step with
     * every freeze bit but the lowest clear starts the death program. */
    int lethal;
    /* A leftward moving ice block whose left neighbour is the boss hazard
     * registers the boss's hit and is consumed. `od_game_tick` clears it at
     * the start and the combat scene consumes it after the step; it is a
     * one-tick event, not persistent state. */
    int boss_hit;
    /* Nonzero when the entry program's own startup-cue request is emitted by
     * the core instead of the flow's level-start agenda: the boss arena's
     * entry and re-entry. */
    int entry_cue;
    /* A moving ice block destroyed by the hazard below it (or by the boss on
     * its left) queues its cell in the effect queue; the drain plays the
     * effect sound four ticks later. Cell + 1 of that block for this tick (0 =
     * none), cleared at the start of `od_game_tick`; the presentation keeps
     * the cell for the drain's steam. */
    unsigned hazard_effect;
    /* Moving flames put out this tick (by a sliding or falling block, or by a
     * falling span) and the span columns a flame stopped, both cleared at the
     * start of `od_game_tick`. The presentation needs them to match the fire
     * count and the shattered-column effects it retains. */
    unsigned flame_kills;
    unsigned cascade_flame_mask;
    od_enemy_list enemies;        /* live enemies of the current level */
    od_enemy_list initial_enemies;/* snapshot restored on a retry */
    uint8_t initial_cells[OD_GRID_H][OD_GRID_W];
    int initial_x, initial_y;
    int level_index;      /* index of the level in its set, or -1 if unknown */
    long enemy_clock;     /* legacy field; unused by the current engine */
    /* Set by the death program (and by the lethal-contact path). While set
     * the decision task returns at once but the level keeps running; only the
     * failure band's retry (od_game_retry) clears it. */
    int dead;
    /* The START pause / failure band is up (set by the flow from `od_pause`,
     * scene_pause.c). Like the death it only stops Hero's decisions and the
     * flames' movement; the level keeps running. */
    int paused;
    int game_over;        /* never set by the engine: the original has no lives */
    int lives;            /* remaining lives; OD_HERO_UNLIMITED = infinite */
    long deaths;          /* times Hero has died */
    long retries;         /* level restarts (a death is a retry) */
    long enemies_extinguished; /* moving flames put out by ice */
} od_game;

/* Initialise from a parsed level record (copies the grid and start position).
 * The enemies are spawned from the level's own command stream; the level index
 * is recovered only from a unique complete grid/start/metadata/commands match
 * in the default JSON package. Empty/terminator-only streams stay anonymous
 * (-1), with no package lookup. No ROM or enemy dump is opened. Campaign/mod
 * callers that know the index use od_game_init_index with their VFS level. */
void od_game_init(od_game *g, const od_level *lv);

/* Like od_game_init but with an explicit enemy set and level index. `set` is
 * read only during the call (the spawned enemies are copied), so it need not
 * outlive the game. Pass set == NULL (or an out-of-range index) for no enemies.
 *
 * A fresh level arms the entry materialization (see entry_hold): the
 * pose/pixels stay unexposed for the first frames, exactly like the
 * original. Callers that start a verified mid-level fixture rather than a
 * real entry (the differential tests, whose fixture tick 0 is already in
 * repose) call od_game_entry_finish to jump straight to the post-entry rest
 * state. */
void od_game_init_ex(od_game *g, const od_level *lv,
                      const od_enemyset *set, int level_index);

/* Start `lv` as level `level_index` (0..149) with the objects of its own
 * command stream (`lv->commands`, which the content package builds from the
 * level's "objects"), so a mod or an editor level that changes them is played
 * as written (C2.1, MJ6). od_game_init is this with the index recovered from
 * a unique complete level match in the default JSON package (C6.4). */
void od_game_init_index(od_game *g, const od_level *lv, int level_index);

/* End the level-entry materialization immediately: stop the entry hold and
 * sequence and settle Hero in the idle rest pose. No time passes and no sound
 * is queued. A no-op when the level is not in its entry state. */
void od_game_entry_finish(od_game *g);

/* Deprecated compatibility ABI (C6.4): ignores path, including NULL; never
 * opens it. Invalidates only the optional default JSON level-index cache. */
void od_game_set_enemy_asset(const char *path);

/* Advance one gameplay tick. Held input is used by phase branches too. */
void od_game_tick(od_game *g, unsigned buttons);

/* Convenience for callers without a frame loop: advances the SAME tick engine
 * through an action, a preceding turn, and its pending support tasks. frame
 * advances accordingly. Replays/renderers should call od_game_tick instead. */
int od_game_action(od_game *g, unsigned buttons);

/* Read a cell; out-of-grid coordinates read as solid (a wall). */
int od_game_cell(const od_game *g, int x, int y);

/* Force the win check (called automatically after every action). */
void od_game_update_clear(od_game *g);

/* Recount fire cells from the grid and refresh the clear flags. Handy for
 * tests that build a board by hand. */
void od_game_recount_fires(od_game *g);

/* Restart the current level from its initial state: grid, Hero, enemies and
 * the enemy clock. Increments `retries`. A death never resolves by itself: the
 * failure band's retry option calls this (callers may also use it directly). */
void od_game_retry(od_game *g);

/* 1-based world of the level being played (level_index / 10 + 1), or 1 when the
 * index is unknown. Used by the renderer to pick the level's sprite set. */
int od_game_world(const od_game *g);

/* Advance every mobile flame one tick with the exact controller
 * (`od_flame_tick`) and latch a lethal contact (`lethal`); the next decision
 * step turns it into the death. With a freeze flag set (an input freeze or
 * the death itself) the flames only animate. Called by od_game_tick; exposed
 * for tests. Returns 1 if a lethal contact was published on this call. The
 * legacy name is kept for the existing ABI. */
int od_game_tick_enemies(od_game *g);

/* Queue a support span because a controller outside `od_game_tick` (the boss
 * attack) changed the live grid, so the gravity cascade reacts exactly as it
 * does after a player-driven change. `cell` is the grid index, `width` the
 * connected span width. */
void od_game_support_changed(od_game *g, unsigned cell, unsigned width);
/* Like the above but for a scan that runs after the caller's cascade yield,
 * so the first cascade step collects without the extra idle tick. */
void od_game_support_changed_now(od_game *g, unsigned cell, unsigned width);

/* Register a lethal contact published by another controller (the boss attack
 * sets it through `od_boss_attack_tick`): it latches the same `lethal` flag
 * as a flame, consumed by the next decision step. */
void od_game_lethal_contact(od_game *g);

/* Queue one sound id from a controller that runs outside `od_game_tick` (the
 * boss attack's spawn cue), keeping the ordered queue the audio layer
 * consumes. Safe to call after `od_game_tick`; it must not be called before,
 * because that resets the queue. */
void od_game_sound_event(od_game *g, unsigned id);

#endif /* OD_GAME_H */
