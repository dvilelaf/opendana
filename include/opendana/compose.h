/* compose.h - pure software composition of a level into NES colour indices.
 *
 * Everything here is pure C11 with no SDL. The SDL layer only converts the
 * resulting index frame to RGB and uploads it. This makes the interesting part
 * (cell type -> CHR tile + palette composition) directly testable and lets the
 * tests compare the output against a captured reference frame.
 *
 * Live composition uses the characterised world_gfx tables (worlds 1..15,
 * including the hidden rounds 100..149).  The legacy cell API retains the
 * world-1 tables.
 */
#ifndef OD_COMPOSE_H
#define OD_COMPOSE_H

#include "opendana/common.h"
#include "opendana/content.h"
#include "opendana/game.h"
#include "opendana/level.h"
#include "opendana/objects.h"
#include "opendana/palette.h"

typedef struct {
    uint8_t index[OD_SCREEN_W * OD_SCREEN_H]; /* NES colour indices 0..0x3F */
} od_index_frame;

/* Resolved presentation data. CHR indices are global, not bank-relative.
 * Coordinates are screen pixels (OAM Y + 1); order is OAM priority order.
 * This is a compositor, not a PPU/CPU emulator. The simulation owns timing. */
typedef struct {
    int x, y;
    uint16_t tile;
    uint8_t attr; /* palette 0..3, behind BG 0x20, flip X/Y 0x40/0x80 */
} od_sprite;

typedef struct {
    uint16_t tiles[30][32];
    uint8_t palettes[30][32];
    od_sprite sprites[64];
    unsigned sprite_count;
    od_palette_ram palette;
    /* Vertical scroll of the background (0..239; screen line y shows background
     * line (y + scroll_y) % 240). Only auto-scrolling rounds set it
     * (od_game.scroll_y); the HUD band is drawn over it afterwards. */
    unsigned scroll_y;
    /* Auto-scrolling rounds show a strip of lava tiles (the fire bank's tiles 4..7,
     * four wide, palette 0) on lines 208..215, scrolled sideways: `lava_base` is the
     * first tile of the strip in the tile set and `lava_x` its horizontal phase. */
    unsigned lava_on;
    unsigned lava_base;
    unsigned lava_x;
} od_presentation;

/* --- Sprites and animation tables (C2.2) ---------------------------------
 * The Hero metasprites, the steam poses and the animated CHR bank tables live
 * in the content package (`graphics/sprites/poses.json`). `od_sprites_load_json`
 * fills them; the compose functions below use nothing else. */
typedef struct od_sprites_candidate od_sprites_candidate;
typedef struct od_compose_content_stage od_compose_content_stage;
od_status od_compose_content_prepare(od_compose_content_stage **out,
    const char *const *sprites_json, const size_t *sprites_sizes,
    size_t sprites_count, const char *arena_json, size_t arena_size);
void od_compose_content_commit(od_compose_content_stage *stage);
void od_compose_content_discard(od_compose_content_stage *stage);
/* Prepare never mutates live caches or *out on error. Commit consumes the
 * candidate, performs no allocation and cannot fail; free discards staging.
 * The VFS owner must prepare ALL resources before any publication. */
od_status od_sprites_prepare_json(od_sprites_candidate **out, const char *json, size_t len);
void od_sprites_commit(od_sprites_candidate *candidate);
void od_sprites_candidate_free(od_sprites_candidate *candidate);
od_status od_sprites_load_json(const char *json, size_t len);
void od_sprites_reset(void);
int od_sprites_loaded(void);
unsigned od_sprites_pose_count(void);
unsigned od_sprites_part_count(void);
const uint8_t *od_sprites_world_banks(void);
/* Static artwork of the arena scene, loaded through the same mod overlay as the package. */
od_status od_arena_load(const od_content_vfs *vfs);
/* The arena's status-bar text row: tile row 28 of the second background page,
 * 32 ASCII tile codes (ROUND, the round value at column 10, TIME, the clock at
 * column 22). The split shows this page below the line 215, so the combat HUD
 * is this row, not the normal-level layout. */
