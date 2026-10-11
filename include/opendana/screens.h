/* screens.h - native scene presentation, independent of SDL and of a ROM. */
#ifndef OD_SCREENS_H
#define OD_SCREENS_H

#include "opendana/compose.h"
#include "opendana/content_gfx.h"
#include "opendana/editor.h"
#include "opendana/game.h"
#include "opendana/objects.h"
#include "opendana/scene_map.h"
#include "opendana/scene_editor.h"
#include "opendana/scene_password.h"
#include "opendana/scene_whatnext.h"
#include "opendana/scene_magicword.h"
#include "opendana/scene_hidden.h"
#include "opendana/cinematic.h"

/* Package-relative scene resource from the retained startup VFS. No filesystem
 * fallback. Rejects incomplete/malformed bytes without changing output. */
od_status od_screen_load(od_presentation *out, const char *path);

#define OD_SCENE_WORDS 2382
#define OD_SCREEN_TRACKS 45

typedef enum {
    OD_SCREEN_INTRO, OD_SCREEN_TITLE, OD_SCREEN_CONVERSATION,
    OD_SCREEN_MENU, OD_SCREEN_MENU_CLOSE, OD_SCREEN_MAP,
    OD_SCREEN_ROUND_SELECT, OD_SCREEN_LEVEL_ENTRY, OD_SCREEN_PLAY,
    OD_SCREEN_PASSWORD, OD_SCREEN_EDITOR, OD_SCREEN_BOSS,
    OD_SCREEN_ENDING, OD_SCREEN_WHATNEXT, OD_SCREEN_HIDDEN, OD_SCREEN_MAGICWORD,
    OD_SCREEN_GOODNIGHT, OD_SCREEN_HIDDEN_INTRO, OD_SCREEN_WORLD_CLEAR,
    OD_SCREEN_UNVERIFIED
} od_screen_id;

typedef struct { uint16_t tick, field, value; } od_scene_change;
typedef struct {
    unsigned duration, count;
    int loop;
    od_scene_change *changes;
} od_scene_track;

/* The native state machine owns input and time. Tracks contain reusable scene
 * layouts, palette cues and ordered actor poses, never frames of RGB/index
 * pixels. No recorded input or reference state is consumed during execution. */
