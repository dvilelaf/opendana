/* main.c - SDL2 front end (thin layer).
 *
 * The scene machine, the session and both simulation loops live in the core
 * (`src/core/flow.c`, `include/opendana/flow.h`). This file only owns what is truly
 * SDL/CLI:
 *
 *   - argument parsing and the developer/reporting subcommands;
 *   - the SDL window, the pad and the audio device;
 *   - the PNG output of `--screenshot`;
 *   - the static (non-looping) screenshot paths.
 *
 * Modes:
 *   (default)        interactive window: menu -> world map -> level -> map
 *   --replay FILE    headless scripted run (reference-compatible input format)
 *   --screenshot F   static render (level, or the world map with --map)
 *
 * It reads files from the content package (plus the replay script the user
 * names). It has nothing to do with a ROM. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "opendana/assets.h"
#include "opendana/audio.h"
#include "opendana/audio_sdl.h"
#include "opendana/clock.h"
#include "opendana/common.h"
#include "opendana/compose.h"
#include "opendana/content.h"
#include "opendana/import.h"
#include "opendana/input_pad.h"
#include "opendana/ui_import.h"
#include "opendana/ui_import_view.h"
#include "opendana/flow.h"
#include "opendana/hud.h"
#include "opendana/level.h"
#include "opendana/palette.h"
#include "opendana/password.h"
#include "opendana/platform.h"
#include "opendana/png.h"
#include "opendana/render_sdl.h"
#include "opendana/replay.h"
#include "opendana/rom_picker.h"
#include <SDL_syswm.h>
#include "opendana/save.h"
#include "opendana/screens.h"
#include "opendana/ui.h"
#include "opendana/ui_catalog.h"
#include "opendana/ui_editor.h"
#include "opendana/ui_font.h"
#include "opendana/ui_strings.h"
#include "opendana/ext/catalog.h"
#include "opendana/ext/leveled.h"
#include "opendana/ext/settings.h"
#include "opendana/ext/state.h"
#include "opendana/userdir.h"
#include "desktop_integration.h"
#include "../import/sha256.h"

#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

#ifndef OD_VERSION
#define OD_VERSION "0.7.0"
#endif

/* Real ending/credits clip (H4.11). --ending --shot-frame takes an reference
 * frame in [OD_ENDING_FIRST, OD_ENDING_LAST]. */
#define OD_ENDING_FIRST 24482
#define OD_ENDING_LAST  47000

typedef struct {
    const char *assets_dir;
    const char *screenshot;
    const char *replay;
    const char *save;
    int level;
    int level_seen;
    int scale;
    int headless;
    int frames;
    long ticks;
    int shot_frame;
    int shot_every;
    int list_levels;
    int hud;
    int hud_frame;
    int map;
    int world;
    int cleared;
    int menu;
    int no_audio;
    int continuing;
    int ending;
    int show_save;
    int no_entry;
    const char *state_out;
    const char *record;      /* --record FILE: write the played button script  */
    int boss;                /* --boss: start in the boss arena (C3.5 tool)    */
    const char *audio_out;
    const char *argb_out;
    int vsync;
    unsigned max_sim_hz;
    const char *hash_out;
    int hash_every;
    int hash_no_audio;
    int hash_render;
    int fullscreen;           /* --fullscreen: borderless desktop fullscreen   */
    int maximized;            /* --maximized: window-manager maximise          */
    const char *content;      /* --content DIR: content package base (C2.1)     */
    const char *import_rom;   /* --import-rom FILE: import the EU ROM (C5.8)   */
    const char *mods[OD_CONTENT_MAX_LAYERS]; /* --mod DIR, repeatable        */
    int mod_count;
    const char *play_level;   /* --play-level FILE: play one level file       */
    int round;                /* --round N: round of the world file (1..10)   */
    const char *validate;     /* --validate PATH: check a file and exit       */
    const char *render_level; /* --render-level FILE: editor thumbnail (C4.1)  */
    const char *out;          /* --out PNG: output of --render-level          */
    const char *settings;     /* --settings FILE: settings.ini (C5.1)         */
    int desktop_cmd;          /* --install-desktop (1) / --uninstall-desktop (2), C6.46 */
    int editor;               /* --editor: open the level editor (C5.5)      */
    const char *editor_file;  /* optional file to open in the editor         */
    int first_run;            /* --first-run: force the import screen        */
    const char *first_run_shot; /* --first-run-shot PNG: capture it          */
    const char *rom_arg;      /* positional ROM: dropped on the app icon    */
#ifdef OD_DEV_TOOLS
    const char *ui_demo;      /* --ui-demo FILE: overlay capture (C5.1)       */
    const char *ui_demo_screen; /* --ui-demo-screen menu|settings|mute|states */
    /* C5.3 determinism harness: save at a tick and load at startup, through
     * the same state module the F5/F9 keys use. */
    const char *state_save;   /* --state-save FILE                            */
    long state_save_at;       /* --state-save-at TICK (-1 = off)              */
    const char *state_load;   /* --state-load FILE                            */
    /* C5.10 video capture instruments. */
    const char *video_demo;   /* --video-demo FILE: scaled window capture     */
    int video_win_w;          /* --video-win W H: explicit window size        */
    int video_win_h;
    int video_integer;        /* --video-integer 0|1 (-1 = from settings)     */
    int video_aspect;         /* --video-aspect 0|1 (-1 = from settings)      */
    const char *focus_demo;   /* --focus-demo FILE: pause-on-focus capture    */
    /* C6.31 session instruments: the console RESET and the modern menu's
     * "New game" at a tick, through the same host functions as F2 and the
     * menu entry (deterministic --ticks runs; 0 = off). */
    long reset_at;            /* --reset-at TICK                              */
    long new_game_at;         /* --new-game-at TICK                           */
#endif
} options;

static void usage(const char *argv0)
{
    printf("OpenDana - an open, moddable puzzle-platformer engine (EU edition)\n\n");
    printf("usage: %s [options]\n\n", argv0);
    printf("  --assets DIR      deprecated; use --content DIR (no legacy dumps read)\n");
    printf("  --scale N         initial window scale (default: 3)\n");
    printf("  --fullscreen      start fullscreen (EWMH; never changes the video mode)\n");
    printf("  --maximized       start with the window maximised (F11 = fullscreen)\n");
    printf("  --save FILE       save file (default: $XDG_DATA_HOME/opendana/\n");
    printf("                    save.dat, else ~/.local/share/.../save.dat, else\n");
    printf("                    ./opendana.sav)\n");
    printf("  --continue        load the save at startup\n");
    printf("  --show-save       print the save file status and exit\n");
    printf("  --replay FILE     run a scripted input file headlessly and exit\n");
    printf("  --frames N        frames to simulate (replay/limit); 0 = until quit\n");
    printf("  --ticks N         deterministic batch: run exactly N ticks with no\n");
    printf("                    wall-clock reference and exit (harness/repro)\n");
    printf("  --shot-frame N    frame at which --screenshot is captured (replay)\n");
    printf("  --shot-every N    capture each Nth tick, reference-compatible filenames\n");
    printf("  --screenshot F    write one 256x240 PNG to F\n");
    printf("  --headless        no visible window (pair with SDL_VIDEODRIVER=dummy)\n");
    printf("  --hud             draw the ROUND/TIME status bar in --screenshot/--replay\n");
    printf("                    output (the interactive game always shows it)\n");
    printf("  --state-out FILE  per-tick app state CSV, boss-link verification\n");
    printf("  --record FILE     record the played buttons as a replay script (C3.5)\n");
    printf("  --no-audio        do not open an audio device\n");
    printf("  --audio-out FILE  headless replay: dump the emitted PCM to a WAV\n");
    printf("  --argb-out FILE   headless: dump each tick's 256x240 ARGB frame\n");
    printf("                    (the H4.11 ending measurement path)\n");
    printf("  --vsync           request present-vsync (off by default; the\n");
    printf("                    simulation clock never depends on it)\n");
    printf("  --sim-hz N        cap the presentation rate at N Hz (default 60;\n");
    printf("                    used by the 60/120/144 Hz characterisation)\n");
    printf("  --hash-out FILE   write per-tick semantic/RGB/PCM hashes and exit;\n");
    printf("                    proves identical hashes at the same tick\n");
    printf("  --hash-every N    hash every Nth tick (default 1)\n");
    printf("  --hash-no-audio   --hash-out without opening the audio engine\n");
    printf("  --hash-render     --hash-out while presenting through the real SDL\n");
    printf("                    renderer (equivalence with the windowed path)\n");
    printf("  --content DIR     content package base (default: content/base)\n");
    printf("  --import-rom FILE import your own EU game ROM once and\n");
    printf("                    write the content package under XDG; the ROM is\n");
    printf("                    not needed afterwards (also: drag the ROM onto\n");
    printf("                    the window on first run)\n");
    printf("  --first-run       show the first-run import screen (drag the ROM)\n");
    printf("  --first-run-shot F  render the first-run screen to F and exit\n");
    printf("  --mod DIR         overlay a mod directory (repeatable)\n");
    printf("  --play-level FILE play one level file (a world file with --round)\n");
    printf("  --round N         round 1..10 of the --play-level world file\n");
    printf("  --validate PATH   validate a pack/mod/level path and exit\n");
    printf("  --render-level FILE  render a level file to a 256x240 PNG thumbnail\n");
    printf("  --out PNG         output file of --render-level\n");
    printf("  --install-desktop   add OpenDana to the applications menu (Linux)\n");
    printf("  --uninstall-desktop remove it again (only what OpenDana made)\n");
    printf("  --settings FILE   settings.ini (default: $XDG_CONFIG_HOME/\n");
    printf("                    opendana/settings.ini); ESC opens the menu\n");
    printf("  --editor [FILE]   open the mouse level editor (C5.5), optionally\n");
    printf("                    loading a custom level or a package world file\n");
    printf("  --version         print version and exit\n");
    printf("  --help            this message\n");
    printf("\ncontrols (F1 shows them in the game; Esc menu > Controls too):\n");
    printf("  Arrows            move, jump\n");
    printf("  Z or Space        A (wand)         X     B (wand)\n");
    printf("  Enter             START            Right Shift   SELECT\n");
    printf("  Esc               menu             M     mute\n");
    printf("  F5 / F9           save / load state in the current slot\n");
    printf("  F6 / F7           previous / next slot\n");
    printf("  F12               screenshot       F2    console RESET\n");
    printf("  F11               fullscreen       F1    controls screen\n");
    printf("  gamepad Guide     menu\n");
    printf("  editor            Ctrl+Z / Ctrl+Y undo / redo, Ctrl+S save, F5 test,\n");
    printf("                    right click erase, wheel material, middle click pick\n");
#ifdef OD_DEV_TOOLS
    printf("\ndevelopment options (opendana-dev only; scene shortcuts for measurement):\n");
    printf("  --level N         play level index 0..149 directly (skips the menu)\n");
    printf("  --hud-frame N     advance N frames before a static --screenshot\n");
    printf("  --map             show the WORLD n map (interactive or screenshot)\n");
    printf("  --world N         world shown on the map, 1..10 (default 1)\n");
    printf("  --cleared N       mark the first N levels cleared (map screenshot)\n");
    printf("  --menu            start the interactive session in the main menu\n");
    printf("  --ending          play the real extracted ending/credits scene\n");
    printf("  --no-entry        start --level after the entry materialization\n");
    printf("                    (verified mid-level fixtures; dev measurement)\n");
    printf("  --boss            start in the boss arena (C3.5 discovery tool)\n");
    printf("  --list-levels     print the level table and exit\n");
    printf("  --ui-demo FILE    compose the modern overlay over level 0 and write\n");
    printf("                    FILE (C5.1 capture; --ui-demo-screen menu|settings|mute|states)\n");
    printf("  --state-save FILE write a .odstate when --state-save-at is reached\n");
    printf("  --state-save-at TICK  tick the determinism harness saves at\n");
    printf("  --state-load FILE  load a .odstate at startup (determinism harness)\n");
    printf("  --video-demo FILE  capture the scaled window (C5.10): --video-win W H,\n");
    printf("                    --video-integer 0|1, --video-aspect 0|1\n");
    printf("  --focus-demo FILE  capture the pause-on-focus screen (C5.10)\n");
    printf("  --reset-at TICK   press the console RESET (F2) at TICK (C6.31)\n");
    printf("  --new-game-at TICK  run the menu's New game at TICK (C6.31)\n");
#endif
    printf("\n");
    printf("Interactive: arrows move/jump; Z or SPACE = wand (A); X = wand (B).\n");
    printf("Menu: UP/DOWN select, A confirms. Map: UP/DOWN switch world,\n");
    printf("LEFT/RIGHT pick a round, A enters, B leaves the round select; B on\n");
    printf("the world goes back to the menu, as in the original.\n");
    printf("ESC opens the modern menu (Continue, States, Editor, Levels,\n");
    printf("Settings, New game, Quit). M mutes. F5 saves a state, F9 loads it, F6/F7\n");
    printf("change the slot. F12 saves a screenshot in the data folder.\n");
    printf("F2 = the console's RESET button (back to the title, save intact);\n");
    printf("F11 = fullscreen (kept in the settings).\n");
}

static int print_levels(const od_assets *assets)
{
    for (int i = 0; i < assets->levels.count; i++) {
        const od_level *lv = &assets->levels.levels[i];
        printf("%3d  start=(%2d,%2d)  meta=0x%02X  commands=%u\n", i,
               od_player_x(lv), od_player_y(lv), lv->metadata, lv->command_len);
    }
    return 0;
}