const uint8_t *od_arena_hud_row(void);
/* Palette brightness table (64 bytes): entry `c` at brightness `b` (0..15) is
 * `(c & 15) | lut[(c & 0x30) + b]`. NULL when the arena data that carries it
 * is not loaded. */
const uint8_t *od_palette_fade_lut(void);

/* Full 256x240 composition including HUD tiles. Enforces the first eight
 * sprites per line and first opaque OAM pixel, including behind-BG priority. */
void od_compose_present(od_index_frame *fb, const od_presentation *present,
                         const uint8_t *chr);

/* --- banded scene display, including the split fine scroll ----------------
 *
 * Scene clips (`assets/screens/<name>.track`, drawn by od_screens_draw) describe a
 * small number of horizontal bands, each with its own scroll, background page
 * and tile banks. The reference does not change band exactly on a scanline: the
 * split handler writes the control and scroll registers while the scanline just
 * above the boundary is still being drawn. On that scanline the outgoing
 * band's coarse X, page and fine Y still select the tiles, but the incoming
 * band's fine X is already latched, so the row is the outgoing band sampled
 * `fine_in - fine_out` pixels further right from the dot of the write. A band
 * model that ignores that fine update leaves a visible defect on the final
 * boss sequence.
 *
 * The exact dot depends on the original handler's timing and is not
 * reproduced; the ideal transition dot is used (see OD_COMPOSE_SPLIT_X).
 * `scene` is a od_screen_id value and `age`/`visible_selection` drive the menu
 * cursor override, exactly as in screens.c, so screens.c can adopt this
 * function verbatim. */
#define OD_COMPOSE_SCENE_MENU 3u   /* od_screen_id OD_SCREEN_MENU */
void od_compose_scene(od_index_frame *fb, const uint16_t *display,
                       unsigned scene, unsigned age,
                       unsigned visible_selection, const uint8_t *chr);

/* One background scanline through the video chip's fetch pipeline, a
 * sub-scanline model. Unlike the tile blit this keeps the real 16-bit lo/hi
 * shift registers, the pattern address register and the register-access
 * timing: `perturb_dots`/`perturb_delta` list the dots whose background half
 * dot a register access changed, delta < 0 meaning the half dot was missed and
 * delta > 0 meaning it ran twice. Either way one output pixel is duplicated or
 * dropped and the pending fetch commit is deferred. All other inputs are the
 * resolved presentation data: `ciram` is the 2 KiB video page, `bank_off[8]`
 * the 1 KiB tile-bank offsets, `v_init` the video address at the start of the
 * last line before the row (its prefetch feeds the row). `fine_x` is the fine X
 * scroll. Writes palette indices into out[256]. Callers pass perturb_count 0
 * for the ideal tile blit. */
void od_ppu_bg_row(uint8_t out[256], const uint8_t ciram[2048],
                    const uint32_t bank_off[8], unsigned fine_x,
                    uint16_t v_init, const unsigned *perturb_dots,
                    const int8_t *perturb_delta, unsigned perturb_count,
                    const uint8_t *chr, const od_palette_ram *pal);

/* Boss pose data, expanded offline. `bank` selects the tile-data window (4 or 5),
 * `pose` is the pose pointer inside that window. The anchor is the screen
 * centre. Returns the number written; zero means unknown pose or insufficient
 * room. Neither this function nor drawing advances animation time. */
unsigned od_hero_pose(od_sprite *out, unsigned capacity, unsigned bank,
                       uint16_t pose, int anchor_x, int anchor_y, int facing);

extern const od_master_palette od_master_palette_reference;
extern const od_master_palette od_master_palette_pal;

/* Resolve the current simulation state, without advancing or retaining time.
 * The caller owns the presentation latch: compose the previous snapshot
 * while gameplay builds the next one. No reference dumps are consumed here.
 * Effect/video queues and animated background banks are not yet represented. */
void od_present_game(od_presentation *present, const od_game *game,
                      const od_palette_ram *pal);