typedef struct {
    od_scene_track tracks[OD_SCREEN_TRACKS];
    /* C5.7: original scripts and logical scene renderer for castle appearance.
     * The corresponding tracks are test references, never runtime input. */
    od_cinematic_data *cinematic_data;
    od_cinematic_data *bootstrap_data;
    /* C6.13 owned eager family; serialized payload MUST zero/rebind this from
     * trusted live screens before candidate validation, never follow/free it. */
    od_cinematic_data *editor_cinematic_data;
    /* C6.14: zero in serialized payload and rebind ONLY from trusted live.
     * Existing state hosts must add this owner before accepting this ABI. */
    od_cinematic_data *castle_round10_data;
    unsigned castle_round10_phase; /* 0 off,1 program,2 cleanup publish,3 complete,4 error */
    od_status castle_round10_status;
    /* C6.15 fifth live owner. Host wire MUST null/rebind this before accepting
     * this ABI; never follow/free an untrusted serialized owner. */
    od_cinematic_data *world_clear_data;
    unsigned world_clear_phase; /* 0 off,1 program,2 cleanup pending,3 published,4 error */
    od_status world_clear_status;
    /* C6.16 sixth opaque owner: host wire MUST NULL/reject nonNULL and rebind
     * solely from trusted live state. Old five-owner hosts are unsafe. */
    od_cinematic_data *goodnight_hidden_data;
    unsigned goodnight_hidden_phase; /* 0off,1program,2hidden cleanup publication,3complete/loop,4error */
    unsigned goodnight_hidden_kind; /* 0 goodnight(root31/highbit),1 hidden(root5) */
    od_status goodnight_hidden_status;
    /* C6.17 seventh trusted eager owner: wireNULL/rejectnonnull/rebind before
     * using this ABI with any state host. No old six-owner saver is safe. */
    od_cinematic_data *boss_ending_data;
    unsigned boss_ending_phase; /* 0off,1program,2cleanup pending,3published,4error */
    unsigned boss_ending_kind; /* 0 root26 boss entry,1 root27 real victory ending */
    od_status boss_ending_status;
    /* C6.17 eighth eager trusted owner. Host epoch changes: wireNULL/reject
     * nonnull/rebind before integration. Cinematic POD itself is unchanged. */
    od_cinematic_data *post_ending_data;
    unsigned post_ending_phase; /*0off,1program,3terminal82F2 forever,4error;2INVALID*/
    unsigned post_ending_kind; /*0 root07 ONLY; not root87/27*/
    od_status post_ending_status;
    od_cinematic cinematic;
    od_castle_info cinematic_info[10];
    uint16_t display[OD_SCENE_WORDS];
    /* Causal password setup. The pending upload is published by the next
     * simulation presentation event; no captured track age supplies readiness. */
    uint16_t password_next[OD_SCENE_WORDS];
    unsigned password_phase, password_record, password_brightness;
    od_cinematic_sounds password_sounds;
    uint16_t editor_next[OD_SCENE_WORDS], editor_board[12][14];
    unsigned editor_phase, editor_record, editor_row, editor_brightness, editor_start;
    od_status editor_status;
    od_cinematic_sounds editor_sounds;
    od_screen_id scene, next;
    unsigned track, local_tick, track_tick, event, age, previous_buttons;
    unsigned selection, visible_selection, title_exit;
    unsigned world, round;
    long tick;
    /* The 16-bit frame counter the display pass increments on every frame it
     * reaches. The flow advances it (flow_frame_tick in flow.c); the map steps
     * Hero's pose and the frame colour on the frames where `frame_tick & 7 == 0`. */
    uint16_t frame_tick;
    /* Nonzero when a flow drives `frame_tick` (od_flow_init); otherwise
     * od_screens_tick counts it as a plain frame counter. */
    unsigned frame_tick_by_flow;
    /* The colour-cycle index (0..2), shared by the title's input loop and the
     * map's frame colour and never reset by a scene. */
    uint8_t frame_cycle;
    /* PISTAS-A: palette entry 15 of the title as the title loop last painted it
     * from the colour table (valid once painted in the current title scene). */
    uint16_t title_colour;
    int title_colour_valid;
    /* One byte shared by two routines and never reset by a scene: the editor
     * colour cycle (reads it raw as the sprite-palette index and writes
     * (x+1)&3) and the cursor colour of "Today's magicword" (steps it 0..4). */
    uint8_t cycle_6f;
    /* MAGICWORD: "Today's magicword" (the `quit` of "What next?") and the
     * good-night cinematic its `No` leads to. */
    od_scene_magicword mw;
    int pending, status;
    /* ROUND entry palette fade-in: up to 16 frames of 32 PPU palette entries,
     * loaded from content/base/screens/tracks/level-entry.fade through the package VFS (H4, C4.2). 0 = no fade. */
    unsigned level_fade_frames;
    uint8_t level_fade[16][32];
    /* WORLD map navigation (D1/H4.6). The map is the original's focus state:
     * `map_focus == 0` is WORLD focus (directions change the world through the
     * advance/retreat tables, `A` moves to the round) and `map_focus == 1` is
     * ROUND focus (LEFT/RIGHT move the round, `A` enters the selected round, `B`
     * returns to the world). `map_words[]` is the password's own progress image
     * (10 worlds, low ten bits = cleared rounds) so the first playable round of a
     * world is the original's computation; it is all zero for a new game.
     * `map_ready` is the map-intro length measured on the PAL route: the password
     * route's map ignores input during its opening (reference f11790..f12330),
     * while the menu route is interactive almost at once; returning from a round
     * sets it to 0. */
    /* The map's opening: input is read from this age on and the split-window
     * reveal ends here (reference: tables cleared at +45). */
#define OD_MAP_REVEAL_FRAMES 45u
#define OD_MAP_REVEAL_PANEL_AGE 47u /* sprites, marks and cursor (f4038) */
    unsigned map_focus;
    unsigned map_ready;
    /* Nonzero when the map was opened by a real scene change: the opening is the
     * split-window reveal, keyed on `map.age`. Direct openings (the dev `--map`
     * shot, the API and its tests) draw the steady map. */
    unsigned map_reveal;
    /* The map was opened with the "return" bit (a solved round, NEW GAME): its loop
     * init (the map's first sprite frame) resumes the ROUND focus. Reference: the
     * resume flag changes at map age 46 (NEW GAME f2662). */
    unsigned map_resume;
    uint16_t map_words[10];
    /* C3.2: the WORLD map as logic. `map_maqueta` is the frozen extracted map
     * layout (background + panel frame); `map` owns the world/round focus,
     * progress image and press-edge navigation. The `world`/`round`/`map_focus`
     * members above are the tick's mirror of `map` for the flow and the
     * measuring host. */
    od_scene_map map;
    uint16_t map_maqueta[OD_SCENE_WORDS];
    /* Castle cinematics (C3.2/A8, CASTILLOS). The castle branch runs the castle
     * sequence (the round-10 castle of worlds 1..9) before the level
     * dispatch. `level_castle` marks the pending round as a castle, so the
     * `LEVEL_ENTRY` scene plays the ordinary entry clip for the measured lead-in
     * and then switches to the world's recorded `castle-<N>.track`. START skips it
     * once the script has enabled it. The world-8 sweep used to be a 1.267-tick
     * retention of the entry frame; every castle is now a recorded cinematic. */
    unsigned level_castle;
    /* CASTILLOS: the world index (0..8) of the castle `level_castle` marks, the
     * clip tick drawn on the current step (-1 outside a castle) and the step on
     * which START skipped it. */
    unsigned castle_world;
    int castle_drawn;
    unsigned castle_skip;
    /* CLOSURE-C3.6: START read during the hidden-access cinematic: 1 on the step
     * that read it, 2 on the black step before the hidden menu. */
    unsigned hidden_skip;
    /* END-OF-WORLD: the end-of-world cinematic of
     * world `world_clear_world` (0..8), entered by the flow where the round-10 exit
     * would open the map, then the castle opening after world 9's, then "What
     * next?" (mode 3). `world_clear_part` is 0 for the world's clip and 1 for the
     * castle opening; `world_clear_drawn` the clip tick drawn on this step (-1
     * outside); `world_clear_skip` 1 on the step START was read, 2 on the following
     * map frame. */
    unsigned world_clear_world;
    unsigned world_clear_part;
    int world_clear_drawn;
    unsigned world_clear_skip;
    unsigned world_clear_seen; /* the clip's track_tick seen on the last step */
    /* CASTLE-APPEARS: `world_clear_part` 2 is the castle appearance of
     * `world_clear_world` (0..9). It ends into the map, not
     * "What next?": `world_clear_to_map` is raised on the step after its last map
     * frame (reference 1-1 last: clip f17991..f18305, map frame f18306, map
     * opening f18307) and the flow opens the map there. */
    unsigned world_clear_to_map;
    /* END-OF-WORLD: after an end-of-world the world counter has moved the world
     * "What next?"'s cont.story reopens the map on to the next one (1-10: counter
     * = 1 at the mode-3 frame f18709, map WORLD 2 at f18875). 0 = not set:
     * cont.story keeps `world`, the world the player was on (C6.23: the map's B,
     * EXIT, the magicword's YES and a password keep the counter). Any option clears
     * it. */
    unsigned whatnext_world;
    /* CLOSURE-C3.6: START in the hidden menu dispatches the round load in
     * the same frame (reference f960), without the
     * map's round entry or its palette fade; the flow consumes it. */
    unsigned hidden_dispatch;
    /* CLOSURE-C3.6: the hidden-menu entry drawn with the VRAM upload split over
     * its third frame (after the cinematic: reference f853/f704); not after a
     * NEW GAME (f1518 full). 2: after a solved hidden round the third frame
     * still shows the VRAM value of the round played (`hidden_stale_level`),
     * the next round from the fourth (C6.31: 11-1 solved, reference f4444 "11-1",
     * f4445 "11-2"). */
    unsigned hidden_partial;
    int hidden_stale_level;
    /* WORLD-MAP: the ROUND focus `A` starts the fade on the map as it stands; it
     * does not show a recorded map. `entry_display` is the map display of the frame
     * the round was accepted on and `entry_from_map` marks a LEVEL_ENTRY scene that
     * fades it. */
    uint16_t entry_display[OD_SCENE_WORDS];
    unsigned entry_from_map;
    /* ROUND-LOAD: the last frame of the scene a round load leaves (map
     * entry, castle, boss sequence), kept for the load's first frame. */
    uint16_t hold_display[OD_SCENE_WORDS];
    od_screen_id hold_scene;
    unsigned hold_age, hold_selection, hold_entry_map;
    /* WORLD-MAP: the map loop changes its state in the frame it reads the pad,
     * but the sprites it writes and the VRAM queue it fills (panel marks) reach the
     * screen on the next display pass (reference 5-6 route: world counter = 4 at
     * f15200, digit and marks of WORLD 5 from f15201). `map_next` is the display of
     * the state after this tick, presented on the next one. */
    uint16_t map_next[OD_SCENE_WORDS];
    unsigned map_next_valid;
    od_scene_whatnext wn;
    /* WHATNEXT: the menus' hearth illustration (scene_whatnext.h). Its counters
     * outlive the scenes like the state they mirror: the first menu leaves them to
     * "What next?", the map init clears them. `hearth_close` counts the menu's
     * 60-frame close, which runs in the first frames of the scene the A selected. */
    od_hearth hearth;
    unsigned hearth_close;
    /* WHATNEXT: the editor clip was recorded from the first menu, so its head
     * is that menu's close and fade. "What next?" plays its own, and its
     * `edit mode` enters the clip this many ticks in (scene_whatnext.h). */
    unsigned editor_skip;
    /* EDITOR-COLOR: clip ticks the menu scene starts into when it follows the
     * editor's EXIT (the MENU clip is the title route's, whose mode 3 sits later in
     * it). */
    unsigned menu_skip;
#define OD_MENU_CLIP_SKIP 2u
    /* The reference's mode-3 frame relative to the menu scene's age 0: the
     * title route enters the scene a frame early (-1), the map's B a frame
     * late (+1). The hearth schedule counts from mode 3. */
    int menu_lead;
    /* WHATNEXT-FADE: the frame that requested the menu init is the reference's
     * mode-3 frame (map B, magicword `Yes`): its init has already blanked the
     * screen (only stale sprites linger), so it is drawn black. */
    int menu_blank;
    /* R10 (CLOSURE-C3.3): the session already holds progress when the title route
     * reaches its menu (a loaded save, or RESET with the save intact). Its mode 3
     * runs the same init as B and EXIT, which draws "What next?" with progress;
     * `menu_init_wait` counts to that frame. */
    int boot_progress;
    unsigned menu_init_wait;
    unsigned password_error;
    unsigned password_error_delay;
    /* The frame the error message reverts (password_error 1 -> 0): the original's
     * handler writes no cursor sprite on it (reference OAM empty at f1880, shown
     * f1881). */
    int password_error_blank;
    od_hidden_scene hidden;
} od_screens;

