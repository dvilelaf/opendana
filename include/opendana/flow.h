/* flow.h - scene machine and play session (pure C11, no SDL).
 *
 * `src/app/main.c` used to own the whole interactive/replay loops: the scene
 * state machine, the per-tick gameplay step, the audio sequencing and the
 * measurement outputs. This module takes that logic into the core so the SDL
 * app is a thin front end (window, pad, device, PNG) and the deterministic
 * harness observes the same code instead of including `main.c`.
 *
 * Two layers:
 *
 *   - `od_session` is the persistent play state: the 150-bit cleared bitmap,
 *     the world/round the map resumes at and the level currently loaded. It is
 *     what survives a *clear* (defect A4: the map used to forget it) and what
 *     `--continue` loads.
 *   - `od_flow` is one running scene machine: the sequence scenes (menu, map,
 *     password, editor, boss, ending) plus the playable level, its HUD clock,
 *     its animation clock and the Boss combat hand-off.
 *
 * The flow never touches SDL. Host services (input, presentation, audio device,
 * screenshots, host clock) come in through `od_flow_host`, so the SDL app and
 * the deterministic harness share every tick of this file. */
#ifndef OD_FLOW_H
#define OD_FLOW_H

#include "opendana/assets.h"
#include "opendana/audio.h"
#include "opendana/clock.h"
#include "opendana/compose.h"
#include "opendana/editor.h"
#include "opendana/game.h"
#include "opendana/hud.h"
#include "opendana/password.h"
#include "opendana/replay.h"
#include "opendana/save.h"
#include "opendana/screens.h"

#include <stdio.h>

/* ---- session ----------------------------------------------------------- */

typedef struct {
    od_progress progress;   /* the 150-bit cleared bitmap + map cursor   */
    int level;               /* level currently loaded, -1 when none      */
    int world;               /* 0-based world the session is in           */
    int round;               /* 0-based round the session is in           */
} od_session;

void od_session_reset(od_session *s);

/* Load the save into `s` (resetting it on any error) and save `s`. Return 1
 * when a valid save was loaded / the write succeeded. `path` may be NULL. */
int od_session_load(od_session *s, const char *path);
int od_session_save(const od_session *s, const char *path);

/* ---- flow options (the subset of the old app `options` the flow uses) --- */

typedef struct {
    const char *assets_dir;   /* resolved by the app                       */
    const char *replay;       /* scripted input path, or NULL              */
    const char *save_path;    /* resolved save path, or NULL               */
    int save_forced;          /* --save given: a replay may write it        */
    int load_save;            /* start from the save file (--continue/auto) */
    const char *screenshot;   /* PNG path, or NULL                         */
    int level;                /* direct level index                        */
    int level_seen;           /* --level given                             */
    int play_level;           /* --play-level: start on the materialisation */
    /* C6.31 (CORE-3, PLAN-V2 §6.3/§6.4): the level is a custom one (the
     * editor's Test, the catalogue's Play, --play-level). It is not a round:
     * the vanilla progress is neither loaded nor marked nor written, and the
     * run finishes where a round would open the map (solved, after the exit
     * tail) or on NEW GAME, like the original editor's user level. */
    int custom_level;
    int no_entry;             /* start at the post-entry rest state        */
    int no_audio;
    int hud;                  /* force the HUD in screenshots/replays      */
    int hud_frame;
    int map;                  /* start on the map                          */
    int world;                /* --world (0-based)                          */
    int cleared;              /* --cleared N                               */
    int menu;                 /* start in the menu                         */
    int continuing;           /* --continue                                */
    int ending;               /* --ending                                  */
    int ticks;                /* --ticks N (deterministic batch)           */
    int frames;               /* --frames N                                */
    int shot_frame;
    int shot_every;
    const char *state_out;
    /* C3.5 support: --record FILE writes the interactive buttons as an
     * reference-compatible replay script (one line per change), so a human
     * play-through can be replayed, measured and verified. */
    const char *record;
    /* C3.5 discovery tool: start in the boss arena (the combat that follows
     * 10-10). Only for finding the winnable route with a human; the route is
     * verified in the reference from the arena entry, never with this flag. */
    int boss;
    const char *hash_out;
    int hash_every;
    int hash_no_audio;
    int hash_render;
    const char *audio_out;
    const char *argb_out;
    int scale, headless, vsync;
    unsigned max_sim_hz;
} od_flow_opts;