static int parse_args(options *o, int argc, char **argv)
{
    memset(o, 0, sizeof(*o));
    o->assets_dir = NULL;
    o->level = 0;
    o->scale = 3;
    o->shot_frame = -1;
    o->world = 0;
    o->round = 1;
#ifdef OD_DEV_TOOLS
    o->state_save_at = -1;
    o->video_integer = -1;
    o->video_aspect = -1;
#endif
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--assets") && i + 1 < argc)
            o->assets_dir = argv[++i];
        else if (!strcmp(a, "--screenshot") && i + 1 < argc)
            o->screenshot = argv[++i];
        else if ((!strcmp(a, "--replay") || !strcmp(a, "--input")) && i + 1 < argc)
            o->replay = argv[++i];
        else if (!strcmp(a, "--save") && i + 1 < argc)
            o->save = argv[++i];
#ifdef OD_DEV_TOOLS
        else if (!strcmp(a, "--level") && i + 1 < argc) {
            o->level = atoi(argv[++i]);
            o->level_seen = 1;
        }
#endif
        else if (!strcmp(a, "--scale") && i + 1 < argc)
            o->scale = atoi(argv[++i]);
        else if (!strcmp(a, "--fullscreen"))
            o->fullscreen = 1;
        else if (!strcmp(a, "--maximized"))
            o->maximized = 1;
        else if (!strcmp(a, "--frames") && i + 1 < argc)
            o->frames = atoi(argv[++i]);
        else if (!strcmp(a, "--ticks") && i + 1 < argc)
            o->ticks = atol(argv[++i]);
        else if (!strcmp(a, "--shot-frame") && i + 1 < argc)
            o->shot_frame = atoi(argv[++i]);
        else if (!strcmp(a, "--shot-every") && i + 1 < argc)
            o->shot_every = atoi(argv[++i]);
#ifdef OD_DEV_TOOLS
        else if (!strcmp(a, "--hud-frame") && i + 1 < argc)
            o->hud_frame = atoi(argv[++i]);
#endif
#ifdef OD_DEV_TOOLS
        else if (!strcmp(a, "--world") && i + 1 < argc)
            o->world = atoi(argv[++i]) - 1;
        else if (!strcmp(a, "--cleared") && i + 1 < argc)
            o->cleared = atoi(argv[++i]);
#endif
        else if (!strcmp(a, "--install-desktop"))
            o->desktop_cmd = 1;
        else if (!strcmp(a, "--uninstall-desktop"))
            o->desktop_cmd = 2;
        else if (!strcmp(a, "--headless"))
            o->headless = 1;
        else if (!strcmp(a, "--hud"))
            o->hud = 1;
#ifdef OD_DEV_TOOLS
        else if (!strcmp(a, "--map"))
            o->map = 1;
        else if (!strcmp(a, "--menu"))
            o->menu = 1;
#endif
        else if (!strcmp(a, "--continue"))
            o->continuing = 1;
#ifdef OD_DEV_TOOLS
        else if (!strcmp(a, "--ending"))
            o->ending = 1;
        else if (!strcmp(a, "--no-entry"))
            o->no_entry = 1;
#endif
        else if (!strcmp(a, "--state-out") && i + 1 < argc)
            o->state_out = argv[++i];
        else if (!strcmp(a, "--record") && i + 1 < argc)
            o->record = argv[++i];
#ifdef OD_DEV_TOOLS
        else if (!strcmp(a, "--boss"))
            o->boss = 1;
#endif
        else if (!strcmp(a, "--show-save"))
            o->show_save = 1;
        else if (!strcmp(a, "--no-audio"))
            o->no_audio = 1;
        else if (!strcmp(a, "--audio-out") && i + 1 < argc)
            o->audio_out = argv[++i];
        else if (!strcmp(a, "--argb-out") && i + 1 < argc)
            o->argb_out = argv[++i];
        else if (!strcmp(a, "--hash-out") && i + 1 < argc)
            o->hash_out = argv[++i];
        else if (!strcmp(a, "--hash-every") && i + 1 < argc)
            o->hash_every = atoi(argv[++i]);
        else if (!strcmp(a, "--hash-no-audio"))
            o->hash_no_audio = 1;
        else if (!strcmp(a, "--hash-render"))
            o->hash_render = 1;
        else if (!strcmp(a, "--vsync"))
            o->vsync = 1;
        else if (!strcmp(a, "--sim-hz") && i + 1 < argc)
            o->max_sim_hz = (unsigned)atoi(argv[++i]);
#ifdef OD_DEV_TOOLS
        else if (!strcmp(a, "--list-levels"))
            o->list_levels = 1;
#endif
        else if (!strcmp(a, "--content") && i + 1 < argc)
            o->content = argv[++i];
        else if (!strcmp(a, "--import-rom") && i + 1 < argc)
            o->import_rom = argv[++i];
        else if (!strcmp(a, "--mod") && i + 1 < argc) {
            if (o->mod_count < OD_CONTENT_MAX_LAYERS)
                o->mods[o->mod_count++] = argv[++i];
        } else if (!strcmp(a, "--play-level") && i + 1 < argc)
            o->play_level = argv[++i];
        else if (!strcmp(a, "--round") && i + 1 < argc)
            o->round = atoi(argv[++i]);
        else if (!strcmp(a, "--validate") && i + 1 < argc)
            o->validate = argv[++i];
        else if (!strcmp(a, "--render-level") && i + 1 < argc)
            o->render_level = argv[++i];
        else if (!strcmp(a, "--out") && i + 1 < argc)
            o->out = argv[++i];
        else if (!strcmp(a, "--settings") && i + 1 < argc)
            o->settings = argv[++i];
        else if (!strcmp(a, "--editor")) {
            o->editor = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                o->editor_file = argv[++i];
        }
        else if (!strcmp(a, "--first-run"))
            o->first_run = 1;
        else if (!strcmp(a, "--first-run-shot") && i + 1 < argc)
            o->first_run_shot = argv[++i];
#ifdef OD_DEV_TOOLS
        else if (!strcmp(a, "--ui-demo") && i + 1 < argc)
            o->ui_demo = argv[++i];
        else if (!strcmp(a, "--ui-demo-screen") && i + 1 < argc)
            o->ui_demo_screen = argv[++i];
        else if (!strcmp(a, "--state-save") && i + 1 < argc)
            o->state_save = argv[++i];
        else if (!strcmp(a, "--state-save-at") && i + 1 < argc)
            o->state_save_at = atol(argv[++i]);
        else if (!strcmp(a, "--state-load") && i + 1 < argc)
            o->state_load = argv[++i];
        else if (!strcmp(a, "--video-demo") && i + 1 < argc)
            o->video_demo = argv[++i];
        else if (!strcmp(a, "--video-win") && i + 2 < argc) {
            o->video_win_w = atoi(argv[++i]);
            o->video_win_h = atoi(argv[++i]);
        } else if (!strcmp(a, "--video-integer") && i + 1 < argc)
            o->video_integer = atoi(argv[++i]) ? 1 : 0;
        else if (!strcmp(a, "--video-aspect") && i + 1 < argc)
            o->video_aspect = atoi(argv[++i]) ? 1 : 0;
        else if (!strcmp(a, "--focus-demo") && i + 1 < argc)
            o->focus_demo = argv[++i];
        else if (!strcmp(a, "--reset-at") && i + 1 < argc)
            o->reset_at = atol(argv[++i]);
        else if (!strcmp(a, "--new-game-at") && i + 1 < argc)
            o->new_game_at = atol(argv[++i]);
#endif
        else if (!strcmp(a, "--version")) {
            printf("opendana %s\n", OD_VERSION);
            exit(0);
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            exit(0);
        } else if (a[0] != '-' && o->rom_arg == NULL) {
            /* C6.21: a positional argument is the ROM the player dropped on the
             * application icon (the desktop passes it as argv[1]). The importer
             * validates it and fails closed. */
            o->rom_arg = a;
        } else {
            fprintf(stderr, "unknown argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }
    if (o->world < 0)
        o->world = 0;
    if (o->world >= OD_WORLD_COUNT)
        o->world = OD_WORLD_COUNT - 1;
    if (o->headless)
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (o->hash_every <= 0)
        o->hash_every = 1;
    return 0;
}

/* ---- save file helpers ------------------------------------------------- */

static void resolve_save_path(const options *o, char *out, size_t cap)
{
    if (o->save)
        snprintf(out, cap, "%s", o->save);
    else
        od_save_default_path(out, cap);
}

static int path_separator(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static int create_directory(const char *dir)
{
#ifdef _WIN32
    int rc = od_userdir_mkdir_p(dir);   /* UTF-8 -> wide (C6.36) */
    if (rc == 0)
        return 0;
    errno = EACCES;
    return -1;
#else
    int rc = mkdir(dir, 0777);
#endif
    if (rc == 0)
        return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(dir, &st) != 0)
            return -1;
        if (S_ISDIR(st.st_mode))
            return 0;
        errno = ENOTDIR;
    }
    return -1;
}

static int ensure_parent_dir(const char *path)
{
    const char *slash = NULL;
    for (const char *p = path; *p; p++)
        if (path_separator(*p))
            slash = p;
    if (!slash || slash == path)
        return 0;
    char dir[1024];
    size_t n = (size_t)(slash - path);
    if (n >= sizeof(dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(dir, path, n);
    dir[n] = '\0';
    size_t root = path_separator(dir[0]) ? 1 : 0;
#ifdef _WIN32
    if (n >= 2 && isalpha((unsigned char)dir[0]) && dir[1] == ':')
        root = n >= 3 && path_separator(dir[2]) ? 3 : 2;
    else if (n >= 2 && path_separator(dir[0]) && path_separator(dir[1])) {
        /* Do not mkdir a UNC server or share, only descendants of the share. */
        root = 2;
        for (int part = 0; part < 2; part++) {
            while (root < n && !path_separator(dir[root]))
                root++;
            if (root < n)
                root++;
        }
    }
#endif
    for (char *q = dir + root; *q; q++) {
        if (!path_separator(*q))
            continue;
        char sep = *q;
        *q = '\0';
        int rc = create_directory(dir);
        *q = sep;
        if (rc != 0)
            return -1;
    }
    return n <= root ? 0 : create_directory(dir);
}

/* ---- settings (C5.1) ---------------------------------------------------- */

static void resolve_settings_path(const options *o, char *out, size_t cap)
{
    if (o->settings)
        snprintf(out, cap, "%s", o->settings);
    else
        od_settings_default_path(out, cap);
}

static void load_settings(const options *o, char *path, size_t cap,
                          od_settings *s)
{
    resolve_settings_path(o, path, cap);
    od_settings_load(s, path);   /* missing file -> defaults, no error */
    if (s->warnings > 0)
        fprintf(stderr, "settings: %s (%d invalid entr%s ignored)\n", path,
                s->warnings, s->warnings == 1 ? "y" : "ies");
    if (s->muted)
        fprintf(stderr, "settings: %s (muted, gain=0)\n", path);
}

/* ---- PNG output -------------------------------------------------------- */

/* Write the composed index frame as RGB, darkening towards black by `fade`
 * (0..256) exactly like the presented ARGB (A10 exit fade). */
static int write_screenshot_fade(const char *path, const od_index_frame *fb,
                                 unsigned fade)
{
    static uint32_t argb[OD_SCREEN_W * OD_SCREEN_H];
    static uint8_t rgb[OD_SCREEN_W * OD_SCREEN_H * 3];
    od_frame_to_argb(fb, &od_master_palette_pal, argb);
    unsigned keep = fade ? 256u - fade : 256u;
    for (int i = 0; i < OD_SCREEN_W * OD_SCREEN_H; i++) {
        uint32_t c = argb[i];
        unsigned r = ((c >> 16) & 0xffu) * keep / 256u;
        unsigned g = ((c >> 8) & 0xffu) * keep / 256u;
        unsigned b = (c & 0xffu) * keep / 256u;
        rgb[i * 3 + 0] = (uint8_t)r;
        rgb[i * 3 + 1] = (uint8_t)g;
        rgb[i * 3 + 2] = (uint8_t)b;
    }
    if (od_png_write_rgb(path, OD_SCREEN_W, OD_SCREEN_H, rgb) != 0) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    printf("wrote %s (%dx%d)\n", path, OD_SCREEN_W, OD_SCREEN_H);
    return 0;
}

/* Static screenshot: level (+HUD), the world map or the ending. */
static int run_screenshot(const options *o, const od_assets *assets)
{
    od_index_frame fb;
    if (o->ending) {
        long frame = o->shot_frame >= 0 ? o->shot_frame : OD_ENDING_FIRST;
        od_screens s;
        if (od_screens_init(&s, o->assets_dir) != OD_OK) return 1;
        od_screens_ending(&s);
        long advance = frame - OD_ENDING_FIRST;
        if (advance < 0) advance = 0;
        if (advance > (long)(OD_ENDING_LAST - OD_ENDING_FIRST))
            advance = (long)(OD_ENDING_LAST - OD_ENDING_FIRST);
        for (long i = 0; i <= advance; ++i) od_screens_tick(&s,0);
        od_screens_draw(&s,&fb,assets->chr);
        od_screens_free(&s);
    } else if (o->map || (o->menu && !o->hud)) {
        od_screens s;
        if (o->map && (o->world || o->cleared)) {
            fprintf(stderr,"map: only the extracted WORLD 1 entry is available\n");
            return 2;
        }
        if (od_screens_init(&s,o->assets_dir) != OD_OK) return 1;
        if (o->map && o->continuing) {
            /* CONTINUE opens the map with the save's own progress image. */
            char save_path[1024];
            od_progress prog;
            resolve_save_path(o, save_path, sizeof save_path);
            if (od_save_read(save_path, &prog) == OD_OK) {
                uint16_t words[OD_WORLD_COUNT];
                od_progress_map_words(&prog, words);
                od_screens_map_progress(&s, words, 0);
            } else {
                od_screens_map(&s);
            }
        } else if (o->map) od_screens_map(&s);
        else od_screens_menu(&s);
        int frames = o->hud_frame > 0 ? o->hud_frame : o->map ? 225 : 100;
        for (int i = 0; i < frames; ++i) od_screens_tick(&s,0);
        od_screens_draw(&s,&fb,assets->chr);
        od_screens_free(&s);
    } else {
        od_render_level_index(&fb, &assets->levels.levels[o->level], o->level,
                               assets->chr, &assets->palette_ram);
    }
    return write_screenshot_fade(o->screenshot, &fb, 0);
}

/* ---- SDL host ----------------------------------------------------------- */

typedef struct {
    const options *o;
    od_renderer rend;
    int use_renderer;
    od_audio_sdl audio;
    int audio_ready;
    unsigned previous_buttons;
    od_pads pads;
    unsigned controller_buttons;
    unsigned controller_ui_previous;
    unsigned pad_ui_previous;    /* C6.32: interface meaning of the pads    */
    int ui_dirty;                /* C6.32: the frozen screen needs a redraw */
    int desktop_checked;         /* C6.46: the applications-menu question was decided */
    int ui_toast_shown;
    Uint32 ui_last_present;
    int test_paused;             /* C6.32: Test paused by focus loss        */
    int quit;
    int reset;   /* F2 (console RESET): handled by the flow, not a game button */
    /* C6.31 (CORE-4/UI-4): game buttons held when a modal screen closes do
     * not reach the game until they are released (Enter closing the menu used
     * to arrive as START and open the pause). `controller_raw` is the pad's
     * NES mask before that gate. */
    unsigned input_mask;
    unsigned controller_raw;

    /* C5.1 modern interface. */
    int ext_on;              /* --settings / ui enabled (OD_EXT=0 turns it off) */
    od_settings settings;
    od_ui ui;
    char settings_path[1024];
    uint32_t last_frame[OD_UI_SCREEN_W * OD_UI_SCREEN_H];
    int have_frame;
    unsigned ui_last_ticks;

    /* C5.10: pause on focus loss, and the deterministic capture instrument
     * (which pushes pointer events already in 256x240 canvas coordinates). */
    int paused_focus;        /* the window lost focus and the game is paused */
    int synth_pointer;

    /* Freezing the host clock while a modal screen is open. `base_val` and
     * `base_real` map the monotonic SDL counter onto a virtual counter that
     * stops advancing during the menu. */
    int frozen;
    uint64_t freeze_value;
    uint64_t base_val, base_real;

    /* C5.3: the live flow (for F5/F9 and the state harness) and the one-shot
     * guard for --state-load. */
    struct od_flow *flow;
    int state_loaded;
    int scene_sound_error_reported; /* live host latch, never persisted */

    /* C5.5: the editor and catalogue screens, the assets they compose with and
     * the composed level preview handed to the editor each frame. */
    od_ui_editor editor;
    od_ui_catalog catalog;
    const od_assets *assets;
    uint32_t ed_preview[OD_UI_SCREEN_W * OD_UI_SCREEN_H];
    uint32_t ed_palette[16 * 16 * 16];
    uint32_t ed_hero[16 * 16];
    int ed_palette_world;
    int ed_has_preview;
    int test_quit;            /* the nested test play asked to return */
    long test_ticks;          /* >0: deterministic test run (instrument) */
    const char *test_hash_out;/* per-tick hashes of the instrument test  */
} sdl_host;

static int ext_enabled(void)
{
    const char *e = getenv("OD_EXT");
    if (e == NULL)
        return 1;
    if (e[0] == '0' || e[0] == '\0')
        return 0;
    if ((e[0] == 'o' || e[0] == 'O') && (e[1] == 'f' || e[1] == 'F'))
        return 0;
    return 1;
}

static uint64_t host_now(sdl_host *h)
{
    if (h->frozen)
        return h->freeze_value;
    return h->base_val + ((uint64_t)SDL_GetPerformanceCounter() - h->base_real);
}

static void host_set_frozen(sdl_host *h, int on)
{
    if (on == h->frozen)
        return;
    if (on) {
        h->freeze_value = host_now(h);
        h->frozen = 1;
    } else {
        h->base_val = h->freeze_value;
        h->base_real = (uint64_t)SDL_GetPerformanceCounter();
        h->frozen = 0;
    }
}

/* C5.10: the flow is frozen while a modal screen is open (C5.1) and while the
 * window is unfocused with "pause on focus loss" on. */
static void host_apply_pause(sdl_host *h)
{
    host_set_frozen(h, od_ui_modal(&h->ui) || h->paused_focus);
}

static void host_save_settings(sdl_host *h)
{
    if (h->settings_path[0] == '\0')
        return;
    /* C6.32 (UI-16): settings and controller bindings share settings.ini.
     * Write both in ONE atomic replace: the settings text with the [input]
     * block merged in. (Two writes dropped [input] in between.) */
    char text[2048];
    int n = od_settings_format(&h->settings, text, sizeof text);
    od_input_config bindings;
    const od_input_config *cfg = &h->pads.config;
    if (h->pads.config_path[0] == '\0') {      /* pads not started here */
        od_input_config_load(&bindings, h->settings_path);
        cfg = &bindings;
    }
    if (n <= 0 || n >= (int)sizeof text || ensure_parent_dir(h->settings_path) != 0 ||
        od_input_config_save_with(cfg, h->settings_path, text) != OD_OK)
        fprintf(stderr, "ui: cannot write %s\n", h->settings_path);
    else
        fprintf(stderr, "ui: settings saved to %s\n", h->settings_path);
}

/* C6.32 (UI-9): the fullscreen setting and the window agree; F11, the pad's
 * fullscreen action and the settings row all go through here and persist. */
static void host_set_fullscreen(sdl_host *h, int on)
{
    if (h->use_renderer)
        od_renderer_set_fullscreen(&h->rend, on);
    if (h->settings.fullscreen != (on ? 1 : 0)) {
        h->settings.fullscreen = on ? 1 : 0;
        if (h->ext_on)
            host_save_settings(h);
    }
}

static void host_toggle_fullscreen(sdl_host *h)
{
    host_set_fullscreen(h, !(h->use_renderer ? od_renderer_is_fullscreen(&h->rend)
                                             : h->settings.fullscreen));
}

static void host_apply_audio(sdl_host *h)
{
    if (h->audio_ready)
        od_audio_sdl_set_gain(&h->audio,
                               h->paused_focus ? 0
                                               : od_settings_gain_permille(&h->settings));
}

/* ---- save states (C5.3) ------------------------------------------------- */

static od_state_context product_state_identity;
static int product_state_identity_ready;
static void host_state_save_slot(sdl_host *h, int slot);
static void host_state_load_slot(sdl_host *h, int slot);
static int host_scene_sound_error(sdl_host *h, const od_flow *f);
static void host_state_delete_slot(sdl_host *h, int slot);

/* Run whatever state operation a key or a click requested from the UI. */
static int host_new_game(sdl_host *h);

static void host_ui_actions(sdl_host *h)
{
    int action = h->ui.state_action;
    int slot = h->ui.state_slot;
    h->ui.state_action = OD_UI_STATE_NONE;
    if (action == OD_UI_STATE_SAVE)
        host_state_save_slot(h, slot);
    else if (action == OD_UI_STATE_LOAD)
        host_state_load_slot(h, slot);
    else if (action == OD_UI_STATE_DELETE)
        host_state_delete_slot(h, slot);
    /* C6.46: the Settings row or the first-run question asked for the launcher. */
    if (h->ui.desktop_action != OD_UI_DESKTOP_NONE) {
        int install = h->ui.desktop_action == OD_UI_DESKTOP_INSTALL;
        char text[600];
        h->ui.desktop_action = OD_UI_DESKTOP_NONE;
        int st = install ? od_desktop_install(NULL, text, sizeof text)
                         : od_desktop_uninstall(NULL, text, sizeof text);
        fprintf(stderr, "desktop: %s (status %d)\n", text, st);
        h->ui.desktop_installed = od_desktop_installed(NULL);
        od_string toast = st == OD_DESKTOP_OK
            ? (install ? OD_STR_DESKTOP_TOAST_ADDED : OD_STR_DESKTOP_TOAST_REMOVED)
            : st == OD_DESKTOP_FOREIGN ? OD_STR_DESKTOP_TOAST_FOREIGN
            : st == OD_DESKTOP_NO_ICONS ? OD_STR_DESKTOP_TOAST_NO_ICONS
            : OD_STR_DESKTOP_TOAST_ERROR;
        od_ui_toast(&h->ui, od_str(toast), 2600);
        h->ui_dirty = 1;
    }
    /* "New game" (C6.31 flow, C6.32 menu entry): the UI sets the flag only
     * after the player confirms. */
    if (h->ui.new_game) {
        h->ui.new_game = 0;
        host_new_game(h);
    }
}

static void host_screenshot(sdl_host *h);

/* Act on what a key or a click did to the UI. C6.32 (UI-2): a click on
 * "Continue" closes the menu like Esc does, so it must unfreeze the game too;
 * every path (key, pointer, pad) ends here. */
static void host_ui_after(sdl_host *h, int was)
{
    int now = od_ui_modal(&h->ui);
    h->ui_dirty = 1;

    if (!was && now)
        SDL_ResetKeyboard();      /* no game key leaks into the paused frame */
    if (was != now)
        h->input_mask = ~0u;      /* C6.31: nor the key that closes it */
    if (was != now) {
        host_apply_pause(h);
        fprintf(stderr, "ui: menu %s\n", now ? "open" : "closed");
        if (now)
            fprintf(stderr, "ui: menu items: Continue, States, Editor, Levels, "
                            "Settings, Controls, New game, Quit\n");
    }
    if (h->ui.save_settings) {
        h->ui.save_settings = 0;
        host_apply_audio(h);
        host_save_settings(h);
    }
    if (h->ui.apply_video) {
        h->ui.apply_video = 0;
        if (h->use_renderer)
            od_renderer_set_fullscreen(&h->rend, h->settings.fullscreen);
    }
    if (h->ui.quit)
        h->quit = 1;
    host_ui_actions(h);
}

/* Dispatch a logical key to the UI and act on the consequences. */
static void host_ui_key(sdl_host *h, od_ui_key key)
{
    int was = od_ui_modal(&h->ui);
    od_ui_handle_key(&h->ui, key);
    host_ui_after(h, was);
    if (key == OD_UI_KEY_MUTE)
        fprintf(stderr, "ui: mute %s (gain=%d)\n",
                h->settings.muted ? "on" : "off",
                od_settings_gain_permille(&h->settings));
}

static void host_editor_refresh(sdl_host *h);
static void host_editor_actions(sdl_host *h);
static void host_catalog_actions(sdl_host *h);
static int host_wide_screen(const sdl_host *h);
static void host_present_wide(sdl_host *h);
static int host_wide_point(sdl_host *h, int mx, int my, int *cx, int *cy);
static void host_wide_release(void);

/* Controller actions follow the keyboard's existing host policy, never NES
 * input. Sample during the event pump too: frozen modal loops have no ticks. */
static void host_controller_poll(sdl_host *h, int test)
{
    od_input_frame in={0};od_pads_poll_controllers(&h->pads,&in);
    /* C6.32 (UI-24): menus follow the pad's own accept/back positions. */
    unsigned pressed=in.ui_buttons&~h->pad_ui_previous;
    h->pad_ui_previous=in.ui_buttons;
    h->controller_ui_previous=in.nes_buttons;
    if(pressed||in.n_actions)h->ui_dirty=1;
    if(h->paused_focus){h->controller_buttons=0;return;}
    int was_modal=od_ui_modal(&h->ui);
    for(int i=0;i<in.n_actions;++i) {
        od_action a=in.actions[i];
        if(test) {
            if(a==OD_ACT_MENU)h->test_quit=1;
            else if(a==OD_ACT_FULLSCREEN&&h->use_renderer)host_toggle_fullscreen(h);
            continue;
        }
        if(a==OD_ACT_RESET) {h->reset=1;continue;}
        if(a==OD_ACT_FULLSCREEN) {if(h->use_renderer)host_toggle_fullscreen(h);continue;}
        if(a==OD_ACT_SCREENSHOT) {host_screenshot(h);continue;}
        if(!h->ext_on) {if(a==OD_ACT_MENU||a==OD_ACT_QUIT)h->quit=1;continue;}
        switch(a) {
        case OD_ACT_MENU:host_ui_key(h,OD_UI_KEY_BACK);break;
        case OD_ACT_MUTE:host_ui_key(h,OD_UI_KEY_MUTE);break;
        case OD_ACT_STATE_SAVE:
            if(h->ui.screen==OD_UI_EDITOR)host_ui_key(h,OD_UI_KEY_EDITOR_TEST);
            else host_state_save_slot(h,h->settings.states_current);
            break;
        case OD_ACT_STATE_LOAD:host_state_load_slot(h,h->settings.states_current);break;
        case OD_ACT_SLOT_PREV:case OD_ACT_SLOT_NEXT: {
            int n=h->settings.states_slots>0?h->settings.states_slots:1;
            h->settings.states_current=(h->settings.states_current+n+(a==OD_ACT_SLOT_NEXT?1:-1))%n;
            host_save_settings(h);char msg[32];snprintf(msg,sizeof msg,od_str(OD_STR_SLOT_N),h->settings.states_current+1);
            od_ui_toast(&h->ui,msg,1200);break;
        }
        case OD_ACT_QUIT:h->quit=1;break;
        default:break; /* help has no invented pad behaviour */
        }
    }
    if(!test&&h->ext_on&&was_modal&&od_ui_modal(&h->ui)) {
        if(pressed&OD_BTN_UP)host_ui_key(h,OD_UI_KEY_UP);
        if(pressed&OD_BTN_DOWN)host_ui_key(h,OD_UI_KEY_DOWN);
        if(pressed&OD_BTN_LEFT)host_ui_key(h,OD_UI_KEY_LEFT);
        if(pressed&OD_BTN_RIGHT)host_ui_key(h,OD_UI_KEY_RIGHT);
        if(pressed&OD_BTN_A)host_ui_key(h,OD_UI_KEY_ACCEPT);
        if(pressed&OD_BTN_B)host_ui_key(h,OD_UI_KEY_BACK);
    }
    h->controller_buttons=(h->paused_focus||(!test&&(was_modal||od_ui_modal(&h->ui)))||
                            (test&&h->test_quit))?0:in.nes_buttons;
    if(!test)h->controller_raw=h->paused_focus?0:in.nes_buttons;
}

static void poll_events(sdl_host *h)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        od_pads_handle_event(&h->pads,&ev);
        if (ev.type == SDL_QUIT) {
            h->quit = 1;
            continue;
        }
        if (ev.type == SDL_WINDOWEVENT)
            h->ui_dirty = 1;          /* exposed, resized, focus: redraw */
        if (ev.type == SDL_WINDOWEVENT) {
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                SDL_ResetKeyboard();
                /* C5.10: pause on focus loss. The flow
                 * stops stepping and the device is silenced, exactly like the
                 * modal menu; regaining the focus resumes. */
                if (!h->paused_focus && h->settings.pause_on_focus_loss &&
                    h->use_renderer) {
                    h->paused_focus = 1;
                    if (h->ext_on)
                        od_ui_toast(&h->ui, od_str(OD_STR_PAUSED), 0x7FFFFFFFu);
                    host_apply_pause(h);
                    host_apply_audio(h);
                    fprintf(stderr, "focus: paused (window lost focus)\n");
                }
            } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                if (h->paused_focus) {
                    h->paused_focus = 0;
                    if (h->ext_on)
                        od_ui_toast(&h->ui, "", 0);
                    host_apply_pause(h);
                    host_apply_audio(h);
                    fprintf(stderr, "focus: resumed (window focused)\n");
                }
            }
            continue;
        }
        /* C5.3/C5.5: the pointer drives the modern menu, the states screen,
         * the editor and the catalogue. */
        if (ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEBUTTONDOWN ||
            ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEWHEEL) {
            if (!h->ext_on || !h->use_renderer)
                continue;
            int mx = 0, my = 0, click = 0, wheel = 0;
            Uint32 state = 0;
            int gx = 0, gy = 0;
            if (ev.type == SDL_MOUSEWHEEL) {
                state = SDL_GetMouseState(&gx, &gy);
                mx = gx;
                my = gy;
                wheel = ev.wheel.y;
            } else if (ev.type == SDL_MOUSEMOTION) {
                mx = ev.motion.x;
                my = ev.motion.y;
                state = ev.motion.state;
            } else {
                mx = ev.button.x;
                my = ev.button.y;
                click = ev.type == SDL_MOUSEBUTTONDOWN;
                state = SDL_GetMouseState(&gx, &gy);
                if (ev.type == SDL_MOUSEBUTTONDOWN) {
                    if (ev.button.button == SDL_BUTTON_LEFT)
                        state |= SDL_BUTTON(SDL_BUTTON_LEFT);
                    else if (ev.button.button == SDL_BUTTON_MIDDLE)
                        state |= SDL_BUTTON(SDL_BUTTON_MIDDLE);
                    else if (ev.button.button == SDL_BUTTON_RIGHT)
                        state |= SDL_BUTTON(SDL_BUTTON_RIGHT);
                }
            }
            int buttons = 0;
            if (state & SDL_BUTTON(SDL_BUTTON_LEFT)) buttons |= OD_UED_BTN_LEFT;
            if (state & SDL_BUTTON(SDL_BUTTON_MIDDLE)) buttons |= OD_UED_BTN_MIDDLE;
            if (state & SDL_BUTTON(SDL_BUTTON_RIGHT)) buttons |= OD_UED_BTN_RIGHT;
            int ww = 0, wh = 0;
            if (h->rend.window)
                SDL_GetWindowSize(h->rend.window, &ww, &wh);
            /* C5.10: the presentation computes its own viewport, so a real
             * pointer event arrives in window pixels and is mapped into the
             * 256x240 canvas through the displayed rectangle (the black
             * borders are not part of the canvas). The deterministic capture
             * instrument (--ui-demo/--video-demo/--focus-demo) pushes events
             * already in canvas coordinates. */
            int cx = mx, cy = my;
            if (!h->synth_pointer && host_wide_screen(h)) {
                /* C6.24: the editor and the catalogue have their own
                 * 384x360 canvas and viewport. */
                host_wide_point(h, mx, my, &cx, &cy);
            } else if (!h->synth_pointer) {
                /* C6.32 (UI-22): viewports are in drawable pixels. */
                int px = mx, py = my;
                od_renderer_point_to_output(&h->rend, mx, my, &px, &py);
                od_video_rect vp;
                od_renderer_get_viewport(&h->rend, &vp);
                if (vp.w > 0 && vp.h > 0) {
                    cx = (px - vp.x) * OD_UI_SCREEN_W / vp.w;
                    cy = (py - vp.y) * OD_UI_SCREEN_H / vp.h;
                    if (px < vp.x) cx = -1;
                    if (py < vp.y) cy = -1;
                }
            }
            if (getenv("OD_POINTER_TRACE"))
                fprintf(stderr, "ui: pointer win=%d,%d click=%d window=%dx%d "
                                "canvas=%d,%d buttons=%d wheel=%d\n", mx, my,
                        click, ww, wh, cx, cy, buttons, wheel);
            int shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
            int ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
            int was = od_ui_modal(&h->ui);
            od_ui_pointer2(&h->ui, cx, cy, buttons, click, wheel, shift, ctrl);
            host_ui_after(h, was);
            host_editor_actions(h);
            host_catalog_actions(h);
            continue;
        }
        /* C6.32 (UI-6): the path field takes the system's text input, so
         * Shift, the keyboard layout and accented letters work. */
        if (ev.type == SDL_TEXTINPUT) {
            if (h->ext_on && h->ui.screen == OD_UI_CATALOG &&
                h->catalog.editing_path) {
                od_ucat_text(&h->catalog, ev.text.text);
                h->ui_dirty = 1;
            }
            continue;
        }
        if (ev.type != SDL_KEYDOWN || ev.key.repeat)
            continue;

        SDL_Keycode sym = ev.key.keysym.sym;
        SDL_Keymod mod = ev.key.keysym.mod;

        /* C5.5: the editor and catalogue reuse the key path but with their own
         * bindings (Ctrl+Z/Y/S, F5 test, P palette). */
        if (od_ui_modal(&h->ui) &&
            (h->ui.screen == OD_UI_EDITOR || h->ui.screen == OD_UI_CATALOG)) {
            if (sym == SDLK_ESCAPE) {
                host_ui_key(h, OD_UI_KEY_BACK);
            } else if (h->ui.screen == OD_UI_EDITOR) {
                if ((mod & KMOD_CTRL) && sym == SDLK_z)
                    host_ui_key(h, OD_UI_KEY_UNDO);
                else if ((mod & KMOD_CTRL) && sym == SDLK_y)
                    host_ui_key(h, OD_UI_KEY_REDO);
                else if ((mod & KMOD_CTRL) && sym == SDLK_s)
                    host_ui_key(h, OD_UI_KEY_EDITOR_SAVE);
                else if (sym == SDLK_F5)
                    host_ui_key(h, OD_UI_KEY_EDITOR_TEST);
                else if (sym == SDLK_p)
                    host_ui_key(h, OD_UI_KEY_PALETTE);
                else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER)
                    host_ui_key(h, OD_UI_KEY_ACCEPT);
                else if (sym == SDLK_F12)
                    host_screenshot(h);
            } else { /* catalogue */
                od_ui_catalog *c = &h->catalog;
                if (c->editing_path) {
                    /* Characters arrive as SDL_TEXTINPUT (above). */
                    if (sym == SDLK_BACKSPACE) {
                        od_ucat_key(c, OD_UCAT_KEY_BACKSPACE, 0);
                    } else if ((mod & KMOD_CTRL) && sym == SDLK_v) {
                        char *clip = SDL_GetClipboardText();
                        if (clip != NULL) {
                            od_ucat_text(c, clip);
                            SDL_free(clip);
                        }
                    } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                        c->editing_path = 0;
                    }
                } else if (sym == SDLK_F12) {
                    host_screenshot(h);
                } else if (sym == SDLK_UP || sym == SDLK_w) {
                    host_ui_key(h, OD_UI_KEY_UP);
                } else if (sym == SDLK_DOWN || sym == SDLK_s) {
                    host_ui_key(h, OD_UI_KEY_DOWN);
                } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER ||
                           sym == SDLK_SPACE || sym == SDLK_z) {
                    host_ui_key(h, OD_UI_KEY_ACCEPT);
                } else if (sym == SDLK_DELETE) {
                    host_ui_key(h, OD_UI_KEY_DELETE);
                }
            }
            h->ui_dirty = 1;
            host_editor_actions(h);
            host_catalog_actions(h);
            continue;
        }

        if (sym == SDLK_F11) {
            host_toggle_fullscreen(h);
            continue;
        }
        if (sym == SDLK_F2) {
            h->reset = 1;   /* the console's RESET button (flow_soft_reset) */
            continue;
        }
        if (!h->ext_on) {
            if (sym == SDLK_ESCAPE)
                h->quit = 1;
            continue;
        }

        /* C5.3: F5 saves, F9 loads and F6/F7 change the slot.
         * They work with the game running or with a screen open. */
        if (sym == SDLK_F5) {
            host_state_save_slot(h, h->settings.states_current);
        } else if (sym == SDLK_F9) {
            host_state_load_slot(h, h->settings.states_current);
        } else if (sym == SDLK_F6 || sym == SDLK_F7) {
            int n = h->settings.states_slots > 0 ? h->settings.states_slots : 1;
            int cur = h->settings.states_current;
            cur += (sym == SDLK_F7) ? 1 : -1;
            if (cur < 0) cur = n - 1;
            if (cur >= n) cur = 0;
            h->settings.states_current = cur;
            /* C6.31 (CORE-10): written now, like the pad's slot keys; the
             * flag alone waited for some later UI key and was often lost. */
            host_save_settings(h);
            char msg[32];
            snprintf(msg, sizeof msg, od_str(OD_STR_SLOT_N), cur + 1);
            od_ui_toast(&h->ui, msg, 1200);
            h->ui_dirty = 1;
        } else if (sym == SDLK_F12) {
            host_screenshot(h);               /* C6.32 (UI-17) */
        } else if (sym == SDLK_F1) {
            host_ui_key(h, OD_UI_KEY_CONTROLS);   /* C6.40 */
        }

        /* Modern interface keys. ESC opens/closes the menu (D1); M toggles
         * mute anywhere; the rest navigate while a modal screen is open. */
        if (sym == SDLK_ESCAPE) {
            host_ui_key(h, OD_UI_KEY_BACK);
        } else if (sym == SDLK_m) {
            host_ui_key(h, OD_UI_KEY_MUTE);
        } else if (od_ui_modal(&h->ui)) {
            if (sym == SDLK_UP || sym == SDLK_w)
                host_ui_key(h, OD_UI_KEY_UP);
            else if (sym == SDLK_DOWN || sym == SDLK_s)
                host_ui_key(h, OD_UI_KEY_DOWN);
            else if (sym == SDLK_LEFT || sym == SDLK_a)
                host_ui_key(h, OD_UI_KEY_LEFT);
            else if (sym == SDLK_RIGHT || sym == SDLK_d)
                host_ui_key(h, OD_UI_KEY_RIGHT);
            else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER ||
                     sym == SDLK_SPACE || sym == SDLK_z)
                host_ui_key(h, OD_UI_KEY_ACCEPT);
            else if (sym == SDLK_DELETE)
                host_ui_key(h, OD_UI_KEY_DELETE);
            else if (sym == SDLK_x)
                host_ui_key(h, OD_UI_KEY_BACK);
            /* any other key is swallowed by the modal menu */
        }
    }
    host_controller_poll(h,0);
    /* C6.31 (CORE-4): while a modal screen is open or the window is paused,
     * every held button is masked until it is released after the close. */
    if (od_ui_modal(&h->ui) || h->paused_focus)
        h->input_mask = ~0u;
}

