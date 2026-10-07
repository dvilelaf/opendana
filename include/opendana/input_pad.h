/* input_pad.h - SDL2 controller front end (C5.2).
 *
 * Only this file (and its .c) needs SDL for input. It opens up to
 * OD_PAD_MAX_PLAYERS game controllers, keeps them in sync with hot-plug
 * events, samples them once per frame and runs the samples through the pure
 * model in `src/ext/input_map.c`. The result is the same 8-bit NES mask the
 * keyboard already produces, plus the interface actions.
 *
 * Integration (main.c, C5.1 owns the dispatch):
 *
 *   static od_pads pads;
 *   od_pads_init(&pads, settings_path, mapping_path);   // once, before the loop
 *   ...
 *   // inside the SDL event pump of sdl_buttons():
 *   od_pads_handle_event(&pads, &ev);
 *   ...
 *   // once per tick, instead of the raw keyboard block:
 *   od_input_frame in = {0};
 *   in.ptr_x = mouse_x; ...;              // the UI fills the pointer fields
 *   od_pads_poll(&pads, &in);
 *   *out |= in.nes_buttons;               // never anything but OD_BTN_*
 *   feed in.actions / in.n_actions to ui.c;
 *   ...
 *   od_pads_close(&pads);                // on exit
 */
#ifndef OD_INPUT_PAD_H
#define OD_INPUT_PAD_H

#include "opendana/ext/input_map.h"

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OD_PAD_MAX_PLAYERS 4

typedef struct {
    SDL_GameController *ctrl;
    SDL_JoystickID id;      /* instance id, for REMOVED events; -1 if free */
    unsigned axis_dpad;     /* hysteresis state kept for the left stick */
} od_pad_slot;

typedef struct {
    od_pad_slot slots[OD_PAD_MAX_PLAYERS];
    int n_slots;            /* number of open controllers (0..MAX) */
    int active;             /* slot of the last controller touched, or -1 */
    od_input_config config;
    unsigned prev_actions;  /* action bits down on the previous frame */
    char config_path[1024];
    int ready;              /* SDL controller subsystem was initialised */
} od_pads;

/* Open the controller subsystem and every controller already connected.
 * `config_path` is the settings.ini to read/write (`[input]` section); it may
 * be NULL, in which case only the defaults are used. `mapping_file` is an
 * optional gamecontrollerdb.txt; NULL disables it. Never fatal: on a machine
 * without controllers (or a dummy video driver) it leaves pads->ready = 0 and
 * the keyboard path keeps working. */
od_status od_pads_init(od_pads *pads, const char *config_path,
                         const char *mapping_file);

void od_pads_close(od_pads *pads);

/* Handle SDL_CONTROLLERDEVICEADDED / SDL_CONTROLLERDEVICEREMOVED. Safe to call
 * for any other event (it ignores them). */
void od_pads_handle_event(od_pads *pads, const SDL_Event *ev);

/* Sample every controller and the keyboard into `frame` (ORs into
 * frame->nes_buttons, appends the action rising edges to frame->actions).
 * Call it once per simulated tick, after the SDL event pump. */
void od_pads_poll(od_pads *pads, od_input_frame *frame);
/* Event-driven host keyboard remains authoritative (including editor F5).
 * This variant never reads keys or duplicates modern keyboard actions. */
void od_pads_poll_controllers(od_pads *pads, od_input_frame *frame);

/* Re-read the `[input]` section of the settings file. */
void od_pads_reload(od_pads *pads);

/* Persist the current bindings back to the settings file. */
od_status od_pads_save(const od_pads *pads);

/* The logical key for an SDL scancode, or -1. Used by the Settings screen to
 * capture a key ("press the key for..."). */
int od_key_from_scancode(int scancode);

/* A short human-readable name for a scancode ("Z", "F5", "LShift"), never
 * NULL. Used by the Settings screen. */
const char *od_scancode_name(int scancode);

#ifdef __cplusplus
}
#endif

#endif /* OD_INPUT_PAD_H */
