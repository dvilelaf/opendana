/* flow.c - scene machine and play session (pure C11, no SDL).
 *
 * This is the app loop that used to live in `src/app/main.c`, moved into the
 * core so both the SDL front end and the deterministic harness run exactly the
 * same ticks. Nothing here opens a window or a device: the host provides input,
 * presentation, audio and its clock through `od_flow_host`.
 *
 * The playable-level state is the old `game_view`, now `od_play_view`, with
 * the HUD clock (H3d) and the scene animation clock (`od_visual_state`, H3g)
 * connected in the real loop (A9), and the level-exit fade (A10) applied to the
 * presented ARGB. Progress survives a *clear* (A4) through `od_session`. */
#include "opendana/flow.h"

#include "opendana/audio.h"
#include "opendana/content_gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Level index of the last normal round: world 10, round 10. Clearing it rolls
 * the ending sequence instead of returning to the map. */
#define OD_FINAL_LEVEL (OD_WORLD_COUNT * OD_WORLD_ROUNDS - 1)

/* Black frames between the editor's play frame and the user level on screen
 * (reference f2903..f2905). */
#define OD_EDITOR_LOAD_BLACK 3u

/* Real ending/credits clip (H4.11). */
#define OD_ENDING_FIRST 24482
#define OD_ENDING_LAST  47000

/* T1: the arena combat hand-off after ROUND 10-10. */
#define OD_COMBAT_ARM_FRAMES_WRITE 4
#define OD_COMBAT_ARM_FRAMES_INIT  5

/* C3.4: the failure delay before the retry / new-game band is 120 ticks, armed
 * by the level-state handler the frame after the death bit is set (2-10: death
 * f16179, delay armed f16180, flag f16300, band f16301; arena f25959 -> band
 * f26081). It lives in scene_pause.c with the rest of the pause handler. */

/* Retry in a normal round reloads it like the reference (2-10: A f16400,
 * 1-1: A f3700). Counted in ticks since the confirm: the level stays frozen
 * while the brightness fades 15 -> 0 and holds black, the grid is restored at
 * +18 and the round is re-armed at +21 (entry state, flames, counter, HUD
 * clock), the same +3 as the staged level load; the picture is back to full
 * brightness at +36. The boss arena (A f26200) fades the same way; its board
 * is written at +17 and it re-arms the combat at +21 too. */
#define OD_RETRY_GRID_TICK 18u
#define OD_RETRY_ARENA_GRID_TICK 17u
#define OD_RETRY_INIT_TICK 21u
#define OD_RETRY_FADE_END  36u
/* NEW GAME shares the first 16 frames and opens the map on the next one. */
#define OD_NEW_GAME_MAP_TICK 16u
/* The reload empties the whole sprite shadow from +15, the frame after the fade
 * reached 0, through the re-arm frame +21: reference 2-10 has 32 visible
 * entries at f16414, none at f16415..f16421 and 16 at f16422. On screen those
 * shadows land on black frames or on the re-arm's sprite-less ones; the composed
 * frame holds no sprites. */
#define OD_RETRY_OAM_CLEAR_TICK 15u
/* The flame registration (slot type and cell) is zeroed at +16 and rewritten
 * from the level record at +18, with the grid; the controller state keeps its
 * old bytes until the re-arm (reference 2-10: types 01 and cells 56 74 78 96 at
 * f16415, zero at f16416/f16417, types 01 and cells 57 75 79 97 from f16418,
 * controller re-initialised at f16421). */
#define OD_RETRY_FLAMES_CLEAR_TICK 16u

/* Brightness shown at tick `k` of the reload. The reference writes 14 at the
 * confirm frame and one less per frame down to 0 (f16400..f16414), holds 0 to
 * +19, then 1, 2, 2 (the re-arm frame does not step) and +1 per frame to 15 at
 * +35; the palette queue lands one frame later, so the picture shows the
 * previous frame's value (measured pixel by pixel). */
static unsigned retry_brightness(unsigned k)
{
    if (k <= 15)
        return 15 - k;
    if (k <= 20)
        return 0;
    if (k <= 23)
        return k == 21 ? 1 : 2;
    return k >= OD_RETRY_FADE_END ? 15 : k - 21;
}

/* Map phase: power-on frames whose video update does not yet increment the two
 * frame counters (the reference holds the first at 0 through f4; flow_frame_tick). */
#define OD_FRAME_TICK_BOOT_FRAMES 5u

/* NOTE: the 540-tick password-map wait was removed by C3.3 (A6): it is replaced
 * by the real "What next?" screen (scene_whatnext.c). */
/* ---- session ----------------------------------------------------------- */

void od_session_reset(od_session *s)
{
    memset(s, 0, sizeof *s);
    od_progress_reset(&s->progress);
    s->level = -1;
    s->world = 0;
    s->round = 0;
}

int od_session_load(od_session *s, const char *path)
{
    od_progress p;
    od_session_reset(s);
    if (!path)
        return 0;
    od_progress_reset(&p);
    od_status st = od_save_read(path, &p);
    if (st == OD_OK) {
        s->progress = p;
        s->world = p.world;
        s->round = p.cursor;
        return 1;
    }
    if (st != OD_ERR_IO)
        fprintf(stderr, "save: ignoring invalid %s: %s\n", path,
                od_status_str(st));
    return 0;
}

int od_session_save(const od_session *s, const char *path)
{
    if (!path)
        return 0;
    od_progress p = s->progress;
    p.world = s->world >= 0 ? s->world : 0;
    p.cursor = s->round >= 0 ? s->round : 0;
    od_status st = od_save_write(path, &p);
    if (st != OD_OK) {
        fprintf(stderr, "save: cannot write %s: %s\n", path,
                od_status_str(st));
        return 0;
    }
    printf("save: wrote %s (%d rounds cleared, world %d round %d)\n", path,
           od_progress_clear_count(&p), p.world + 1, p.cursor + 1);
    return 1;
}

/* C6.23: a state never overwrites save.dat. Loading a save state (F9) replaces
 * the whole session, its progress included, so the next write would put that
 * older bitmap over the file and lose rounds. The write therefore keeps every
 * round already cleared on disk: the bitmap only grows. `uncleared` (-1 = none)
 * is the one bit the original itself takes back, 10-10 on the arena's new-game
 * option, which stays cleared from the file too. The session in memory is not
 * touched (the state stays what it was). */
static void flow_save_except(od_flow *f, int uncleared)
{
    /* A scripted replay is a measurement: it only writes the save when the
     * caller named one explicitly (the interactive session always saves). */
    if (f->replay_mode && !f->opt.save_forced)
        return;
    /* C6.31 (CORE-3): a custom level is not a round; nothing is written. */
    if (f->opt.custom_level)
        return;
    od_session out = f->session;
    od_progress disk;
    od_progress_reset(&disk);
    if (f->opt.save_path && od_save_read(f->opt.save_path, &disk) == OD_OK) {
        for (size_t i = 0; i < sizeof out.progress.cleared; ++i)
            out.progress.cleared[i] |= disk.cleared[i];
        if (uncleared >= 0)
            od_progress_set_cleared(&out.progress, uncleared, 0);
    }
    od_session_save(&out, f->opt.save_path);
}

static void flow_save(od_flow *f)
{
    flow_save_except(f, -1);
}

/* ---- genuine (native) level-entry resources ---------------------------- */

/* The level-load fade-in is a brightness ramp on the round's own palette:
 * brightness max(1, t) through the imported brightness table
 * (`od_palette_fade_lut`), applied by `flow_view_draw`. The ramp length is the
 * measured level-entry window that the scene machine also releases on
 * (`OD_LEVEL_ENTRY_FRAMES`, 13 frames); the optional captured
 * `screens/tracks/level-entry.fade` table is not read. */
#define OD_FLOW_ENTRY_FADE_FRAMES 13u

/* Native availability of the castle-appears cinematic for `world`. The family
 * `screens/cinematics/castle-appears.json` is loaded and validated eagerly by
 * `od_screens_init` (`load_cinematics`), one scene per world 0..9; worlds 0..8
 * can raise the flow's castle-appears latch. This is the native replacement for
 * the optional `screens/world-clears.json` clip gate. */
int od_flow_castle_appears_available(const od_screens *s, int world)
{
    if (s == NULL || world < 0 || world > 8)
        return 0;
    unsigned index = 0;
    return od_cinematic_scene_index(s->cinematic_data, (unsigned)world,
                                     &index) == OD_OK;
}

/* ---- presentation of one playable level (the old game_view) ------------ */

static void flow_view_init(od_flow *f, const uint8_t *fade,
                           unsigned fade_frames)
{
    od_play_view *v = &f->view;
    /* The shared timing counters are global state that only the level task
     * updates: a retry reload or a new round keeps them (reference: the right
     * counter from f3200 resumes at 23 on f3222; R1 keeps 01 01 .. 13 from 1-1
     * through the map into 1-2). */
    unsigned repeat = v->pause.menu.repeat;
    uint8_t counters[8];
    memcpy(counters, v->pause.menu.counters, sizeof counters);
    /* The new-press byte is the video update's own edge of every frame, whatever
     * the scene: a button held from the scene before the round is not a new
     * press in it (castles: START held through the castle skip does not open
     * the pause; reference 8-10 START f18200..f18205, no pause sound). */
    unsigned previous_buttons = v->previous_buttons;
    memset(v, 0, sizeof *v);
    v->pause.menu.repeat = repeat;
    memcpy(v->pause.menu.counters, counters, sizeof counters);
    v->previous_buttons = previous_buttons;
    v->clock.countdown = 120; /* also 120 in the PAL reference. */
    v->hud.visible = v->hud.queued = v->clock;
    od_visual_init(&v->visual, &f->game);
    od_present_game_visual(&v->queued, &f->game, &v->visual,
                            &f->assets->palette_ram);
    v->visible = v->queued;
    v->fade = fade;
    v->fade_frames = fade_frames;
    v->fade_tick = 0;
}

/* A level is on screen from its materialisation frame. The bank animation
 * counters make their initialising call on the frame BEFORE it: reference 1-1
 * at f2219, level on screen at f2220; 2-10 f15967/f15968; 1-2 f4419/f4420. The
 * scene clock therefore already holds one call here, then advances once per
 * frame. */
static void flow_view_materialised(od_flow *f)
{
    od_play_view *v = &f->view;
    v->visual.ticks = 1;
    od_present_game_visual(&v->queued, &f->game, &v->visual,
                            &f->assets->palette_ram);
    v->visible = v->queued;
}

/* The bar of a round that has just reached the screen: its rows are still
 * empty on that frame; the play loop's start queues the bar template and the
 * round value, shown the next frame, and the clock writer's digits the frame
 * after (reference R1 f2220 empty, f2221 "ROUND 1-1 TIME" + boxes, f2222
 * digits; retry f3221/f3222; arena f25749/f25750). */
static void flow_view_bar_load(od_flow *f)
{
    f->view.bar_empty = 1;
    f->view.level_template_queued = 1;
}

/* Start a level in the staged loading state. */
static void flow_view_begin_loading(od_flow *f, int level, const uint8_t *fade,
                                    unsigned fade_frames)
{
    memset(&f->game, 0, sizeof f->game);
    f->game.level_index = level;
    f->game.facing = 1;
    f->game.hero_active = 1;
    f->game.entry_hold = OD_ENTRY_HOLD_TICKS;
    flow_view_init(f, fade, fade_frames);
    f->view.previous_buttons = f->last_buttons;
    f->view.load_stage = 1;
    /* Every round load starts with no clear recorded: without this, the second
     * round solved in a session skipped the progress mark and the save (reference
     * progress word 01 after 1-1 and 03 after 1-2 on the owner's recording). */
    f->clear_timer = -1;
    f->world_clear_world = -1;
    f->castle_appears_world = -1;
}

/* ---- resume point: the inputs since the level started (C6.62) ----------- */

static void level_log_arm(od_flow *f)
{
    od_level_log *l = &f->lvl_log;
    l->armed = 0;
    l->count = 0;
    if (f->opt.custom_level || f->from_editor || f->level < 0 ||
        f->level >= OD_LEVEL_COUNT)
        return;
    memset(&l->r, 0, sizeof l->r);
    l->r.kind = OD_RESUME_LEVEL;
    memcpy(l->r.cleared, f->session.progress.cleared, sizeof l->r.cleared);
    l->r.progress_world = f->session.progress.world;
    l->r.cursor = f->session.progress.cursor;
    l->r.session_level = f->session.level;
    l->r.world = f->session.world;
    l->r.round = f->session.round;
    l->r.level = f->level;
    l->r.menu_repeat = f->view.pause.menu.repeat;
    memcpy(l->r.menu_counters, f->view.pause.menu.counters, sizeof l->r.menu_counters);
    l->r.oam_rotation = f->oam_rotation;
    l->r.oam_known = (uint8_t)(f->oam_rotation_known != 0);
    l->r.start_tick = f->sim_tick;
    l->armed = 1;
}

static void level_log_push(od_flow *f, unsigned buttons)
{
    od_level_log *l = &f->lvl_log;
    if (!l->armed)
        return;
    if (f->screen != OD_FLOW_PLAY || buttons > 0xffu ||
        l->count >= OD_RESUME_MAX_TICKS) {
        l->armed = 0;
        return;
    }
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 4096;
        uint8_t *grown = realloc(l->buf, cap);
        if (grown == NULL) {
            l->armed = 0;
            return;
        }
        l->buf = grown;
        l->cap = cap;
    }
    l->buf[l->count++] = (uint8_t)buttons;
}

/* Advance one staged loading tick. Returns 1 on the materialisation frame. */
static int flow_view_loading_tick(od_flow *f, const od_level *lv, int no_entry)
{
    od_play_view *v = &f->view;
    if (!v->load_stage)
        return 0;
    if (v->load_stage == OD_PLAY_LOAD_GRID) {
        memcpy(f->game.cells, lv->grid, sizeof f->game.cells);
    } else if (v->load_stage == OD_PLAY_LOAD_FULL) {
        level_log_arm(f);
        level_log_push(f, f->last_buttons);
        od_game_init_index(&f->game, lv, f->level);
        if (no_entry)
            od_game_entry_finish(&f->game);
        flow_view_init(f, v->fade, v->fade_frames);
        /* The object-pass counter is kept from the map: the next round's first object
         * pass continues it (R1: frozen at 2 from the A of f4400, 3 at f4421; the
         * native mode is one less). */
        if (f->oam_rotation_known)
            f->view.visual.oam_mode = (uint8_t)((f->oam_rotation + 3) & 3);
        f->oam_rotation_known = 0;
        flow_view_materialised(f);
        flow_view_bar_load(f);
        v->load_stage = 0;
        return 1;
    }
    ++v->load_stage;
    return 0;
}

/* Re-arm at +21 of a retry reload. A normal round restarts from its initial
 * state with the entry hold (od_game_retry). The boss arena re-runs its entry
 * instead: f26221 equals f25749, and od_game_retry would recount the arena's
 * zero fires as a clear. The sprite rotation survives the reload (2-10: 1 from
 * f16399 to f16421, +1 from f16422). */
static void flow_retry_rearm(od_flow *f, unsigned buttons)
{
    od_play_view *v = &f->view;
    unsigned retry_tick = v->retry_tick;
    uint8_t oam_mode = v->visual.oam_mode;
    int combat = v->combat_active;
    if (!combat)
        od_game_retry(&f->game);
    flow_view_init(f, NULL, 0);
    if (combat) {
        od_combat_init(&v->combat, &f->game);
        od_visual_init(&v->visual, &f->game);
        v->combat_active = 1;
    }
    v->visual.oam_mode = oam_mode;
    /* A normal round is re-armed like a level load: the bank animation made its
     * first call at +20, so the clock holds one call (1-1 f3720/f3721, 2-10
     * f16420/f16421). */
    if (!combat)
        v->visual.ticks = 1;
    if (combat)
        od_present_game_combat(&v->queued, &f->game, &v->visual,
                                &f->assets->palette_ram, &v->combat.boss,
                                &v->combat.attack);
    else
        od_present_game_visual(&v->queued, &f->game, &v->visual,
                                &f->assets->palette_ram);
    v->visible = v->queued;
    v->retry_tick = retry_tick;
    f->clear_timer = -1;
    f->world_clear_world = -1;
    f->castle_appears_world = -1;
    v->previous_buttons = buttons;
    flow_view_bar_load(f);
}

/* The level task stops during the reload, but the scene's tile animation keeps
 * its cadence (1-1: one bank every 2 frames, another every 4, through f3715;
 * arena: two banks through f26214), so the frozen round still animates. The
 * arena's banks follow its frame counter: the frozen room is presented at the
 * age it would have reached `k` ticks after the confirm, without touching the
 * game. */