#ifdef OD_DEV_TOOLS
/* Inject a key press into the SDL event queue. Used only by the C5.1 capture
 * instrument, so the ESC that opens the menu travels the same SDL_PollEvent
 * path as a real keyboard in the product. */
static void push_key(SDL_Keycode sym, SDL_Scancode sc)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN;
    e.key.type = SDL_KEYDOWN;
    e.key.state = SDL_PRESSED;
    e.key.repeat = 0;
    e.key.keysym.sym = sym;
    e.key.keysym.scancode = sc;
    SDL_PushEvent(&e);
}

/* C5.3: inject a pointer event (window coordinates) so the states/menu mouse
 * path travels the same SDL_PollEvent queue as a real mouse. */
static void push_mouse(int x, int y, int click)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (click) {
        e.type = SDL_MOUSEBUTTONDOWN;
        e.button.type = SDL_MOUSEBUTTONDOWN;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.state = SDL_PRESSED;
        e.button.x = x;
        e.button.y = y;
    } else {
        e.type = SDL_MOUSEMOTION;
        e.motion.type = SDL_MOUSEMOTION;
        e.motion.x = x;
        e.motion.y = y;
    }
    SDL_PushEvent(&e);
}

/* C5.10: inject a window focus event so the pause-on-focus path travels the
 * same SDL_PollEvent queue as the real window manager. */
static void push_focus(int gained)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_WINDOWEVENT;
    e.window.type = SDL_WINDOWEVENT;
    e.window.event = gained ? SDL_WINDOWEVENT_FOCUS_GAINED
                            : SDL_WINDOWEVENT_FOCUS_LOST;
    SDL_PushEvent(&e);
}
#endif /* OD_DEV_TOOLS */

static uint64_t sdl_counter(void *ctx)
{
    return host_now((sdl_host *)ctx);
}
static uint64_t sdl_frequency(void *ctx) { (void)ctx; return (uint64_t)SDL_GetPerformanceFrequency(); }

static void sdl_sleep(void *ctx, unsigned ms)
{
    sdl_host *h = ctx;
    /* With the menu open the flow advances no ticks, so this is where the
     * frozen frame (plus overlay) is refreshed and input is still read. */
    poll_events(h);
    /* C6.38: while the menu freezes the clock the flow step loop never runs
     * (no tick is due), so a quit raised by Quit or by closing the window
     * would wait for the next event to unfreeze it. End the run right here. */
    if (h->quit && h->flow)
        h->flow->running = 0;
    if (h->frozen && h->use_renderer && h->have_frame) {
        Uint32 now = SDL_GetTicks();
        od_ui_tick(&h->ui, now - h->ui_last_ticks);
        h->ui_last_ticks = now;
        /* C6.32 (UI-27): a frozen screen only changes with input, a toast
         * coming or going, or a window event; otherwise it is not presented
         * again (it used to be every ~18 ms, a whole core with llvmpipe). A
         * slow refresh every second stays as a safety net. */
        int toast = h->ui.toast_ms > 0;
        if (toast != h->ui_toast_shown) {
            h->ui_toast_shown = toast;
            h->ui_dirty = 1;
        }
        if (!h->ui_dirty && now - h->ui_last_present < 1000) {
            host_editor_actions(h);
            host_catalog_actions(h);
            SDL_Delay(ms);
            return;
        }
        h->ui_dirty = 0;
        h->ui_last_present = now;
        host_editor_refresh(h);
        od_ui_draw(&h->ui);
        od_renderer_set_video(&h->rend, h->settings.integer_scale,
                               h->settings.aspect_87);
        if (host_wide_screen(h))
            host_present_wide(h);
        else
            od_renderer_present_overlay(&h->rend, h->last_frame,
                h->ext_on && h->ui.canvas_active ? h->ui.canvas : NULL);
        host_editor_actions(h);
        host_catalog_actions(h);
    }
    SDL_Delay(ms);
}

static void sdl_buttons(void *ctx, long tick, unsigned *out, int *quit)
{
    sdl_host *h = ctx;
    (void)tick;
    poll_events(h);
    unsigned buttons = h->controller_raw;
    /* Read the keyboard even when the window is not focused: raising is not
     * reliable under every window manager, and an unfocused game that ignores
     * every key looks frozen. Stuck keys are avoided by the FOCUS_LOST reset.
     * While the modern menu is open the game keys do not reach the simulation
     * (the flow is frozen anyway), and after it closes the keys still held
     * from it stay masked until released (C6.31, CORE-4/UI-4). */
    if (h->use_renderer && !h->paused_focus) {
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        if (keys[SDL_SCANCODE_LEFT])  buttons |= OD_BTN_LEFT;
        if (keys[SDL_SCANCODE_RIGHT]) buttons |= OD_BTN_RIGHT;
        if (keys[SDL_SCANCODE_UP])    buttons |= OD_BTN_UP;
        if (keys[SDL_SCANCODE_DOWN])  buttons |= OD_BTN_DOWN;
        if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_SPACE]) buttons |= OD_BTN_A;
        if (keys[SDL_SCANCODE_X])     buttons |= OD_BTN_B;
        if (keys[SDL_SCANCODE_RETURN]) buttons |= OD_BTN_START;
        if (keys[SDL_SCANCODE_RSHIFT]) buttons |= OD_BTN_SELECT;
    }
    if (h->paused_focus || od_ui_modal(&h->ui)) {
        h->input_mask = ~0u;
        buttons = 0;
    } else {
        h->input_mask &= buttons;          /* released keys leave the mask */
        buttons &= ~h->input_mask;
    }
    *out = buttons;
    *quit = h->quit;
}

/* Console RESET (F2): edge-triggered, consumed by the flow. */
static int sdl_reset_poll(void *ctx)
{
    sdl_host *h = ctx;
    int r = h->reset;
    h->reset = 0;
#ifdef OD_DEV_TOOLS
    if (h->o && h->o->reset_at > 0 && h->flow && h->flow->sim_tick == h->o->reset_at)
        r = 1;   /* C6.31 instrument: F2 at a tick */
#endif
    if (r && h->flow) {
        fprintf(stderr, "reset: console RESET at tick %ld\n", h->flow->sim_tick);
        h->input_mask = ~0u;
    }
    return r;
}

static int sdl_renderer_init(void *ctx, int scale, int headless, int vsync)
{
    sdl_host *h = ctx;
    h->use_renderer = 1;
    h->ui_last_ticks = SDL_GetTicks();
    int r = od_renderer_init(&h->rend, scale, headless, vsync);
    if(r==0) {
        char mapping[1200];snprintf(mapping,sizeof mapping,"%s",h->settings_path);
        char *slash=od_path_last_sep(mapping);if(slash)snprintf(slash+1,sizeof mapping-(size_t)(slash+1-mapping),"gamecontrollerdb.txt");
        else snprintf(mapping,sizeof mapping,"gamecontrollerdb.txt");
        od_pads_init(&h->pads,h->settings_path[0]?h->settings_path:NULL,mapping);
    }
    /* C5.10: the presentation options come from the settings. */
    od_renderer_set_video(&h->rend, h->settings.integer_scale,
                           h->settings.aspect_87);
    /* Anchor the virtual host clock after SDL is up (the menu freezes it). */
    h->base_real = (uint64_t)SDL_GetPerformanceCounter();
    h->base_val = h->base_real;
    h->frozen = 0;
    return r;
}
static void sdl_renderer_shutdown(void *ctx)
{
    sdl_host *h = ctx;
    od_pads_close(&h->pads);
    if (h->use_renderer) {
        host_wide_release();       /* C6.24 editor/test-play textures      */
        od_ui_shutdown(&h->ui);   /* the overlay texture owns the renderer */
        od_renderer_shutdown(&h->rend);
    }
}
static int sdl_renderer_has_vsync(void *ctx)
{
    sdl_host *h = ctx;
    return od_renderer_has_vsync(&h->rend);
}
static void sdl_present(void *ctx, const od_flow *f, const uint32_t *argb)
{
    sdl_host *h = ctx;
    if (host_scene_sound_error(h,f)) return;
    if (!h->use_renderer)
        return;
    /* C6.41: OD_FRAME_TRACE=1 prints the wall time between two presented
     * frames (host stalls); measurement only, it changes nothing. */
    {
        static int trace = -1;
        static uint64_t last;
        if (trace < 0)
            trace = getenv("OD_FRAME_TRACE") != NULL;
        if (trace) {
            uint64_t t = (uint64_t)SDL_GetPerformanceCounter();
            if (last)
                fprintf(stderr, "frame-trace: %ld %.3f\n", f->sim_tick,
                        (double)(t - last) * 1000.0 /
                            (double)SDL_GetPerformanceFrequency());
            last = t;
        }
    }
    memcpy(h->last_frame, argb, sizeof h->last_frame);
    h->have_frame = 1;
    if (h->ext_on) {
        Uint32 now = SDL_GetTicks();
        /* C6.35: in the deterministic --ticks batch UI timers (toasts) run on
         * simulated time, 20 ms a tick, not on how fast the host executes. */
        Uint32 dt = (h->o != NULL && h->o->ticks > 0) ? 20u : now - h->ui_last_ticks;
        od_ui_tick(&h->ui, dt);
        h->ui_last_ticks = now;
        od_ui_draw(&h->ui);
    }
    /* C5.10: the options are applied every frame (they can change live in the
     * settings screen) and the overlay shares the game viewport. */
    od_renderer_set_video(&h->rend, h->settings.integer_scale,
                           h->settings.aspect_87);
    if (host_wide_screen(h))      /* C6.24: the editor/catalogue canvas */
        host_present_wide(h);
    else
        od_renderer_present_overlay(&h->rend, argb,
            h->ext_on && h->ui.canvas_active ? h->ui.canvas : NULL);
}