/* Causal round10 producer. No durations, captured cues or skip ages. Completion
 * means cleanup OAM/NT publication has occurred. Empty queue remains managed. */
int od_screens_castle_round10_active(const od_screens *s);
int od_screens_castle_round10_complete(const od_screens *s);
int od_screens_castle_round10_skip_ready(const od_screens *s);
od_status od_screens_castle_round10_status(const od_screens *s);
/* Before the next simulation step: native background transfer owns the display
 * pass, not a captured lost-frame list. Query only while active. */
int od_screens_castle_round10_frame_tick_enabled(const od_screens *s);
/* Native parts0/1 only; part2 remains the independent castle-appearance family.
 * The display-pass predicate is consumed BEFORE the next simulation step, never
 * from age. complete means cleanup actually published and no opening root is
 * pending. */
int od_screens_world_clear_active(const od_screens *s);
int od_screens_world_clear_complete(const od_screens *s);
int od_screens_world_clear_skip_ready(const od_screens *s);
int od_screens_world_clear_frame_tick_enabled(const od_screens *s);
/* Goodnight stays active/managed after clip completion in the original palette
 * loop; hidden completes only after causal cleanup_stage3 publication. */
int od_screens_goodnight_hidden_active(const od_screens *s);
int od_screens_goodnight_hidden_complete(const od_screens *s);
int od_screens_goodnight_hidden_kind(const od_screens *s);
int od_screens_goodnight_hidden_skip_ready(const od_screens *s);
int od_screens_goodnight_hidden_frame_tick_enabled(const od_screens *s);
od_status od_screens_goodnight_hidden_status(const od_screens *s);
int od_screens_boss_ending_active(const od_screens *s);
int od_screens_boss_ending_complete(const od_screens *s);
int od_screens_boss_ending_kind(const od_screens *s);
int od_screens_boss_ending_skip_ready(const od_screens *s);
int od_screens_boss_ending_frame_tick_enabled(const od_screens *s);
od_status od_screens_boss_ending_status(const od_screens *s);
od_status od_screens_post_ending_begin(od_screens *s);
int od_screens_post_ending_active(const od_screens *s);
int od_screens_post_ending_program_finished(const od_screens *s);
int od_screens_post_ending_complete(const od_screens *s); /*always false: no caller return*/
int od_screens_post_ending_kind(const od_screens *s);
int od_screens_post_ending_skip_ready(const od_screens *s); /*always false*/
int od_screens_post_ending_frame_tick_enabled(const od_screens *s);
od_status od_screens_post_ending_status(const od_screens *s);
od_status od_screens_world_clear_status(const od_screens *s);