/* ---- host services ------------------------------------------------------ */

struct od_flow;

typedef struct {
    void *ctx;
    /* Host monotonic clock (never SDL-inferred in the core). */
    uint64_t (*counter)(void *ctx);
    uint64_t (*frequency)(void *ctx);
    void (*sleep_ms)(void *ctx, unsigned ms);
    /* Input for the tick being simulated. `quit` set when the host stops.
     * When a replay is configured the flow reads it instead. */
    void (*buttons)(void *ctx, long tick, unsigned *out, int *quit);
    /* Interface service hook (C5.3): called once per simulated tick, before
     * the step, with the live flow. The host services the interface actions
     * that the deterministic/replay loop cannot reach through SDL events
     * (F5 save / F9 load and the slot keys). May be NULL. */
    void (*service)(void *ctx, struct od_flow *f, long tick);
    /* Console RESET request (edge-triggered): non-zero on the frame the host
     * saw the reset key. The flow restarts the opening scene and reloads the
     * session from the save file, like the console's RESET button. */
    int (*reset_poll)(void *ctx);
    /* Renderer lifetime (may all be NULL for a headless host). */
    int (*renderer_init)(void *ctx, int scale, int headless, int vsync);
    void (*renderer_shutdown)(void *ctx);
    int (*renderer_has_vsync)(void *ctx);
    /* Exactly one presentation call per simulated tick. `f` is the live flow
     * so a measuring host can read its scene/game state. */
    void (*present)(void *ctx, const struct od_flow *f, const uint32_t *argb);
    /* Write the composed frame (--screenshot). `numbered` picks the per-frame
     * name; `frame` is the label to use. Returns 0 on success. */
    int (*screenshot)(void *ctx, const struct od_flow *f,
                      const od_index_frame *fb, long frame, int numbered);
    /* Audio device (SDL). Absent or failing -> the flow uses the core engine.
     * The four driver callbacks below are bound to the same `ctx`. */
    int (*audio_open)(void *ctx);
    void (*audio_close)(void *ctx);
    void (*audio_tick)(void *ctx);
    void (*audio_request)(void *ctx, unsigned id);
    void (*audio_play_music)(void *ctx, int track);
    void (*audio_reset)(void *ctx);
} od_flow_host;

/* ---- flow state --------------------------------------------------------- */

/* One playable level's presentation state (the old `game_view`). */
typedef struct {
    od_hud_clock clock;
    od_hud_latch hud;
    od_presentation visible, queued;
    unsigned previous_buttons;
    od_pause pause;          /* START pause / level-failure menu (C3.4) */
    /* The bar template the pause close writes, queued like the clock and shown one
     * frame later: `round_blank` until the round value is rewritten, `time_boxes`
     * until the clock writer next writes the digits. */
    struct { unsigned char round_blank, time_boxes; } bar_queued, bar_visible;
    /* The bar of a round load: on the frame the round reaches the screen the bar
     * rows are still empty (`bar_empty`); the play loop's start writes the bar
     * template and has the round writer put the value, shown one frame later for
     * one frame (`level_template`) before the clock writer fills the digits. */
    unsigned char bar_empty, level_template_queued, level_template;
    /* The user level's first frame on screen still has the editor's palette
     * (reference f2906): the round's own palette is uploaded the next frame. */
    unsigned char old_palette;
    uint8_t old_palette_ram[32];
    /* A round load from the scene machine (map, castle, boss sequence): the
     * load's first frame still shows the previous scene, the next ones are
     * black until the round reaches the screen. */
    unsigned char load_from_scene;
    int new_game;             /* last confirm chose NEW GAME over TRY AGAIN */
    /* Retry reload: 1 + ticks since the confirm, 0 when no reload/fade is running. */
    unsigned retry_tick;
    /* The reload fade was started by the new-game option: the map opens at +16. */
    int retry_new_game;
    const uint8_t *fade;
    unsigned fade_frames, fade_tick;
    od_combat combat;
    int combat_active, combat_pending, combat_arm;
    unsigned load_stage;
    od_visual_state visual;
} od_play_view;