/* Elapsed calls after animation initialisation, on the shared simulation
 * clock. Returns the tile bank for slots 2, 4 and 5 (slot 3 is world terrain).
 * world is 1-based (1..10). The caller owns scene start and the presentation
 * latch; this function never ticks. */
unsigned od_background_bank(unsigned slot, unsigned world, unsigned long ticks);

/* Steam and dynamic sprites belong to simulation presentation state, not
 * drawing. Initialise at level entry and step ONCE after each game tick,
 * including frozen gameplay. before/after are consecutive native game states.
 * Return 0 without mutation for a missing tick or unsupported quench
 * provenance. ticks starts at the actual scene initialisation, not at reset or
 * the first interactive frame. sprite_mode is the NEXT build mode; the current
 * snapshot uses the preceding mode. Scene scheduler integration is required. */
typedef struct {
    unsigned long ticks;
    long game_frame;
    uint8_t queue[16], queued;
    /* What the background shows at each queued cell until its queued effect lands:
     * the fire tile for a pushed block (the ice sprite covers it; `pal` 0xFF) or,
     * for a cascade that landed on fires, the landed run's tile (with its run
     * neighbours) in the fire's palette. Index-aligned with `queue`. */
    struct { uint8_t type, pal, left, right; } qimg[16];
    struct { uint8_t cell, phase, wait, active; } steam[16];
    /* Cascade drop: the grid commits the move when the block starts falling, but
     * the background rewrite only lands when it does, eight ticks later. `fall`
     * retains the source background tile for that window so the image keeps
     * painting fire/ice under the falling block. `type` is the original cell
     * value (fire, or connected ice). When the drop starts on a just-quenched
     * cell (a fire drop into a cell whose effect dispatched this tick),
     * `dst_quench` keeps that cell's tile while it is in the first half of the
     * descent, painted in its pre-move palette `dst_pal` (the empty-floor
     * palette the quench attribute write left). `left`/`right` are the run's own
     * neighbours of this cell (the image is composed from a copy of the run, 0 at
     * both ends), which is what joins a falling stone to its ice. */
    struct { uint8_t cell, type, wait, dst_pal, dst_quench, left, right; } fall[16];
    unsigned fall_count;
    /* The wand's cells (`game.magic_redraw`): the grid changes at the decision, but
     * the background keeps the previous metatiles of that cell and its
     * neighbours until the program redraws them (only the sparkle shows
     * meanwhile). `left`/`right` are the neighbours the old metatile was drawn
     * with, 0xFF outside the field. */
    struct { uint8_t cell, type, left, right; } magic[3];
    unsigned magic_count;
    /* The boss room's background column writes. A cell write (the attack's, or the drain of an
     * effect) also rewrites the facing column of each horizontal neighbour from the grid as it is
     * at that moment, and the background keeps what was written until another write covers the
     * column (the last write wins). `colw` is that log, oldest first: column `side` (0 left, 1
     * right) of `cell` drawn as `type` beside `nb`, written by the write of cell `src`.
     * Only these two writers are logged, so an entry also stops applying when the grid byte of
     * `src` (`sraw`) or of `cell` (`mraw`) changes (the attack's own hazard types count as empty),
     * when the falling blocks that paint `cell` change phase (`fsig`: the fall writer rewrites
     * the cells it passes) and, for the attack's entries (`by_attack`), at its next script step.
     * `colw_on` is set once the attack is stepped (boss room only). */
    struct { uint8_t cell, side, type, nb, src, sraw, mraw, by_attack, fsig; } colw[24];
    unsigned colw_count;
    uint8_t colw_on, attack_seen;
    uint8_t oam_mode;
} od_visual_state;
void od_visual_init(od_visual_state *visual, const od_game *game);
int od_visual_tick(od_visual_state *visual, const od_game *before,
                     const od_game *after);
/* Boss room: record the background writes of the attack script that ran this tick (its cells
 * with the facing columns of their neighbours). Call once per tick, after the combat step and
 * before od_visual_tick, so the order of the writes within the tick is kept. */