od_status od_screens_init(od_screens *s, const char *assets_dir);
/* Rebind the active native program after a pointer-free state restore. */
void od_screens_rebind_cinematic(od_screens *s);
/* Native cinematic mailbox; consume AFTER screens simulation, never drawing.
 * managed=false for legacy/interacting families not converted here. */
od_status od_screens_take_sounds(od_screens *s, od_cinematic_sounds *out);
void od_screens_free(od_screens *s);
void od_screens_tick(od_screens *s, unsigned buttons);
/* The first frame of a round load still shows the scene it came from: the map
 * entry's next fade step, or the last frame of a castle / the boss sequence.
 * `load_hold` is called once when the load starts, `draw_load_hold` draws that
 * frame. */
void od_screens_load_hold(od_screens *s);
void od_screens_draw_load_hold(const od_screens *s, od_index_frame *fb,
                                const uint8_t *chr);
void od_screens_draw(const od_screens *s, od_index_frame *fb, const uint8_t *chr);
/* Draw with the presentation palette. The hidden menu needs it: its text is CHR
 * pixels mapped through palette entry 1, unlike the extracted scene layouts,
 * which come with their own colours. */
void od_screens_draw_pal(const od_screens *s, od_index_frame *fb,
                          const uint8_t *chr, const od_palette_ram *pal);