/* Blend an ARGB overlay (alpha 0 = transparent) over an opaque ARGB frame. */
static uint32_t argb_over(uint32_t src, uint32_t dst)
{
    unsigned a = (src >> 24) & 0xffu;
    if (a == 0)
        return dst;
    if (a == 255)
        return src;
    unsigned sr = (src >> 16) & 0xffu, sg = (src >> 8) & 0xffu, sb = src & 0xffu;
    unsigned dr = (dst >> 16) & 0xffu, dg = (dst >> 8) & 0xffu, db = dst & 0xffu;
    unsigned r = (sr * a + dr * (255u - a)) / 255u;
    unsigned g = (sg * a + dg * (255u - a)) / 255u;
    unsigned b = (sb * a + db * (255u - a)) / 255u;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

/* Screenshot including the modern overlay (alpha over the NES frame). */
static int write_screenshot_overlay(const char *path, const od_index_frame *fb,
                                    unsigned fade, const uint32_t *overlay)
{
    static uint32_t argb[OD_SCREEN_W * OD_SCREEN_H];
    static uint8_t rgb[OD_SCREEN_W * OD_SCREEN_H * 3];
    od_frame_to_argb(fb, &od_master_palette_pal, argb);
    unsigned keep = fade ? 256u - fade : 256u;
    for (int i = 0; i < OD_SCREEN_W * OD_SCREEN_H; i++) {
        uint32_t c = argb[i];
        if (overlay)
            c = argb_over(overlay[i], c);
        unsigned r = ((c >> 16) & 0xffu) * keep / 256u;
        unsigned g = ((c >> 8) & 0xffu) * keep / 256u;
        unsigned b = (c & 0xffu) * keep / 256u;
        rgb[i * 3 + 0] = (uint8_t)r;
        rgb[i * 3 + 1] = (uint8_t)g;
        rgb[i * 3 + 2] = (uint8_t)b;
    }
    if (od_png_write_rgb(path, OD_SCREEN_W, OD_SCREEN_H, rgb) != 0) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    printf("wrote %s (%dx%d)\n", path, OD_SCREEN_W, OD_SCREEN_H);
    return 0;
}

/* ---- save states (C5.3) ------------------------------------------------- */

static int write_argb_png(const char *path, const uint32_t *argb)
{
    static uint8_t rgb[OD_SCREEN_W * OD_SCREEN_H * 3];
    for (int i = 0; i < OD_SCREEN_W * OD_SCREEN_H; i++) {
        uint32_t c = argb[i];
        rgb[i * 3 + 0] = (uint8_t)((c >> 16) & 0xffu);
        rgb[i * 3 + 1] = (uint8_t)((c >> 8) & 0xffu);
        rgb[i * 3 + 2] = (uint8_t)(c & 0xffu);
    }
    return od_png_write_rgb(path, OD_SCREEN_W, OD_SCREEN_H, rgb);
}

static void state_thumb_path(const char *state_path, char *out, size_t cap)
{
    const char *dot = strrchr(state_path, '.');
    if (dot != NULL && (size_t)(dot - state_path) + 5 < cap)
        snprintf(out, cap, "%.*s.png", (int)(dot - state_path), state_path);
    else
        snprintf(out, cap, "%s.png", state_path);
}

/* Every UI/developer entry point uses the same selected audio backend.
 * Identity is usable only when the content owner supplies loader snapshot
 * provenance; rereading disk after assets_load is not such proof. */
static int product_state_snapshot_identity(const od_content_vfs *vfs)
{
    product_state_identity_ready = 0; /* never retain a previous session's proof */
#if defined(OD_CONTENT_SNAPSHOT_API) && OD_CONTENT_SNAPSHOT_API == 1
    /* Proposed C6.2 contract: all loaders, including later/lazy scene/audio
     * loads, share immutable captured bytes; digest covers that full snapshot.
     * A paths-only inventory macro is deliberately insufficient here. */
    if (od_state_vfs_digest(vfs,product_state_identity.content) != OD_OK) return 0;
    /* C6.62: without them a state still loads, only never across content changes. */
    (void)od_state_rom_identity(vfs,product_state_identity.rom,product_state_identity.mods);
    return 1;
#else
    (void)vfs;
    return 0;
#endif
}

static void product_content_release(od_assets *assets,od_content_vfs *vfs)
{
    product_state_identity_ready = 0;
    od_assets_free(assets);
#if defined(OD_CONTENT_SNAPSHOT_API) && OD_CONTENT_SNAPSHOT_API == 1
    od_content_vfs_free(vfs); /* release the owner, not gfx's retained borrow */
#else
    (void)vfs;
#endif
}

static int host_state_available(sdl_host *h)
{
    if (h->flow && h->flow->scene_sound_status != OD_OK) {
        fprintf(stderr,"state: save/load unavailable; scene audio failed (status %d)\n",
                (int)h->flow->scene_sound_status);
        return 0;
    }
    if (product_state_identity_ready && h->flow) return 1;
    fprintf(stderr,"state: save/load unavailable; loaded content identity is not verified\n");
    return 0;
}
static od_status host_state_write(sdl_host *h, const char *path)
{
    if (!host_state_available(h)) return OD_ERR_FORMAT;
    od_state_context ctx = product_state_identity;
    ctx.screen_owner = &h->flow->screens;
    if (h->flow->audio_device)
        return od_audio_sdl_state_save(&h->audio,h->flow,path,&ctx);
    ctx.audio = &h->flow->audio_core;
    return od_state_save_ex(h->flow,path,&ctx);
}
static od_status host_state_read(sdl_host *h, const char *path)
{
    if (!host_state_available(h)) return OD_ERR_FORMAT;
    od_state_context ctx = product_state_identity;
    ctx.screen_owner = &h->flow->screens;
    if (h->flow->audio_device)
        return od_audio_sdl_state_load(&h->audio,h->flow,path,&ctx);
    ctx.audio = &h->flow->audio_core;
    return od_state_load_ex(h->flow,path,&ctx);
}

static void host_state_save_slot(sdl_host *h, int slot)
{
    if (h->flow == NULL)
        return;
    if (!host_state_available(h)) {
        od_ui_states_status(&h->ui, od_str(h->flow->scene_sound_status != OD_OK ?
            OD_STR_STATES_UNAVAILABLE_AUDIO : OD_STR_STATES_UNAVAILABLE_CONTENT), 1600);
        return;
    }
    char path[1200];
    od_state_default_path(path, sizeof path, slot + 1);
    if (ensure_parent_dir(path) != 0 || host_state_write(h, path) != OD_OK) {
        fprintf(stderr, "state: cannot save %s\n", path);
        od_ui_states_status(&h->ui, od_str(OD_STR_STATE_ERROR), 1600);
        return;
    }
    fprintf(stderr, "state: saved %s (tick %ld)\n", path, h->flow->sim_tick);
    od_ui_states_status(&h->ui, od_str(OD_STR_STATE_SAVED), 1400);
    char thumb[1300];
    state_thumb_path(path, thumb, sizeof thumb);
    od_userdir_remove(thumb);   /* never write through a link left at slot-N.png */
    if (h->have_frame)
        write_argb_png(thumb, h->last_frame);
    else {
        static uint32_t argb[OD_SCREEN_W * OD_SCREEN_H];
        od_frame_to_argb(&h->flow->fb, &od_master_palette_pal, argb);
        write_argb_png(thumb, argb);
    }
    if (h->ui.screen == OD_UI_STATES)
        od_ui_states_refresh(&h->ui, slot);
}

static void host_state_load_slot(sdl_host *h, int slot)
{
    if (h->flow == NULL)
        return;
    if (!host_state_available(h)) {
        od_ui_states_status(&h->ui, od_str(h->flow->scene_sound_status != OD_OK ?
            OD_STR_STATES_UNAVAILABLE_AUDIO : OD_STR_STATES_UNAVAILABLE_CONTENT), 1600);
        return;
    }
    char path[1200];
    od_state_default_path(path, sizeof path, slot + 1);
    if (host_state_read(h, path) != OD_OK) {
        fprintf(stderr, "state: rejected %s\n", path);
        od_state_reject why = od_state_last_reject();
        od_ui_states_status(&h->ui, od_str(why == OD_STATE_REJECT_CONTENT ? OD_STR_STATE_BAD_CONTENT :
            why == OD_STATE_REJECT_FORMAT ? OD_STR_STATE_BAD_FORMAT : OD_STR_STATE_ERROR), 3200);
        return;
    }
    fprintf(stderr, "state: loaded %s (tick %ld)\n", path, h->flow->sim_tick);
    od_ui_states_status(&h->ui, od_str(OD_STR_STATE_LOADED), 1400);
    if (h->ui.screen == OD_UI_STATES)
        od_ui_states_refresh(&h->ui, slot);
}

static void host_state_delete_slot(sdl_host *h, int slot)
{
    char path[1200], thumb[1300];
    od_state_default_path(path, sizeof path, slot + 1);
    state_thumb_path(path, thumb, sizeof thumb);
    od_userdir_remove(path);
    od_userdir_remove(thumb);
    od_ui_states_status(&h->ui, od_str(OD_STR_STATE_REMOVED), 1200);
    if (h->ui.screen == OD_UI_STATES)
        od_ui_states_refresh(&h->ui, slot);
}

/* ---- level editor and catalogue (C5.5) --------------------------------- */

static uint64_t sdl_counter(void *ctx);
static uint64_t sdl_frequency(void *ctx);
static int sdl_reset_poll(void *ctx);
static void poll_events(sdl_host *h);
static int sdl_renderer_init(void *ctx, int scale, int headless, int vsync);
static void sdl_renderer_shutdown(void *ctx);

/* Compose the document with the core compositor into `ed_preview`. */
static void host_editor_compose(sdl_host *h)
{
    if (h->assets == NULL)
        return;
    static uint8_t cmd[OD_LED_CMD_MAX];
    od_level lv;
    if (od_led_to_level(&h->editor.doc, &lv, cmd, sizeof cmd) != OD_OK)
        return;
    int world = h->editor.doc.theme_world;
    if (world < 1) world = 1;
    if (world > 15) world = 15;
    int level_index = (world - 1) * 10;
    od_index_frame fb;
    od_render_level_index(&fb, &lv, level_index, h->assets->chr,
                           &h->assets->palette_ram);
    od_frame_to_argb(&fb, &od_master_palette_pal, h->ed_preview);
    h->ed_has_preview = 1;
    if (!od_led_has_hero(&h->editor.doc)) {
        /* C6.24: the compositor always draws Hero; with no start cell (0xFF)
         * she landed at x 241, y 232 over the bottom-right cell, now visible.
         * Take that corner from a render with her at (0,0) instead. */
        static uint32_t clean[OD_SCREEN_W * OD_SCREEN_H];
        lv.player_position = 0x00;
        od_render_level_index(&fb, &lv, level_index, h->assets->chr,
                               &h->assets->palette_ram);
        od_frame_to_argb(&fb, &od_master_palette_pal, clean);
        for (int y = 14 * 16; y < OD_SCREEN_H; y++)
            memcpy(&h->ed_preview[(size_t)y * OD_SCREEN_W + 15 * 16],
                   &clean[(size_t)y * OD_SCREEN_W + 15 * 16],
                   16 * sizeof clean[0]);
    }

    /* Per-cell-type swatches: a scratch level with type t in column t, row 1,
     * composed with the same compositor (real game graphics, not invented).
     * Hero stands in column 15, rows 3-4, for her own swatch. */
    if (h->ed_palette_world != world) {
        od_level pl;
        memset(&pl, 0, sizeof pl);
        pl.player_position = (uint8_t)((4 << 4) | 15);
        for (int t = 0; t < 16; t++)
            pl.grid[1][t] = (uint8_t)t;
        od_render_level_index(&fb, &pl, level_index, h->assets->chr,
                               &h->assets->palette_ram);
        static uint32_t pargb[OD_SCREEN_W * OD_SCREEN_H];
        od_frame_to_argb(&fb, &od_master_palette_pal, pargb);
        for (int t = 0; t < 16; t++)
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++)
                    h->ed_palette[((size_t)t * 16 + y) * 16 + x] =
                        pargb[(size_t)(y + 16) * OD_SCREEN_W + (x + t * 16)];
        /* The sprite is 16x24 from y = 5*16-24 = 56: its upper 16 rows (hat
         * and face), so the swatch exists before Hero is placed. */
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                h->ed_hero[(size_t)y * 16 + x] =
                    pargb[(size_t)(56 + y) * OD_SCREEN_W + (15 * 16 + x)];
        h->ed_palette_world = world;
    }
}

static void host_editor_refresh(sdl_host *h)
{
    if (h->ui.screen != OD_UI_EDITOR || h->assets == NULL)
        return;
    host_editor_compose(h);
    h->editor.preview = h->ed_preview;
    h->editor.preview_w = OD_UI_SCREEN_W;
    h->editor.preview_h = OD_UI_SCREEN_H;
    h->editor.palette_swatch = h->ed_palette;
    h->editor.hero_swatch = h->ed_hero;
}

/* ---- C6.24: presentation of the editor, the catalogue and Test --------- */

_Static_assert(OD_UCAT_W == OD_UED_W && OD_UCAT_H == OD_UED_H,
               "the editor and the catalogue share one canvas");

/* A host-owned canvas and its streaming texture. The texture belongs to the
 * renderer it was made on; host_wide_release forgets it before shutdown. */
typedef struct {
    SDL_Texture *tex;
    SDL_Renderer *owner;
    int w, h;
} host_canvas;
static uint32_t g_wide[OD_UED_W * OD_UED_H];
static uint32_t g_test_view[OD_UED_TEST_W * OD_UED_TEST_H];
static host_canvas g_wide_tex = { NULL, NULL, OD_UED_W, OD_UED_H };
static host_canvas g_test_tex = { NULL, NULL, OD_UED_TEST_W, OD_UED_TEST_H };

static void host_wide_release(void)
{
    if (g_wide_tex.tex) SDL_DestroyTexture(g_wide_tex.tex);
    if (g_test_tex.tex) SDL_DestroyTexture(g_test_tex.tex);
    g_wide_tex.tex = g_test_tex.tex = NULL;
    g_wide_tex.owner = g_test_tex.owner = NULL;
}

static int host_wide_screen(const sdl_host *h)
{
    return h->ext_on &&
           (h->ui.screen == OD_UI_EDITOR || h->ui.screen == OD_UI_CATALOG);
}

static void host_canvas_viewport(sdl_host *h, const host_canvas *c, int aspect,
                                 od_ued_rect *vp)
{
    int ww = c->w, wh = c->h;
    if (h->rend.window)
        od_renderer_output_size(&h->rend, &ww, &wh);   /* drawable pixels */
    od_ued_fit(ww, wh, c->w, c->h, h->settings.integer_scale, aspect, vp);
}

/* Window pixel -> canvas pixel; -1 outside the presented rectangle. */
static int host_wide_point(sdl_host *h, int mx, int my, int *cx, int *cy)
{
    od_ued_rect vp;
    host_canvas_viewport(h, &g_wide_tex, h->settings.aspect_87, &vp);
    od_renderer_point_to_output(&h->rend, mx, my, &mx, &my);
    int in = mx >= vp.x && my >= vp.y && mx < vp.x + vp.w && my < vp.y + vp.h;
    *cx = in ? (mx - vp.x) * OD_UED_W / vp.w : -1;
    *cy = in ? (my - vp.y) * OD_UED_H / vp.h : -1;
    return in;
}

static void host_present_canvas(sdl_host *h, host_canvas *c, const uint32_t *px,
                                int aspect)
{
    od_renderer *r = &h->rend;
    if (r->renderer == NULL)
        return;
    if (c->tex == NULL || c->owner != r->renderer) {
        c->tex = SDL_CreateTexture(r->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, c->w, c->h);
        c->owner = r->renderer;
        if (c->tex == NULL)
            return;
    }
    if (SDL_UpdateTexture(c->tex, NULL, px, c->w * (int)sizeof(uint32_t)) != 0)
        return;
    od_ued_rect vp;
    host_canvas_viewport(h, c, aspect, &vp);
    SDL_Rect dst = { vp.x, vp.y, vp.w, vp.h };
    SDL_SetRenderDrawColor(r->renderer, 0, 0, 0, 255);
    SDL_RenderClear(r->renderer);
    SDL_RenderCopy(r->renderer, c->tex, NULL, &dst);
    od_renderer_cursor_update(r);
    SDL_RenderPresent(r->renderer);
}

static void host_draw_wide(sdl_host *h)
{
    if (h->ui.screen == OD_UI_CATALOG)
        od_ucat_draw(&h->catalog, g_wide, OD_UED_W, OD_UED_H);
    else
        od_ued_draw(&h->editor, g_wide, OD_UED_W, OD_UED_H);
    /* A toast (focus pause, a pad's mute or slot) over the bottom edge. */
    if (h->ui.toast_ms > 0 && h->ui.toast[0] != '\0') {
        char t[64];
        od_ui_font_fit(h->ui.toast, OD_UED_W - 24, 1, t, sizeof t);
        int w = od_ui_font_width(t, 1) + 16, x = (OD_UED_W - w) / 2;
        int y = OD_UED_H - 22;
        for (int py = y; py < y + 18; py++)
            for (int px = x; px < x + w; px++)
                g_wide[(size_t)py * OD_UED_W + px] =
                    (py == y || py == y + 17 || px == x || px == x + w - 1)
                        ? 0xFFB0B0D0u : 0xFF303040u;
        od_ui_font_text(g_wide, OD_UED_W, OD_UED_H, x + 8, y + 5, t,
                         0xFFFFFFFFu, 1);
    }
}

static void host_present_wide(sdl_host *h)
{
    host_draw_wide(h);
    host_present_canvas(h, &g_wide_tex, g_wide, h->settings.aspect_87);
}

/* C6.32 (UI-17): F12 writes what is on screen to
 * <data>/opendana/screenshots/opendana-YYYYMMDD-HHMMSS[-n].png: the game frame
 * (without the menu over it), or the editor/catalogue canvas. A new name is
 * taken every time; nothing is overwritten. */
static void host_screenshot(sdl_host *h)
{
    char dir[1100];
    od_ucat_default_dir(dir, sizeof dir);              /* .../levels */
    char *slash = od_path_last_sep(dir);
    if (slash != NULL)
        *slash = '\0';
    else
        snprintf(dir, sizeof dir, ".");
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    char stamp[32] = "screenshot";
    if (tmv != NULL)
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tmv);
    char path[1300];
    int ok = 0;
    for (int k = 1; k < 100; k++) {
        if (k == 1)
            snprintf(path, sizeof path, "%s/screenshots/opendana-%s.png", dir, stamp);
        else
            snprintf(path, sizeof path, "%s/screenshots/opendana-%s-%d.png", dir, stamp, k);
        FILE *probe = od_userdir_fopen(path, "rb");
        if (probe == NULL) {
            ok = 1;
            break;
        }
        fclose(probe);
    }
    int wide = host_wide_screen(h);
    const uint32_t *px = wide ? g_wide : h->last_frame;
    int w = wide ? OD_UED_W : OD_SCREEN_W, hh = wide ? OD_UED_H : OD_SCREEN_H;
    static unsigned char rgb[OD_UED_W * OD_UED_H * 3];
    for (int i = 0; i < w * hh; i++) {
        rgb[3 * i] = (unsigned char)(px[i] >> 16);
        rgb[3 * i + 1] = (unsigned char)(px[i] >> 8);
        rgb[3 * i + 2] = (unsigned char)px[i];
    }
    if (ok && (wide || h->have_frame) && ensure_parent_dir(path) == 0 &&
        od_png_write_rgb(path, w, hh, rgb) == 0) {
        fprintf(stderr, "screenshot: %s\n", path);
        od_ui_toast(&h->ui, od_str(OD_STR_SCREENSHOT_SAVED), 1400);
    } else {
        fprintf(stderr, "screenshot: cannot write %s\n", path);
        od_ui_toast(&h->ui, od_str(OD_STR_SCREENSHOT_ERROR), 1600);
    }
    h->ui_dirty = 1;
}