void od_visual_attack_step(od_visual_state *visual, const od_game *game,
                           const od_boss_attack *attack);
/* Freeze flag that gates the status clock: non-zero freezes od_hud_clock.
 * It is the game's freeze bits plus the celebration flag. */
uint8_t od_scene_flags(const od_game *game);
void od_present_game_visual(od_presentation *present, const od_game *game,
                             const od_visual_state *visual,
                             const od_palette_ram *pal);
/* As od_present_game_visual(), but also composes the boss's sprite entries
 * (`boss`) from its pose words. Used by the arena scene; `boss` NULL is
 * identical to od_present_game_visual(). */
void od_present_game_boss(od_presentation *present, const od_game *game,
                           const od_visual_state *visual,
                           const od_palette_ram *pal, const od_boss *boss);
/* Complete arena, including the attack's background writes. */
void od_present_game_combat(od_presentation *present, const od_game *game,
                             const od_visual_state *visual,
                             const od_palette_ram *pal, const od_boss *boss,
                             const od_boss_attack *attack);
/* The hero stays first; other sprites rotate/reverse in the remaining slots. */
void od_order_sprites(od_presentation *present, unsigned hero_count, unsigned mode);

/* Presentation latch. The video chip publishes what gameplay wrote one tick
 * earlier, so the displayed image is the snapshot built at the previous tick.
 * Build a snapshot every tick and display the one returned; NULL on the first
 * tick. The caller owns the clock. */
typedef struct {
    od_presentation frame[2];
    unsigned next;
    int primed;
} od_present_latch;
void od_present_latch_init(od_present_latch *latch);
const od_presentation *od_present_latch_build(od_present_latch *latch,
                                                const od_game *game,
                                                const od_visual_state *visual,
                                                const od_palette_ram *pal);

void od_frame_clear(od_index_frame *fb, uint8_t bg_index);

/* Draw an arbitrary 16x15 grid of cell values (fills the whole 256x240 frame).
 * This is the shared core of the static level render and the live game. */
void od_render_cells(od_index_frame *fb, const uint8_t cells[OD_GRID_H][OD_GRID_W],
                      const uint8_t *chr, const od_palette_ram *pal);

/* Draw the 16x15 level grid (fills the whole 256x240 frame). */
void od_render_level_field(od_index_frame *fb, const od_level *lv,
                            const uint8_t *chr, const od_palette_ram *pal);

/* Overlay a Hero sprite bottom-aligned in cell (cx,cy). `facing` < 0 mirrors
 * the sprite horizontally. */
void od_render_hero(od_index_frame *fb, int cx, int cy, int facing,
                     const uint8_t *chr, const od_palette_ram *pal);

/* Overlay the player sprite at the level's start position. */
void od_render_player(od_index_frame *fb, const od_level *lv,
                       const uint8_t *chr, const od_palette_ram *pal);

/* Field plus player. */
void od_render_level(od_index_frame *fb, const od_level *lv,
                      const uint8_t *chr, const od_palette_ram *pal);

/* Field plus player, using the per-world graphics of `level_index` when they are
 * characterised (worlds 1..15, including the hidden rounds); falls back to the
 * world-1 tables otherwise. */
void od_render_level_index(od_index_frame *fb, const od_level *lv, int level_index,
                            const uint8_t *chr, const od_palette_ram *pal);

/* Draw current simulation presentation (not a latched frame). */
void od_render_game(od_index_frame *fb, const od_game *g,
                     const uint8_t *chr, const od_palette_ram *pal);

/* Convert NES colour indices to 0xAARRGGBB. NULL and the app's historical
 * NTSC default select the pinned PAL 2C07 profile (emphasis zero). Explicit
 * od_master_palette_reference selects the old NTSC fixture profile. */
void od_frame_to_argb(const od_index_frame *fb, const od_master_palette *mpal,
                       uint32_t *out);

#endif /* OD_COMPOSE_H */