void od_screens_retry(od_index_frame *fb, const uint8_t *chr, unsigned selection);
const char *od_screens_name(const od_screens *s);
/* Explicit developer entry points, not additional vanilla menu options. */
void od_screens_menu(od_screens *s);

/* Console RESET: re-enter the opening scene (title/intro) reusing the loaded
 * tracks. The save file is untouched; the caller reloads the session. */
void od_screens_boot(od_screens *s);

/* C6.31: the screens exactly as od_screens_init leaves them (power-on with
 * cleared RAM), keeping only what init loaded: the tracks, the native
 * cinematic programs and their timing records, the level-entry fade and the
 * map background. Every runtime field (password entry and its error message,
 * the editor, the map, the cinematics in flight, ...) restarts. Used by the
 * console RESET and by the modern menu's "New game" (CORE-5, CORE-6). */
void od_screens_power_on(od_screens *s);
void od_screens_map(od_screens *s);
/* Enter the WORLD map with the password's own progress image (low bits) and the
 * measured intro length in ticks. `words[w]` is world w+1; a zero word selects
 * round 1 of that world. `ready` is the map-intro length: the password route's
 * map ignores input until it has opened (PAL f11790..f12330), so the caller
 * passes the measured value; the menu route uses od_screens_map(), which is
 * interactive almost immediately. */
void od_screens_map_progress(od_screens *s, const uint16_t words[10],
                              unsigned ready);
/* Same, but opens the map on `world` (0-based), so a clear resumes on the
 * frontier world (C3.2 R2: after 1-10 the map opens on WORLD 2). */
void od_screens_map_progress_world(od_screens *s, const uint16_t words[10],
                                    int world, unsigned ready);