static void flow_retry_scene_clock(od_flow *f, unsigned k)
{
    od_play_view *v = &f->view;
    if (v->combat_active) {
        od_game aged = f->game;
        aged.frame += (long)k + 1;
        od_present_game_combat(&v->queued, &aged, &v->visual,
                                &f->assets->palette_ram, &v->combat.boss,
                                &v->combat.attack);
        return;
    }
    /* The round's reload re-initialises the bank animation: its first call lands
     * at +20, one frame before the re-arm (1-1 f3720, 2-10 f16420). From +16 to
     * +19 its counters stand still under a black picture (brightness 0), so only
     * the restart is visible. */
    if (k == OD_RETRY_INIT_TICK - 1)
        v->visual.ticks = 0;
    else
        v->visual.ticks++;
    od_present_game_visual(&v->queued, &f->game, &v->visual,
                            &f->assets->palette_ram);
    /* The level loop does not run during the reload, so the strip's scroll stays as it
     * stands and nothing is counted ahead (castle 9-10 f24399..f24442). */
    v->queued.lava_x = f->game.lava_x % 32u;
}

static void flow_view_tick(od_flow *f, unsigned buttons)
{
    od_play_view *v = &f->view;
    unsigned pressed = buttons & ~v->previous_buttons;
    v->previous_buttons = buttons;
    od_hud_begin_frame(&v->hud);
    v->bar_visible = v->bar_queued;
    /* The load's empty bar lasts its frame, the bar template one more. */
    v->bar_empty = 0;
    v->old_palette = 0;
    v->level_template = v->level_template_queued;
    v->level_template_queued = 0;
    v->visible = v->queued;
    v->new_game = 0;

    /* Retry reload: from the confirm until the re-arm the level task does not run;
     * the round (or the boss arena) is re-armed at +21, presented without a
     * gameplay step like a level load. */
    if (v->retry_tick) {
        unsigned k = v->retry_tick++;
        /* New game runs the same frozen fade and opens the map on the first black
         * frame (+16). */
        if (v->retry_new_game && k == OD_NEW_GAME_MAP_TICK) {
            v->retry_tick = 0;
            v->retry_new_game = 0;
            v->new_game = 1;         /* the caller opens the map */
            return;
        }
        if (k < OD_RETRY_INIT_TICK) {
            f->game.sound_request = 0;
            f->game.sound_count = 0;
            if (v->combat_active) {
                /* The arena reload: the board at +17 (f26217) and the hero's cell at +20
                 * (f26220), one frame before the entry state. */
                if (k == OD_RETRY_ARENA_GRID_TICK)
                    od_combat_load_board(&f->game);
                if (k == OD_RETRY_INIT_TICK - 1) {
                    f->game.hero_x = 7;
                    f->game.hero_y = 1;
                }
            } else if (k == OD_RETRY_FLAMES_CLEAR_TICK) {
                for (int i = 0; i < OD_FLAME_MAX; ++i)
                    f->game.flames[i].type = f->game.flames[i].cell = 0;
            } else if (k == OD_RETRY_GRID_TICK) {
                memcpy(f->game.cells, f->game.initial_cells,
                       sizeof f->game.cells);
                for (int i = 0; i < OD_FLAME_MAX; ++i) {
                    f->game.flames[i].type = f->game.initial_flames[i].type;
                    f->game.flames[i].cell = f->game.initial_flames[i].cell;
                }
            }
            flow_retry_scene_clock(f, k);
            if (k >= OD_RETRY_OAM_CLEAR_TICK)
                v->queued.sprite_count = 0;
            return;
        }
        if (k == OD_RETRY_INIT_TICK) {
            flow_retry_rearm(f, buttons);
            v->queued.sprite_count = 0;
            return;
        }
        if (k >= OD_RETRY_FADE_END)
            v->retry_tick = 0;
    }

    /* The level-state task runs first in the frame. Its handler (scene_pause.c)
     * arms the failure delay after a death, opens the retry / new-game band on START
     * or when the delay drains, closes it on a second START and reads the cursor.
     * The pause flag (`game.paused`) only blocks the hero's decisions: the level
     * keeps running under the band, as after a death. */
    unsigned game03 = 0;
    if (f->game.freeze)
        game03 |= OD_PAUSE_GAME_FROZEN;
    if (f->game.cleared &&
        (f->game.hero_action == 0x18 || f->game.celebration_done))
        game03 |= OD_PAUSE_GAME_CLEAR;   /* celebration flag */
    if (f->game.dead)
        game03 |= OD_PAUSE_GAME_DEAD;
    if (f->game.cleared && f->game.exit_frames >= 2)
        game03 |= OD_PAUSE_GAME_EXIT;    /* the level task stops in the exit tail */
    f->game.r90_resume = 0;
    /* The handler's task state as the frame begins: the pause flag and the close
     * sequence's yields (scene_pause.c `hold`). */
    int paused_before = v->pause.paused;
    unsigned hold_before = v->pause.hold;
    int pause_event = od_pause_frame(&v->pause, buttons, pressed, game03);
    /* Closing writes the bar template (round blank, blank tile in the time cells);
     * after the two yields the round value is set and the round writer puts the
     * value back. Both reach the screen one frame later: reference 1-1 close f2470
     * -> "ROUND  -" f2471-f2472, value f2473. */
    if (pause_event == OD_PAUSE_CLOSED)
        v->bar_queued.round_blank = v->bar_queued.time_boxes = 1;
    else if (hold_before == 1 && v->pause.hold == 0)
        v->bar_queued.round_blank = 0;
    if (pause_event == OD_PAUSE_CONFIRMED) {
        if (v->pause.new_game) {
            /* New game: the confirm frame starts the reload fade, the band stays up with
             * the cursor on new game (reference f2600-f2615). In a user level the same fade
             * runs and +16 enters the editor's init (reference f3200 confirm, mode change
             * at f3216). */
            f->game.sound_request = 0;
            f->game.sound_count = 0;
            v->retry_tick = 1;
            v->retry_new_game = 1;
            flow_retry_scene_clock(f, 0);
        } else {
            /* Retry: the confirm frame is the first of the reload (the level task stops
             * here: 2-10 frozen from f16400). */
            f->game.sound_request = 0;
            f->game.sound_count = 0;
            v->retry_tick = 1;
            /* The level loop does not iterate on this frame: the strip on screen was prepared
             * counting an iteration that never comes. */
            v->visible.lava_x = f->game.lava_x % 32u;
            flow_retry_scene_clock(f, 0);
        }
        return;
    }
    f->game.paused = v->pause.paused;
    /* The first of the two yields after closing the pause runs with the freeze
     * bit clear (reference f26301). */
    f->game.r90_resume = hold_before == 2;
    /* The arena's object pass runs before the handler in the same task: it sees
     * the pause flag as the frame began and waits out the close sequence. */
    f->game.objects_paused = paused_before || hold_before > 0;

    od_game before = f->game;
    if (v->combat_active)
        od_combat_tick(&v->combat, &f->game, buttons);
    else
        od_game_tick(&f->game, buttons);
    /* Scene animation clock (A9): steam, falling tiles and the animated CHR
     * banks step once per gameplay tick, exactly as the H3 fixture. */
    if (v->combat_active)
        od_visual_attack_step(&v->visual, &f->game, &v->combat.attack);
    od_visual_tick(&v->visual, &before, &f->game);
    /* The freeze bit applies on clear/death and while the boss attack is alive
     * (`attack_lock`: the clock stays put from f26191, the init frame); a nonzero
     * scene flag (pending work or an active effect) freezes too. About 23 extra
     * ticks were measured without this gate. */
    if (od_hud_clock_tick(&v->clock, (uint8_t)(f->game.cleared || f->game.dead ||
                                                f->game.attack_lock),
                           (uint8_t)(od_scene_flags(&f->game) | f->game.paused))) {
        od_hud_queue(&v->hud, &v->clock);
        /* The clock writer runs after the handler: its digits cover the template's
         * boxes (1-1: same frame; arena: the attack's freeze bit gates it until the
         * first yield, reference boxes f26301, digits f26302). */
        v->bar_queued.time_boxes = 0;
    }
    if (v->combat_active)
        od_present_game_combat(&v->queued, &f->game, &v->visual,
                                &f->assets->palette_ram, &v->combat.boss,
                                &v->combat.attack);
    else
        od_present_game_visual(&v->queued, &f->game, &v->visual,
                                &f->assets->palette_ram);
    /* The handler's sound request (band opened, cursor moved) reaches the sound
     * queue after the gameplay step, which clears it. */
    if (v->pause.sound)
        od_game_sound_event(&f->game, v->pause.sound);
    if (v->fade)
        v->fade_tick++;
}

static void flow_view_draw(const od_flow *f, od_index_frame *fb, int hud)
{
    const od_play_view *v = &f->view;
    /* The load clears the sprite shadow and the first object pass fills it, so a
     * round that has just reached the screen shows no sprites on its first two
     * frames and every sprite from +2 (4-10 after the castle: flames from f18110,
     * materialised f18108). The hero shows from her first entry pose, which the
     * entry hold already times. */
    int show_sprites = 1;
    if (v->fade && v->fade_tick < 2)
        show_sprites = 0;
    od_presentation shown = v->visible;
    od_palette_ram pal = f->assets->palette_ram;
    if (v->fade && v->fade_tick < v->fade_frames) {
        const uint8_t *lut = od_palette_fade_lut();
        if (lut) {
            /* Map worlds: the round's fade-in is the brightness table on the round's own
             * palette, brightness max(1, t): the extracted world-1 clip (level-entry.fade)
             * is exactly that for 1-x (13/13 frames), and any other world fades in its own
             * colours. The captured table is never read: this ramp is the only load fade. */
            unsigned b = v->fade_tick < 1 ? 1u : v->fade_tick;
            for (unsigned i = 0; i < 32; ++i) {
                unsigned c = v->visible.palette.ram[i] & 0x3fu;
                shown.palette.ram[i] = (uint8_t)((c & 15u) | lut[(c & 0x30u) + b]);
            }
            memcpy(pal.ram, shown.palette.ram, 32);
        }
    }
    /* The user level's first frame keeps the editor's palette (f2906). */
    if (v->old_palette)
        memcpy(shown.palette.ram, v->old_palette_ram, 32);
    /* TRY AGAIN reload: the re-armed round shows no sprites on its first two
     * frames and no HUD on the first (reference f16421/f16422, f3721/f3722). */
    unsigned retry_k = v->retry_tick ? v->retry_tick - 1 : 0;
    int rearm = v->retry_tick && retry_k >= OD_RETRY_INIT_TICK &&
                retry_k <= OD_RETRY_INIT_TICK + 1;
    if (!show_sprites || rearm)
        shown.sprite_count = 0;
    od_compose_present(fb, &shown, f->assets->chr);
    /* In the boss arena the bar is the room's own background text (the round value
     * one column left of the normal layout) and it fades with the room palette:
     * reference f25750-f25760. */
    char value[16];
    if (f->from_editor)
        snprintf(value, sizeof value, "1");  /* the displayed world is the stored index plus one */
    else
        od_hud_round_text(value, sizeof value, f->level);
    /* the '-' stays in column 12, a two-digit world starts in column 10. */
    int val_col = OD_HUD_ROUND_VAL_COL -
                  (v->combat_active || f->level >= 90 ? 1 : 0);
    if (hud && v->bar_empty)
        /* A round load's first frame on screen: the bar rows are empty
         * (R1 f2220, TRY AGAIN f3221, arena f25749; CARGA-RONDA.md). */
        memset(fb->index + OD_HUD_TOP * OD_SCREEN_W, 0x0F,
               (OD_SCREEN_H - OD_HUD_TOP) * OD_SCREEN_W);
    else if (hud && v->level_template)
        /* The play loop start's template with the round value, over the normal bar or
         * the arena's own row alike (same position in both). */
        od_hud_draw_level_template(fb, value, val_col, f->assets->chr,
                                    v->combat_active ? &shown.palette : &pal);
    else if (hud && (v->bar_visible.round_blank || v->bar_visible.time_boxes)) {
        /* The pause close's template, over the normal bar or the arena's own row
         * alike: it is written at the same position in both. */
        od_hud_draw_template(fb, v->bar_visible.round_blank ? NULL : value,
                              val_col,
                              v->bar_visible.time_boxes ? NULL : &v->hud.visible,
                              f->assets->chr,
                              v->combat_active ? &shown.palette : &pal);
    } else if (hud && v->combat_active)
        od_hud_draw_row(fb, od_arena_hud_row(), &v->hud.visible,
                         f->assets->chr, &shown.palette);
    else if (hud && f->from_editor)
        /* The editor's user level: "ROUND 1" (the only user level of the product's
         * editor). */
        od_hud_draw_clock_user(fb, 0, &v->hud.visible, f->assets->chr, &pal);
    else if (hud)
        od_hud_draw_clock(fb, f->level, &v->hud.visible, f->assets->chr, &pal);
    /* The TRY AGAIN band stays up, cursor on TRY AGAIN, until the reload
     * re-arms the round. */
    if (v->pause.active || (v->retry_tick && retry_k < OD_RETRY_INIT_TICK))
        od_screens_retry(fb, f->assets->chr,
                          v->pause.active ? v->pause.menu.visible_selection
                                          : v->retry_new_game ? 1u : 0u);
    /* Auto-scrolling rounds: the status band, and the TRY AGAIN band over it, start two
     * lines higher (their tile row sits under the lava strip, which begins on line
     * 206), so the band moves two lines up and its last two lines are background
     * (C6.74, castle 1-10: the glyph rows are 223..229, 225..231 elsewhere). */
    if (shown.lava_on && (hud || v->pause.active || v->retry_tick)) {
        memmove(fb->index + OD_HUD_TOP * OD_SCREEN_W,
                fb->index + (OD_HUD_TOP + 2) * OD_SCREEN_W,
                (OD_SCREEN_H - OD_HUD_TOP - 2) * OD_SCREEN_W);
        memset(fb->index + (OD_SCREEN_H - 2) * OD_SCREEN_W, 0x0F, 2 * OD_SCREEN_W);
    }
    /* Reload fade: the whole picture through the brightness table. Once re-armed,
     * the arena fades in through its own palette. */
    const uint8_t *lut = od_palette_fade_lut();
    if (v->retry_tick && lut &&
        !(v->combat_active && retry_k >= OD_RETRY_INIT_TICK)) {
        unsigned b = retry_brightness(retry_k);
        for (unsigned i = 0; i < OD_SCREEN_W * OD_SCREEN_H; ++i) {
            uint8_t c = fb->index[i];
            fb->index[i] = (uint8_t)((c & 15) | lut[(c & 0x30) + b]);
        }
    }
}

/* ---- password / editor overlays ---------------------------------------- */

/* Mark the accepted magicword's rounds in the progress bitmap. */
static void password_apply(od_session *s, const od_password_view *pv)
{
    uint16_t words[OD_PASSWORD_WORDS];
    if (!od_password_ui_words(&pv->ui, words))
        return;
    for (int w = 0; w < OD_PASSWORD_WORDS; ++w)
        for (int j = 0; j < OD_WORLD_ROUNDS; ++j)
            if (words[w] & (1u << j))
                od_progress_set_cleared(&s->progress, w * OD_WORLD_ROUNDS + j, 1);
}

/* Publish the session's progress image (low ten bits plus the world flags) into
 * the scene's map words, so the map a password or the editor's follow-up menu
 * opens shows exactly the progress the original would. It uses
 * `od_progress_map_words`, not the raw magicword words. */
static void flow_set_map_words(od_flow *f)
{
    uint16_t words[OD_WORLD_COUNT];
    od_progress_map_words(&f->session.progress, words);
    for (int w = 0; w < OD_WORLD_COUNT; ++w)
        f->screens.map_words[w] = words[w];
}

/* R10: the session's progress (a loaded save, or RESET with the save intact)
 * reaches the screens at boot: the title route's menu init then draws the
 * follow-up menu like B and EXIT do, and its continue option opens the map with
 * the saved rounds. */
static void flow_publish_boot_progress(od_flow *f)
{
    flow_set_map_words(f);
    int progress = 0;
    for (int w = 0; w < OD_WORLD_COUNT; ++w)
        progress |= f->screens.map_words[w] != 0;
    f->screens.boot_progress = progress;
}

static void editor_begin(od_editor_view *ev)
{
    od_editor_init(&ev->ed);
    ev->shown = ev->ed;
    memset(&ev->cyc, 0, sizeof ev->cyc);
    ev->cyc_shown = ev->cyc;
    ev->cyc_started = 0;
    ev->primed = 0;
    ev->active = 1;
}

/* Back from a user level (solved, or new game): the original runs the editor's
 * own init, which reloads the screen and its palette and resets the tools
 * (od_editor_resume). Prepare the real board publisher BEFORE any subsequent
 * screens tick; the init reloads the palette and colour cycle. */