/* Password-entry scene state (C3.3): the PPU latch is one tick behind the CPU
 * and the field/cursor geometry live in scene_password.c. */
typedef od_scene_password od_password_view;

typedef struct {
    od_editor ed;
    od_editor shown;   /* one PPU frame behind: the cursor draw latch (C3.7) */
    /* The palette buffer writes of the colour editor and the copy the video update
     * has uploaded (one frame behind, like `shown`). */
    od_editor_cycle cyc, cyc_shown;
    int cyc_started;    /* the loop's first pass captured the init palette */
    /* C6.31: editor loop passes run since the init's last job (0, 1, 2+).
     * On the first pass the screen still shows that job's upload; the
     * editor's own (one-frame-behind) picture is drawn from the second. */
    int primed;
    od_level level;
    int active;
} od_editor_view;

/* Audio integration (H6): one sequencer tick per simulation tick, before the
 * gameplay step; SFX only from accepted actions. */
typedef struct {
    void (*tick)(void *ctx);
    void (*request)(void *ctx, unsigned id);
    void (*play_music)(void *ctx, int track);
    void (*reset_init)(void *ctx);
    void *ctx;
    int track;
    unsigned level_req;
    int celebration;
    long play_age;
    int was_play;
} od_audio_driver;

enum { OD_FLOW_SEQUENCE = 0, OD_FLOW_PLAY = 1, OD_FLOW_UNVERIFIED = 2 };

/* The mutable simulation state, grouped as one serialisable block.
 * `od_flow` embeds it through an anonymous union, so the historical
 * `f->session`, `f->screens`, `f->game`, ... spellings keep working while
 * `f->state` names the whole block for the state module.
 *
 * Some sub-structures hold transient pointers (the loaded track change
 * arrays in `screens`, the running magicword program, the audio engine and
 * the audio driver callbacks). `od_state_save` stores them zeroed (or as a
 * small index) and `od_flow_state_rebind` re-links them against the loaded
 * resources. */
/* Test-only: tests/state-evolution-run.sh builds a second tree with this
 * define to change the layout of the serialised block, as an engine change
 * would. Never defined in a product build. */
#ifdef OD_TEST_EXTRA_STATE_FIELD
#define OD_FLOW_TEST_EXTRA_MEMBER ; uint32_t test_extra_field
#else
#define OD_FLOW_TEST_EXTRA_MEMBER
#endif

#define OD_FLOW_STATE_MEMBERS                                              \
    od_session session;                                                    \
    od_screens screens;                                                    \
    od_game game;                                                          \
    int screen;                 /* OD_FLOW_SEQUENCE / _PLAY / _UNVERIFIED */ \
    int level;                                                             \
    int playing;                                                           \
    int from_editor;                                                       \
    unsigned editor_load;   /* editor PLAY staged load, 0 = none */        \
    uint8_t editor_palette[32];                                            \
    long clear_timer;                                                      \
    int world_clear_world;  /* FIN-DE-MUNDO world, -1 = none */            \
    int castle_appears_world; /* CASTILLO-APARECE world, -1 = none */      \
    od_play_view view;                                                    \
    od_password_view pw;                                                  \
    od_editor_view evw;                                                   \
    od_audio_driver audio;  /* sequencer driver: scalars + callbacks */   \
    od_audio audio_core;    /* the audio engine handle + counters */      \
    int audio_core_on;                                                     \
    uint8_t oam_rotation;   /* value carried through the map */          \
    int oam_rotation_known;                                                \
    unsigned frame_tick_boot;      /* boot frames left */             \
    unsigned fade_black;    /* 0..256 level-exit fade to black (A10) */   \
    unsigned last_buttons;                                                 \
    long sim_tick;                                                         \
    uint64_t rgb_hash, pcm_hash                                            \
    OD_FLOW_TEST_EXTRA_MEMBER