/* A solved round hands back to the map in ROUND focus on the world's first
 * uncleared round. Applied now on a steady (API) map; on a map opened with its
 * reveal it is applied by the loop init, at map age 46, like the original. */
void od_screens_map_round_focus_after_clear(od_screens *s);
/* Frames the map's opening runs ahead: 1 for the castle map script (world
 * 9's castle cleared), 0 for the ordinary one; its loop init is at map age 46 - lead. */
unsigned od_screens_map_lead(const od_screens *s);
/* Validated level index (0..99) of the map's current focus, or -1. */
int od_screens_map_level(const od_screens *s);
/* 0-based world the map is focused on. */
int od_screens_map_world(const od_screens *s);
void od_screens_editor(od_screens *s);
int od_screens_editor_ready(const od_screens *s);
od_status od_screens_editor_status(const od_screens *s);
/* Copy the resumed logical board during preparation; retains no pointer.
 * Returning caller must supply it before advancing the publisher. */
od_status od_screens_editor_prepare_board(od_screens *s,const od_editor *ed);
/* The menu init (mode 3): "What next?" with progress, "Where to begin from?"
 * otherwise, entered on the next tick (map B, editor EXIT). */
void od_screens_menu_init(od_screens *s, int progress);
/* CLOSURE-C3.6: NEW GAME in a hidden round (100..149) re-enters the hidden menu on the
 * round just played, on its +16 frame (reference 12-10: A f1500, menu f1516,
 * "ROUND 12-10" from f1518). */
void od_screens_hidden_return(od_screens *s, int level);
/* C6.31 (CORE-12): a solved hidden round. Where a normal round's exit opens the
 * map, the original re-enters the hidden menu on the NEXT round: measured with 11-1
 * solved (tests/data/session-hidden-11-1-input.txt): the exit tail and fade as a
 * normal round through f4443 (black from f4441), the menu from f4444 with the
 * played value still in VRAM ("ROUND 11-1") and "ROUND 11-2" from f4445. Only
 * 11-1 is measured; after 15-10 (149) the next round is not measured and the
 * menu's own A step (wrap to 11-1) is used. */
void od_screens_hidden_solved(od_screens *s, int level);
/* Re-enter the editor scene at clip tick `tick` (its own init, after the
 * menu part of the entry clip), as the original does back from a user level. */
void od_screens_editor_return(od_screens *s, unsigned tick);
void od_screens_password(od_screens *s);
int od_screens_password_ready(const od_screens *s);
/* Pure PASSWORD/editor task/buffer validation and opaque identity check against an
 * initialized trusted owner. Never dereferences candidate content pointers.
 * No mutation/allocation/IO. Editor's active program additionally checks native
 * pc/text/transfer bounds against the trusted owner. NOT a universal validator
 * for other scenes/programs or host atomic restore. */
od_status od_screens_validate_candidate(const od_screens *candidate,
                                         const od_screens *live);
/* "What next?" menu (C3.3): the valid-magicword destination and the editor's
 * EXIT destination. `cont.story` schedules the map (with `map_words`), `edit
 * mode` schedules the editor; `quit` ("Today's magicword") is not part of C3.3
 * and stays on the screen. */
void od_screens_whatnext(od_screens *s);
/* Boss sequence (H4.10): the world-10 queen briefing, the boss room and the
 * Boss dialogue, extracted as one automatic PAL track. Entered from the
 * world map when the world-10 crown is taken; it ends into ROUND 10-10. */
void od_screens_boss(od_screens *s);
/* Ending/credits sequence (H4.11): the automatic cutscene the PAL reference
 * plays after five ice hits defeat Boss in the combat mode, from the defeat
 * through the illustrated credits to the final hint screen. Extracted as one
 * automatic PAL track; the last screen loops the reference's own blink. Entered
 * from the boss victory (see the main.c notes). */
void od_screens_ending(od_screens *s);
/* END-OF-WORLD: the end-of-world cinematic of `world_index` (0..8), entered
 * on the frame the round-10 exit would open the map (reference 1-10: the clip
 * from f17632). It ends into "What next?" (after the castle opening for world
 * 9), exactly as the original's menu (f18709). */
void od_screens_world_clear(od_screens *s, int world_index);
/* CASTLE-APPEARS: the castle appearance of `world_index` (0..9), entered on
 * the frame the round's exit would open the map; it raises
 * `world_clear_to_map` on the step after its last map frame. */