/* ---- nested "Test" play: the document through the game's own path ------ */

static od_flow g_test_flow;
static uint8_t g_test_cmd[OD_LED_CMD_MAX];

static void test_poll_events(sdl_host *h)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        od_pads_handle_event(&h->pads,&ev);
        if (ev.type == SDL_QUIT) {
            h->test_quit = 1;
            h->quit = 1;
        } else if (ev.type == SDL_WINDOWEVENT &&
                   ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            /* C6.32 (UI-21): Test pauses on focus loss like the game. */
            if (h->settings.pause_on_focus_loss && !h->test_paused) {
                h->test_paused = 1;
                host_set_frozen(h, 1);
                fprintf(stderr, "focus: test paused (window lost focus)\n");
            }
        } else if (ev.type == SDL_WINDOWEVENT &&
                   ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            if (h->test_paused) {
                h->test_paused = 0;
                host_set_frozen(h, 0);
                fprintf(stderr, "focus: test resumed\n");
            }
        } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
            SDL_Keycode sym = ev.key.keysym.sym;
            if (sym == SDLK_ESCAPE)
                h->test_quit = 1;      /* ESC returns to the editor */
            else if (sym == SDLK_F11)
                host_toggle_fullscreen(h);
            else if (sym == SDLK_F12)
                host_screenshot(h);
        }
    }
    host_controller_poll(h,1);
}

static void test_buttons(void *ctx, long tick, unsigned *out, int *quit)
{
    sdl_host *h = ctx;
    (void)tick;
    test_poll_events(h);
    unsigned buttons = h->controller_buttons;
    if (!h->test_quit) {
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        if (keys[SDL_SCANCODE_LEFT])  buttons |= OD_BTN_LEFT;
        if (keys[SDL_SCANCODE_RIGHT]) buttons |= OD_BTN_RIGHT;
        if (keys[SDL_SCANCODE_UP])    buttons |= OD_BTN_UP;
        if (keys[SDL_SCANCODE_DOWN])  buttons |= OD_BTN_DOWN;
        if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_SPACE]) buttons |= OD_BTN_A;
        if (keys[SDL_SCANCODE_X])     buttons |= OD_BTN_B;
        if (keys[SDL_SCANCODE_RETURN]) buttons |= OD_BTN_START;
        if (keys[SDL_SCANCODE_RSHIFT]) buttons |= OD_BTN_SELECT;
    }
    *out = buttons;
    *quit = h->test_quit ? 1 : 0;
}

static int test_renderer_init(void *ctx, int scale, int headless, int vsync)
{
    (void)ctx; (void)scale; (void)headless; (void)vsync;
    return OD_OK;      /* the window of the outer session is reused */
}

/* C6.24: Test shows every level row. The original draws its ROUND/TIME bar
 * over frame rows 216..239 (row 14 and half of row 13), which the vanilla game
 * keeps; here the same tick is composed again with the bar off, and the
 * level goes on top with the game's own bar below it (od_ued_test_view).
 * Nothing of the simulation changes: `hud` is a drawing flag, restored before
 * returning, and the flow's frame and hashes are the ones already made. */
static void test_present(void *ctx, const struct od_flow *f, const uint32_t *argb)
{
    sdl_host *h = ctx;
    memcpy(h->last_frame, argb, sizeof h->last_frame);
    h->have_frame = 1;
    if (!h->use_renderer)
        return;
    od_renderer_set_video(&h->rend, h->settings.integer_scale,
                           h->settings.aspect_87);
    if (f != &g_test_flow) {
        od_renderer_present(&h->rend, argb);
        return;
    }
    static od_index_frame fb;
    static uint32_t level[OD_SCREEN_W * OD_SCREEN_H];
    int hud = g_test_flow.hud;
    g_test_flow.hud = 0;
    od_flow_draw(&g_test_flow, &fb);
    g_test_flow.hud = hud;
    od_frame_to_argb(&fb, &od_master_palette_pal, level);
    if (f->fade_black) {               /* same exit fade as the flow's frame */
        unsigned keep = 256u - f->fade_black;
        for (int i = 0; i < OD_SCREEN_W * OD_SCREEN_H; i++) {
            uint32_t c = level[i];
            level[i] = 0xff000000u | ((((c >> 16) & 0xffu) * keep / 256u) << 16) |
                       ((((c >> 8) & 0xffu) * keep / 256u) << 8) |
                       ((c & 0xffu) * keep / 256u);
        }
    }
    od_ued_test_view(argb, level, g_test_view);
    host_present_canvas(h, &g_test_tex, g_test_view, h->settings.aspect_87);
}

static void test_sleep(void *ctx, unsigned ms)
{
    sdl_host *h = ctx;
    /* While paused the nested flow runs no tick, so its input callback is
     * not called: read the window here to see the focus come back. */
    if (h->test_paused)
        test_poll_events(h);
    SDL_Delay(ms ? ms : 1);
}

/* C6.34: the nested run plays through the game's own engine and device. The
 * flow only borrows them: it never opens a second device nor closes the live
 * one, and the caller saves and restores the engine around the run. */
static int sdl_audio_open(void *ctx);
static void sdl_audio_close(void *ctx);
static void sdl_audio_tick_cb(void *ctx);
static void sdl_audio_request_cb(void *ctx, unsigned id);
static void sdl_audio_music_cb(void *ctx, int track);
static void sdl_audio_reset_cb(void *ctx);
static int test_audio_open(void *ctx) { return ((sdl_host *)ctx)->audio_ready; }
static void test_audio_close(void *ctx) { (void)ctx; }

/* Run `doc` in a nested flow on a copy of the assets, exactly like the
 * --play-level API, until ESC or the host quits. */
static void host_run_test(sdl_host *h, const od_led_doc *doc)
{
    if (h->assets == NULL || doc == NULL)
        return;
    od_level lv;
    if (od_led_to_level(doc, &lv, g_test_cmd, sizeof g_test_cmd) != OD_OK) {
        snprintf(h->editor.status, sizeof h->editor.status, "%s",
                 od_str(OD_STR_TEST_ERROR));
        h->editor.status_error = 1;
        return;
    }
    static od_assets ta;
    ta = *h->assets;                      /* borrows the live buffers */
    int idx = doc->number > 0 ? doc->number - 1 : 0;
    if (idx < 0 || idx >= ta.levels.count)
        idx = 0;
    ta.levels.levels[idx] = lv;

    od_flow_opts fo;
    memset(&fo, 0, sizeof fo);
    fo.assets_dir = h->o->assets_dir;
    fo.level = idx;
    fo.level_seen = 1;
    fo.play_level = 1;
    fo.custom_level = 1;   /* C6.31 (CORE-3): no progress; ends on solve/NEW GAME */
    /* C6.34 (UI-21): sound unless the game itself has none (--no-audio or no
     * device); mute and volume are the gain of the shared device. */
    fo.no_audio = h->o->no_audio || !h->audio_ready;
    fo.scale = h->o->scale > 0 ? h->o->scale : 3;
    fo.load_save = 0;
    if (h->test_ticks > 0) {
        fo.ticks = h->test_ticks;
        fo.hash_out = h->test_hash_out;
        fo.hash_every = 1;
        fo.hash_no_audio = 1;
    }

    od_flow_host host;
    memset(&host, 0, sizeof host);
    host.ctx = h;
    host.counter = sdl_counter;
    host.frequency = sdl_frequency;
    host.sleep_ms = test_sleep;
    host.buttons = test_buttons;
    host.reset_poll = sdl_reset_poll;
    host.renderer_init = test_renderer_init;
    host.present = test_present;
    host.audio_open = test_audio_open;
    host.audio_close = test_audio_close;
    host.audio_tick = sdl_audio_tick_cb;
    host.audio_request = sdl_audio_request_cb;
    host.audio_play_music = sdl_audio_music_cb;
    host.audio_reset = sdl_audio_reset_cb;

    h->test_quit = 0;
    h->test_paused = 0;
    int was_frozen = h->frozen;
    host_set_frozen(h, 0);
    /* C6.31 (CORE-15): the game's own last frame (the paused background and
     * the F5 thumbnail) is restored after the test. */
    static uint32_t game_frame[OD_UI_SCREEN_W * OD_UI_SCREEN_H];
    memcpy(game_frame, h->last_frame, sizeof game_frame);
    int had_frame = h->have_frame;
    fprintf(stderr, "editor: testing level (ESC returns)\n");
    int result = OD_FLOW_CUSTOM_NONE;
    /* C6.34: the interrupted game's engine, put back exactly after the run. */
    od_audio_sdl_checkpoint *audio_cp =
        fo.no_audio ? NULL : od_audio_sdl_checkpoint_save(&h->audio);
    if (od_flow_init(&g_test_flow, &ta, &fo, &host) == OD_OK) {
        od_flow_run_interactive(&g_test_flow);
        result = g_test_flow.custom_result;
    }
    od_flow_free(&g_test_flow);
    if (audio_cp != NULL) {
        uint64_t after = od_audio_sdl_checkpoint_restore(&h->audio, audio_cp);
        if (getenv("OD_AUDIO_TRACE"))   /* C6.34: product evidence of the restore */
            fprintf(stderr, "audio-trace: music_on=%d ticks=%llu engine before Test %016llx, after return %016llx (%s)\n",
                    h->audio.audio.music_on, (unsigned long long)h->audio.audio.ticks,
                    (unsigned long long)od_audio_sdl_checkpoint_hash(audio_cp),
                    (unsigned long long)after,
                    after == od_audio_sdl_checkpoint_hash(audio_cp) ? "equal" : "DIFFERENT");
        od_audio_sdl_checkpoint_free(audio_cp);
    }
    memcpy(h->last_frame, game_frame, sizeof game_frame);
    h->have_frame = had_frame;
    h->input_mask = ~0u;
    fprintf(stderr, "editor: test finished (%s)\n",
            result == OD_FLOW_CUSTOM_SOLVED ? "solved"
            : result == OD_FLOW_CUSTOM_NEW_GAME ? "NEW GAME" : "left");
    h->test_paused = 0;
    h->ui_dirty = 1;
    host_set_frozen(h, was_frozen);
    snprintf(h->editor.status, sizeof h->editor.status, "%s",
             od_str(OD_STR_TEST_FINISHED));
    h->editor.status_error = 0;
}

static void host_close_modal(sdl_host *h)
{
    h->ui.screen = OD_UI_HIDDEN;
    h->input_mask = ~0u;          /* C6.31 (CORE-4) */
    host_apply_pause(h);
}

/* C6.31: the modern menu's "New game" (owner's decision, 2026-10-06 23:40).
 * The menu asks for confirmation and then calls this: save.dat is kept as
 * save.dat.bak and cleared, and the game restarts like a power-on with cleared
 * RAM, so its menu is "Where to begin from?" (beginning, magicword, password)
 * after the title (od_flow_new_game). The menu closes. Returns 0 on success;
 * on failure nothing changed and the menu stays open with the reason. */
static int host_new_game(sdl_host *h)
{
    if (h->flow == NULL)
        return -1;
    char bak[1200];
    if (od_flow_new_game(h->flow, bak, sizeof bak) != OD_OK) {
        od_ui_toast(&h->ui, "New game failed: progress kept", 2000);
        return -1;
    }
    fprintf(stderr, "ui: new game (%s)\n", bak[0] ? bak : "no save to keep");
    if (od_ui_modal(&h->ui))
        host_close_modal(h);
    h->input_mask = ~0u;
    od_ui_toast(&h->ui, "New game", 1400);
    return 0;
}

/* Only catalog-generated ids; render the detached saved board, never the
 * running frame or the mutable editor preview. No flow tick/history edits. */
static int host_editor_thumbnail(sdl_host *h)
{
    od_ui_editor *e=&h->editor;
    const od_led_doc *d=&e->thumbnail_doc;
    if(!d->id[0]||!memchr(d->id,0,sizeof d->id))return -1;
    for(const char *p=d->id;*p;++p)
        if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'))return -1;
    if(!memchr(e->catalog_dir,0,sizeof e->catalog_dir)||!e->catalog_dir[0])return -1;
    char name[OD_LED_ID_MAX+8];snprintf(name,sizeof name,"%s.png",d->id);
    int result=-1;
#ifndef _WIN32
    /* C6.32 (UI-14): folders on the way may be aliases (Silverblue's /home, a
     * data folder moved to another disk); <id>.png itself is never followed
     * (the AT_SYMLINK_NOFOLLOW check and unlinkat below). */
    if(strstr(e->catalog_dir,"/../")||!strncmp(e->catalog_dir,"../",3))return -1;
    int dir=open(e->catalog_dir,O_RDONLY|O_DIRECTORY);
    if(dir<0)return -1;
    struct stat st;
    if(!fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW)&&!S_ISREG(st.st_mode)) {
        /* Unlink a stale symlink itself, never its target. No replacement. */
        if(S_ISLNK(st.st_mode))unlinkat(dir,name,0);
        close(dir);return -1;
    }
    /* Invalidate old preview BEFORE any encoder/allocation/render failure. */
    if(unlinkat(dir,name,0)&&errno!=ENOENT){close(dir);return -1;}
    char tempdir[128];
    snprintf(tempdir,sizeof tempdir,"/dev/fd/%d/.preview-XXXXXX",dir);
    if(!mkdtemp(tempdir)){close(dir);return -1;}
    char temp[160];snprintf(temp,sizeof temp,"%s/board.png",tempdir);
#else
    /* Private exclusive temporary directory, no existing temp file reused.
     * Windows rename uses MoveFileEx for atomic replacement, never remove+write. */
    DWORD attr=od_userdir_win_attrs(e->catalog_dir);
    if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY)||
       (attr&FILE_ATTRIBUTE_REPARSE_POINT))return -1;
    char dest[1200];int size=snprintf(dest,sizeof dest,"%s/%s",e->catalog_dir,name);
    if(size<0||(size_t)size>=sizeof dest)return -1;
    attr=od_userdir_win_attrs(dest);
    if(attr!=INVALID_FILE_ATTRIBUTES&&(attr&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))return -1;
    if(od_userdir_win_delete(dest)!=0)return -1;
    char tempdir[1200],temp[1240];int created=0;
    for(unsigned i=0;i<32&&!created;++i) {
        snprintf(tempdir,sizeof tempdir,"%s/.preview-%lu-%llu-%u",e->catalog_dir,
          (unsigned long)GetCurrentProcessId(),(unsigned long long)SDL_GetPerformanceCounter(),i);
        created=od_userdir_win_mkdir(tempdir);
    }
    if(!created)return -1;
    snprintf(temp,sizeof temp,"%s/board.png",tempdir);
#endif
    od_level level;uint8_t commands[OD_LED_CMD_MAX];od_index_frame fb;
    static uint32_t pixels[OD_SCREEN_W*OD_SCREEN_H];
    if(h->assets&&d->theme_world>=1&&d->theme_world<=15&&
       d->n_objects>=0&&d->n_objects<=OD_LED_MAX_OBJECTS&&
       od_led_to_level(d,&level,commands,sizeof commands)==OD_OK) {
        od_render_level_index(&fb,&level,(d->theme_world-1)*10,h->assets->chr,&h->assets->palette_ram);
        od_frame_to_argb(&fb,&od_master_palette_pal,pixels);
        if(!write_argb_png(temp,pixels)) {
#ifndef _WIN32
            result=renameat(AT_FDCWD,temp,dir,name);
#else
            result=od_userdir_win_move_replace(temp,dest);
#endif
        }
    }
    od_userdir_remove(temp);
#ifndef _WIN32
    rmdir(tempdir);close(dir);
#else
    od_userdir_win_rmdir(tempdir);
#endif
    return result;
}

static void host_editor_actions(sdl_host *h)
{
    od_ui_editor *e = &h->editor;
    if(e->thumbnail_pending) {
        e->thumbnail_pending=0;
        int failed=host_editor_thumbnail(h);
        od_ucat_refresh(&h->catalog);
        if(failed) {
            /* JSON succeeded. Do not claim the document save was rolled back. */
            snprintf(e->status,sizeof e->status,od_str(OD_STR_LEVEL_SAVED),e->thumbnail_doc.id);
            e->status_error=1;
            fprintf(stderr,"editor: JSON saved; preview unavailable for %s\n",e->thumbnail_doc.id);
        }
    }
    if (h->ui.screen != OD_UI_EDITOR)
        return;
    if (e->request_open) {
        e->request_open = 0;
        od_ucat_open(&h->catalog);
        h->catalog.return_to_editor = 1;   /* Back returns here (UI-7) */
        h->ui.screen = OD_UI_CATALOG;
        return;
    }
    if (e->request_test) {
        e->request_test = 0;
        host_run_test(h, &e->doc);
        return;
    }
    if (e->request_back) {
        e->request_back = 0;
        host_close_modal(h);
    }
}

static void catalog_entry_path(sdl_host *h, const char *id, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.json", h->catalog.dir, id);
}

static void host_catalog_actions(sdl_host *h)
{
    od_ui_catalog *c = &h->catalog;
    int req = c->request;
    if (req == OD_UCAT_REQ_NONE)
        return;
    c->request = OD_UCAT_REQ_NONE;
    char path[1400];
    switch (req) {
    case OD_UCAT_REQ_PLAY: {
        catalog_entry_path(h, c->request_id, path, sizeof path);
        od_led_doc td;
        memset(&td, 0, sizeof td);
        if (od_led_load_json(&td, path) == OD_OK)
            host_run_test(h, &td);
        else {
            snprintf(c->status, sizeof c->status,
                     od_str(OD_STR_LEVEL_LOAD_ERROR), c->request_id);
            c->status_error = 1;
        }
        od_led_free(&td);
        break;
    }
    case OD_UCAT_REQ_EDIT: {
        catalog_entry_path(h, c->request_id, path, sizeof path);
        if (od_ued_load(&h->editor, path) == OD_OK) {
            h->editor.open = 1;
            h->catalog.return_to_editor = 0;
            h->ui.screen = OD_UI_EDITOR;
        }
        break;
    }
    case OD_UCAT_REQ_BACK:
        /* C6.32 (UI-7): back where it was opened from, the editor or the
         * menu (Duplicate, Import and Export run in ui_catalog.c). */
        if (c->return_to_editor) {
            c->return_to_editor = 0;
            h->ui.screen = OD_UI_EDITOR;
        } else {
            h->ui.screen = OD_UI_MENU;
            h->ui.selected = 3;            /* "Levels" */
        }
        h->ui_dirty = 1;
        break;
    default:
        break;
    }
}

/* --editor: a small loop that only serves the editor and the catalogue. */
static void host_editor_loop(sdl_host *h)
{
    if (sdl_renderer_init(h, h->o->scale > 0 ? h->o->scale : 3,
                          h->o->headless, h->o->vsync) != OD_OK) {
        fprintf(stderr, "editor: cannot open the renderer\n");
        return;
    }
    memset(h->last_frame, 0, sizeof h->last_frame);
    h->have_frame = 1;
    h->ui.screen = OD_UI_EDITOR;
    h->editor.open = 1;
    if (h->o->editor_file != NULL)
        od_ued_load(&h->editor, h->o->editor_file);
    /* C6.34: the standalone editor has no game flow; Test still gets sound. */
    int own_audio = !h->o->no_audio && !h->o->headless && !h->audio_ready &&
                    sdl_audio_open(h);
    while (!h->quit && h->ui.screen != OD_UI_HIDDEN) {
        Uint32 now = SDL_GetTicks();
        od_ui_tick(&h->ui, now - h->ui_last_ticks);
        h->ui_last_ticks = now;
        /* C6.32: closing the window with unsaved changes asks first, once. */
        if (h->quit && h->editor.doc.dirty &&
            h->editor.confirm == OD_UED_CONFIRM_NONE && !h->editor.request_back) {
            h->quit = 0;
            h->ui.screen = OD_UI_EDITOR;
            h->editor.confirm = OD_UED_CONFIRM_BACK;
        }
        host_editor_refresh(h);
        od_ui_draw(&h->ui);
        od_renderer_set_video(&h->rend, h->settings.integer_scale,
                               h->settings.aspect_87);
        host_present_wide(h);   /* editor or catalogue, C6.24 canvas */
        SDL_Delay(16);
        int confirming = h->editor.confirm != OD_UED_CONFIRM_NONE;
        poll_events(h);
        if (h->quit && confirming)
            break;                 /* closed again while asking: really quit */
        host_editor_actions(h);
        host_catalog_actions(h);
    }
    if (own_audio)
        sdl_audio_close(h);
    sdl_renderer_shutdown(h);
}