typedef struct { OD_FLOW_STATE_MEMBERS; } od_flow_state;

/* ---- layout-independent resume point (C6.62) ----------------------------- */

/* What a save state records besides the raw dump so that it survives a change
 * of the engine's structures: the session at the start of the level in play
 * and every input given since, or just the session when no level is running.
 * Plain integers and bytes only; nothing here depends on a structure layout. */
enum { OD_RESUME_NONE = 0, OD_RESUME_MAP = 1, OD_RESUME_LEVEL = 2 };
#define OD_RESUME_MAX_TICKS (1u << 22)

typedef struct {
    int kind;                       /* OD_RESUME_*                          */
    uint8_t cleared[OD_SAVE_CLEARED_BYTES];
    int progress_world, cursor;     /* map focus of the session             */
    int session_level, world, round;
    int level;                      /* level index (OD_RESUME_LEVEL)        */
    unsigned menu_repeat;           /* pause-menu repeat state carried over */
    uint8_t menu_counters[8];
    uint8_t oam_rotation, oam_known;
    long start_tick;                /* tick of the level's first frame      */
    long tick;                      /* tick the state was taken at          */
    const uint8_t *buttons;         /* one byte per tick since start_tick   */
    size_t count;
} od_flow_resume;

typedef struct {
    int armed;
    od_flow_resume r;               /* session at the level start           */
    uint8_t *buf;
    size_t count, cap;
} od_level_log;

typedef struct od_flow {
    /* Borrowed. */
    const od_assets *assets;
    od_flow_opts opt;
    od_flow_host host;
    int host_ok;

    /* The session (PLAN-V2 section 5.1). `state` names the whole block for the
     * state module; the anonymous struct keeps every historical field access
     * working. Both alias the same bytes. */
    union {
        od_flow_state state;
        struct { OD_FLOW_STATE_MEMBERS; };
    };

    /* Replay. */
    od_replay replay;
    int replay_on;

    /* Audio device routing. */
    int audio_on, audio_device;
    /* Runtime bridge failure, outside serialized state. Non-OK freezes step
     * after invalid/overflowed scene audio; host must surface the status.
     * Successful init/reset sets OK. Does not change save payload/layout. */
    od_status scene_sound_status;

    /* C6.31: how a custom level run finished (OD_FLOW_CUSTOM_*), outside the
     * serialised state. Once set the flow stops stepping; the interactive
     * loop returns (to the editor, or the --play-level process exits). */
    int custom_result;
    long custom_result_tick;

    /* C6.62: inputs since the level started; outside the serialised state. */
    od_level_log lvl_log;

    /* Measurement outputs (dev; nothing here is required by the product). */
    FILE *hash_fp, *state_fp, *argb_fp;
    /* --record: the interactive button script (C3.5). */
    FILE *record_fp;
    unsigned record_prev;
    int record_started;
    long present;
    int running;
    long stop_at;
    int replay_mode;
    int hud;
    od_index_frame fb;       /* the frame composed by the last step          */

    /* --audio-out accumulation. */
    int16_t *pcm_out;
    size_t pcm_out_count, pcm_out_cap;
} od_flow;

/* Initialise `f` for `opt`, loading the save and entering the starting scene.
 * Returns OD_OK or an error. */
od_status od_flow_init(od_flow *f, const od_assets *assets,
                         const od_flow_opts *opt, const od_flow_host *host);