static void editor_return(od_flow *f)
{
    od_editor_resume(&f->evw.ed);
    f->evw.shown = f->evw.ed;
    memset(&f->evw.cyc, 0, sizeof f->evw.cyc);
    f->evw.cyc_shown = f->evw.cyc;
    f->evw.cyc_started = 0;
    f->evw.primed = 0;
    f->evw.active = 1;
    od_screens_editor_return(&f->screens, 0); /* compatibility arg, no seek */
    f->scene_sound_status = od_screens_editor_prepare_board(&f->screens, &f->evw.ed);
    f->from_editor = 0;
}

static void editor_user_level(od_editor_view *ev)
{
    memset(&ev->level, 0, sizeof ev->level);
    for (unsigned y = 0; y < OD_GRID_H; ++y)
        for (unsigned x = 0; x < OD_GRID_W; ++x)
            ev->level.grid[y][x] = ev->ed.cells[y][x];
    ev->level.player_position = (uint8_t)(ev->ed.start ? ev->ed.start : 0x11u);
    ev->level.metadata = 0;
    ev->level.command_len = 0;
    ev->level.commands = NULL;
}

static void flow_sequence_draw(const od_flow *f, od_index_frame *fb)
{
    const od_screens *s = &f->screens;
    if (s->scene == OD_SCREEN_PASSWORD && f->pw.active) {
        uint8_t tiles[24];
        od_scene_password_field_tiles(&f->pw, tiles);
        od_screens_draw_password(s, tiles, f->pw.shown.position,
                                  f->pw.shown.cursor, fb, f->assets->chr);
    } else if (s->scene == OD_SCREEN_EDITOR && !f->evw.active && s->pending) {
        /* EXIT's menu init (mode 3, A+1) has already blanked the editor
         * (reference f2251/f12751; only the round digit lingers, 22 px). */
        for (size_t i = 0; i < OD_SCREEN_W * OD_SCREEN_H; ++i)
            fb->index[i] = 0x0f;
    } else if (s->scene == OD_SCREEN_EDITOR && f->evw.active &&
               od_screens_editor_ready(s) && f->evw.primed >= 2) {
        /* C6.31: on the loop's first pass the init's last upload is still
         * the picture (draw_pal below); the round digit and the cursor its
         * CPU wrote show from the next frame (reference f2099/f2100,
         * f12599/f12600, R7 return f3952/f3953). */
        od_screens_draw_editor_cycle(s, &f->evw.shown, &f->evw.cyc_shown, fb,
                                      f->assets->chr);
    } else {
        od_screens_draw_pal(s, fb, f->assets->chr, &f->assets->palette_ram);
    }
}

/* Level-exit fade. Both exits of a solved room - the map and the boss ending -
 * run the same fade: from the frame after the celebration's end the brightness
 * drops by one per frame from 15 to 0 through the brightness table, and the
 * palette reaches the screen one frame later. Measured: 1-1 celebration end
 * f3974, brightness 14 at f3975 .. 0 at f3989, map f3991; the arena
 * celebration end f26606, 0 at f26621, ending f26623. `exit_frames` is 1 on the
 * celebration-end frame, so the picture's brightness is 17 - exit_frames from 3
 * on, 0 on the last tail frame and on the combat hand-off. The whole picture
 * fades, HUD included. */
static void flow_exit_fade(const od_flow *f, od_index_frame *fb)
{
    /* `exit_frames` only counts once the celebration is over, and the --boss
     * tool publishes it at the tail's end like the solve (hand-off black). */
    const od_game *g = &f->game;
    if (!g->cleared || g->exit_frames < 3)
        return;
    const uint8_t *lut = od_palette_fade_lut();
    if (!lut)
        return;
    unsigned b = g->exit_frames >= 17 ? 0u : (unsigned)(17 - g->exit_frames);
    for (unsigned i = 0; i < OD_SCREEN_W * OD_SCREEN_H; ++i) {
        uint8_t c = fb->index[i];
        fb->index[i] = (uint8_t)((c & 15) | lut[(c & 0x30) + b]);
    }
}

void od_flow_draw(const od_flow *f, od_index_frame *fb)
{
    if (f->scene_sound_status != OD_OK) return;
    if (f->screen == OD_FLOW_PLAY && f->playing && f->view.load_stage &&
        f->view.load_from_scene) {
        /* A round load from the map, a castle or the boss sequence: its first frame
         * still shows that scene (the map's next fade step), the next five are black
         * until the round reaches the screen (R1 f2214 map, f2215..f2219 black, f2220
         * 1-1; 8-10 f19281..f19286; 10-10 f21352..f21357). */
        if (f->view.load_stage == 1)
            od_screens_draw_load_hold(&f->screens, fb, f->assets->chr);
        else
            memset(fb->index, 0x0F, sizeof fb->index);
    } else if (f->screen == OD_FLOW_SEQUENCE && f->editor_load > 1) {
        memset(fb->index, 0x0F, sizeof fb->index);
    } else if (f->screen == OD_FLOW_PLAY && f->playing) {
        flow_view_draw(f, fb, f->hud);
        flow_exit_fade(f, fb);
    } else {
        flow_sequence_draw(f, fb);
    }
}

const char *od_flow_scene_name(const od_flow *f)
{
    if (f->screen == OD_FLOW_PLAY)
        return "play";
    if (f->screen == OD_FLOW_UNVERIFIED)
        return "unverified";
    return od_screens_name(&f->screens);
}

/* ---- audio integration (H6) -------------------------------------------- */

static void audio_core_tick_cb(void *ctx) { od_audio_tick((od_audio *)ctx); }
static void audio_core_reset_cb(void *ctx) { od_audio_reset_init((od_audio *)ctx); }
static int g_audio_trace;
static long g_audio_tick;
static void audio_core_request_cb(void *ctx, unsigned id)
{
    if (g_audio_trace)
        fprintf(stderr, "audio req %u @%ld\n", id, g_audio_tick);
    (void)od_audio_request((od_audio *)ctx, id);
}
static void audio_core_music_cb(void *ctx, int track)
{
    od_audio_play_music((od_audio *)ctx, track);
}

static void audio_begin(const od_audio_driver *d)
{
    if (d->tick)
        d->tick(d->ctx);
}

/* Every sound request of the frame is appended to an 8-slot queue, so two cues
 * of one frame both reach the driver (reference 1-1 f3715: two cues). Forward
 * the step's whole queue, in order. */
static void audio_accept(od_audio_driver *d, const od_game *g)
{
    if (!d->request || !g)
        return;
    for (unsigned i = 0; i < g->sound_count && i < 8; ++i)
        d->request(d->ctx, g->sound_queue[i]);
}

typedef struct { int scene; unsigned age; unsigned id; } audio_scene_cue;
static const audio_scene_cue audio_scene_cues[] = {
    /* The pending stop of the title route remains legacy bootstrap here.
     * The menu's own stop/theme are its native audio entry action, not ages.
     * Map B/editor EXIT remain their separate mode3 family in audio_menu. */
    { OD_SCREEN_MENU,           0,  1 },
    /* Follow-up menu (mode 3 init, magicword): the menu song request and the first
     * menu flag fire on the frame after the mode-3 frame and four later, on every
     * entry (reference END f11761/f11765, map B f4401/f4405, magicword Yes
     * f12701/f12705). */
    { OD_SCREEN_WHATNEXT,       1,  1 },
    { OD_SCREEN_WHATNEXT,       5,  8 },
    { OD_SCREEN_MAP,            0,  1 },
    /* The map song (26, at map age 5) is keyed on the map's own age in
     * `audio_scene_transitions`, whatever its focus scene. */
    /* Native root05 owns init/program/cleanup/skip. The independent mode0D
     * menu retains its own queued stop and START event (not map confirm37). */
    { OD_SCREEN_HIDDEN,         2,  1 },
    /* The map's own stop when it hands over to the round (after any castle sweep)
     * is `audio_scene_transitions`, not an age cue. */
    /* Root27 owns its executed init/program/cleanup requests. */
};
#define AUDIO_PLAY_AGE_START_SFX 37u
#define AUDIO_PLAY_AGE_LEVEL_THEME 76u

static void audio_scene(od_audio_driver *d, int screen, int scene,
                        unsigned scene_age, int pending, int pending_scene,
                        int level, int cleared, int entry_cue, int menu_lead,
                        const od_cinematic_sounds *bootstrap)
{
    if (!d->request)
        return;
    /* Accepted native setup/program families retain their entire ordered batch, even
     * when flow already handed off. An empty managed batch is never legacy
     * permission. The sound command in the data is data, not a new compiled ID cue. */
    if (bootstrap->managed)
        for (unsigned i = 0; i < bootstrap->count; ++i)
            d->request(d->ctx, bootstrap->ids[i]);
    if (screen == OD_FLOW_PLAY) {
        if (!d->was_play) {
            d->was_play = 1;
            d->play_age = 0;
            d->level_req = 0;
            d->celebration = 0;
        }
        unsigned age = (unsigned)++d->play_age;
        /* In the boss arena the core emits the entry's own cue 41 (`entry_cue`); the
         * agenda's copy would double it (--boss). */
        if (age == AUDIO_PLAY_AGE_START_SFX && !entry_cue)
            d->request(d->ctx, 41);
        if (age == AUDIO_PLAY_AGE_LEVEL_THEME) {
            /* The round number is tested before the world's table is loaded. Hidden
             * worlds retain that round-10 branch; the public helper's >=100 fallback must
             * not erase it. Reuse the verified normal round-10 request, not a new content ID
             * or a special captured level149 expectation. */
            unsigned music_level = (unsigned)level;
            if (level >= 100 && level % OD_WORLD_ROUNDS == OD_WORLD_ROUNDS - 1)
                music_level = OD_WORLD_ROUNDS - 1;
            unsigned req = od_audio_level_request(music_level);
            d->level_req = req;
            d->request(d->ctx, req);
        }
        /* The celebration's theme fade (ID 2, none on ROUND 10-10) is requested by the
         * core on the tick the hero's celebration pose starts (reference 1-1 f3847),
         * through the game queue. */
        (void)cleared;
        return;
    }
    d->was_play = 0;
    d->play_age = 0;
    d->level_req = 0;
    d->celebration = 0;
    /* START in the hidden menu queues 1 on its frame (reference f960), where the
     * round load starts directly (no map round entry and its 37). The boss
     * sequence's phase-3 completion schedules PLAY on its own frame, and the level
     * entry queues 1 there too: the arena hand-over (reference arenang f21353, one
     * frame after the boss `finish`'s direct stop at f21351). */
    if (pending && pending_scene == OD_SCREEN_PLAY &&
        (scene == OD_SCREEN_HIDDEN || scene == OD_SCREEN_BOSS))
        d->request(d->ctx, 1);
    for (size_t i = 0; i < sizeof audio_scene_cues / sizeof *audio_scene_cues; i++) {
        const audio_scene_cue *c = &audio_scene_cues[i];
        if (c->scene == OD_SCREEN_MENU && menu_lead > 0)
            continue; /* the menu init's own cues: audio_menu */
        if (c->scene == OD_SCREEN_MENU && scene == OD_SCREEN_CONVERSATION)
            continue; /* source-cleanup stop, not a second pending stop */
        if (c->age == 0) {
            if (pending && pending_scene == c->scene) d->request(d->ctx, c->id);
        } else if (scene == c->scene && scene_age == c->age) {
            d->request(d->ctx, c->id);
        }
    }
}

/* Scene-change requests that are not tied to a scene age:
 *  - the map hands over to the round: it sets mode 6 and requests 1 on the
 *    frame its fade has ended, after any castle sweep. That is the second
 *    loading stage here (R1: reference f2215, materialisation f2220 = stage 0);
 *  - a map opened straight from a solved round (no pending scene) gets its
 *    script's stop request on its first frame, age 0 (f3991); the menu route
 *    gets it from the pending cue (f1475);
 *  - the map song 26 at map age 5 on every real opening. */
/* The song-script families write their executed songs/stops; no boss clip
 * agenda. */

static void audio_scene_transitions(od_audio_driver *d, const od_flow *f)
{
    if (!d->request)
        return;
    /* A fresh confirm of the round focus requests the entry sound BEFORE its fade
     * and optional castle script. screens publishes exactly that native map event as
     * pending LEVEL_ENTRY (OD_MAP_EVENT_ENTER); it is not an elapsed-age cue or a
     * rendered frame. Keep it before the other transition requests, as in the former
     * agenda. */
    if (f->screen == OD_FLOW_SEQUENCE && f->screens.pending &&
        f->screens.next == OD_SCREEN_LEVEL_ENTRY)
        d->request(d->ctx, 37);
    if (f->screen == OD_FLOW_PLAY && f->view.load_stage == 2)
        d->request(d->ctx, 1);
    int map = f->screen == OD_FLOW_SEQUENCE && f->screens.map_reveal &&
              (f->screens.scene == OD_SCREEN_MAP ||
               f->screens.scene == OD_SCREEN_ROUND_SELECT);
    if (map && f->screens.map.age == 0 && !f->screens.pending)
        d->request(d->ctx, 1);
    /* The map-script's song request: the map song five frames after the map's first
     * (R1 f3996, menu route f1480). The castle map script runs it one frame earlier,
     * with its reveal (10-10 password route: open f11575, song f11579). */
    if (map && f->screens.map.age + od_screens_map_lead(&f->screens) == 5)
        d->request(d->ctx, 26);
}

/* Round-10 requests are drained from the executed native program below.
 * Legacy castle cues/skip ages remain package inventory, never a fallback. */

/* Executed world-clear/opening and castle-appearance batches retain their
 * complete ordered slot, including empty managed batches after handoff. */
static void audio_world_clear(od_audio_driver *d,
                              const od_cinematic_sounds *events)
{
    /* A managed empty batch forbids fallback;
     * ID0 is ordinary content. Emission stays at the old family's ordered slot,
     * even if this step has already dispatched its final handoff to the map. */
    if (events->managed) {
        if (d->request)
            for (unsigned i = 0; i < events->count; ++i)
                d->request(d->ctx, events->ids[i]);
        return;
    }
}

/* Magicword:
 *  - the follow-up menu's A queues 35 on its own frame, whatever the option
 *    (continue, quit and edit mode: reference f12200; map B route f4800);
 *  - the magicword screen's init requests its song three frames into the scene
 *    (reference f12278, B route f4878);
 * night-clip requests are native program events, never night clip cues. */
static void audio_magicword(od_audio_driver *d, const od_flow *f)
{
    const od_screens *s = &f->screens;
    const od_magicword_art *art = od_gfx_magicword_art();
    if (!d->request || f->screen != OD_FLOW_SEQUENCE)
        return;
    if (s->scene == OD_SCREEN_WHATNEXT && s->wn.leaving == OD_WHATNEXT_EXIT_TICKS)
        d->request(d->ctx, 35);
    /* Static v2 emits its data-defined request through the managed batch;
     * the legacy captured age is valid only for the distinct v1 provider. */
    if (od_gfx_static_magicword() || !art)
        return;
    if (s->scene == OD_SCREEN_MAGICWORD && s->age == art->song_age + 1u)
        d->request(d->ctx, art->song_id);
}

/* Scene requests of the menus:
 *  - the map's B and the editor's EXIT open the start-point menu through the
 *    mode-3 init, like the follow-up menu: the first request on the frame after
 *    mode 3 and the menu song four frames later (reference B f1800 ->
 *    1801/1805, EXIT without progress f2251 -> 2252/2256); the title route keeps
 *    its own cues;
 *  - the first menu's A queues 35 on its frame whatever the option (reference
 *    `beginning` f1400, `continue` f1500, `edit mode` f1700);
 *  - editor init/program sounds now come from the native screens publisher,
 *    including entry from either menu. No editor clip-age or menu-close
 *    countdown duplicates its own initial stop. */
/* Accepted menu option -> sound request 35, not a cinematic cue. Read the
 * post-simulation accept BEFORE dispatch; returning an empty managed batch
 * forbids inferring another accept from later state. Like the cinematic batch,
 * this event is local to one simulation tick. */
static od_cinematic_sounds flow_menu_sounds(const od_flow *f)
{
    od_cinematic_sounds events = {0};
    const od_screens *s = &f->screens;
    if (f->screen == OD_FLOW_SEQUENCE && s->scene == OD_SCREEN_MENU) {
        events.managed = 1;
        if (s->pending && (s->next == OD_SCREEN_MENU_CLOSE ||
                           s->next == OD_SCREEN_PASSWORD || s->next == OD_SCREEN_EDITOR)) {
            events.count = 1;
            events.ids[0] = 35;
        }
    }
    return events;
}

/* The conversation caller has entered the initial menu, not merely queued a
 * destination. This is the menu AUDIO init action: stop the previous driver,
 * then select its theme. The old age3/age7 agenda is not a setup-ready query.
 * Presentation setup is still owned by screens; do not claim it complete here.
 * Map/editor mode3 re-entry remains a separate, unverified sound family. */
static od_cinematic_sounds flow_menu_entry_sounds(od_screen_id before,
                                                  const od_flow *f)
{
    od_cinematic_sounds events = {0};
    if (f->screen == OD_FLOW_SEQUENCE && before == OD_SCREEN_CONVERSATION &&
        f->screens.scene == OD_SCREEN_MENU && f->screens.menu_lead < 0) {
        events.managed = 1;
        events.count = 2;
        events.ids[0] = 1;
        events.ids[1] = 8;
    }
    return events;
}