/* Failure remains runtime-latched until legitimate init/reset. A slot must not
 * clear it or allow another partial batch. Never serialize this host latch. */
static int host_scene_sound_error(sdl_host *h, const od_flow *f)
{
    if (!f || f->scene_sound_status == OD_OK) {
        h->scene_sound_error_reported = 0;
        return 0;
    }
    h->quit = 1;
    if (h->flow == f) h->flow->running = 0;
    if (!h->scene_sound_error_reported) {
        h->scene_sound_error_reported = 1;
        h->ui.screen = OD_UI_MENU;
        const char *message = "The session stopped because scene audio failed. Restart the game to continue.";
        fprintf(stderr,"scene audio: session stopped (status %d)\n",(int)f->scene_sound_status);
        od_ui_states_status(&h->ui,message,1600);
        if (!h->o || !h->o->headless)
            (void)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Scene Audio Error",message,NULL);
    }
    return 1;
}

/* Product error service is always enabled. CLI state harness stays dev-only. */
/* C6.40: the first time a game starts (the flow enters "play") show the
 * "Esc: menu | F1: controls" hint once and remember it in settings.ini. Only
 * in the interactive product: never in replays, batches, hashes or headless
 * runs, so no overlay reaches a measured frame. */
static void host_controls_hint(sdl_host *h, const struct od_flow *f)
{
    if (h->settings.controls_hint_shown || !h->ext_on || !h->use_renderer ||
        h->o->headless || h->o->replay || h->o->ticks > 0 || h->o->hash_out ||
        h->o->state_out || h->o->argb_out || od_ui_modal(&h->ui) ||
        strcmp(od_flow_scene_name(f), "play") != 0)
        return;
    od_ui_toast(&h->ui, od_str(OD_STR_CONTROLS_HINT_ONCE), 4000);
    h->ui_dirty = 1;
    h->settings.controls_hint_shown = 1;
    host_save_settings(h);
    fprintf(stderr, "ui: controls hint shown\n");
}

/* C6.46: the first time an installed package (AppImage or TAR) is opened
 * interactively, ask once whether to add it to the applications menu. Never in
 * replays, batches, headless or measurement runs. The answer is remembered in
 * settings.ini by the UI; a package that already has our launcher is not asked. */
static void host_desktop_prompt(sdl_host *h)
{
    if (h->desktop_checked)
        return;
    if (!h->ext_on || !h->use_renderer || h->o->headless || h->o->replay ||
        h->o->ticks > 0 || h->o->hash_out || h->o->state_out || h->o->argb_out ||
        h->o->screenshot || h->o->frames > 0 || h->o->play_level) {
        h->desktop_checked = 1;
        return;
    }
    if (od_ui_modal(&h->ui))
        return;                              /* wait until the menu is closed */
    h->desktop_checked = 1;
    if (h->settings.desktop_prompted || !od_desktop_supported() ||
        !od_desktop_packaged(NULL))
        return;
    if (od_desktop_installed(NULL)) {
        h->settings.desktop_prompted = 1;
        host_save_settings(h);
        return;
    }
    h->ui.screen = OD_UI_MENU;
    h->ui.selected = 0;
    h->ui.confirm = OD_UI_CONFIRM_DESKTOP;
    h->ui.confirm_yes = 0;                   /* No is focused */
    host_ui_after(h, 0);
    fprintf(stderr, "ui: desktop question shown\n");
}

static void sdl_service(void *ctx, struct od_flow *f, long tick)
{
    sdl_host *h = ctx;
    if (host_scene_sound_error(h,f)) return;
    host_desktop_prompt(h);
    host_controls_hint(h, f);
#ifdef OD_DEV_TOOLS
    if (h->o->state_save != NULL && h->o->state_save_at >= 0 &&
        tick == h->o->state_save_at) {
        if (host_state_available(h) && ensure_parent_dir(h->o->state_save) == 0 &&
            host_state_write(h, h->o->state_save) == OD_OK)
            fprintf(stderr, "state: saved %s at tick %ld\n",
                    h->o->state_save, tick);
        else
            fprintf(stderr, "state: cannot save %s\n", h->o->state_save);
    }
    if (h->o->new_game_at > 0 && tick == h->o->new_game_at)
        host_new_game(h);
    if (h->o->state_load != NULL && tick == 0 && !h->state_loaded) {
        h->state_loaded = 1;
        if (host_state_read(h, h->o->state_load) == OD_OK)
            fprintf(stderr, "state: loaded %s (tick %ld)\n",
                    h->o->state_load, f->sim_tick);
        else
            fprintf(stderr, "state: rejected %s\n", h->o->state_load);
    }
#else
    (void)tick;
#endif
}

static int sdl_screenshot(void *ctx, const od_flow *f, const od_index_frame *fb,
                          long frame, int numbered)
{
    sdl_host *h = ctx;
    unsigned fade = f ? f->fade_black : 0;
    const uint32_t *overlay = NULL;
    if (h->ext_on && od_ui_visible(&h->ui)) {
        od_ui_draw(&h->ui);
        overlay = h->ui.canvas;
    }
    if (!numbered)
        return write_screenshot_overlay(h->o->screenshot, fb, fade, overlay);
    char path[1200];
    const char *base_name = h->o->screenshot;
    const char *dot = strrchr(base_name,'.');
    const char *slash = od_path_last_sep(base_name);
    if (dot && slash && dot < slash) dot = NULL;
    size_t base = dot ? (size_t)(dot-base_name) : strlen(base_name);
    int n = snprintf(path,sizeof path,"%.*s_%05ld%s",(int)base,base_name,
                     frame,dot ? dot : ".png");
    if (n < 0 || n >= (int)sizeof path) return 1;
    return write_screenshot_overlay(path, fb, fade, overlay);
}

static int sdl_audio_open(void *ctx)
{
    sdl_host *h = ctx;
    if (od_audio_sdl_open(&h->audio, h->o->assets_dir) != OD_OK)
        return 0;
    h->audio_ready = 1;
    host_apply_audio(h);
    return 1;
}
static void sdl_audio_close(void *ctx)
{
    sdl_host *h = ctx;
    if (h->audio_ready)
        od_audio_sdl_close(&h->audio);
    h->audio_ready = 0;
}
static void sdl_audio_tick_cb(void *ctx) { od_audio_sdl_tick(&((sdl_host*)ctx)->audio); }
static void sdl_audio_request_cb(void *ctx, unsigned id)
{ od_audio_sdl_request(&((sdl_host*)ctx)->audio, id); }
static void sdl_audio_music_cb(void *ctx, int track)
{ od_audio_sdl_play_music(&((sdl_host*)ctx)->audio, track); }
static void sdl_audio_reset_cb(void *ctx)
{ od_audio_sdl_reset_init(&((sdl_host*)ctx)->audio); }

static void host_fill(od_flow_host *host, sdl_host *h)
{
    memset(host, 0, sizeof *host);
    host->ctx = h;
    host->counter = sdl_counter;
    host->frequency = sdl_frequency;
    host->sleep_ms = sdl_sleep;
    host->buttons = sdl_buttons;
    host->reset_poll = sdl_reset_poll;
    host->service = sdl_service;
    host->renderer_init = sdl_renderer_init;
    host->renderer_shutdown = sdl_renderer_shutdown;
    host->renderer_has_vsync = sdl_renderer_has_vsync;
    host->present = sdl_present;
    host->screenshot = sdl_screenshot;
    host->audio_open = sdl_audio_open;
    host->audio_close = sdl_audio_close;
    host->audio_tick = sdl_audio_tick_cb;
    host->audio_request = sdl_audio_request_cb;
    host->audio_play_music = sdl_audio_music_cb;
    host->audio_reset = sdl_audio_reset_cb;
}

static void opts_from_options(od_flow_opts *fo, const options *o,
                              const char *save_path)
{
    memset(fo, 0, sizeof *fo);
    fo->assets_dir = o->assets_dir;
    fo->replay = o->replay;
    fo->save_path = save_path;
    fo->save_forced = o->save != NULL;
    fo->screenshot = o->screenshot;
    fo->level = o->level;
    fo->level_seen = o->level_seen;
    fo->play_level = o->play_level != NULL && o->render_level == NULL;
    fo->custom_level = o->play_level != NULL;   /* C6.31 (CORE-3) */
    fo->no_entry = o->no_entry;
    fo->no_audio = o->no_audio;
    fo->hud = o->hud;
    fo->hud_frame = o->hud_frame;
    fo->map = o->map;
    fo->world = o->world;
    fo->cleared = o->cleared;
    fo->menu = o->menu;
    fo->continuing = o->continuing;
    fo->ending = o->ending;
    fo->ticks = (int)o->ticks;
    fo->frames = o->frames;
    fo->shot_frame = o->shot_frame;
    fo->shot_every = o->shot_every;
    fo->state_out = o->state_out;
    fo->record = o->record;
    fo->boss = o->boss;
    fo->hash_out = o->hash_out;
    fo->hash_every = o->hash_every;
    fo->hash_no_audio = o->hash_no_audio;
    fo->hash_render = o->hash_render;
    fo->audio_out = o->audio_out;
    fo->argb_out = o->argb_out;
    fo->scale = o->scale;
    fo->headless = o->headless;
    fo->vsync = o->vsync;
    fo->max_sim_hz = o->max_sim_hz;
}

#ifdef OD_DEV_TOOLS
/* C5.10 capture instrument. It renders the first playable frame of a level
 * through the real renderer at an explicit window size and video options, then
 * reads the window backbuffer and checks, pixel by pixel, that the scaling was
 * nearest-neighbour (every presented pixel is a source pixel) and that the
 * borders are black. It writes the window as a PNG so the result can be looked
 * at. Only the dev binary exposes --video-demo. */
static int run_video_demo(const options *o, const od_assets *assets)
{
    static sdl_host h;   /* C6.79: ~1.6 MiB, off the 2 MiB Windows stack; the memset below resets it */
    memset(&h, 0, sizeof h);
    h.o = o;
    h.ext_on = 1;
    h.assets = assets;
    load_settings(o, h.settings_path, sizeof h.settings_path, &h.settings);
    if (o->video_integer >= 0)
        h.settings.integer_scale = o->video_integer;
    if (o->video_aspect >= 0)
        h.settings.aspect_87 = o->video_aspect;
    od_ui_init(&h.ui, &h.settings);

    od_renderer_request_size(o->video_win_w, o->video_win_h);
    if (sdl_renderer_init(&h, o->scale > 0 ? o->scale : 1, 1, 0) != OD_OK) {
        fprintf(stderr, "video demo: cannot open the renderer\n");
        od_ui_shutdown(&h.ui);
        return 1;
    }
    od_renderer_set_video(&h.rend, h.settings.integer_scale,
                           h.settings.aspect_87);

    static uint32_t src[OD_SCREEN_W * OD_SCREEN_H];
    od_index_frame fb;
    int idx = o->level;
    if (idx < 0 || idx >= assets->levels.count)
        idx = 0;
    od_render_level_index(&fb, &assets->levels.levels[idx], idx, assets->chr,
                           &assets->palette_ram);
    od_frame_to_argb(&fb, &od_master_palette_pal, src);

    od_renderer_present(&h.rend, src);
    uint32_t *win = NULL;
    int ww = 0, wh = 0;
    if (od_renderer_read_window(&h.rend, &win, &ww, &wh) != OD_OK) {
        fprintf(stderr, "video demo: cannot read the window\n");
        od_ui_shutdown(&h.ui);
        sdl_renderer_shutdown(&h);
        return 1;
    }

    od_video_rect vp;
    od_video_viewport(ww, wh, h.settings.integer_scale, h.settings.aspect_87,
                       &vp);
    /* Nearest-neighbour proof: SDL's exact sampling phase differs from a plain
     * floor() map at non-uniform scales (8:7), so instead of guessing the
     * mapping we check the invariant that matters: every presented pixel is
     * exactly one of the source colours (an interpolating scaler would create
     * new colours), and every pixel outside the viewport is black. */
    enum { MAX_COLORS = 4096 };
    static uint32_t colors[MAX_COLORS];
    int ncolors = 0;
    for (int i = 0; i < OD_SCREEN_W * OD_SCREEN_H && ncolors < MAX_COLORS; i++) {
        uint32_t c = src[i] & 0x00FFFFFFu;
        int found = 0;
        for (int j = 0; j < ncolors; j++)
            if (colors[j] == c) { found = 1; break; }
        if (!found)
            colors[ncolors++] = c;
    }

    long mismatches = 0, outside = 0;
    for (int y = 0; y < wh; y++) {
        for (int x = 0; x < ww; x++) {
            uint32_t got = win[(size_t)y * (size_t)ww + (size_t)x];
            if (x >= vp.x && x < vp.x + vp.w && y >= vp.y && y < vp.y + vp.h) {
                uint32_t c = got & 0x00FFFFFFu;
                int found = 0;
                for (int j = 0; j < ncolors; j++)
                    if (colors[j] == c) { found = 1; break; }
                if (!found)
                    mismatches++;
            } else {
                outside++;
                if ((got & 0x00FFFFFFu) != 0)
                    mismatches++;
            }
        }
    }

    /* Exact integer geometry (the default, square pixels): every source pixel
     * becomes a whole k x k block. This pins the zoom factor and the offset. */
    long block_bad = 0;
    if (h.settings.integer_scale && !h.settings.aspect_87) {
        int k = vp.h / OD_SCREEN_H;
        for (int sy = 0; k >= 1 && sy < OD_SCREEN_H; sy++) {
            for (int sx = 0; sx < OD_SCREEN_W; sx++) {
                uint32_t want = src[(size_t)sy * OD_SCREEN_W + sx] & 0x00FFFFFFu;
                for (int dy = 0; dy < k; dy++) {
                    for (int dx = 0; dx < k; dx++) {
                        uint32_t got =
                            win[(size_t)(vp.y + sy * k + dy) * (size_t)ww +
                                (size_t)(vp.x + sx * k + dx)] & 0x00FFFFFFu;
                        if (got != want)
                            block_bad++;
                    }
                }
            }
        }
    }

    uint8_t *rgb = (uint8_t *)malloc((size_t)ww * (size_t)wh * 3);
    if (rgb != NULL) {
        for (int i = 0; i < ww * wh; i++) {
            uint32_t c = win[i];
            rgb[i * 3 + 0] = (uint8_t)((c >> 16) & 0xffu);
            rgb[i * 3 + 1] = (uint8_t)((c >> 8) & 0xffu);
            rgb[i * 3 + 2] = (uint8_t)(c & 0xffu);
        }
        if (od_png_write_rgb(o->video_demo, ww, wh, rgb) != 0)
            fprintf(stderr, "video demo: cannot write %s\n", o->video_demo);
        else
            printf("wrote %s (%dx%d)\n", o->video_demo, ww, wh);
        free(rgb);
    }

    printf("video demo: win=%dx%d integer=%d aspect=%d viewport=%d,%d,%dx%d\n",
           ww, wh, h.settings.integer_scale, h.settings.aspect_87,
           vp.x, vp.y, vp.w, vp.h);
    printf("video demo: nearest=%s mismatches=%ld border=%ld blocks=%s\n",
           mismatches == 0 ? "OK" : "FAIL", mismatches, outside,
           block_bad == 0 ? "OK" : "FAIL");

    free(win);
    od_ui_shutdown(&h.ui);
    sdl_renderer_shutdown(&h);
    return (mismatches == 0 && block_bad == 0) ? 0 : 1;
}

/* C5.10 pause-on-focus instrument: inject a real SDL focus loss through
 * poll_events and check the host freezes (and shows "Paused"), then inject the
 * regain and check it resumes. The capture shows the paused banner. */
static int run_focus_demo(const options *o, const od_assets *assets)
{
    static sdl_host h;   /* C6.79: ~1.6 MiB, off the 2 MiB Windows stack; the memset below resets it */
    memset(&h, 0, sizeof h);
    h.o = o;
    h.ext_on = 1;
    h.assets = assets;
    h.synth_pointer = 1;
    load_settings(o, h.settings_path, sizeof h.settings_path, &h.settings);
    h.settings.pause_on_focus_loss = 1;
    od_ui_init(&h.ui, &h.settings);

    if (sdl_renderer_init(&h, o->scale > 0 ? o->scale : 2, 1, 0) != OD_OK) {
        fprintf(stderr, "focus demo: cannot open the renderer\n");
        od_ui_shutdown(&h.ui);
        return 1;
    }

    static uint32_t src[OD_SCREEN_W * OD_SCREEN_H];
    od_index_frame fb;
    int idx = o->level;
    if (idx < 0 || idx >= assets->levels.count)
        idx = 0;
    od_render_level_index(&fb, &assets->levels.levels[idx], idx, assets->chr,
                           &assets->palette_ram);
    od_frame_to_argb(&fb, &od_master_palette_pal, src);
    h.have_frame = 1;
    memcpy(h.last_frame, src, sizeof h.last_frame);

    push_focus(0);
    poll_events(&h);
    int paused = h.paused_focus;
    int frozen = h.frozen;
    od_ui_draw(&h.ui);
    if (o->focus_demo != NULL)
        write_screenshot_overlay(o->focus_demo, &fb, 0, h.ui.canvas);

    push_focus(1);
    poll_events(&h);
    int resumed = (!h.paused_focus && !h.frozen);

    printf("focus demo: paused=%d frozen=%d resumed=%d\n", paused, frozen,
           resumed);
    printf("focus demo: paused_after_resume=%d frozen_after_resume=%d\n",
           h.paused_focus, h.frozen);

    od_ui_shutdown(&h.ui);
    sdl_renderer_shutdown(&h);
    return (paused && frozen && resumed) ? 0 : 1;
}
#endif /* OD_DEV_TOOLS */

/* ------------------------------------------------------------------------- */
/* C5.8 first-run screen                                                      */
/* ------------------------------------------------------------------------- */

/* C6.33: 1 when `dir` holds a package that is not this version's import (an
 * older importer wrote it, or it was edited): its files are hashed against
 * the reference manifest embedded in the binary (C6.29), only the files that
 * manifest lists. 0 when it matches or there is no package. */
static int content_is_stale(const char *dir, char *why, size_t cap)
{
    if (!od_import_content_present(dir))
        return 0;
    return od_content_check_imported(dir, why, cap) != OD_OK;
}

/* C6.48: the first-run screen at the window's own resolution (Inter), or the
 * 8x8 canvas screen when the fonts are missing (`view` NULL). */
typedef struct {
    od_ui_import_view *view;
    uint32_t *buf;
    size_t cap;
} ir_screen;

static void ir_present(sdl_host *h, ir_screen *s, const od_ui_import *st)
{
    if (s->view == NULL) {
        od_ui_import_draw(st, h->ui.canvas);
        od_renderer_present(&h->rend, h->ui.canvas);
        return;
    }
    int w = 0, hh = 0;
    od_renderer_output_size(&h->rend, &w, &hh);
    if (w < 1 || hh < 1)
        return;
    if ((size_t)w * (size_t)hh > s->cap) {
        free(s->buf);
        s->cap = (size_t)w * (size_t)hh;
        s->buf = (uint32_t *)malloc(s->cap * sizeof(uint32_t));
        if (s->buf == NULL) {
            s->cap = 0;
            return;
        }
    }
    od_ui_import_view_draw(s->view, st, s->buf, w, hh);
    od_renderer_present_native(&h->rend, s->buf, w, hh);
}

/* The import blocks, so say so first: draw "Importing..." and then run it. */
static void ir_import(sdl_host *h, ir_screen *s, od_ui_import *st)
{
    od_ui_import_set_status(st, od_str(OD_STR_FR_IMPORTING), 0);
    st->busy = 1;
    ir_present(h, s, st);
    od_ui_import_run(st);
}

/* C6.76: the game window as the chooser's parent (the dialog opens on top of it, as its
 * child, and the game is no longer minimised). Fields stay 0 when SDL cannot say (Wayland
 * native, a platform without a window id, a headless run). */