void od_flow_free(od_flow *f);

/* Re-link the transient pointers inside `f->state` after a state has been
 * restored from a `.odstate` (or otherwise copied in): the level-entry fade
 * palette (which lives in `screens.level_fade`) and the running "Today's
 * magicword" program (which lives in the package art). The track change
 * arrays are re-linked by `od_state_load`, which still holds the freshly
 * loaded `screens` when it applies the payload. */
void od_flow_state_rebind(od_flow *f);

/* C6.31: results of a custom level run (od_flow.custom_result). */
enum { OD_FLOW_CUSTOM_NONE = 0, OD_FLOW_CUSTOM_SOLVED = 1,
       OD_FLOW_CUSTOM_NEW_GAME = 2 };

/* C6.31: the modern menu's "New game" (owner's decision, 2026-10-06). The
 * save file, if any, is copied to `<save>.bak` (its name is written to
 * `backup`, which may be NULL) and removed, and the flow restarts exactly like
 * a power-on with cleared RAM: the opening scene, and on the title's START the
 * menu "Where to begin from?" (beginning, magicword, password). The original
 * screens are not changed. Fails (and changes nothing) when the backup or the
 * removal fails, or for a custom level run. */
od_status od_flow_new_game(od_flow *f, char *backup, size_t cap);

/* C6.31 (CORE-14): a console RESET, a state load or "New game" breaks the
 * timeline a --record script describes (the reference-compatible format has no
 * such event). The recording is closed with a `#` line saying why, so the file
 * only ever holds one coherent timeline from reset. No-op without --record. */
void od_flow_timeline_break(od_flow *f, const char *why);

/* C6.62: describe the point a state taken now can be resumed from. `out`
 * borrows the input log of `f`; it is valid until the next step. */
void od_flow_resume_describe(const od_flow *f, od_flow_resume *out);
/* Rebuild that point on `f` through the level-start path of --play-level and
 * replay the inputs (no audio, no presentation). OD_OK leaves `f` at
 * `r->tick` with the state the original run had there. */
od_status od_flow_resume_apply(od_flow *f, const od_flow_resume *r);
/* After a state has been restored from the raw dump: keep (or drop, when
 * `r` is NULL or not a level) the input log so later saves stay resumable. */
void od_flow_resume_adopt(od_flow *f, const od_flow_resume *r);

/* One simulation tick with this tick's held buttons. */
void od_flow_step(od_flow *f, unsigned buttons);
/* Draw the current scene into `fb`. */
void od_flow_draw(const od_flow *f, od_index_frame *fb);
/* Scene name for a measuring host: "play" while a level runs, else the
 * sequence scene name. */
const char *od_flow_scene_name(const od_flow *f);

/* The two core loops the app used to own. They drive `od_flow_step` with the
 * host clock and presentation exactly as the shipped product does. */
void od_flow_run_interactive(od_flow *f);
/* Replay completion is distinct from host cancellation. Error values preserve
 * od_status numerically, including the scene sound source's exact status.
 * Hosts must consume this result; cancellation is not completed max_frames. */
typedef enum {
    OD_FLOW_REPLAY_OK = OD_OK,
    OD_FLOW_REPLAY_IO = OD_ERR_IO,
    OD_FLOW_REPLAY_FORMAT = OD_ERR_FORMAT,
    OD_FLOW_REPLAY_RANGE = OD_ERR_RANGE,
    OD_FLOW_REPLAY_NOMEM = OD_ERR_NOMEM,
    OD_FLOW_REPLAY_CANCELLED
} od_flow_replay_status;
od_flow_replay_status od_flow_run_replay(od_flow *f);

/* Level-entry loading window staged by `od_flow`. */
#define OD_PLAY_LOAD_GRID 3u
#define OD_PLAY_LOAD_FULL 6u
#define OD_ENTRY_HOLD_TICKS 20u

#endif /* OD_FLOW_H */