static void audio_menu(od_audio_driver *d, const od_flow *f,
                       const od_cinematic_sounds *events)
{
    const od_screens *s = &f->screens;
    /* Emit at this family's original slot, even after a scene handoff. */
    if (d->request && events->managed)
        for (unsigned i = 0; i < events->count; ++i)
            d->request(d->ctx, events->ids[i]);
    if (!d->request || f->screen != OD_FLOW_SEQUENCE)
        return;
    if (s->scene == OD_SCREEN_MENU && s->menu_lead > 0 && !s->pending) {
        if (s->age == 1)
            d->request(d->ctx, 1);
        else if (s->age == 5)
            d->request(d->ctx, 8);
    }
}

/* Retry in a round stops the driver on the first frame after the reload fade
 * reaches black (request 1; 2-10: A f16400 -> f16416; START pause in 1-1: A
 * f2500 -> f2516). The boss arena does not (A f26200: none). */
#define OD_RETRY_STOP_TICK 16u

/* The retry reload's requests outside the gameplay step. The band's pause cue
 * is the pause handler's own (scene_pause.c, published through the game's
 * queue); it takes the SFX channels and cuts the death cue still sounding.
 *  - a round: `1` at +16, then the re-arm at +21 replays the entry agenda, so
 *    41 and the world theme follow as on a fresh entry (2-10: f16457 and
 *    f16496 = +57 and +96; 1-1: f2557 and f2596);
 *  - the boss arena re-runs its entry (`od_combat_init`): no stop and no theme,
 *    only the entry's own 41 from the core (f26257 = +57). */
static void audio_play_events(od_audio_driver *d, const od_play_view *v)
{
    if (!d->request || !v->retry_tick || v->combat_active)
        return;
    unsigned k = v->retry_tick - 1;      /* ticks since the confirm */
    if (k == OD_RETRY_STOP_TICK)
        d->request(d->ctx, 1);
    if (k == OD_RETRY_INIT_TICK) {
        d->play_age = 0;                 /* the entry cue and the theme again */
        d->level_req = 0;
    }
}

/* ---- measurement outputs ----------------------------------------------- */