static void picker_parent(sdl_host *h, od_picker_parent *pp)
{
    memset(pp, 0, sizeof *pp);
    if (h == NULL || h->rend.window == NULL)
        return;
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(h->rend.window, &wm))
        return;
#if defined(SDL_VIDEO_DRIVER_X11)
    if (wm.subsystem == SDL_SYSWM_X11)
        pp->x11_window = (unsigned long)wm.info.x11.window;
#endif
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
    if (wm.subsystem == SDL_SYSWM_WINDOWS)
        pp->native_window = (void *)wm.info.win.window;
#endif
}

/* Set while the game window is minimised because the chooser could not attach to it. */
static int g_picker_minimised;

static void ir_start_picker(options *o, sdl_host *h, ir_screen *s, od_ui_import *st,
                            od_rom_picker *picker,
                            int *active)
{
    (void)s;
    char last[1024];
    od_picker_parent pp;
    picker_parent(h, &pp);
    od_rom_picker_begin_child(picker, od_rom_picker_last_dir(last, sizeof last) ? last : NULL,
                              (!o->headless && h->rend.window) ? &pp : NULL);
    *active = 1;
    g_picker_minimised = 0;
    od_ui_import_set_status(st, od_str(OD_STR_FR_WAITING), 0);
}

/* Show the first-run import screen. In `capture` mode the screen is rendered
 * to that PNG and the process returns (no window); otherwise it runs the real
 * event loop: the "Open ROM..." button (C6.21), SDL_DROPFILE or a typed path.
 * Returns 0 once the content was imported, so the caller starts the game in the
 * same process, and 1 when the player left without importing. */
static int run_first_run(options *o, const char *capture)
{
    char dest[2048];
    if (od_import_default_dir(dest, sizeof(dest), NULL) != OD_OK) {
        fprintf(stderr, "first run: cannot resolve the user content directory\n");
        return 1;
    }
    od_ui_import st;
    od_ui_import_init(&st, dest);
    /* C6.33: the default package is there but from another version: the
     * screen says so and asks for the ROM again. Its files stay until the new
     * import has succeeded (od_import_rom swaps the tree in only then). */
    {
        char why[512] = {0};
        if (content_is_stale(dest, why, sizeof why))
            od_ui_import_set_stale(&st, 1);
    }

    /* C6.21: the system file chooser runs without blocking the game. */
    od_rom_picker picker;
    memset(&picker, 0, sizeof picker);
    int picker_active = 0;

    /* Optional demonstration/typing hook for the capture and tests. */
    const char *prefill = getenv("OD_FIRST_RUN_PATH");
    if (prefill != NULL && prefill[0] != '\0')
        od_ui_import_set_path(&st, prefill);

    if (capture != NULL) {
        static uint32_t canvas[OD_SCREEN_W * OD_SCREEN_H];
        od_ui_import_draw(&st, canvas);
        return write_argb_png(capture, canvas);
    }

    static sdl_host h;   /* C6.79: ~1.6 MiB, off the 2 MiB Windows stack; the memset below resets it */
    memset(&h, 0, sizeof(h));
    h.o = o;
    h.ext_on = 0;
    od_settings_defaults(&h.settings);
    od_ui_init(&h.ui, &h.settings);
    {
        int rw = 0, rh = 0; /* OD_FIRST_RUN_SIZE=WxH: window size for the captures */
        const char *sz = getenv("OD_FIRST_RUN_SIZE");
        if (sz != NULL && sscanf(sz, "%dx%d", &rw, &rh) == 2)
            od_renderer_request_size(rw, rh);
    }
    if (sdl_renderer_init(&h, o->scale > 0 ? o->scale : 3, o->headless, o->vsync) != OD_OK) {
        fprintf(stderr, "first run: cannot open the renderer\n");
        return 1;
    }
    /* C6.48: Inter at the window's resolution; the 8x8 canvas screen when the
     * fonts are missing. */
    ir_screen scr;
    memset(&scr, 0, sizeof scr);
    scr.view = od_ui_import_view_open(NULL, NULL);
    if (prefill != NULL && prefill[0] != '\0')
        st.typing = 1;
    /* Product capture hook (tests): write the window to a PNG
     * after a few frames; OD_FIRST_RUN_AUTOIMPORT=1 first imports the prefilled
     * path, so the error state can be captured. */
    const char *shot = getenv("OD_FIRST_RUN_CAPTURE");
    int autoimport = getenv("OD_FIRST_RUN_AUTOIMPORT") != NULL;
    int frame = 0;
    /* Keep the intent explicit: the window accepts file drops. */
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    while (!st.done && !st.quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                st.quit = 1;
            } else if (event.type == SDL_DROPFILE) {
                od_ui_import_set_path(&st, event.drop.file);
                SDL_free(event.drop.file);
                if (st.path_len)
                    ir_import(&h, &scr, &st);
            } else if (event.type == SDL_TEXTINPUT) {
                od_ui_import_append(&st, event.text.text);
            } else if (event.type == SDL_KEYDOWN) {
                st.kbd = 1;
                switch (event.key.keysym.sym) {
                case SDLK_BACKSPACE: od_ui_import_backspace(&st); break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    /* C6.48: Enter imports a typed path, else opens the chooser. */
                    if (scr.view != NULL && st.typing && st.path_len)
                        ir_import(&h, &scr, &st);
                    else if (scr.view != NULL && !picker_active)
                        ir_start_picker(o, &h, &scr, &st, &picker, &picker_active);
                    else if (scr.view == NULL)
                        od_ui_import_run(&st);
                    break;
                case SDLK_ESCAPE: st.quit = 1; break;
                default: break;
                }
            } else if (event.type == SDL_MOUSEMOTION ||
                       event.type == SDL_MOUSEBUTTONDOWN ||
                       event.type == SDL_MOUSEBUTTONUP) {
                int mx = event.type == SDL_MOUSEMOTION ? event.motion.x
                                                       : event.button.x;
                int my = event.type == SDL_MOUSEMOTION ? event.motion.y
                                                       : event.button.y;
                int cx = mx, cy = my, hit;
                od_renderer_point_to_output(&h.rend, mx, my, &mx, &my);
                if (scr.view != NULL) {
                    int ow = 0, oh = 0;
                    od_renderer_output_size(&h.rend, &ow, &oh);
                    hit = od_ui_import_view_button_hit(scr.view, &st, ow, oh, mx, my);
                } else {
                    od_video_rect vp;
                    od_renderer_get_viewport(&h.rend, &vp);
                    if (vp.w > 0 && vp.h > 0) {
                        cx = (mx - vp.x) * OD_UI_SCREEN_W / vp.w;
                        cy = (my - vp.y) * OD_UI_SCREEN_H / vp.h;
                    }
                    hit = od_ui_import_button_hit(cx, cy);
                }
                if (event.type == SDL_MOUSEMOTION) {
                    st.hot = hit;
                    st.kbd = 0;
                } else if (event.button.button == SDL_BUTTON_LEFT) {
                    if (event.type == SDL_MOUSEBUTTONDOWN) {
                        if (scr.view != NULL) {
                            st.pressed = hit && !picker_active;
                        } else if (hit && !picker_active) {
                            ir_start_picker(o, &h, &scr, &st, &picker, &picker_active);
                        }
                    } else {
                        /* C6.48: a click completes on release, over the button. */
                        int was = st.pressed;
                        st.pressed = 0;
                        if (was && hit && !picker_active)
                            ir_start_picker(o, &h, &scr, &st, &picker, &picker_active);
                    }
                }
            }
        }
        if (picker_active) {
            char chosen[1024];
            char perr[160] = {0};
            od_pick_result pr = od_rom_picker_poll(&picker, chosen,
                                                      sizeof chosen, perr, sizeof perr);
            /* C6.76: the last resort. The dialog could not be made a child of the game window
             * (no window id, Wayland native, or the tool refused --attach): minimise the game
             * so the dialog is not left behind it, as before C6.76. */
            if (pr == OD_PICK_PENDING && !picker.attached && !g_picker_minimised &&
                !o->headless && h.rend.window) {
                fprintf(stderr, "first run: the chooser could not attach to the window; "
                                "minimising it (last resort)\n");
                SDL_MinimizeWindow(h.rend.window);
                g_picker_minimised = 1;
            }
            if (pr != OD_PICK_PENDING) {
                picker_active = 0;
                if (!o->headless && h.rend.window) {
                    if (g_picker_minimised)
                        SDL_RestoreWindow(h.rend.window);
                    SDL_RaiseWindow(h.rend.window);
                }
                g_picker_minimised = 0;
                if (pr == OD_PICK_OK) {
                    od_rom_picker_remember(chosen);
                    od_ui_import_set_path(&st, chosen);
                    ir_import(&h, &scr, &st);
                } else if (pr == OD_PICK_UNAVAILABLE || pr == OD_PICK_ERROR) {
                    st.no_chooser = 1; /* C6.48: now the path field shows */
                    if (perr[0])
                        fprintf(stderr, "first run: file chooser: %s\n", perr);
                    od_ui_import_set_status(&st, od_str(OD_STR_FR_NO_CHOOSER), 1);
                } else {
                    od_ui_import_set_status(&st, "", 0);
                }
            }
        }
        frame++;
        if (shot != NULL && frame == 2 && autoimport && st.path_len)
            ir_import(&h, &scr, &st);
        ir_present(&h, &scr, &st);
        if (shot != NULL && frame == 6) {
            uint32_t *px = NULL;
            int pw = 0, ph = 0;
            if (od_renderer_read_window(&h.rend, &px, &pw, &ph) == OD_OK) {
                uint8_t *rgb = (uint8_t *)malloc((size_t)pw * (size_t)ph * 3);
                if (rgb != NULL) {
                    for (long i = 0; i < (long)pw * ph; i++) {
                        rgb[3 * i] = (uint8_t)(px[i] >> 16);
                        rgb[3 * i + 1] = (uint8_t)(px[i] >> 8);
                        rgb[3 * i + 2] = (uint8_t)px[i];
                    }
                    od_png_write_rgb(shot, pw, ph, rgb);
                    free(rgb);
                }
                free(px);
            }
            st.quit = 1;
            st.done = 0;
        }
        SDL_Delay(16);
    }
    if (st.done) {
        /* Show the confirmation before the game starts in this same process. */
        ir_present(&h, &scr, &st);
        if (!o->headless)
            SDL_Delay(1200);
        printf("content imported into %s\n", dest);
    }
    /* C6.29 (UI-20): never leave the system file chooser running. */
    if (picker_active)
        od_rom_picker_cancel(&picker);
    od_ui_import_view_close(scr.view);
    free(scr.buf);
    sdl_renderer_shutdown(&h);
    return st.done ? 0 : 1;
}

/* C6.41: the writer thread of od_userdir_write_atomic_bg. The save of a
 * cleared round used to fsync on the game thread (60-235 ms on a plain disk, a
 * visible hitch on the clear frame). Jobs run in order, each through the same
 * od_userdir_write_atomic (temporary file, fsync, rename), so the file is
 * never half-written; the flush waits for the queue to drain. */
typedef struct async_job {
    struct async_job *next;
    char *path;
    uint8_t *data;
    size_t len;
} async_job;
static struct {
    SDL_Thread *thread;
    SDL_mutex *mu;
    SDL_cond *cv;
    async_job *head, *tail;
    int busy, stop;
} g_aw;

static int async_worker(void *arg)
{
    (void)arg;
    SDL_LockMutex(g_aw.mu);
    for (;;) {
        while (g_aw.head == NULL && !g_aw.stop)
            SDL_CondWait(g_aw.cv, g_aw.mu);
        if (g_aw.head == NULL)
            break;
        async_job *j = g_aw.head;
        g_aw.head = j->next;
        if (g_aw.head == NULL)
            g_aw.tail = NULL;
        g_aw.busy = 1;
        SDL_UnlockMutex(g_aw.mu);
        if (od_userdir_write_atomic(j->path, j->data, j->len) != OD_OK)
            fprintf(stderr, "save: cannot write %s\n", j->path);
        free(j->path);
        free(j->data);
        free(j);
        SDL_LockMutex(g_aw.mu);
        g_aw.busy = 0;
        SDL_CondBroadcast(g_aw.cv);
    }
    SDL_UnlockMutex(g_aw.mu);
    return 0;
}

static int async_submit(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    async_job *j = calloc(1, sizeof *j);
    if (j == NULL)
        return 1;
    j->path = malloc(strlen(path) + 1);
    j->data = malloc(len ? len : 1);
    if (j->path == NULL || j->data == NULL) {
        free(j->path); free(j->data); free(j);
        return 1;
    }
    strcpy(j->path, path);
    if (len)
        memcpy(j->data, data, len);
    j->len = len;
    SDL_LockMutex(g_aw.mu);
    if (g_aw.tail)
        g_aw.tail->next = j;
    else
        g_aw.head = j;
    g_aw.tail = j;
    SDL_CondBroadcast(g_aw.cv);
    SDL_UnlockMutex(g_aw.mu);
    return 0;
}

static void async_flush(void *ctx)
{
    (void)ctx;
    if (g_aw.mu == NULL)
        return;
    SDL_LockMutex(g_aw.mu);
    while (g_aw.head != NULL || g_aw.busy)
        SDL_CondWait(g_aw.cv, g_aw.mu);
    SDL_UnlockMutex(g_aw.mu);
}

static void async_shutdown(void)
{
    if (g_aw.thread == NULL)
        return;
    od_userdir_set_async(NULL);       /* flushes, then writes synchronously */
    SDL_LockMutex(g_aw.mu);
    g_aw.stop = 1;
    SDL_CondBroadcast(g_aw.cv);
    SDL_UnlockMutex(g_aw.mu);
    SDL_WaitThread(g_aw.thread, NULL);
    g_aw.thread = NULL;
}

static void async_install(void)
{
    if (g_aw.thread != NULL || getenv("OD_SAVE_SYNC") != NULL)
        return;           /* OD_SAVE_SYNC: the pre-C6.41 behaviour, for measuring */
    g_aw.mu = SDL_CreateMutex();
    g_aw.cv = SDL_CreateCond();
    if (g_aw.mu == NULL || g_aw.cv == NULL)
        return;
    g_aw.thread = SDL_CreateThread(async_worker, "od-save", NULL);
    if (g_aw.thread == NULL)
        return;                       /* stays synchronous */
    od_userdir_async a = { NULL, async_submit, async_flush };
    od_userdir_set_async(&a);
    atexit(async_shutdown);
}

/* C6.93: the name SDL gives to the window system and, above all, to the sound server, so
 * the mixer lists the program as OpenDana (not as the ALSA plug-in or the binary). The
 * hints only exist in newer headers and only name things: they do not touch the simulation. */
static void set_sdl_app_names(void)
{
#ifdef SDL_HINT_APP_NAME
    SDL_SetHint(SDL_HINT_APP_NAME, "OpenDana");
#endif
#ifdef SDL_HINT_AUDIO_DEVICE_APP_NAME
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "OpenDana");
#endif
#ifdef SDL_HINT_AUDIO_DEVICE_STREAM_NAME
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "OpenDana");
#endif
}