void od_screens_castle_appears(od_screens *s, int world_index);
/* The timing record of the clip the WORLD_CLEAR scene plays (the world's end,
 * the castle opening or the castle appearance); NULL when not declared. */
const od_castle_info *od_screens_world_clear_info(const od_screens *s);

/* Boss combat scene (T1): the hand-off the game performs when ROUND 10-10 is
 * cleared (reference f25744). The app's gameplay view owns the `od_game` (Hero,
 * the arena board and the HUD clock); this struct owns the two verified Boss
 * controllers and the extra grid row the attack may address.
 * `od_combat_init` runs the observable part of the combat initialisation: it
 * loads the arena board from the package (screens/boss-arena.json), puts Hero on
 * cell (7,1) with her entry sequence armed, sets the arena objective counter to
 * 1 and zeroes the boss/attack objects. `od_combat_tick` runs the original
 * intra-frame order (attack slot 1, then main slot 0, then Hero) on the same
 * simulation clock. Verified tick-by-tick against the PAL reference in
 * tests/test_screens_combat.c. */
typedef struct {
    od_boss boss;
    od_boss_attack attack;
    uint8_t grid[256];   /* 256-byte grid view, rows 0..14 plus the extra row 15 */
    int have_attack;     /* the attack slot is registered */
    int active;          /* the combat scene has been entered */
    int defeated;        /* the clear ran: objects cleared and the objective decremented to 0, so Hero
                          * celebrates */
    int victory;         /* the celebration and its exit tail are over: the app
                          * leaves combat for the ending (H4.11 / T1). */
} od_combat;

/* initialise the arena board and Hero's entry sequence on `g`. */
void od_combat_init(od_combat *c, od_game *g);
/* The arena board alone, as a TRY AGAIN reload writes it before the re-arm
 * (reference f26217). */
void od_combat_load_board(od_game *g);
/* One combat tick: attack (slot 1), boss (slot 0) and the gameplay step. */
void od_combat_tick(od_combat *c, od_game *g, unsigned buttons);

/* TRY AGAIN / NEW GAME menu (H4.8 pause / H4.9 fail). The menu is the same
 * original screen in both cases: the bottom HUD band is replaced while the
 * level keeps running behind it. The cursor reads the button auto-repeat: a
 * button sets its bit on its press edge and then again 24 frames later and
 * every 4 frames while it stays held, measured on the PAL reference. The repeat
 * runs every frame of the level task, paused or not, so a direction held before
 * the band opens keeps its own repeat phase. `visible_selection` is the latch
 * the renderer must draw: the original PPU publishes the write one tick after
 * the input, like the other menus. */
typedef struct {
    unsigned selection;          /* 0 = TRY AGAIN, 1 = NEW GAME */
    unsigned visible_selection;  /* drawn this tick (one-tick PPU latch) */
    unsigned repeat;             /* pressed or auto-repeated buttons    */
    uint8_t counters[8];         /* per-button repeat counters          */
    unsigned held;               /* od_retry_tick: last frame's buttons   */
} od_retry_menu;
void od_retry_reset(od_retry_menu *m);
/* update `repeat` from this frame's held and pressed buttons. */
void od_retry_input(od_retry_menu *m, unsigned held, unsigned pressed);
/* One cursor step from `repeat`. A confirms the current option (returns 1,
 * *new_game = 0 TRY AGAIN / 1 NEW GAME); otherwise RIGHT (first) or LEFT moves
 * the cursor and sets *moved (menu sound). */
int od_retry_select(od_retry_menu *m, int *new_game, int *moved);
/* Convenience for a menu that is up every frame: latch, repeat and cursor step.
 * `buttons` is the held mask; the press edge is taken from the previous call.
 * Returns 1 and sets *new_game when A confirms. */
int od_retry_tick(od_retry_menu *m, unsigned buttons, int *new_game);