static uint64_t fnv1a64(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

static const char *screen_name(int screen)
{
    switch (screen) {
    case OD_FLOW_SEQUENCE:   return "sequence";
    case OD_FLOW_PLAY:       return "play";
    case OD_FLOW_UNVERIFIED: return "unverified";
    default:                  return "?";
    }
}

static unsigned semantic_hash(const od_game *g, int screen, int level,
                              unsigned buttons)
{
    char buf[192];
    int n = snprintf(buf, sizeof buf,
                     "t=%s lvl=%d x=%d y=%d f=%d act=%u ph=%u pose=%u b=%u "
                     "fires=%d clear=%d pend=%d dead=%d",
                     screen_name(screen), level, g->hero_x, g->hero_y, g->facing,
                     g->hero_action, g->hero_phase, g->hero_pose, buttons,
                     g->fires, g->cleared, g->pending, g->dead);
    if (n < 0) return 0;
    size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
    return (unsigned)(fnv1a64(0xcbf29ce484222325ull, buf, len) & 0xFFFFFFFFu);
}

static void app_state_header(FILE *f)
{
    fputs("tick,screen,scene,level,cleared,exit,combat,act,cell,tgt,wait,ox,oy,"
          "pose,bank,px,py,fires,boss,poses,atk,alive,grid,flames\n", f);
}

static void app_state_row(FILE *f, long tick, int screen, const od_screens *s,
                          int level, const od_game *g, const od_play_view *v)
{
    fprintf(f, "%ld,%s,%s,%d,%d,%d,%d,%u,%d,%d,%u,%d,%d,%u,%u,%d,%d,%d,",
            tick, screen_name(screen), od_screens_name(s), level, g->cleared,
            g->level_exit, v->combat_active, g->hero_action,
            g->hero_y * 16 + g->hero_x,
            g->hero_target_y * 16 + g->hero_target_x, g->hero_wait,
            g->hero_ox, g->hero_oy, g->hero_pose, g->hero_set,
            g->hero_px, g->hero_py, g->fires);
    for (int i = 0; i < 16; ++i) fprintf(f, "%02x", v->combat.boss.state[i]);
    fputc(',', f);
    for (int i = 0; i < 8; ++i)
        fprintf(f, "%02x%02x", v->combat.boss.poses[i] & 0xff,
                                (v->combat.boss.poses[i] >> 8) & 0xff);
    fputc(',', f);
    for (int i = 0; i < 4; ++i) fprintf(f, "%02x", v->combat.attack.state[i]);
    fprintf(f, ",%d,", v->combat.attack.alive ? 1 : 0);
    if (v->combat_active) {
        /* End-of-tick board: the live board (the attack's `grid` copy is taken before
         * the gameplay step and misses its cascade) plus the attack's extra row 15. */
        for (int y = 0; y < 15; ++y)
            for (int x = 0; x < 16; ++x) fprintf(f, "%02x", g->cells[y][x]);
        for (int i = 240; i < 256; ++i) fprintf(f, "%02x", v->combat.grid[i]);
    } else {
        for (int y = 0; y < 15; ++y)
            for (int x = 0; x < 16; ++x) fprintf(f, "%02x", g->cells[y][x]);
    }
    /* C6.47: the eight flame slots (type, cell, state[8]) for MJ1 of the flame/ice
     * contact; empty slots print as zeros like the original memory. */
    fputc(',', f);
    for (int i = 0; i < 8; ++i) {
        if (i < g->flame_count) {
            fprintf(f, "%02x%02x", g->flames[i].type, g->flames[i].cell);
            for (int k = 0; k < 8; ++k) fprintf(f, "%02x", g->flames[i].state[k]);
        } else {
            fputs("0000000000000000", f);
            fputs("0000", f);
        }
    }
    fputc('\n', f);
}

static int write_wav(const char *path, const int16_t *samples, size_t count,
                     int rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    uint32_t data = (uint32_t)(count * 2), size = data + 36;
    uint16_t channels = 1, bits = 16, block = (uint16_t)(channels * bits / 8);
    uint32_t byte_rate = (uint32_t)(rate * block);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    h[4]=(uint8_t)size; h[5]=(uint8_t)(size>>8); h[6]=(uint8_t)(size>>16); h[7]=(uint8_t)(size>>24);
    memcpy(h+8, "WAVEfmt ", 8);
    h[16]=16; h[17]=0; h[18]=0; h[19]=0;
    h[20]=1; h[21]=0;
    h[22]=(uint8_t)channels; h[23]=(uint8_t)(channels>>8);
    h[24]=(uint8_t)rate; h[25]=(uint8_t)(rate>>8); h[26]=(uint8_t)(rate>>16); h[27]=(uint8_t)(rate>>24);
    h[28]=(uint8_t)byte_rate; h[29]=(uint8_t)(byte_rate>>8); h[30]=(uint8_t)(byte_rate>>16); h[31]=(uint8_t)(byte_rate>>24);
    h[32]=(uint8_t)block; h[33]=(uint8_t)(block>>8);
    h[34]=(uint8_t)bits; h[35]=(uint8_t)(bits>>8);
    memcpy(h+36, "data", 4);
    h[40]=(uint8_t)data; h[41]=(uint8_t)(data>>8); h[42]=(uint8_t)(data>>16); h[43]=(uint8_t)(data>>24);
    int ok = fwrite(h,1,44,f)==44 && fwrite(samples,2,count,f)==count;
    fclose(f);
    return ok ? 0 : 1;
}

/* ---- the per-tick scene machine ---------------------------------------- */

/* New game in the boss arena: without the arena's end-of-fight bit, the loop
 * un-clears ROUND 10-10 in the progress word and clears the world's castle
 * latch, keeps the position on world 10 round 10 and opens the map (mode 5). The
 * map keeps the "return" bit the 10-10 clear set, so its init resumes in the
 * round focus on the castle round. Reference: world-10 progress word 43FF ->
 * 41FF and the position bytes 63 09 09 at f26380, the map's opening sequence
 * f26380-f26422, init at f26425, and the first A it reads is the round
 * handler's on f26426, 62 frames after the confirming A of f26364. That A
 * enters ROUND 10-10 through the boss sequence. The 62 frames are the reload
 * fade (+16, flow_view_tick) and the map's own opening. */
static void flow_enter_map_after_round(od_flow *f);

static void flow_new_game_from_arena(od_flow *f)
{
    od_progress_set_cleared(&f->session.progress, OD_FINAL_LEVEL, 0);
    f->session.world = OD_FINAL_LEVEL / OD_WORLD_ROUNDS;
    f->session.round = OD_FINAL_LEVEL % OD_WORLD_ROUNDS;
    f->session.level = OD_FINAL_LEVEL;
    f->view.combat_active = 0;
    f->view.combat_pending = 0;
    flow_save_except(f, OD_FINAL_LEVEL);
    flow_enter_map_after_round(f);
}

/* A solved round's exit tail hands over to the map (mode 5). That frame is the
 * map's first (reference f3991 in R1): the map is at age 0 on it, exactly like
 * the menu route's (f1475), so it is not stepped here. The opening plays the
 * split-window reveal and reads no input until it returns (reference: an A at
 * f4010 is ignored), the same 45 frames as the menu route. New game opens the
 * map the same way at +16 of its fade, with the return bit too (pause new game
 * in 1-1: A f2600, map mode f2616, return bit set at f2662). */
static void flow_enter_map_after_round(od_flow *f)
{
    uint16_t words[OD_WORLD_COUNT];
    od_progress_map_words(&f->session.progress, words);
    od_screens_map_progress_world(&f->screens, words, f->session.world,
                                   OD_MAP_REVEAL_FRAMES);
    f->screens.map_reveal = 1;
    /* the return bit is set: the loop init resumes the round focus at map age 46. */
    od_screens_map_round_focus_after_clear(&f->screens);
    f->screen = OD_FLOW_SEQUENCE;
    /* the sprite rotation leaves the round frozen by its exit tail (R1: 3 from
     * f3975). */
    f->oam_rotation = (uint8_t)((f->view.visual.oam_mode + 1) & 3);
    f->oam_rotation_known = 1;
}

/* The sprite rotation on the map: its object pass (the sprites drawn from age
 * 47) steps the rotation once per frame from age 46 up to the frame before the
 * confirming A, and nothing steps it from that A to the round's first object
 * pass (R1: frozen 3 through f4036, 0 at f4037, ..., 2 at f4399, frozen from
 * the A of f4400 to f4420). A castle sweep or any other scene in between is
 * not modelled, so the value is dropped. */
#define OD_MAP_OAM_FIRST_AGE 46u

static void flow_map_oam_rotation(od_flow *f)
{
    if (!f->oam_rotation_known)
        return;
    od_screen_id scene = f->screens.scene;
    if (scene == OD_SCREEN_MAP || scene == OD_SCREEN_ROUND_SELECT) {
        /* The castle map's init is one frame earlier. */
        if (!f->screens.pending &&
            f->screens.map.age + od_screens_map_lead(&f->screens) >=
                OD_MAP_OAM_FIRST_AGE)
            f->oam_rotation = (uint8_t)((f->oam_rotation + 1) & 3);
    } else if ((scene != OD_SCREEN_LEVEL_ENTRY && scene != OD_SCREEN_PLAY) ||
               f->screens.level_castle) {
        f->oam_rotation_known = 0;
    }
}

/* Every video update that reaches the end of its handler increments a 16-bit
 * frame counter; the frames whose update is off while the main code loads (or
 * whose update returns early) lose that increment. The main loop of a frame
 * sees the value its update left, so the step decides from the state the
 * previous step left. Measured on the PAL reference, the lost frames are, by
 * scene event:
 *   - power-on/RESET: the counter stays 0 through f4 (`frame_tick_boot`);
 *   - the opening cinematic: its frames 198, 1489 and 2331 (no START: f198,
 *     f1489, f2331); START skips to the title, whose load loses START+8
 *     (START f250/f300/f420 -> f258/f308/f428);
 *   - the map opening: +3 (f1478, f3994, f2619, f11578, f26383);
 *   - a round-10 castle: native background transfer render_enabled, queried
 *     before the step (no captured lost-frame calendar);
 *   - the boss sequence of ROUND 10-10: f19789/f19790 (+1474/+1475);
 *   - a round's load: the two frames before the materialisation (1-1
 *     f2218/9 -> f2220, 1-2 f4418/9, 8-10 f19285/6, 10-10 f21356/7);
 *   - a retry reload: +19 and +20 (1-1 A f3200 -> f3219/f3220);
 *   - the arena entry: +2 and +3 of the arena mode (f25746/f25747).
 * The password, menu and follow-up scenes lose none. */
static void flow_frame_tick(od_flow *f)
{
    const od_screens *s = &f->screens;
    const od_play_view *v = &f->view;
    int lost = 0;
    if (f->frame_tick_boot) {
        --f->frame_tick_boot;
        return;
    }
    if (f->screen == OD_FLOW_SEQUENCE) {
        if (od_screens_post_ending_active(s)) {
            if (od_screens_post_ending_frame_tick_enabled(s)) ++f->screens.frame_tick;
            return;
        }
        if (od_screens_boss_ending_active(s)) {
            if (od_screens_boss_ending_frame_tick_enabled(s)) ++f->screens.frame_tick;
            return;
        }
        if (od_screens_goodnight_hidden_active(s)) {
            if (od_screens_goodnight_hidden_frame_tick_enabled(s)) ++f->screens.frame_tick;
            return;
        }
        switch (s->scene) {
        case OD_SCREEN_INTRO:
            lost = s->age == 198 || s->age == 1489 || s->age == 2331;
            break;
        case OD_SCREEN_TITLE:
            lost = s->age == 7;
            break;
        case OD_SCREEN_MAP:
        case OD_SCREEN_ROUND_SELECT:
            lost = s->map_reveal && s->map.age == 2;
            break;
        case OD_SCREEN_LEVEL_ENTRY:
            /* Query BEFORE simulation: the native background transfer owns the actual
             * renderer flag, not a captured lost-frame list. */
            if (od_screens_castle_round10_active(s))
                lost = !od_screens_castle_round10_frame_tick_enabled(s);
            break;
        case OD_SCREEN_WORLD_CLEAR: {
            if (od_screens_world_clear_active(s)) {
                lost = !od_screens_world_clear_frame_tick_enabled(s);
                break;
            }
            /* Independent castle appearance (part2) is unchanged. */
            const od_castle_info *c = s->world_clear_part == 2
                                           ? od_screens_world_clear_info(s) : NULL;
            unsigned t = s->age;
            for (unsigned k = 0; c && !s->world_clear_skip && k < c->frame_tick_lost_count; ++k)
                if (c->frame_tick_lost[k] == t)
                    lost = 1;
            break;
        }
        case OD_SCREEN_EDITOR:
            /* The user level's load loses one frame, the 3rd black frame (editor PLAY A
             * f2900: counter value 82 on f2904 and f2905; A f3650: one lost between f3652
             * and f3914), which phases the colour cycle after the return. */
            lost = f->editor_load == OD_EDITOR_LOAD_BLACK;
            break;
        default:
            break;
        }
    } else if (f->screen == OD_FLOW_PLAY) {
        if (v->load_stage == 4 || v->load_stage == 5)
            lost = 1;
        else if (!v->combat_active && !v->retry_new_game &&
                 (v->retry_tick == 19 || v->retry_tick == 20))
            lost = 1;
        else if (v->combat_pending && !v->combat_active &&
                 (v->combat_arm == 1 || v->combat_arm == 2))
            lost = 1;
    }
    if (!lost)
        ++f->screens.frame_tick;
}

/* Drain immediately after the scene simulation, before flow can change its
 * family at dispatch. Only accepted families emit; other sources drain and
 * discard until their full caller/source list is independently verified.
 * No queued batch or new pointer enters the saved simulation state. */
static od_status flow_take_scene_sounds(od_flow *f, od_cinematic_sounds *out)
{
    od_cinematic_sounds batch = {0};
    memset(out, 0, sizeof *out);
    od_status st = od_screens_take_sounds(&f->screens, &batch);
    if (st != OD_OK) return st;
    if (batch.overflow || batch.count > sizeof batch.ids) return OD_ERR_FORMAT;
    if (od_screens_post_ending_active(&f->screens) ||
        od_screens_boss_ending_active(&f->screens) ||
        od_screens_goodnight_hidden_active(&f->screens) ||
        od_screens_world_clear_active(&f->screens) ||
        od_screens_castle_round10_active(&f->screens) ||
        (f->screens.scene == OD_SCREEN_WORLD_CLEAR && f->screens.world_clear_part == 2) ||
        f->screens.scene == OD_SCREEN_CONVERSATION ||
        f->screens.scene == OD_SCREEN_INTRO || f->screens.scene == OD_SCREEN_TITLE ||
        (f->screens.scene == OD_SCREEN_MAGICWORD && od_gfx_static_magicword()) ||
        f->screens.scene == OD_SCREEN_PASSWORD || f->screens.scene == OD_SCREEN_EDITOR) {
        if (!batch.managed) return OD_ERR_FORMAT;
        /* Caller abort of an active script performs a cleanup stop after its executed
         * commands. Native finish already emits it. screens publishes the accepted skip
         * as this pending handoff; no age, inferred buttons, captured request or
         * duplicate-stop suppression. */
        if (f->screens.scene == OD_SCREEN_CONVERSATION &&
            f->screens.pending && f->screens.next == OD_SCREEN_MENU &&
            !f->screens.cinematic.finished) {
            if (batch.count == sizeof batch.ids) return OD_ERR_FORMAT;
            batch.ids[batch.count++] = 1;
        }
        *out = batch;
    }
    return OD_OK;
}

/* Executed actions use an additive local batch, independently of the editor
 * setup/program batch from screens. Never manage unrelated families. The grid
 * movement queues sound request 34 and the entry into play queues a stop. Drain
 * before any dispatcher can reset the source state. */
static od_status flow_take_editor_actions(od_editor *ed, od_cinematic_sounds *out)
{
    uint8_t actions[OD_EDITOR_ACTION_CAPACITY];
    size_t count = 0;
    memset(out, 0, sizeof *out);
    od_status st = od_editor_take_actions(ed, actions, sizeof actions, &count);
    if (st != OD_OK) return st;
    if (count > sizeof out->ids) return OD_ERR_FORMAT;
    for (size_t i = 0; i < count; ++i) {
        if (actions[i] == OD_EDITOR_ACTION_GRID_MOVE) out->ids[i] = 34;
        else if (actions[i] == OD_EDITOR_ACTION_PLAY_ENTER) out->ids[i] = 1;
        else { memset(out, 0, sizeof *out); return OD_ERR_FORMAT; }
    }
    out->count = (unsigned)count;
    return OD_OK;
}

/* C6.31 (CORE-3): a custom level run is over. */
static void flow_custom_finish(od_flow *f, int result)
{
    f->custom_result = result;
    f->custom_result_tick = f->sim_tick;
    printf("play-level: %s at tick %ld (custom level; no progress written)\n",
           result == OD_FLOW_CUSTOM_SOLVED ? "solved" : "NEW GAME", f->sim_tick);
}

void od_flow_step(od_flow *f, unsigned buttons)
{
    if (f->lvl_log.armed && f->screen == OD_FLOW_PLAY)
        level_log_push(f, buttons);
    else
        f->lvl_log.armed = 0;
    /* Error remains inspectable by the host and halts further simulation.
     * Never truncate a bad batch or silently resume its captured agenda. */
    if (f->scene_sound_status != OD_OK) return;
    /* C6.31: a finished custom level run holds its last frame. Ticks still
     * count, so a deterministic batch (--ticks N) runs exactly N ticks. */
    if (f->custom_result) {
        f->sim_tick++;
        return;
    }
    if (f->screen == OD_FLOW_SEQUENCE && od_screens_post_ending_active(&f->screens)) {
        f->scene_sound_status = od_screens_post_ending_status(&f->screens);
        if (f->scene_sound_status != OD_OK) return;
        if (!f->screens.cinematic.sounds.managed ||
            f->screens.cinematic.sounds.overflow ||
            f->screens.cinematic.sounds.count > sizeof f->screens.cinematic.sounds.ids ||
            f->screens.pending) {
            f->scene_sound_status = OD_ERR_FORMAT;
            return;
        }
        /* The end marker only ends the finite program. The engine publishes forever;
         * no START/complete/caller-return test may dispatch this owner. */
    }
    if (f->screen == OD_FLOW_SEQUENCE && od_screens_boss_ending_active(&f->screens)) {
        f->scene_sound_status = od_screens_boss_ending_status(&f->screens);
        if (f->scene_sound_status != OD_OK) return;
        if (!f->screens.cinematic.sounds.managed ||
            f->screens.cinematic.sounds.overflow ||
            f->screens.cinematic.sounds.count > sizeof f->screens.cinematic.sounds.ids) {
            f->scene_sound_status = OD_ERR_FORMAT;
            return;
        }
        if (f->screens.pending && !od_screens_boss_ending_complete(&f->screens)) {
            f->scene_sound_status = OD_ERR_FORMAT;
            return;
        }
        if (od_screens_boss_ending_kind(&f->screens) == 1 &&
            od_screens_boss_ending_complete(&f->screens)) {
            if (f->screens.pending || f->screens.cinematic.sounds.count) {
                f->scene_sound_status = OD_ERR_FORMAT;
                return;
            }
            /* The program switch resets the graphics context, sets the animation counter to
             * 0 and starts the next song script. The graphics dispatcher installs a split
             * handler, NOT palette data, and the song script immediately clears that
             * handler state before its own entry (no intervening yield); begin resets native
             * scratch/context. The counter has no reached consumer in the infinite engine
             * loop. Do not synthesize a palette, sound or unreachable loop. The previous
             * root's final batch was drained on the cleanup publication step above; switch
             * opaque owners only past that boundary. */
            f->scene_sound_status = od_screens_post_ending_begin(&f->screens);
            if (f->scene_sound_status != OD_OK) return;
        }
    }
    if (f->screen == OD_FLOW_SEQUENCE && od_screens_goodnight_hidden_active(&f->screens)) {
        f->scene_sound_status = od_screens_goodnight_hidden_status(&f->screens);
        if (f->scene_sound_status != OD_OK) return;
        if (od_screens_goodnight_hidden_kind(&f->screens) == 1 &&
            f->screens.pending && f->screens.next == OD_SCREEN_HIDDEN &&
            !od_screens_goodnight_hidden_complete(&f->screens)) {
            f->scene_sound_status = OD_ERR_FORMAT;
            return;
        }
    }
    if (f->screen == OD_FLOW_SEQUENCE && od_screens_world_clear_active(&f->screens)) {
        f->scene_sound_status = od_screens_world_clear_status(&f->screens);
        if (f->scene_sound_status != OD_OK) return;
        /* World9's transient root25 completion cannot dispatch before42. */
        if (f->screens.pending && f->screens.next == OD_SCREEN_WHATNEXT &&
            !od_screens_world_clear_complete(&f->screens)) {
            f->scene_sound_status = OD_ERR_FORMAT;
            return;
        }
    }
    if (f->screen == OD_FLOW_SEQUENCE && od_screens_castle_round10_active(&f->screens)) {
        f->scene_sound_status = od_screens_castle_round10_status(&f->screens);
        if (f->scene_sound_status != OD_OK) return;
        /* Cleanup must have been published before screens may dispatch PLAY. */
        if (f->screens.pending && f->screens.next == OD_SCREEN_PLAY &&
            !od_screens_castle_round10_complete(&f->screens)) {
            f->scene_sound_status = OD_ERR_FORMAT;
            return;
        }
    }
    od_cinematic_sounds scene_sounds = {0};
    od_cinematic_sounds menu_sounds = {0};
    od_cinematic_sounds bootstrap_sounds = {0};
    od_cinematic_sounds editor_actions = {0};
    /* Static init has already queued its semantic one-shot. Validate/drain it
     * before advancing the video update/audio; a managed empty slot remains
     * authoritative. A scene entered by this step is drained below at its normal
     * handoff. */
    if (f->screen == OD_FLOW_SEQUENCE &&
        f->screens.scene == OD_SCREEN_MAGICWORD && od_gfx_static_magicword()) {
        f->scene_sound_status = flow_take_scene_sounds(f, &scene_sounds);
        if (f->scene_sound_status != OD_OK) return;
    }
    f->last_buttons = buttons;
    flow_frame_tick(f);
    g_audio_tick = f->sim_tick;
    if (f->audio_on) {
        if (f->sim_tick == 3 && f->audio.reset_init)
            f->audio.reset_init(f->audio.ctx);
        audio_begin(&f->audio);
    }

    if (f->screen == OD_FLOW_SEQUENCE) {
        od_screen_id before = f->screens.scene;
        od_screens_tick(&f->screens, buttons);
        if (od_screens_post_ending_active(&f->screens)) {
            f->scene_sound_status = od_screens_post_ending_status(&f->screens);
            if (f->scene_sound_status != OD_OK) return;
            if (f->screens.pending) {
                f->scene_sound_status = OD_ERR_FORMAT;
                return;
            }
        }
        if (od_screens_boss_ending_active(&f->screens)) {
            f->scene_sound_status = od_screens_boss_ending_status(&f->screens);
            if (f->scene_sound_status != OD_OK) return;
            if (f->screens.pending && !od_screens_boss_ending_complete(&f->screens)) {
                f->scene_sound_status = OD_ERR_FORMAT;
                return;
            }
        }
        /* World 9 round 10 calls the request only with the high progress byte's sign
         * clear. Screens' generic schedule is refined here before its next dispatch,
         * never by scene age. */
        if (f->screens.scene == OD_SCREEN_LEVEL_ENTRY && f->screens.pending &&
            f->screens.next == OD_SCREEN_BOSS) {
            if (f->screens.world != 9 || f->screens.round != 9) {
                f->scene_sound_status = OD_ERR_FORMAT;
                return;
            }
            if (f->screens.map_words[9] & 0x8000u)
                f->screens.next = OD_SCREEN_PLAY;
        }
        if (od_screens_goodnight_hidden_active(&f->screens)) {
            f->scene_sound_status = od_screens_goodnight_hidden_status(&f->screens);
            if (f->scene_sound_status != OD_OK) return;
        }
        if (od_screens_world_clear_active(&f->screens)) {
            f->scene_sound_status = od_screens_world_clear_status(&f->screens);
            if (f->scene_sound_status != OD_OK) return;
        }
        if (od_screens_castle_round10_active(&f->screens)) {
            f->scene_sound_status = od_screens_castle_round10_status(&f->screens);
            if (f->scene_sound_status != OD_OK) return;
        }
        if (f->screens.scene == OD_SCREEN_EDITOR) {
            f->scene_sound_status = od_screens_editor_status(&f->screens);
            if (f->scene_sound_status != OD_OK) return;
        }
        od_cinematic_sounds executed_sounds = {0};
        f->scene_sound_status = flow_take_scene_sounds(f, &executed_sounds);
        if (f->scene_sound_status != OD_OK) return;
        if (scene_sounds.count > sizeof scene_sounds.ids - executed_sounds.count) {
            f->scene_sound_status = OD_ERR_FORMAT;
            return;
        }
        memcpy(scene_sounds.ids + scene_sounds.count, executed_sounds.ids,
               executed_sounds.count);
        scene_sounds.count += executed_sounds.count;
        scene_sounds.managed |= executed_sounds.managed;
        /* This handoff calls the graphics dispatcher, NOT a sound request. The native
         * constructor already owns the sole init stop; do not synthesize another stop
         * from this handoff. */
        if (od_screens_post_ending_active(&f->screens) ||
            od_screens_boss_ending_active(&f->screens) ||
            od_screens_goodnight_hidden_active(&f->screens) ||
            od_screens_castle_round10_active(&f->screens) ||
            f->screens.scene == OD_SCREEN_CONVERSATION ||
            f->screens.scene == OD_SCREEN_INTRO || f->screens.scene == OD_SCREEN_TITLE ||
            f->screens.scene == OD_SCREEN_PASSWORD || f->screens.scene == OD_SCREEN_EDITOR) {
            bootstrap_sounds = scene_sounds;
            memset(&scene_sounds, 0, sizeof scene_sounds);
        }
        menu_sounds = flow_menu_sounds(f);
        od_cinematic_sounds entry_sounds = flow_menu_entry_sounds(before, f);
        if (entry_sounds.managed)
            menu_sounds = entry_sounds;
        flow_map_oam_rotation(f);
        if (f->screens.scene == OD_SCREEN_WORLD_CLEAR && f->screens.world_clear_to_map) {
            /* Castle appearance: after the castle appearance's last frame the clear
             * handler returns to the round's map hand-over: the map opens on this frame
             * like after any round, with the return bit (reference 1-1 last: map mode
             * f18307, first request, round focus on round 10). The castle clip breaks the
             * sprite-rotation value (not modelled). */
            f->screens.world_clear_to_map = 0;
            flow_enter_map_after_round(f);
            f->oam_rotation_known = 0;
        } else if (f->screens.scene == OD_SCREEN_PASSWORD) {
            f->screens.password_error_blank = 0;
            if (od_screens_password_ready(&f->screens)) {
                if (!f->pw.active)
                    od_scene_password_begin(&f->pw);
                od_password_result pr =
                    od_scene_password_tick(&f->pw, buttons);
                if (pr == OD_PASSWORD_ACCEPTED) {
                    password_apply(&f->session, &f->pw);
                    f->pw.active = 0;
                    flow_save(f);
                    /* The accepted magicword queues 36 on its frame, the
                     * mode-3 frame of "What next?" (reference END f11760;
                     * WHATNEXT-FADE). */
                    if (f->audio_on && f->audio.request)
                        f->audio.request(f->audio.ctx, 36);
                    /* C3.3: the accepted magicword opens the "What next?"
                     * menu; its `cont.story` A later opens the map with the
                     * progress `password_apply` just wrote. No 540-tick wait
                     * stands in for the missing screen (A6). */
                    flow_set_map_words(f);
                    od_screens_whatnext(&f->screens);
                } else if (pr == OD_PASSWORD_REJECTED) {
                    /* The invalid-password message stays for 180 ticks. It reaches the screen one
                     * video frame later (f1701 in the measured route), like the progress write. */
                    f->screens.password_error_delay = 1;
                } else if (f->screens.password_error_delay) {
                    f->screens.password_error_delay = 0;
                    f->screens.password_error = 180;
                } else if (f->screens.password_error) {
                    /* The tick the message reverts carries no cursor sprite
                     * (reference OAM empty at RAM f1880, shown f1881). */
                    if (--f->screens.password_error == 0)
                        f->screens.password_error_blank = 1;
                }
            }
        } else if (f->screens.scene == OD_SCREEN_EDITOR && f->editor_load) {
            if (++f->editor_load > OD_EDITOR_LOAD_BLACK + 1u) {
                f->editor_load = 0;
                od_game_init(&f->game, &f->evw.level);
                flow_view_init(f, NULL, 0);
                /* The user level starts like any level: the bank animation's first call comes
                 * 22 frames before the first entry pose (reference f2905 and f2927). */
                flow_view_materialised(f);
                flow_view_bar_load(f);
                f->view.old_palette = 1;
                memcpy(f->view.old_palette_ram, f->editor_palette, 32);
                f->clear_timer = -1;
                f->world_clear_world = -1;
                f->castle_appears_world = -1;
                f->from_editor = 1;
                f->playing = 1;
                f->level = 0;
                f->screen = OD_FLOW_PLAY;
            }
        } else if (f->screens.scene == OD_SCREEN_EDITOR) {
            if (od_screens_editor_ready(&f->screens)) {
                if (!f->evw.active)
                    editor_begin(&f->evw);
                /* The sprite/background update reaches the screen one frame after the CPU
                 * state, so the drawn editor is the pre-tick state. The CPU reads the current
                 * buttons (the press edge and the held paint mask). */
                /* While the blocking routine stalls the loop no sprite/background update is
                 * queued: the picture keeps the pass before START until the loop resumes
                 * (reference f2801..f2804). */
                /* The loop's first pass: the palette buffer holds the init's
                 * palette (the clip's recorded cycle must not leak in). */
                if (!f->evw.cyc_started) {
                    od_scene_editor_cycle_start(&f->evw.cyc, f->screens.display);
                    f->evw.cyc_started = 1;
                }
                unsigned was_stalled = f->evw.ed.stall;
                const int first_pass = f->evw.primed == 0;
                if (!was_stalled && !first_pass)
                    f->evw.shown = f->evw.ed;
                if (f->evw.primed < 2)
                    f->evw.primed++;
                f->evw.cyc_shown = f->evw.cyc;
                od_editor_event ee = od_editor_tick(&f->evw.ed, buttons);
                f->scene_sound_status = flow_take_editor_actions(&f->evw.ed, &editor_actions);
                if (f->scene_sound_status != OD_OK) return;
                if (ee == OD_EDITOR_EVENT_DATA_ERROR) {
                    f->scene_sound_status = OD_ERR_FORMAT;
                    return;
                }
                /* The blocked frames show the string re-upload. */
                f->evw.cyc_shown.toolbar_phase =
                    was_stalled ? (uint8_t)(OD_EDITOR_TOOLBAR_STALL -
                                            f->evw.ed.stall)
                                : 0;
                /* Each pass of the editor loop ends with the palette-buffer step. A pass
                 * blocked in the stall runs it only on its last frame (the 4th after START), so
                 * none runs while `stall` is set. */
                /* C6.31: the first pass (whose CPU wrote the cursor, still
                 * shown next frame) leaves the colour cycle at its start, so
                 * the first editor picture has the init's colours (reference
                 * f12600, f2100, f3953). */
                if (f->evw.ed.stall == 0 && !first_pass)
                    od_scene_editor_cycle_step(&f->evw.cyc, f->screens.frame_tick,
                                                &f->screens.frame_cycle,
                                                &f->screens.cycle_6f);
                if (ee == OD_EDITOR_EVENT_PLAY) {
                    /* PLAY loads the user level like a round: the editor stays on screen this
                     * frame, three black frames follow and the level reaches the screen on the
                     * fourth (reference A f2900: editor f2902, black f2903..f2905, level f2906). */
                    editor_user_level(&f->evw);
                    for (unsigned i = 0; i < 32; ++i)
                        f->editor_palette[i] =
                            (uint8_t)(f->screens.display[i] & 0x3fu);
                    /* The palette buffer keeps the cycle's last writes (reference f2902 colour 26
                     * and sprites 30, still on f2906). */
                    od_scene_editor_cycle_apply_ram(&f->evw.cyc,
                                                     f->editor_palette);
                    f->editor_load = 1;
                } else if (ee == OD_EDITOR_EVENT_EXIT) {
                    /* C3.7: the edited level is exported in the package JSON format before the
                     * discard. The host names the output file with OD_EDITOR_EXPORT (the interactive
                     * app has no menu slot for it); without it the discard stands. */
                    const char *exp = getenv("OD_EDITOR_EXPORT");
                    if (exp && exp[0])
                        od_editor_export(&f->evw.ed, exp, 1, 1, "editor");
                    od_editor_discard(&f->evw.ed);
                    f->evw.active = 0;
                    /* EXIT opens the menu init (mode 3, A+1), which draws the follow-up menu when
                     * any progress byte is set and the start-point menu otherwise, like B on the map
                     * (reference: no progress, A f2250 -> start-point menu; after a password ->
                     * follow-up menu). The C3.7 row's "EXIT -> follow-up menu" holds only with
                     * progress. */
                    flow_set_map_words(f);
                    int progress = 0;
                    for (int w = 0; w < OD_WORLD_COUNT; ++w)
                        progress |= f->screens.map_words[w] != 0;
                    /* Without progress the menu clip (the title route's) enters two ticks in
                     * (od_screens_menu_init; reference fade 1..15 at f2257..f2271; offsets 0..6
                     * measured, 2 is the minimum). */
                    od_screens_menu_init(&f->screens, progress);
                }
            }
        } else if (f->screens.scene == OD_SCREEN_PLAY && !f->playing) {
            /* Native load fade: the round's own palette through the imported brightness
             * table, never the captured `.fade` table. The enable marker is the screens' own
             * buffer address (the table path ignores its bytes); without the table there is
             * no ramp. */
            const uint8_t *fade = NULL;
            unsigned fade_frames = 0;
            if (od_palette_fade_lut() != NULL) {
                fade = f->screens.level_fade[0];
                fade_frames = OD_FLOW_ENTRY_FADE_FRAMES;
            }
            /* A5: the level comes from the validated map focus (0..99), never
             * from an unchecked world*10+round. */
            int play_level = od_screens_map_level(&f->screens);
            if (play_level < 0)
                play_level = 0;
            /* The position bytes take the entered round; new game keeps them, so its map
             * opens on the round's world even when that is not the session's (map-stepped
             * 9-1 from a world-1 game: reference round byte 08 and world 9 after new game).
             * A clear still moves them on through the progress cursor. */
            f->session.world = play_level / OD_WORLD_ROUNDS;
            f->session.round = play_level % OD_WORLD_ROUNDS;
            f->level = play_level;
            if (f->screens.hidden_dispatch) {
                fade = NULL;
                fade_frames = 0;
            }
            flow_view_begin_loading(f, play_level, fade, fade_frames);
            /* The load's first frame still shows the scene it leaves, the next ones are
             * black. */
            f->view.load_from_scene = 1;
            od_screens_load_hold(&f->screens);
            /* The staged load consumes the hand-over frame the scene it leaves already
             * published, so a cinematic entry starts the load further in than a map entry.
             * A map entry's first (load_from_scene) frame is the reference's hand-over, so
             * the base stage (1) lands its grid at +2 and the hero at +5 (R1 f2221, f2222).
             * A castle (LEVEL_ENTRY with level_castle) and the boss sequence have already
             * drawn that hand-over as their own final published frame, so they skip it:
             * castle one extra stage (R2 f16628, R3c f19287, R6 f15968) and the boss two,
             * because its phase-3 frame is already a published hand-over (R3d/R4 f21358).
             * C6.17's per-block hold makes the castle/boss transfer spend the frames the
             * transfer loop really spends, so the published hand-over moves one tick later
             * and the load advances one more stage to keep the reference tick. No mask,
             * tolerance, wait, retention or realignment (AGENTS.md section L3). */
            if (f->screens.hold_scene == OD_SCREEN_BOSS)
                f->view.load_stage = 3;
            else if (f->screens.hold_scene == OD_SCREEN_LEVEL_ENTRY && f->screens.level_castle)
                f->view.load_stage = 2;
            if (f->screens.hidden_dispatch) {
                /* The hidden menu's START frame was the dispatch itself and
                 * still showed the menu (reference f960): the load runs black
                 * from the next frame (f961; the partial uploads of f962 and
                 * f964 are not modelled) and materialises on f965, two
                 * stages earlier than a map entry, with no fade. */
                f->view.load_stage = 3;
                f->screens.hidden_dispatch = 0;
            }
            f->playing = 1;
            f->from_editor = 0;
            f->screen = OD_FLOW_PLAY;
        }
    } else if (f->screen == OD_FLOW_PLAY) {
        int loading = f->view.load_stage != 0;
        if (loading)
            f->view.previous_buttons = buttons; /* the press edge is kept */
        if (flow_view_loading_tick(f, &f->assets->levels.levels[f->level],
                                   f->opt.no_entry)) {
            /* Materialisation frame: presented without a gameplay step. */
        } else {
            flow_view_tick(f, buttons);
            if (f->audio_on) {
                audio_accept(&f->audio, &f->game);
                audio_play_events(&f->audio, &f->view);
            }
            if (f->view.combat_active && f->view.combat.victory) {
                f->view.combat_active = 0;
                f->view.combat.victory = 0;
                /* Real victory enters the ending sequence. Execute ONE native constructor
                 * step, drain its own init stop; no direct stop or two clip ticks to retime
                 * captured art. */
                od_screens_ending(&f->screens);
                od_screens_tick(&f->screens, 0);
                f->scene_sound_status = od_screens_boss_ending_status(&f->screens);
                if (f->scene_sound_status != OD_OK) return;
                f->scene_sound_status = flow_take_scene_sounds(f, &bootstrap_sounds);
                if (f->scene_sound_status != OD_OK) return;
                f->screen = OD_FLOW_SEQUENCE;
                f->playing = 0;
            } else if (f->view.new_game) {
                if (f->from_editor) {
                    editor_return(f);
                    if (f->scene_sound_status != OD_OK) return;
                    f->screen = OD_FLOW_SEQUENCE;
                    f->playing = 0;
                } else if (f->opt.custom_level) {
                    /* C6.31 (CORE-3): NEW GAME leaves a custom level (back to
                     * the editor), like the original editor's user level. */
                    flow_custom_finish(f, OD_FLOW_CUSTOM_NEW_GAME);
                } else {
                    /* New game returns to the world map with the session's own progress, exactly
                     * like the reference: the failed-1-2 route keeps 1-1 cleared and leaves the
                     * cursor on round 2. It is not a session reset. */
                    if (f->view.combat_active)
                        flow_new_game_from_arena(f);
                    else if (f->level >= OD_HIDDEN_FIRST) {
                        /* A hidden round goes back to the hidden-round menu on the round just played
                         * (reference 12-10: menu f1516, position byte kept). */
                        od_screens_hidden_return(&f->screens, f->level);
                        f->screen = OD_FLOW_SEQUENCE;
                    } else
                        flow_enter_map_after_round(f);
                    f->playing = 0;
                }
            } else if (f->view.combat_active && f->view.combat.defeated) {
                /* Boss is down and Hero celebrates in the arena; the
                 * combat publishes `victory` after the exit tail. ROUND 10-10
                 * was already completed and saved when it was solved. */
            } else if (f->game.cleared) {
                if (f->clear_timer < 0) {
                    /* A user level is not a round: the original leaves the progress words
                     * untouched (reference, solved editor level f3769..f4400). */
                    f->world_clear_world = -1;
                    f->castle_appears_world = -1;
                    if (!f->from_editor && !f->opt.custom_level) {
                        /* World end: the clear's progress walk decides on the progress before the
                         * round is marked.
                         * Castle appearance: the same walk's branch (a world's rounds 1..8 filled with 9
                         * or 10 done and bit 6 clear) plays the sequence for that world. */
                        int cw = -1, copen = 0;
                        od_clear_cinematic cc = od_progress_clear_cinematic(
                            &f->session.progress, f->level, &cw, &copen);
                        if (cc == OD_CLEAR_WORLD_END)
                            f->world_clear_world = cw;
                        else if (cc == OD_CLEAR_CASTLE_APPEARS &&
                                 od_flow_castle_appears_available(&f->screens, cw))
                            f->castle_appears_world = cw;
                        od_progress_complete(&f->session.progress, f->level);
                        if (f->level == OD_FINAL_LEVEL) {
                            /* ROUND 10-10 has no next round: the original keeps the position on world 10
                             * round 10 through the combat and into the ending (R4). */
                            f->session.world = f->level / OD_WORLD_ROUNDS;
                            f->session.round = f->level % OD_WORLD_ROUNDS;
                        } else {
                            f->session.world = f->session.progress.world;
                            f->session.round = f->session.progress.cursor;
                        }
                        f->session.level = f->level;
                        flow_save(f);
                    }
                    f->clear_timer = 0;
                } else if (f->from_editor && f->game.level_exit) {
                    /* The solved user level runs the rounds' exit tail and fade; where a round
                     * opens the map the original enters the editor's own init (celebration end
                     * f3897 -> mode change f3914, like 1-1's f3974 -> map f3991), not its menu
                     * entry. */
                    editor_return(f);
                    if (f->scene_sound_status != OD_OK) return;
                    f->clear_timer = 1;
                    f->screen = OD_FLOW_SEQUENCE;
                    f->playing = 0;
                } else if (f->from_editor) {
                    /* the user level's exit tail */
                } else if (f->opt.custom_level) {
                    /* C6.31 (CORE-3): a custom level runs the rounds' exit
                     * tail and fade and finishes where a round would open the
                     * map, like the original editor's user level returning to
                     * its editor (above). No progress, no save, no map. */
                    if (f->game.level_exit)
                        flow_custom_finish(f, OD_FLOW_CUSTOM_SOLVED);
                } else if (f->game.level_exit) {
                    if (f->level != OD_FINAL_LEVEL && f->world_clear_world >= 0) {
                        /* World end: where the exit would open the map the original raises the
                         * world-end mode (1-10: map hand-over frame f17632) and plays the world's end,
                         * then the follow-up menu (mode 3, f18709). Its first frame is drawn on this
                         * one, like the map's. */
                        od_screens_world_clear(&f->screens, f->world_clear_world);
                        f->world_clear_world = -1;
                        f->screen = OD_FLOW_SEQUENCE;
                        f->playing = 0;
                        f->oam_rotation_known = 0;
                        od_screens_tick(&f->screens, 0);
                        /* The first native step executes on this PLAY exit.
                         * Drain before audio/reset, including audio-off. */
                        f->scene_sound_status = od_screens_world_clear_status(&f->screens);
                        if (f->scene_sound_status != OD_OK) return;
                        f->scene_sound_status = flow_take_scene_sounds(f, &scene_sounds);
                        if (f->scene_sound_status != OD_OK) return;
                    } else if (f->level != OD_FINAL_LEVEL && f->castle_appears_world >= 0) {
                        /* Castle appearance: the castle appearance takes the map's hand-over frame too
                         * (1-1 cleared last: world-end mode f17991..f18305, last frame f18306); the map
                         * opens on the frame after (f18307), below. */
                        od_screens_castle_appears(&f->screens, f->castle_appears_world);
                        f->castle_appears_world = -1;
                        f->screen = OD_FLOW_SEQUENCE;
                        f->playing = 0;
                        f->oam_rotation_known = 0;
                        od_screens_tick(&f->screens, 0);
                        /* The first native step executes on this PLAY exit. Drain before audio/reset,
                         * including audio-off, like the world-clear branch above: the clip tick already
                         * queued its first-frame cue, and the next step's own od_screens_tick would
                         * clear the source state before the normal handoff drain. */
                        f->scene_sound_status = od_screens_world_clear_status(&f->screens);
                        if (f->scene_sound_status != OD_OK) return;
                        f->scene_sound_status = flow_take_scene_sounds(f, &scene_sounds);
                        if (f->scene_sound_status != OD_OK) return;
                    } else if (f->level >= OD_HIDDEN_FIRST) {
                        /* C6.31 (CORE-12): a solved hidden round goes back to the hidden-round menu on
                         * the next round where a round's exit opens the map (reference 11-1: black
                         * f4442/f4443, menu f4444, "ROUND 11-2" f4445). Not the map. */
                        od_screens_hidden_solved(&f->screens, f->level);
                        f->screen = OD_FLOW_SEQUENCE;
                        f->playing = 0;
                        f->oam_rotation_known = 0;
                    } else if (f->level != OD_FINAL_LEVEL) {
                        flow_enter_map_after_round(f);
                        f->playing = 0;
                    } else if (!f->view.combat_pending) {
                        f->view.combat_pending = 1;
                        f->view.combat_arm = 0;
                    } else if (++f->view.combat_arm == 1) {
                        /* The arena entry zeroes the HUD clock on its first frame (reference f25745,
                         * arena mode from f25744). The clock is gated until the play loop reloads the
                         * countdown below. */
                        memset(&f->view.clock, 0, sizeof f->view.clock);
                    } else if (f->view.combat_arm == OD_COMBAT_ARM_FRAMES_WRITE) {
                        f->game.hero_x = 7;
                        f->game.hero_y = 1;
                        f->game.fires = 1;
                        od_present_game_visual(&f->view.queued, &f->game,
                                                &f->view.visual,
                                                &f->assets->palette_ram);
                        f->view.visible = f->view.queued;
                    } else if (f->view.combat_arm >= OD_COMBAT_ARM_FRAMES_INIT) {
                        od_combat_init(&f->view.combat, &f->game);
                        /* The play loop reloads the countdown (reference f25749) over the clock the
                         * arena entry zeroed: the arena counts from 00:00:00, not from the solved
                         * round's time. */
                        f->view.clock.countdown = 120;
                        f->view.hud.visible = f->view.hud.queued = f->view.clock;
                        /* Combat resets game.frame: the old visual clock would
                         * reject every subsequent tick (including ice falls). */
                        od_visual_init(&f->view.visual, &f->game);
                        f->view.combat_active = 1;
                        od_present_game_combat(&f->view.queued, &f->game,
                                                &f->view.visual,
                                                &f->assets->palette_ram,
                                                &f->view.combat.boss,
                                                &f->view.combat.attack);
                        f->view.visible = f->view.queued;
                        /* The arena enters the same play loop start: empty bar, then the template
                         * (reference f25749/f25750). */
                        flow_view_bar_load(f);
                    }
                }
            }
        }
    }

    /* Music and the solution fade follow the same scene state (H6). */
    int audio_play = f->screen == OD_FLOW_PLAY && f->view.load_stage == 0;
    if (f->audio_on) {
        if (f->audio.request)
            for (unsigned i = 0; i < editor_actions.count; ++i)
                f->audio.request(f->audio.ctx, editor_actions.ids[i]);
        audio_scene(&f->audio, audio_play ? OD_FLOW_PLAY : OD_FLOW_SEQUENCE,
                    audio_play ? OD_SCREEN_PLAY : f->screens.scene,
                    f->screens.age, f->screens.pending, (int)f->screens.next,
                    f->level, audio_play ? f->game.cleared : 0,
                    audio_play ? f->game.entry_cue : 0, f->screens.menu_lead,
                    &bootstrap_sounds);
        audio_scene_transitions(&f->audio, f);
        audio_world_clear(&f->audio, &scene_sounds);
        audio_magicword(&f->audio, f);
        audio_menu(&f->audio, f, &menu_sounds);
    }

    /* The level-exit fade is drawn into the frame itself (flow_exit_fade): the
     * ARGB blackout this field used to apply is gone. */
    f->fade_black = 0;

    od_flow_draw(f, &f->fb);
    f->sim_tick++;
}

/* Convert the last composed frame to ARGB, applying the exit fade. */
static void flow_frame_argb(const od_flow *f, uint32_t *argb)
{
    od_frame_to_argb(&f->fb, &od_master_palette_pal, argb);
    if (f->fade_black) {
        unsigned keep = 256u - f->fade_black;
        for (int i = 0; i < OD_SCREEN_W * OD_SCREEN_H; i++) {
            uint32_t c = argb[i];
            unsigned r = ((c >> 16) & 0xffu) * keep / 256u;
            unsigned g = ((c >> 8) & 0xffu) * keep / 256u;
            unsigned b = (c & 0xffu) * keep / 256u;
            argb[i] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* ---- init / free ------------------------------------------------------- */

/* C6.31: every simulation field back to what od_flow_init leaves (power-on
 * with cleared RAM), with the session loaded from the save file when `load`.
 * Only the loaded resources (inside the screens), the audio driver and engine
 * and the host-side tick/hash counters are kept. Before C6.31 the RESET kept
 * the password entry (`pw`), the game's editor (`evw`), a staged editor PLAY
 * load (`editor_load`, which left the intro black forever), the password error
 * message and the pause view (CORE-5, CORE-6). */
static void flow_power_on(od_flow *f, int load)
{
    f->scene_sound_status = OD_OK;
    od_session_reset(&f->session);
    if (load && od_session_load(&f->session, f->opt.save_path))
        f->session.level = -1;
    od_screens_power_on(&f->screens);
    flow_publish_boot_progress(f);
    memset(&f->game, 0, sizeof f->game);
    f->game.hero_active = 1;
    f->game.facing = 1;
    f->screen = OD_FLOW_SEQUENCE;
    f->level = -1;
    f->playing = 0;
    f->from_editor = 0;
    f->editor_load = 0;
    memset(f->editor_palette, 0, sizeof f->editor_palette);
    f->clear_timer = -1;
    f->world_clear_world = -1;
    f->castle_appears_world = 0;      /* as od_flow_init's memset */
    memset(&f->view, 0, sizeof f->view);
    memset(&f->pw, 0, sizeof f->pw);
    memset(&f->evw, 0, sizeof f->evw);
    f->oam_rotation = 0;
    f->oam_rotation_known = 0;
    f->frame_tick_boot = OD_FRAME_TICK_BOOT_FRAMES;
    f->fade_black = 0;
    f->last_buttons = 0;
}

/* Console RESET button (F2 in the SDL host): restart the opening scene with
 * the save file intact and the session reloaded from it, like a power-on (the
 * RESET path itself is not measured on the reference). The loaded tracks, the
 * audio device and the assets are reused. */
static void flow_soft_reset(od_flow *f)
{
    od_flow_timeline_break(f, "console RESET");
    flow_power_on(f, f->opt.load_save);
}

void od_flow_timeline_break(od_flow *f, const char *why)
{
    if (f == NULL || f->record_fp == NULL)
        return;
    fprintf(f->record_fp, "# recording stopped at tick %ld: %s (not expressible "
                          "in the replay format)\n", f->sim_tick, why ? why : "?");
    fclose(f->record_fp);
    f->record_fp = NULL;
    fprintf(stderr, "record: stopped at tick %ld (%s); %s keeps the timeline "
                    "before it\n", f->sim_tick, why ? why : "?", f->opt.record);
}

/* ---- resume point (C6.62) ----------------------------------------------- */

static void resume_fill_session(od_flow_resume *r, const od_flow *f)
{
    memcpy(r->cleared, f->session.progress.cleared, sizeof r->cleared);
    r->progress_world = f->session.progress.world;
    r->cursor = f->session.progress.cursor;
    r->session_level = f->session.level;
    r->world = f->session.world;
    r->round = f->session.round;
}

void od_flow_resume_describe(const od_flow *f, od_flow_resume *out)
{
    memset(out, 0, sizeof *out);
    out->tick = f->sim_tick;
    const od_level_log *l = &f->lvl_log;
    if (f->opt.custom_level || f->screen == OD_FLOW_UNVERIFIED)
        return;
    if (f->screen == OD_FLOW_PLAY) {
        if (l->armed && f->playing && l->count > 0 &&
            (long)l->count == f->sim_tick - l->r.start_tick) {
            *out = l->r;
            out->kind = OD_RESUME_LEVEL;
            out->tick = f->sim_tick;
            out->buttons = l->buf;
            out->count = l->count;
        } else if (!l->armed && !f->from_editor && f->view.load_stage != 0) {
            /* The round is still loading: nothing of it has been played. */
            out->kind = OD_RESUME_MAP;
            resume_fill_session(out, f);
        }
        return;
    }
    out->kind = OD_RESUME_MAP;
    resume_fill_session(out, f);
}

od_status od_flow_resume_apply(od_flow *f, const od_flow_resume *r)
{
    if (f == NULL || r == NULL || f->assets == NULL || f->opt.custom_level ||
        f->scene_sound_status != OD_OK)
        return OD_ERR_RANGE;
    if (r->kind != OD_RESUME_MAP && r->kind != OD_RESUME_LEVEL)
        return OD_ERR_RANGE;
    if (r->kind == OD_RESUME_LEVEL &&
        (r->level < 0 || r->level >= f->assets->levels.count ||
         r->buttons == NULL || r->count == 0 || r->count > OD_RESUME_MAX_TICKS ||
         r->start_tick < 0 || r->tick != r->start_tick + (long)r->count))
        return OD_ERR_RANGE;

    /* Back to a clean session, the way a console reset leaves it. */
    od_session_reset(&f->session);
    memcpy(f->session.progress.cleared, r->cleared, sizeof r->cleared);
    f->session.progress.world = r->progress_world;
    f->session.progress.cursor = r->cursor;
    f->session.level = r->session_level;
    f->session.world = r->world;
    f->session.round = r->round;
    f->scene_sound_status = OD_OK;
    od_screens_power_on(&f->screens);
    flow_publish_boot_progress(f);
    memset(&f->game, 0, sizeof f->game);
    f->game.hero_active = 1;
    f->game.facing = 1;
    f->screen = OD_FLOW_SEQUENCE;
    f->level = -1;
    f->playing = 0;
    f->from_editor = 0;
    f->editor_load = 0;
    memset(f->editor_palette, 0, sizeof f->editor_palette);
    f->clear_timer = -1;
    f->world_clear_world = -1;
    f->castle_appears_world = 0;
    memset(&f->view, 0, sizeof f->view);
    memset(&f->pw, 0, sizeof f->pw);
    memset(&f->evw, 0, sizeof f->evw);
    f->oam_rotation = 0;
    f->oam_rotation_known = 0;
    f->frame_tick_boot = 0;
    f->fade_black = 0;
    f->last_buttons = 0;
    f->lvl_log.armed = 0;
    f->lvl_log.count = 0;
    f->custom_result = 0;

    if (r->kind == OD_RESUME_MAP) {
        flow_set_map_words(f);
        od_screens_map_progress_world(&f->screens, f->screens.map_words,
                                      f->session.progress.world, 0);
        f->sim_tick = r->tick;
        return OD_OK;
    }

    /* The level: the same start as --play-level, with the saved session. */
    f->level = r->level;
    f->view.pause.menu.repeat = r->menu_repeat;
    memcpy(f->view.pause.menu.counters, r->menu_counters, sizeof r->menu_counters);
    f->oam_rotation = r->oam_rotation;
    f->oam_rotation_known = r->oam_known;
    f->screen = OD_FLOW_PLAY;
    flow_view_begin_loading(f, r->level, NULL, 0);
    f->view.load_stage = OD_PLAY_LOAD_FULL;
    f->playing = 1;
    f->sim_tick = r->start_tick;

    /* The inputs run silently: the sound follows from the final state. */
    const int audio_on = f->audio_on;
    f->audio_on = 0;
    for (size_t i = 0; i < r->count && f->scene_sound_status == OD_OK; ++i)
        od_flow_step(f, r->buttons[i]);
    f->audio_on = audio_on;
    if (f->scene_sound_status != OD_OK || f->sim_tick != r->tick)
        return OD_ERR_FORMAT;
    return OD_OK;
}

void od_flow_resume_adopt(od_flow *f, const od_flow_resume *r)
{
    od_level_log *l = &f->lvl_log;
    l->armed = 0;
    l->count = 0;
    if (r == NULL || r->kind != OD_RESUME_LEVEL || r->buttons == NULL ||
        r->count == 0 || r->count > OD_RESUME_MAX_TICKS ||
        f->screen != OD_FLOW_PLAY || f->opt.custom_level)
        return;
    if (l->cap < r->count) {
        uint8_t *grown = realloc(l->buf, r->count);
        if (grown == NULL)
            return;
        l->buf = grown;
        l->cap = r->count;
    }
    memcpy(l->buf, r->buttons, r->count);
    l->count = r->count;
    l->r = *r;
    l->r.buttons = NULL;
    l->armed = 1;
}

od_status od_flow_new_game(od_flow *f, char *backup, size_t cap)
{
    if (backup && cap)
        backup[0] = '\0';
    if (f == NULL || f->opt.custom_level)
        return OD_ERR_RANGE;
    const char *path = f->opt.save_path;
    if (path) {
        od_progress probe;
        if (od_save_read(path, &probe) != OD_ERR_IO) {
            /* A file is there (valid or not): keep it, then clear it, so the
             * monotonic save write (CORE-1) cannot bring the rounds back. */
            char bak[1200];
            od_status st = od_save_backup(path, bak, sizeof bak);
            if (st != OD_OK) {
                fprintf(stderr, "new game: cannot back up %s; nothing changed\n", path);
                return st;
            }
            if (od_save_delete(path) != OD_OK) {
                fprintf(stderr, "new game: cannot clear %s; nothing changed\n", path);
                return OD_ERR_IO;
            }
            printf("new game: %s kept as %s and cleared\n", path, bak);
            if (backup && cap)
                snprintf(backup, cap, "%s", bak);
        }
    }
    od_flow_timeline_break(f, "new game");
    flow_power_on(f, 0);
    printf("new game: power-on with no progress (tick %ld)\n", f->sim_tick);
    return OD_OK;
}

static od_status flow_start_scene(od_flow *f)
{
    const od_flow_opts *o = &f->opt;
    if (o->ending) {
        od_screens_ending(&f->screens);
        f->screen = OD_FLOW_SEQUENCE;
        f->playing = 0;
    } else if (o->level_seen && o->play_level) {
        /* --play-level (the editor API, section L2): the first tick is the round's
         * materialisation frame, presented without a gameplay step, exactly like the
         * real flow's load (R1: 1-1 materialises at reference f2220 and the hero's first
         * entry pose follows 21 ticks later). */
        f->screen = OD_FLOW_PLAY;
        f->level = o->level;
        flow_view_begin_loading(f, o->level, NULL, 0);
        f->view.load_stage = OD_PLAY_LOAD_FULL;
        f->playing = 1;
    } else if (o->level_seen) {
        f->screen = OD_FLOW_PLAY;
        f->level = o->level;
        od_game_init_index(&f->game, &f->assets->levels.levels[o->level], o->level);
        if (o->no_entry)
            od_game_entry_finish(&f->game);
        flow_view_init(f, NULL, 0);
        /* The initial state is the materialisation frame. */
        flow_view_materialised(f);
        f->playing = 1;
    } else if (o->map || o->continuing) {
        /* C3.2/C3.3: `--map` opens the logic map. With `--world N` and/or
         * `--cleared N` it shows a synthetic progress image (per-world capture);
         * otherwise it shows the session's own progress image, so `--continue` shows the
         * saved rounds. */
        if (o->world > 0 || o->cleared > 0) {
            uint16_t words[OD_WORLD_COUNT];
            od_progress p;
            od_progress_reset(&p);
            int n = o->cleared;
            if (n > OD_LEVEL_COUNT)
                n = OD_LEVEL_COUNT;
            for (int i = 0; i < n; ++i)
                od_progress_set_cleared(&p, i, 1);
            od_progress_map_words(&p, words);
            /* main.c stores --world 1..10 as 0-based (0..9). */
            int world = o->world > 0 ? o->world : p.world;
            od_screens_map_progress_world(&f->screens, words, world, 0);
        } else {
            flow_set_map_words(f);
            od_screens_map_progress(&f->screens, f->screens.map_words, 0);
        }
    } else if (o->boss) {
        /* C3.5 discovery tool (see od_flow_opts.boss): start on the solved
         * ROUND 10-10 state so the flow runs its OWN verified hand-off (arm
         * sequence -> od_combat_init), exactly as when 10-10 is solved. The
         * combat branch below is nested under `game.cleared`, so the tool must
         * publish the same latches the solve does: cleared + level_exit with
         * the clear timer already drained. `od_combat_init` then loads the
         * arena and discards this puzzle state. */
        f->screen = OD_FLOW_PLAY;
        f->level = OD_FINAL_LEVEL;
        flow_view_init(f, NULL, 0);
        od_game_init_index(&f->game, &f->assets->levels.levels[OD_FINAL_LEVEL],
                            OD_FINAL_LEVEL);
        od_game_entry_finish(&f->game);
        f->game.fires = 0;
        f->game.cleared = 1;
        f->game.exit_open = 1;
        f->game.level_exit = 1;
        /* The solve reaches level_exit at the end of its exit tail, so the hand-off
         * frames are blacked out exactly as on the real route (bug5 f49633-f49639,
         * reference f25744-f25748). */
        f->game.exit_frames = OD_LEVEL_EXIT_FRAMES;
        f->clear_timer = 0;
        f->playing = 1;
    } else if (o->menu) {
        od_screens_menu(&f->screens);
    }
    return OD_OK;
}

od_status od_flow_init(od_flow *f, const od_assets *assets,
                         const od_flow_opts *opt, const od_flow_host *host)
{
    memset(f, 0, sizeof *f);
    f->assets = assets;
    f->opt = *opt;
    if (host)
        f->host = *host;
    f->host_ok = host != NULL;
    f->level = -1;
    f->clear_timer = -1;
    f->world_clear_world = -1;
    f->hud = opt->hud || !opt->level_seen;
    f->stop_at = opt->ticks > 0 ? opt->ticks
                                : (opt->frames > 0 ? opt->frames : 0);

    od_session_reset(&f->session);
    /* C6.31 (CORE-3): a custom level run never reads the vanilla progress. */
    if (opt->load_save && !opt->custom_level &&
        od_session_load(&f->session, opt->save_path)) {
        printf("save: loaded %s (%d rounds cleared)\n", opt->save_path,
               od_progress_clear_count(&f->session.progress));
        f->session.level = -1;
    }

    od_status st = od_screens_init(&f->screens, opt->assets_dir);
    if (st != OD_OK)
        return st;
    flow_publish_boot_progress(f);
    if (opt->level_seen && (opt->level < 0 || opt->level >= assets->levels.count))
        return OD_ERR_RANGE;

    memset(&f->game, 0, sizeof f->game);
    f->game.hero_active = 1;
    f->game.facing = 1;
    f->frame_tick_boot = OD_FRAME_TICK_BOOT_FRAMES;
    f->screens.frame_tick_by_flow = 1;

    return flow_start_scene(f);
}

void od_flow_free(od_flow *f)
{
    if (!f)
        return;
    if (f->audio_device && f->host.audio_close)
        f->host.audio_close(f->host.ctx);
    if (f->audio_core_on)
        od_audio_shutdown(&f->audio_core);
    if (f->hash_fp) fclose(f->hash_fp);
    if (f->state_fp) fclose(f->state_fp);
    if (f->record_fp) fclose(f->record_fp);
    if (f->argb_fp) fclose(f->argb_fp);
    free(f->pcm_out);
    free(f->lvl_log.buf);
    od_replay_free(&f->replay);
    od_screens_free(&f->screens);
    memset(f, 0, sizeof *f);
}

/* C5.3: re-link the transient pointers that point into the loaded resources.
 * The state block is a by-value copy, so a `.odstate` stores these pointers
 * zeroed (or as a small index) and this rebuilds them. */
void od_flow_state_rebind(od_flow *f)
{
    if (f == NULL)
        return;
    /* The native level-entry ramp is enabled by `fade_frames > 0`; the marker
     * is the screens' own 16x32 buffer address (its bytes are unused: the LUT
     * path in flow_view_draw computes the palette from the round's own). */
    f->view.fade = f->view.fade_frames > 0 ? f->screens.level_fade[0] : NULL;
    /* The running "Today's magicword" VRAM program lives in the package art.
     * The state stores its kind (0 none, 1 the screen program, 2 the
     * question) in the pointer slot for the state module to decode; here we
     * translate that integer back into the art pointer. */
    if (f->screens.mw.prog != NULL) {
        uintptr_t kind = (uintptr_t)f->screens.mw.prog;
        const od_magicword_art *art = od_gfx_magicword_art();
        if (art == NULL || kind > 2)
            f->screens.mw.prog = NULL;
        else
            f->screens.mw.prog = kind == 2 ? art->question : art->screen;
    }
}

/* ---- audio setup ------------------------------------------------------- */

static void flow_audio_setup(od_flow *f)
{
    const od_flow_opts *o = &f->opt;
    const int harness = o->hash_out != NULL || o->state_out != NULL;
    int want_core = (harness && !o->hash_no_audio) || o->audio_out != NULL;
    int device_ok = 0;

    if (!o->no_audio && !harness && !o->headless && !o->audio_out && o->ticks <= 0
        && !f->replay_mode && f->host.audio_open) {
        if (f->host.audio_open(f->host.ctx)) {
            device_ok = 1;
            f->audio_device = 1;
            f->audio.tick = f->host.audio_tick;
            f->audio.request = f->host.audio_request;
            f->audio.play_music = f->host.audio_play_music;
            f->audio.reset_init = f->host.audio_reset;
            f->audio.ctx = f->host.ctx;
        } else {
            fprintf(stderr, "audio: no device, continuing without sound\n");
        }
    }
    if (want_core) {
        if (od_audio_init(&f->audio_core, o->assets_dir) == OD_OK) {
            f->audio_core_on = 1;
            f->audio.tick = audio_core_tick_cb;
            f->audio.request = audio_core_request_cb;
            f->audio.play_music = audio_core_music_cb;
            f->audio.reset_init = audio_core_reset_cb;
            f->audio.ctx = &f->audio_core;
        } else {
            fprintf(stderr, "audio: cannot load the extracted sequence\n");
        }
        if (o->audio_out)
            g_audio_trace = 1;
    }
    f->audio_on = device_ok || f->audio_core_on;
}

/* ---- outputs ----------------------------------------------------------- */

static int flow_open_hash(od_flow *f)
{
    const od_flow_opts *o = &f->opt;
    if (!o->hash_out)
        return 0;
    f->hash_fp = fopen(o->hash_out, "wb");
    if (!f->hash_fp) {
        fprintf(stderr, "cannot write %s\n", o->hash_out);
        return 1;
    }
    f->rgb_hash = f->pcm_hash = 0xcbf29ce484222325ull;
    fprintf(f->hash_fp, "# od H9 per-tick hash trace (EU/PAL %llu/%llu s/tick, "
                        "%.6f Hz)\n",
            (unsigned long long)OD_CLOCK_PERIOD_NUM,
            (unsigned long long)OD_CLOCK_PERIOD_DEN, OD_CLOCK_HZ);
    fprintf(f->hash_fp, "# freq=%llu max_sim_hz=%u vsync=%d audio=%d det=%d ticks=%d\n",
            (unsigned long long)(f->host_ok ? f->host.frequency(f->host.ctx) : 0),
            o->max_sim_hz, o->vsync, f->audio_on, o->ticks > 0, o->ticks);
    fprintf(f->hash_fp, "# tick semantic args rgb pcm\n");
    return 0;
}

/* C3.5: the recorded button script (a human play-through) in the same format
 * the replay parser and the reference understand: `FRAME BUTTON[+BUTTON...]`. */
static void flow_record_write(FILE *fp, long tick, unsigned b)
{
    fprintf(fp, "%ld ", tick);
    if (!b) {
        fputs("-\n", fp);
        return;
    }
    int first = 1;
    static const struct { unsigned bit; const char *name; } names[] = {
        { OD_BTN_A, "A" }, { OD_BTN_B, "B" },
        { OD_BTN_SELECT, "SELECT" }, { OD_BTN_START, "START" },
        { OD_BTN_UP, "UP" }, { OD_BTN_DOWN, "DOWN" },
        { OD_BTN_LEFT, "LEFT" }, { OD_BTN_RIGHT, "RIGHT" },
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (b & names[i].bit) {
            fprintf(fp, "%s%s", first ? "" : "+", names[i].name);
            first = 0;
        }
    }
    fputc('\n', fp);
}

static int flow_open_record(od_flow *f)
{
    if (!f->opt.record)
        return 0;
    f->record_fp = fopen(f->opt.record, "wb");
    if (!f->record_fp) {
        fprintf(stderr, "cannot write %s\n", f->opt.record);
        return 1;
    }
    return 0;
}

static int flow_open_state(od_flow *f)
{
    if (!f->opt.state_out)
        return 0;
    f->state_fp = fopen(f->opt.state_out, "wb");
    if (!f->state_fp) {
        fprintf(stderr, "cannot write %s\n", f->opt.state_out);
        return 1;
    }
    app_state_header(f->state_fp);
    return 0;
}

static void flow_render_audio(od_flow *f)
{
    if (!f->audio_core_on)
        return;
    size_t avail = od_audio_available(&f->audio_core);
    int16_t block[4096];
    while (avail > 0) {
        int chunk = (int)(avail < sizeof block / sizeof block[0]
                              ? avail : sizeof block / sizeof block[0]);
        od_audio_render(&f->audio_core, block, chunk);
        if (f->opt.hash_out)
            f->pcm_hash = fnv1a64(f->pcm_hash, block,
                                  sizeof(int16_t) * (size_t)chunk);
        if (f->opt.audio_out) {
            if (f->pcm_out_count + (size_t)chunk > f->pcm_out_cap) {
                f->pcm_out_cap = (f->pcm_out_count + (size_t)chunk) * 2 + OD_AUDIO_RATE;
                int16_t *grown = realloc(f->pcm_out, f->pcm_out_cap * sizeof *f->pcm_out);
                if (!grown) { free(f->pcm_out); f->pcm_out = NULL;
                              f->pcm_out_count = f->pcm_out_cap = 0; }
                else f->pcm_out = grown;
            }
            if (f->pcm_out) {
                memcpy(f->pcm_out + f->pcm_out_count, block,
                       sizeof(int16_t) * (size_t)chunk);
                f->pcm_out_count += (size_t)chunk;
            }
        }
        avail -= (size_t)chunk;
    }
}

/* ---- the two core loops ------------------------------------------------ */

void od_flow_run_interactive(od_flow *f)
{
    const od_flow_opts *o = &f->opt;
    const int harness = o->hash_out != NULL || o->state_out != NULL;
    const int deterministic = o->ticks > 0;
    const int use_renderer = !harness || o->hash_render;
    int have_replay = 0;

    f->replay_mode = 0;
    f->hud = 1; /* the interactive session always shows the status bar */
    if (o->replay) {
        if (od_replay_load_file(&f->replay, o->replay) != OD_OK) {
            fprintf(stderr, "cannot load replay '%s'\n", o->replay);
            return;
        }
        have_replay = f->replay_on = 1;
    }
    if (use_renderer && f->host.renderer_init &&
        f->host.renderer_init(f->host.ctx, o->scale, o->headless, o->vsync) != OD_OK)
        return;
    flow_audio_setup(f);
    if (flow_open_hash(f) || flow_open_state(f) || flow_open_record(f))
        goto out;

    static uint32_t argb[OD_SCREEN_W * OD_SCREEN_H];
    od_clock clock;
    if (f->host_ok)
        od_clock_init(&clock, f->host.frequency(f->host.ctx),
                       f->host.counter(f->host.ctx));
    const long max_catchup = 8;
    const long stop_at = deterministic ? o->ticks
                                       : (o->frames > 0 ? o->frames : 0);
    f->stop_at = stop_at;
    f->running = 1;

    while (f->running) {
        long due;
        if (deterministic) {
            if (stop_at > 0 && f->sim_tick >= stop_at)
                break;
            due = od_clock_advance_fixed(&clock);
        } else {
            uint64_t now = f->host_ok ? f->host.counter(f->host.ctx) : 0;
            due = od_clock_advance(&clock, now, max_catchup);
        }
        if (f->host.reset_poll && f->host.reset_poll(f->host.ctx))
            flow_soft_reset(f);
        for (long i = 0; i < due; i++) {
            /* C5.3: interface service (F5/F9 harness) runs before the input is
             * read, so a load changes sim_tick (and therefore the replay
             * buttons of this tick) before the step. */
            if (f->host.service)
                f->host.service(f->host.ctx, f, f->sim_tick);
            unsigned buttons = 0;
            int quit = 0;
            if (have_replay)
                buttons = od_replay_buttons_at(&f->replay, f->sim_tick);
            else if (f->host_ok && f->host.buttons)
                f->host.buttons(f->host.ctx, f->sim_tick, &buttons, &quit);
            if (quit) {
                /* Without this the host keeps returning quit=1 and the loop
                 * only breaks: the game stops advancing but keeps the last
                 * frame (the 2026-10-01 "frozen" report, A20). */
                f->running = 0;
                break;
            }
            if (f->record_fp &&
                (!f->record_started || buttons != f->record_prev)) {
                flow_record_write(f->record_fp, f->sim_tick, buttons);
                f->record_prev = buttons;
                f->record_started = 1;
            }
            od_flow_step(f, buttons);
            flow_frame_argb(f, argb);
            flow_render_audio(f);
            if (use_renderer && f->host.present)
                f->host.present(f->host.ctx, f, argb);
            f->present++;
            /* C6.31 (CORE-3): a finished custom level returns to the editor
             * (or ends the --play-level process); a deterministic batch keeps
             * its tick count on the held frame. */
            if (f->custom_result && !deterministic) {
                f->running = 0;
                break;
            }
            if (f->state_fp &&
                (f->screen == OD_FLOW_PLAY ||
                 f->screens.scene == OD_SCREEN_ENDING))
                app_state_row(f->state_fp, f->sim_tick, f->screen, &f->screens,
                              f->level, &f->game, &f->view);
            if (f->hash_fp &&
                (f->sim_tick % o->hash_every == 0 || f->sim_tick == stop_at)) {
                f->rgb_hash = fnv1a64(f->rgb_hash, argb,
                                      sizeof(uint32_t) * OD_SCREEN_W * OD_SCREEN_H);
                fprintf(f->hash_fp, "%ld %s %08X %016llX %016llX\n",
                        f->sim_tick, screen_name(f->screen),
                        semantic_hash(&f->game, f->screen, f->level, buttons),
                        (unsigned long long)f->rgb_hash,
                        (unsigned long long)f->pcm_hash);
            }
            if (stop_at > 0 && f->sim_tick >= stop_at) {
                f->running = 0;
                break;
            }
        }
        if (f->running && !deterministic && !o->headless && due == 0
            && f->host.sleep_ms) {
            unsigned ms = f->host_ok
                        ? od_clock_sleep_ms(&clock, f->host.counter(f->host.ctx))
                        : 0;
            if (ms > 0)
                f->host.sleep_ms(f->host.ctx, ms);
        }
    }

    if (o->screenshot && f->sim_tick > 0 && f->host.screenshot)
        f->host.screenshot(f->host.ctx, f, &f->fb, f->sim_tick - 1, 0);

    if (o->audio_out) {
        if (!f->pcm_out_count)
            fprintf(stderr, "audio: nothing to write to %s\n", o->audio_out);
        else if (write_wav(o->audio_out, f->pcm_out, f->pcm_out_count, OD_AUDIO_RATE))
            fprintf(stderr, "audio: cannot write %s\n", o->audio_out);
        else
            printf("audio: wrote %s (%zu samples, %.3f s)\n", o->audio_out,
                   f->pcm_out_count, (double)f->pcm_out_count / OD_AUDIO_RATE);
    }

    if (f->hash_fp)
        fprintf(f->hash_fp, "# end ticks=%ld x(y)=%d,%d cleared=%d dropped=%ld "
                            "resync=%d present=%ld\n",
                f->sim_tick, f->game.hero_x, f->game.hero_y, f->game.cleared,
                clock.dropped, clock.resync_count, f->present);
    printf("session: cleared=%d/%d world=%d cursor=%d\n",
           od_progress_clear_count(&f->session.progress), OD_LEVEL_COUNT,
           f->session.world + 1, f->session.round + 1);
    if (o->vsync || o->headless) {
        int vs = f->host_ok && f->host.renderer_has_vsync
               ? f->host.renderer_has_vsync(f->host.ctx) : 0;
        printf("clock: ticks=%ld hz=%.6f vsync=%d dropped=%ld\n",
               clock.tick, OD_CLOCK_HZ, use_renderer ? vs : 0, clock.dropped);
    }
out:
    if (use_renderer && f->host.renderer_shutdown)
        f->host.renderer_shutdown(f->host.ctx);
}

od_flow_replay_status od_flow_run_replay(od_flow *f)
{
    if (!f) return OD_FLOW_REPLAY_FORMAT;
    if (f->scene_sound_status != OD_OK) {
        f->running = 0;
        return (od_flow_replay_status)f->scene_sound_status;
    }
    const od_flow_opts *o = &f->opt;
    f->replay_mode = 1;
    f->hud = o->hud || !o->level_seen;
    if (o->replay) {
        od_status status = od_replay_load_file(&f->replay, o->replay);
        if (status != OD_OK) {
            fprintf(stderr, "cannot load replay '%s'\n", o->replay);
            return (od_flow_replay_status)status;
        }
        f->replay_on = 1;
    }
    long frames = o->frames > 0 ? o->frames : f->replay.last_frame + 120;
    long shot_at = o->shot_frame >= 0 ? o->shot_frame : frames - 1;
    if (!o->level_seen && o->ending && o->shot_frame >= 0)
        shot_at = o->shot_frame - OD_ENDING_FIRST;
    if (shot_at < 0) shot_at = 0;
    if (shot_at > frames - 1) shot_at = frames - 1;

    flow_audio_setup(f);

    static uint32_t argb[OD_SCREEN_W * OD_SCREEN_H];
    if (o->argb_out) {
        f->argb_fp = fopen(o->argb_out, "wb");
        if (!f->argb_fp) {
            fprintf(stderr, "cannot write %s\n", o->argb_out);
            return OD_FLOW_REPLAY_IO;
        }
    }

    int shot_done = 0;
    f->running = 1;
    for (long frame = 0; frame < frames; frame++) {
        /* C5.3: service before reading the input, so a state load changes the
         * replay position of this tick. */
        if (f->host.service)
            f->host.service(f->host.ctx, f, f->sim_tick);
        if (f->scene_sound_status != OD_OK) {
            f->running = 0;
            return (od_flow_replay_status)f->scene_sound_status;
        }
        if (!f->running)
            return OD_FLOW_REPLAY_CANCELLED;
        unsigned buttons = od_replay_buttons_at(&f->replay, f->sim_tick);
        od_flow_step(f, buttons);
        if (f->scene_sound_status != OD_OK) {
            f->running = 0;
            return (od_flow_replay_status)f->scene_sound_status;
        }
        if (!f->running)
            return OD_FLOW_REPLAY_CANCELLED;
        if (f->argb_fp) {
            flow_frame_argb(f, argb);
            if (fwrite(argb, sizeof(uint32_t), OD_SCREEN_W * OD_SCREEN_H, f->argb_fp)
                != OD_SCREEN_W * OD_SCREEN_H)
                return OD_FLOW_REPLAY_IO;
        }
        if (o->screenshot && f->host.screenshot &&
            (frame == shot_at || (o->shot_every > 0 && frame % o->shot_every == 0))) {
            if (o->shot_every > 0 && frame % o->shot_every == 0) {
                long label = (!o->level_seen && o->ending)
                           ? OD_ENDING_FIRST + frame : frame;
                if (f->host.screenshot(f->host.ctx, f, &f->fb, label, 1))
                    return OD_FLOW_REPLAY_IO;
            }
            if (frame != shot_at)
                continue;
            shot_done = 1;
            if (f->host.screenshot(f->host.ctx, f, &f->fb, frame, 0))
                return OD_FLOW_REPLAY_IO;
        }
    }
    if (o->screenshot && !shot_done && o->shot_every <= 0 && f->host.screenshot)
        if (f->host.screenshot(f->host.ctx, f, &f->fb, frames - 1, 0))
            return OD_FLOW_REPLAY_IO;

    flow_render_audio(f);
    if (o->audio_out) {
        if (!f->pcm_out_count)
            fprintf(stderr, "audio: nothing to write to %s\n", o->audio_out);
        else if (write_wav(o->audio_out, f->pcm_out, f->pcm_out_count, OD_AUDIO_RATE))
            return OD_FLOW_REPLAY_IO;
        else
            printf("audio: wrote %s (%zu samples, %.3f s)\n", o->audio_out,
                   f->pcm_out_count, (double)f->pcm_out_count / OD_AUDIO_RATE);
    }

    printf("replay: %d events, %ld frames, hero=(%d,%d) fires=%d/%d cleared=%d "
           "level=%d\n",
           f->replay.count, frames, f->game.hero_x, f->game.hero_y,
           f->game.fires, f->game.start_fires, f->game.cleared, f->level);
    if (f->opt.save_forced && !o->level_seen && f->game.cleared)
        flow_save(f);   /* C6.23: the same monotonic write as every clear */
    if (!o->level_seen)
        printf("screens: tick=%ld scene=%s age=%u\n", f->screens.tick,
               od_screens_name(&f->screens), f->screens.age);
    f->running = 0;
    return OD_FLOW_REPLAY_OK;
}