int main(int argc, char **argv)
{
    options o;
    int rc = parse_args(&o, argc, argv);
    if (rc)
        return rc;
    set_sdl_app_names();

    if (!o.headless && !o.replay && !o.show_save) {
        char path[1024];
        resolve_save_path(&o, path, sizeof(path));
        if (od_save_is_older_format(path))
            fprintf(stderr, "%s\n", od_str(OD_STR_SAVE_OLDER));
    }

    if (o.show_save) {
        char path[1024];
        od_progress prog;
        resolve_save_path(&o, path, sizeof(path));
        if (od_save_is_older_format(path)) {
            printf("save: %s (%s)\n", path, od_str(OD_STR_SAVE_OLDER));
            return 1;
        }
        od_status sst = od_save_read(path, &prog);
        if (sst == OD_ERR_IO) {
            printf("save: %s (missing)\n", path);
            return 0;
        }
        if (sst != OD_OK) {
            printf("save: %s (invalid: %s)\n", path, od_status_str(sst));
            return 1;
        }
        printf("save: %s rounds=%d world=%d cursor=%d\n", path,
               od_progress_clear_count(&prog), prog.world + 1, prog.cursor + 1);
        return 0;
    }

    /* C6.46: the launcher commands need no content and no window. */
    if (o.desktop_cmd) {
        char text[600];
        int st = o.desktop_cmd == 1 ? od_desktop_install(NULL, text, sizeof text)
                                    : od_desktop_uninstall(NULL, text, sizeof text);
        printf("%s\n", text);
        return st == OD_DESKTOP_OK ? 0 : 1;
    }

    /* --validate needs no assets: check a pack, a mod or a level file. */
    if (o.validate) {
        od_validate_report rep;
        od_status vst = od_content_validate(o.validate, &rep);
        printf("validate: %s: %d pack(s), %d level(s), %d object(s), %d error(s)\n",
               o.validate, rep.packs, rep.levels, rep.objects, rep.errors);
        if (rep.message[0])
            printf("  %s\n", rep.message);
        return vst == OD_OK ? 0 : 2;
    }

    /* C5.8: one-time ROM import. The importer is C inside the binary; the
     * output goes to the user's XDG data directory and the ROM is never opened
     * again. Failure writes nothing. */
    if (o.import_rom != NULL) {
        char dest[2048];
        char err[256] = {0};
        if (od_import_default_dir(dest, sizeof(dest), NULL) != OD_OK) {
            fprintf(stderr, "import: cannot resolve the user content directory\n");
            return 1;
        }
        printf("import: %s -> %s\n", o.import_rom, dest);
        od_status ist = od_import_rom(o.import_rom, dest, NULL, err, sizeof(err));
        if (ist != OD_OK) {
            fprintf(stderr, "import: failed: %s\n", err[0] ? err : od_status_str(ist));
            return 1;
        }
        printf("content imported; the ROM is not needed to load the extracted data\n");
        return 0;
    }

    /* C6.21: a positional argument is the ROM the desktop passed when the player
     * dropped it on the application icon. Import it once and continue into the
     * game in this same process; the CLI `--import-rom` stays one-shot. */
    if (o.rom_arg != NULL) {
        char dest[2048];
        char err[256] = {0};
        if (od_import_default_dir(dest, sizeof(dest), NULL) != OD_OK) {
            fprintf(stderr, "import: cannot resolve the user content directory\n");
            return 1;
        }
        printf("import: %s -> %s\n", o.rom_arg, dest);
        od_status ist = od_import_rom(o.rom_arg, dest, NULL, err, sizeof(err));
        if (ist != OD_OK) {
            fprintf(stderr, "import: failed: %s\n", err[0] ? err : od_status_str(ist));
            return 1;
        }
        printf("content imported; starting the game\n");
    }

    /* C5.8: explicit first-run screen (interactive or capture). C6.21: after a
     * successful import the game starts here, in the same process. */
    if (o.first_run_shot != NULL)
        return run_first_run(&o, o.first_run_shot);
    if (o.first_run && run_first_run(&o, NULL) != 0)
        return 0;   /* the player left without importing */

    /* C5.8: with no content anywhere, the product opens the import screen
     * instead of failing, unless this is a measurement/headless invocation.
     * C6.33: the same screen, with its "another version" text, when the
     * default package (the XDG import) is not this version's import: stale
     * data is never played. An explicit package (--content, OD_CONTENT, the
     * development fallback) or a measurement run only gets a warning. */
    {
        int first_run_default = o.content == NULL && !o.headless && !o.replay &&
            o.ticks == 0 && o.screenshot == NULL && o.hash_out == NULL &&
            o.state_out == NULL && o.argb_out == NULL && o.play_level == NULL &&
            o.validate == NULL && !o.list_levels && !o.show_save && o.editor == 0;
        char cdir[2048];
        char why[512] = {0};
        int resolved = od_import_resolve_content(cdir, sizeof(cdir), o.content) == OD_OK;
        if (first_run_default && (!resolved || !od_import_content_present(cdir))) {
            if (run_first_run(&o, NULL) != 0)
                return 0;   /* the player left without importing */
            /* fall through: the package just imported loads below */
        } else if (resolved && content_is_stale(cdir, why, sizeof why)) {
            const char *env = getenv("OD_CONTENT");
            char xdg[2048];
            int is_default = o.content == NULL && (env == NULL || env[0] == '\0') &&
                od_import_default_dir(xdg, sizeof xdg, NULL) == OD_OK &&
                strcmp(xdg, cdir) == 0;
            if (first_run_default && is_default) {
                fprintf(stderr, "content: %s is from another version (%s); "
                        "import the ROM again\n", cdir, why);
                if (run_first_run(&o, NULL) != 0)
                    return 0;   /* left without importing: stale data is not played */
            } else {
                fprintf(stderr, "warning: content %s is not this version's import (%s)\n",
                        cdir, why);
            }
        }
    }

    /* C6.2: select the package before creating the runtime bundle. All level
     * commands, CHR and palettes come from this VFS; no legacy assets fallback. */
    if (o.assets_dir != NULL)
        fprintf(stderr, "--assets is deprecated and ignored; loading the content package instead\n");
    static od_content_vfs content_vfs;
    static od_content_level content_level;
    static char content_dir[1024] = {0};
    od_assets assets;
    memset(&assets, 0, sizeof assets);
    static char mod_dirs[OD_CONTENT_MAX_LAYERS][1200];
    const char *mod_paths[OD_CONTENT_MAX_LAYERS];
    int interactive_default = o.content == NULL && !o.headless && !o.replay &&
        o.ticks == 0 && o.screenshot == NULL && o.hash_out == NULL &&
        o.state_out == NULL && o.argb_out == NULL && o.play_level == NULL &&
        o.validate == NULL && !o.list_levels && !o.show_save && o.editor == 0;
    /* C6.21: if the default package exists but is not loadable (e.g. a stale v1
     * package), offer the import screen once and retry with the fresh package. */
    for (int attempt = 0; ; attempt++) {
        int mods_ok = od_import_resolve_content(content_dir, sizeof content_dir,
                                                 o.content) == OD_OK;
        for (int i = 0; mods_ok && i < o.mod_count; i++) {
            /* C4.1: `--mod example` is the mod ID of mods/example. */
            if (od_content_resolve_mod(mod_dirs[i], sizeof mod_dirs[i], content_dir,
                                        o.mods[i]) != OD_OK) {
                fprintf(stderr, "cannot find mod '%s'\n", o.mods[i]);
                mods_ok = 0;
            }
            mod_paths[i] = mod_dirs[i];
        }
        if (mods_ok &&
            od_content_vfs_init(&content_vfs, content_dir, mod_paths, o.mod_count) == OD_OK &&
            od_assets_load_content(&assets, &content_vfs) == OD_OK) {
            printf("content: %s (%d levels, %d mod(s))\n", content_dir,
                   assets.levels.count, o.mod_count);
            break;
        }
        product_content_release(&assets, &content_vfs);
        if (interactive_default && attempt == 0) {
            if (run_first_run(&o, NULL) != 0)
                return 0;
            continue;
        }
        fprintf(stderr, "cannot load the content package from '%s'\n", content_dir);
        return 1;
    }
    /* Legacy scene/audio interfaces retain the name assets_dir, but resolve
     * their data through the package VFS installed above. */
    o.assets_dir = content_dir;
    od_status st;

    /* D3: bind to bytes of this executable, not just structure sizes. Paths
     * are used only to read bytes and never enter the persisted identity. */
    const char *executable = argv[0];
#if defined(__linux__)
    executable = "/proc/self/exe";
#elif defined(_WIN32)
    char executable_path[4096];
    DWORD executable_len = GetModuleFileNameA(NULL,executable_path,sizeof executable_path);
    if (executable_len > 0 && executable_len < sizeof executable_path)
        executable = executable_path;
#endif
    /* No identity query here: flow_init still has scene/audio loaders to run.
     * C6.2 may certify the complete pinned snapshot only after their seals. */
    product_state_identity_ready = 0;

    /* C4.1: --render-level FILE --out PNG is the external editor's thumbnail.
     * It renders one level file statically, i.e. it is exactly
     * `--play-level FILE --screenshot PNG` (same loader, same compositor), so
     * the thumbnail equals the first playable frame of that level. */
    if (o.render_level) {
        if (o.out == NULL) {
            fprintf(stderr, "--render-level needs --out PNG\n");
            product_content_release(&assets,&content_vfs);
            return 2;
        }
        o.play_level = o.render_level;
        if (o.round <= 0)
            o.round = 1;
        o.screenshot = o.out;
    }

    /* --play-level FILE: the editor API plays one level file. A world file is
     * selected with --round. The loaded level replaces its slot so every mode
     * (replay, interactive, screenshot) sees it. */
    if (o.play_level) {
        if (od_content_level_load(&content_level, o.play_level, o.round) != OD_OK) {
            fprintf(stderr, "cannot load level '%s'\n", o.play_level);
            product_content_release(&assets,&content_vfs);
            return 1;
        }
        int idx = content_level.number > 0 ? content_level.number - 1 : 0;
        if (idx < 0 || idx >= assets.levels.count)
            idx = 0;
        assets.levels.levels[idx] = content_level.level;
        /* External level loader does not yet expose captured-byte provenance.
         * Hashing the path AFTER loading can identify different bytes. */
        product_state_identity_ready = 0;
        fprintf(stderr,"state: external level snapshot identity unavailable; save/load disabled\n");
        o.level = idx;
        o.level_seen = 1;
        printf("play-level: %s round %d -> index %d\n", o.play_level, o.round, idx);
    }

    if (o.list_levels) {
        rc = print_levels(&assets);
        product_content_release(&assets,&content_vfs);
        return rc;
    }

    if (o.level < 0 || o.level >= assets.levels.count) {
        fprintf(stderr, "level %d out of range (0..%d)\n", o.level, assets.levels.count - 1);
        product_content_release(&assets,&content_vfs);
        return 1;
    }

#ifdef OD_DEV_TOOLS
    if (o.video_demo) {
        int vrc = run_video_demo(&o, &assets);
        product_content_release(&assets,&content_vfs);
        return vrc;
    }
    if (o.focus_demo) {
        int frc = run_focus_demo(&o, &assets);
        product_content_release(&assets,&content_vfs);
        return frc;
    }

    /* --ui-demo FILE: compose the modern overlay over a level frame and write
     * it. Instrument for captures; it feeds the same
     * key handler the product's event loop uses, so it exercises ESC -> menu
     * and the Quit entry. */
    if (o.ui_demo) {
        static sdl_host h;   /* C6.79: ~1.6 MiB, off the 2 MiB Windows stack; the memset below resets it */
        memset(&h, 0, sizeof h);
        h.o = &o;
        h.ext_on = 1;
        h.assets = &assets;
        h.test_ticks = 0;
        h.test_hash_out = NULL;
        h.synth_pointer = 1;   /* pointer events pushed in canvas pixels */
        load_settings(&o, h.settings_path, sizeof h.settings_path, &h.settings);
        od_ui_init(&h.ui, &h.settings);
        {
            char cat_dir[1024];
            od_ucat_default_dir(cat_dir, sizeof cat_dir);
            od_ued_init(&h.editor, cat_dir);
            od_ucat_init(&h.catalog, cat_dir);
            h.ui.editor = &h.editor;
            h.ui.catalog = &h.catalog;
        }
        if (sdl_renderer_init(&h, o.scale > 0 ? o.scale : 1, 1, 0) != OD_OK) {
            fprintf(stderr, "ui demo: cannot open the renderer\n");
            od_ui_shutdown(&h.ui);
            product_content_release(&assets,&content_vfs);
            return 1;
        }
        const char *scr = o.ui_demo_screen ? o.ui_demo_screen : "menu";
        /* ESC through the SDL queue: the same path the product uses. */
        push_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        poll_events(&h);
        if (!strcmp(scr, "settings")) {
            for (int i = 0; i < 4; i++) {
                push_key(SDLK_DOWN, SDL_SCANCODE_DOWN);
                poll_events(&h);
            }
            push_key(SDLK_RETURN, SDL_SCANCODE_RETURN);
            poll_events(&h);
        } else if (!strcmp(scr, "mute")) {
            /* Close the menu again so the toast shows over the level. */
            push_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
            poll_events(&h);
            push_key(SDLK_m, SDL_SCANCODE_M);
            poll_events(&h);
        } else if (!strcmp(scr, "states")) {
            /* Keyboard path: menu -> States -> slot list. */
            push_key(SDLK_DOWN, SDL_SCANCODE_DOWN);
            poll_events(&h);
            push_key(SDLK_RETURN, SDL_SCANCODE_RETURN);
            poll_events(&h);
        } else if (!strcmp(scr, "states-mouse")) {
            /* Mouse path: ESC menu open, click the "States" row (index 1,
             * panel 44,24, rows of 18 from y=58). Canvas coordinates. */
            push_mouse(128, 81, 0);
            poll_events(&h);
            push_mouse(128, 81, 1);
            poll_events(&h);
        } else if (!strcmp(scr, "catalog")) {
            /* C5.5: the catalogue opened from the menu with the mouse (the
             * "Levels" row). run.sh pre-populates the XDG directory. */
            push_mouse(128, 112, 0);
            poll_events(&h);
            push_mouse(128, 112, 1);
            poll_events(&h);
        } else if (!strncmp(scr, "editor", 6)) {
            /* C5.5: the editor opened from the menu with the mouse (the
             * "Editor" row), then a level painted with real pointer events. */
            push_mouse(128, 94, 0);
            poll_events(&h);
            push_mouse(128, 94, 1);
            poll_events(&h);
            /* Points of the editor's own canvas (C6.24), from its geometry:
             * the centre of a cell, a palette row or a button. */
            int px, py, pw, ph;
            /* Two blocks on row 3. */
            od_ued_cell_rect(3, 3, &px, &py, &pw, &ph);
            push_mouse(px + pw / 2, py + ph / 2, 1);
            poll_events(&h);
            od_ued_cell_rect(4, 3, &px, &py, &pw, &ph);
            push_mouse(px + pw / 2, py + ph / 2, 1);
            poll_events(&h);
            /* Fire (palette index 14) at (5,5). */
            od_ued_palette_rect(14, &px, &py, &pw, &ph);
            push_mouse(px + pw / 2, py + ph / 2, 1);
            poll_events(&h);
            od_ued_cell_rect(5, 5, &px, &py, &pw, &ph);
            push_mouse(px + pw / 2, py + ph / 2, 1);
            poll_events(&h);
            /* Hero (palette index 16) at (2,2). */
            od_ued_palette_rect(16, &px, &py, &pw, &ph);
            push_mouse(px + pw / 2, py + ph / 2, 1);
            poll_events(&h);
            od_ued_cell_rect(2, 2, &px, &py, &pw, &ph);
            push_mouse(px + pw / 2, py + ph / 2, 1);
            poll_events(&h);
            if (!strcmp(scr, "editor-save") || !strcmp(scr, "editor-test")) {
                od_ued_button_rect(OD_UED_BTN_SAVE, &px, &py, &pw, &ph);
                push_mouse(px + pw / 2, py + ph / 2, 1);
                poll_events(&h);
                host_editor_actions(&h);
                if (!strcmp(scr, "editor-test")) {
                    h.test_ticks = 300;
                    h.test_hash_out = getenv("OD_EDITOR_TEST_HASH");
                    /* C6.31 (CORE-15): the game's last frame (paused
                     * background, F5 thumbnail) must survive the test. */
                    for (size_t i = 0; i < OD_UI_SCREEN_W * OD_UI_SCREEN_H; i++)
                        h.last_frame[i] = 0xFF000000u | (uint32_t)(i * 2654435761u);
                    static uint32_t game_frame[OD_UI_SCREEN_W * OD_UI_SCREEN_H];
                    memcpy(game_frame, h.last_frame, sizeof game_frame);
                    od_ued_button_rect(OD_UED_BTN_TEST, &px, &py, &pw, &ph);
                    push_mouse(px + pw / 2, py + ph / 2, 1);
                    poll_events(&h);
                    host_editor_actions(&h);
                    printf("ui demo: test kept the game frame=%d\n",
                           !memcmp(game_frame, h.last_frame, sizeof game_frame));
                }
            }
        }
        host_editor_refresh(&h);
        od_ui_draw(&h.ui);
        od_index_frame dfb;
        od_render_level_index(&dfb, &assets.levels.levels[o.level], o.level,
                               assets.chr, &assets.palette_ram);
        printf("ui demo: screen=%s esc_opens_menu=%d selected=%d gain=%d\n",
               scr, h.ui.screen != OD_UI_HIDDEN, h.ui.selected,
               od_settings_gain_permille(&h.settings));
        int occupied = 0;
        int slots = h.settings.states_slots;
        for (int i = 0; i < slots && i < OD_SETTINGS_STATES_MAX; i++)
            occupied += h.ui.slot_occupied[i] ? 1 : 0;
        printf("ui demo: states screen=%s focus=%d occupied=%d\n",
               h.ui.screen == OD_UI_STATES ? "states" : "other",
               h.ui.slot_focus, occupied);
        printf("ui demo: menu items: %s, %s, %s, %s, %s, %s, %s, %s\n",
               od_str(OD_STR_CONTINUE), od_str(OD_STR_STATES),
               od_str(OD_STR_EDITOR), od_str(OD_STR_LEVELS),
               od_str(OD_STR_SETTINGS), od_str(OD_STR_CONTROLS),
               od_str(OD_STR_NEW_GAME),
               od_str(OD_STR_QUIT));
        if (h.ui.screen == OD_UI_EDITOR) {
            char msg[160];
            int errors = od_ued_validate(&h.editor, msg, sizeof msg);
            printf("ui demo: editor screen=editor cell(3,3)=%02X cell(4,3)=%02X "
                   "cell(5,5)=%02X hero=(%d,%d) errors=%d msg=%s saved=%s path=%s\n",
                   h.editor.doc.grid[3][3], h.editor.doc.grid[3][4],
                   h.editor.doc.grid[5][5],
                   od_led_has_hero(&h.editor.doc) ? h.editor.doc.player & 0x0F : -1,
                   od_led_has_hero(&h.editor.doc) ? h.editor.doc.player >> 4 : -1,
                   errors, msg, h.editor.last_saved_id, h.editor.last_saved_path);
        } else if (h.ui.screen == OD_UI_CATALOG) {
            printf("ui demo: catalog screen=catalog entries=%d focus=%d first=%s\n",
                   h.catalog.n, h.catalog.focus,
                   h.catalog.n > 0 ? h.catalog.entries[0].id : "-");
        }
        if (host_wide_screen(&h)) {
            /* C6.24: the editor and the catalogue are their own canvas. */
            host_draw_wide(&h);
            static unsigned char wrgb[OD_UED_W * OD_UED_H * 3];
            for (int i = 0; i < OD_UED_W * OD_UED_H; i++) {
                wrgb[3 * i] = (unsigned char)(g_wide[i] >> 16);
                wrgb[3 * i + 1] = (unsigned char)(g_wide[i] >> 8);
                wrgb[3 * i + 2] = (unsigned char)g_wide[i];
            }
            rc = od_png_write_rgb(o.ui_demo, OD_UED_W, OD_UED_H, wrgb) ? 1 : 0;
        } else {
            rc = write_screenshot_overlay(o.ui_demo, &dfb, 0, h.ui.canvas);
        }
        od_ui_shutdown(&h.ui);
        od_ued_free(&h.editor);
        od_ucat_free(&h.catalog);
        sdl_renderer_shutdown(&h);
        product_content_release(&assets,&content_vfs);
        return rc;
    }
#endif

    /* The static screenshot paths never run the simulation loop. */
    if (o.screenshot && !o.replay && !(o.headless && o.frames > 0) &&
        !o.ticks && !o.hash_out && !o.state_out && !o.argb_out) {
        rc = run_screenshot(&o, &assets);
        product_content_release(&assets,&content_vfs);
        return rc;
    }

    /* The scripted replay path is a fresh session from reset: it does not load
     * the user save (the old `run_replay` did not either). The interactive loop
     * loads it for CONTINUE/auto-resume. */
    int use_replay = !(o.hash_out || o.state_out || o.ticks > 0) &&
                     (o.replay || (o.headless && o.frames > 0) ||
                      (o.argb_out && !o.screenshot));

    /* C6.31 (CORE-11): a measurement or scripted run (--ticks, --hash-out,
     * --state-out, --argb-out, --audio-out, --replay, --headless) and a custom
     * level (--play-level) never read or write the player's own save.dat:
     * only a file named with --save is used. */
    int measuring = use_replay || o.ticks > 0 || o.hash_out || o.state_out ||
                    o.argb_out || o.audio_out || o.headless || o.replay ||
                    o.play_level;
    int user_save = o.save != NULL || !measuring;
    char save_path[1024];
    resolve_save_path(&o, save_path, sizeof(save_path));
    if (user_save && ensure_parent_dir(save_path) != 0) {
        fprintf(stderr, "cannot create parent directory for '%s': %s\n", save_path, strerror(errno));
        product_content_release(&assets,&content_vfs);
        return 1;
    }
    if (!user_save)
        fprintf(stderr, "save: none (measurement run; --save FILE to use one)\n");

    od_flow_opts fo;
    opts_from_options(&fo, &o, user_save ? save_path : NULL);
    /* A scripted replay is a fresh session from reset, EXCEPT when the user
     * explicitly asks to CONTINUE: R10 resumes on the map with the save. */
    fo.load_save = user_save && (!use_replay || o.continuing);
    static sdl_host h;   /* C6.79: ~1.6 MiB, off the 2 MiB Windows stack; the memset below resets it */
    memset(&h, 0, sizeof h);
    h.o = &o;
    h.ext_on = ext_enabled();
    if (h.ext_on)
        load_settings(&o, h.settings_path, sizeof h.settings_path, &h.settings);
    else
        od_settings_defaults(&h.settings);
    od_ui_init(&h.ui, &h.settings);
    h.ui.desktop_installed = od_desktop_installed(NULL);   /* C6.46 */
    /* C5.5: the editor and the catalogue. Their directory follows XDG and the
     * screens are owned by the host, borrowed by the UI. */
    {
        char cat_dir[1024];
        od_ucat_default_dir(cat_dir, sizeof cat_dir);
        od_ued_init(&h.editor, cat_dir);
        od_ucat_init(&h.catalog, cat_dir);
        h.ui.editor = &h.editor;
        h.ui.catalog = &h.catalog;
        h.assets = &assets;
    }
    od_flow_host host;
    host_fill(&host, &h);

    od_flow flow;
    st = od_flow_init(&flow, &assets, &fo, &host);
    if (st != OD_OK) {
        fprintf(stderr, "cannot start the session: %s\n", od_status_str(st));
        product_content_release(&assets,&content_vfs);
        return 1;
    }
    h.flow = &flow;   /* C5.3: F5/F9 and the state harness act on this flow */
    product_state_identity_ready = !o.play_level &&
        product_state_snapshot_identity(&content_vfs);
    /* C6.43: the executable is not part of the identity; the field stays zero. */
    memset(product_state_identity.build, 0, sizeof product_state_identity.build);
    (void)executable;
    if (!product_state_identity_ready)
        fprintf(stderr,"state: loaded content snapshot identity unavailable; save/load disabled\n");

    if (o.editor) {
        od_renderer_request_fullscreen(o.fullscreen ||
                                        (h.ext_on && h.settings.fullscreen));
        od_renderer_request_maximized(o.maximized);
        host_editor_loop(&h);
        od_ued_free(&h.editor);
        od_ucat_free(&h.catalog);
        od_flow_free(&flow);
        product_content_release(&assets,&content_vfs);
        return 0;
    }

    if (!o.headless)
        async_install();   /* C6.41: the progress write leaves the game thread */
    if (use_replay) {
        od_flow_replay_status replay_status = od_flow_run_replay(&flow);
        if (replay_status == OD_FLOW_REPLAY_CANCELLED)
            fprintf(stderr,"replay: cancelled before completion\n");
        else if (replay_status != OD_FLOW_REPLAY_OK) {
            rc = 1;
            fprintf(stderr,"replay: stopped with error (status %d); output is incomplete\n",
                    (int)replay_status);
            /* The scene-audio latch is shown by the shared handler below.
             * Other replay/ARGB/screenshot/WAV errors need their own notice. */
            if (flow.scene_sound_status == OD_OK) {
                const char *message = "Replay stopped because an operation failed. Output is incomplete.";
                h.ui.screen = OD_UI_MENU;
                od_ui_states_status(&h.ui,message,1600);
                if (!o.headless)
                    (void)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Replay Error",message,NULL);
            }
        }
    } else {
        od_renderer_request_fullscreen(o.fullscreen ||
                                        (h.ext_on && h.settings.fullscreen));
        od_renderer_request_maximized(o.maximized);
        od_flow_run_interactive(&flow);
    }
    if (host_scene_sound_error(&h,&flow)) rc = 1;
    async_shutdown();   /* C6.41: nothing queued is lost at exit */

    od_flow_free(&flow);
    product_content_release(&assets,&content_vfs);
    return rc;
}