/* Pause and level-failure state (C3.4, A12): one frame of the level-state task,
 * i.e. the auto-repeat and the handler.
 *
 *   - It does nothing while a block is in flight or the round is clear.
 *   - A death arms the failure delay (120 frames) on the next frame; the band
 *     opens when it drains, or at once if START is pressed first.
 *   - START opens the band (paused flag, menu sound, cursor on TRY AGAIN); a
 *     second START closes it again, unless Hero is dead. Closing suspends the
 *     task for two frames.
 *   - With the band up, A confirms and LEFT/RIGHT move the cursor.
 *
 * `paused` only blocks Hero's decisions; the level loop keeps running. `active`
 * is the band on screen, published one display pass after `paused` changes. The
 * code lives in `src/core/scene_pause.c`. */
typedef struct {
    od_retry_menu menu;  /* cursor, latch and auto-repeat */
    unsigned delay;       /* failure delay; 0 = not counting          */
    unsigned hold;        /* frames the task stays suspended after closing */
    int paused;           /* START pause or failure band requested */
    int active;           /* the band is shown (one display pass after `paused`) */
    int new_game;         /* set when A confirmed NEW GAME                */
    unsigned sound;       /* sound request of this frame or 0           */
} od_pause;

/* The level-state bits (other than the pause request) that the handler reads, built by the caller. */
#define OD_PAUSE_GAME_FROZEN 0xE0u /* a block in flight              */
#define OD_PAUSE_GAME_CLEAR  0x10u /* the round is solved             */
#define OD_PAUSE_GAME_DEAD   0x08u /* Hero's death                   */
/* Not a player-state bit: the solved round's exit tail has stopped the level
 * task's input step (reference R1: its last run is f3975, one frame after the
 * clear flag at f3974; the repeat counters stand still from f3976). */
#define OD_PAUSE_GAME_EXIT   0x01u

enum { OD_PAUSE_NONE, OD_PAUSE_OPENED, OD_PAUSE_CLOSED, OD_PAUSE_CONFIRMED };

void od_pause_init(od_pause *p);
/* One frame of the level-state task. `held`/`pressed` are this frame's
 * buttons and press edges, `game03` the OD_PAUSE_GAME_* bits of the level state.
 * Returns OD_PAUSE_*; on CONFIRMED `p->new_game` is 0 (TRY AGAIN) or 1. */
int od_pause_frame(od_pause *p, unsigned held, unsigned pressed,
                    unsigned game03);

/* Return the level-entry palette fade-in and its frame count (may be 0).
 * The palettes are the resolved PPU RAM of the original, one 32-byte row per
 * gameplay tick; the app applies them while the level fades in. */
unsigned od_screens_level_fade(const od_screens *s, const uint8_t **palettes);

/* Password entry overlay (H5): 24 field cells plus the field and palette
 * cursors, composited over the extracted password screen. `field_tiles[i]`
 * is the nametable tile for cell i (the caller resolves a filled code through
 * od_password_glyph and passes the empty-cell tile 0xAD when unfilled);
 * `position` is the 0..23 field cursor and `cursor` the 0..34 palette cursor.
 * Before the extracted screen is fully drawn the plain scene is returned. */
void od_screens_draw_password(const od_screens *s,
                               const uint8_t field_tiles[24], unsigned position,
                               unsigned cursor, od_index_frame *fb,
                               const uint8_t *chr);

/* Editor overlay (H5): the extracted editor screen with the EDIT/PLAY/EXIT
 * toolbar cursor moved to `toolbar` (0..2) when `show_cursor` is set. The
 * palette and grid cursors of the extracted screen are not reconstructed;
 * hide the toolbar cursor outside TOOLBAR mode rather than misplace it. */
void od_screens_draw_editor(const od_screens *s, unsigned toolbar,
                             int show_cursor, od_index_frame *fb,
                             const uint8_t *chr);

/* C3.7 editor overlay: the real editor scene. Patches the board, the A/B tool
 * markers and the current cursor from `ed` (scene_editor.c) over the extracted
 * editor maqueta. */
void od_screens_draw_editor_full(const od_screens *s, const od_editor *ed,
                                  od_index_frame *fb, const uint8_t *chr);

/* The same overlay plus the palette entries the editor loop's colour cycle
 * (scene_editor.c) has written (`cyc` may be NULL). */
void od_screens_draw_editor_cycle(const od_screens *s, const od_editor *ed,
                                   const od_editor_cycle *cyc,
                                   od_index_frame *fb, const uint8_t *chr);

#endif
