/* input_map.h - pure C11 model of the input translation.
 *
 * This module owns the whole path between a physical controller (or a keyboard
 * key) and the two things the rest of the program understands:
 *
 *   - the 8-bit NES button mask (`OD_BTN_*`, include/opendana/game.h), which is the
 *     only input that ever reaches the simulation. Replays and the reference use
 *     the same mask, so a controller must never invent a new kind of event;
 *   - the modern interface actions (`od_action`), detected on the rising edge
 *     of their binding. Those never reach `od_flow_step`.
 *
 * It has no SDL dependency: the SDL front end (`src/app/input_pad.c`) turns
 * `SDL_GameController` state into `od_pad_state` and logical keys into
 * `od_key` bits, and calls the functions here.
 *
 * The default controller mapping is by POSITION, as decided by the owner (D2):
 * the right-hand button is NES A and the bottom button is NES B, whatever the
 * label printed on the pad. It is reassignable and persisted in settings.ini.
 *
 * The game's original keyboard bindings (arrow keys, Z/Space, X, Enter, right
 * Shift) are unchanged; the reassignment here only owns the modern shortcuts.
 */
#ifndef OD_EXT_INPUT_MAP_H
#define OD_EXT_INPUT_MAP_H

#include "opendana/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Interface actions. OD_ACT_NONE is the "unbound" value; the rest are also
 * used as a 1-based bit index by OD_ACT_BIT. */
typedef enum {
    OD_ACT_NONE = 0,
    OD_ACT_MENU,
    OD_ACT_STATE_SAVE,
    OD_ACT_STATE_LOAD,
    OD_ACT_SLOT_PREV,
    OD_ACT_SLOT_NEXT,
    OD_ACT_MUTE,
    OD_ACT_RESET,
    OD_ACT_FULLSCREEN,
    OD_ACT_SCREENSHOT,
    OD_ACT_HELP,
    OD_ACT_QUIT
} od_action;

#define OD_ACTION_COUNT (OD_ACT_QUIT + 1)

/* One bit per action, indexed from 1 so NONE has no bit. */
#define OD_ACT_BIT(a) ((a) > OD_ACT_NONE ? (1u << ((unsigned)(a) - 1u)) : 0u)

/* Per-frame input frame handed to the interface. `nes_buttons`
 * is the only field the simulation reads. `actions` are this frame's rising
 * edges, most recent last; `n_actions` their count. The pointer fields are for
 * the mouse-driven interface and are filled by the SDL front end. */
typedef struct {
    unsigned nes_buttons;
    od_action actions[8];
    int n_actions;
    int ptr_x, ptr_y, ptr_buttons, ptr_pressed, ptr_released, wheel;
    /* C6.32 (UI-24): what the controllers mean for the modern interface, by
     * position and independent of the NES mapping (od_pad_ui_buttons). Never
     * read by the simulation. */
    unsigned ui_buttons;
} od_input_frame;

/* ------------------------------------------------------------------------- */
/* Physical controller buttons, named by POSITION (D2), in the order SDL's   */
/* SDL_GameControllerButton uses for the first few.                          */
/* ------------------------------------------------------------------------- */
typedef enum {
    OD_PAD_SOUTH = 0,   /* bottom face button (Xbox A / PS cross) */
    OD_PAD_EAST,        /* right face button (Xbox B / PS circle) */
    OD_PAD_WEST,        /* left face button (Xbox X / PS square) */
    OD_PAD_NORTH,       /* top face button (Xbox Y / PS triangle) */
    OD_PAD_BACK,        /* Back / Select / Share                    */
    OD_PAD_GUIDE,       /* Guide / Home / PS button                 */
    OD_PAD_START,       /* Start / Options / Plus                   */
    OD_PAD_LSTICK,      /* left stick click                         */
    OD_PAD_RSTICK,      /* right stick click                        */
    OD_PAD_LB,          /* left shoulder                            */
    OD_PAD_RB,          /* right shoulder                           */
    OD_PAD_DPAD_UP,
    OD_PAD_DPAD_DOWN,
    OD_PAD_DPAD_LEFT,
    OD_PAD_DPAD_RIGHT,
    OD_PAD_COUNT
} od_pad_button;

#define OD_PAD_BIT(b) (1u << (unsigned)(b))
#define OD_PAD_DOWN(state, b) (((state)->buttons & OD_PAD_BIT(b)) != 0u)

/* Raw controller state. `buttons` is a bitmask of OD_PAD_BIT. Axes are the
 * raw SDL values (-32768..32767). `axis_dpad` is the NES d-pad bits the left
 * stick currently resolves to; it is carried between frames so the hysteresis
 * is deterministic (call od_pad_resolve_axes once per frame). */
typedef struct {
    unsigned buttons;
    int axis_left_x, axis_left_y;
    int axis_right_x, axis_right_y;
    unsigned axis_dpad;
} od_pad_state;

/* What a physical button (or a logical key) does. At most one destination:
 * nothing, one NES button, or one interface action. */
typedef enum {
    OD_BIND_NONE = 0,
    OD_BIND_NES,
    OD_BIND_ACTION
} od_bind_kind;

typedef struct {
    unsigned char kind;   /* od_bind_kind */
    unsigned short value; /* OD_BTN_* when NES, od_action when ACTION */
} od_binding;

/* Controller configuration. `deadzone` and `hysteresis` are millesimals of the
 * full deflection (500 = 0.5, 100 = 0.1). `axis_dpad` enables the left stick
 * as a d-pad (inclusive of the physical d-pad). */
typedef struct {
    od_binding bind[OD_PAD_COUNT];
    int deadzone;    /* engage threshold, default 500 */
    int hysteresis;  /* release is deadzone - hysteresis, default 100 */
    int axis_dpad;   /* default 1 */
} od_pad_map;

/* Logical keyboard keys the pure model understands. `od_keys_to_nes` and
 * `od_keys_actions` take a bitmask of OD_KEY_BIT over these. The SDL front
 * end maps scancodes to them (`od_key_from_scancode`). */
typedef enum {
    OD_KEY_UP = 0,
    OD_KEY_DOWN,
    OD_KEY_LEFT,
    OD_KEY_RIGHT,
    OD_KEY_A,
    OD_KEY_B,
    OD_KEY_START,
    OD_KEY_SELECT,
    OD_KEY_MENU,
    OD_KEY_STATE_SAVE,
    OD_KEY_STATE_LOAD,
    OD_KEY_SLOT_PREV,
    OD_KEY_SLOT_NEXT,
    OD_KEY_MUTE,
    OD_KEY_RESET,
    OD_KEY_FULLSCREEN,
    OD_KEY_SCREENSHOT,
    OD_KEY_HELP,
    OD_KEY_QUIT,
    OD_KEY_COUNT
} od_key;

#define OD_KEY_BIT(k) (1u << (unsigned)(k))
#define OD_KEY_PRESSED(mask, k) (((mask) & OD_KEY_BIT(k)) != 0u)

typedef struct {
    od_binding bind[OD_KEY_COUNT];
} od_key_map;

/* Complete persistent configuration (settings.ini `[input]`). */
typedef struct {
    od_pad_map pad;
    od_key_map keymap;
} od_input_config;

/* ------------------------------------------------------------------------- */
/* Defaults and pure translation                                            */
/* ------------------------------------------------------------------------- */

/* By-position defaults (D2): SOUTH -> NES B, EAST -> NES A, START -> START,
 * BACK -> SELECT, GUIDE -> menu, d-pad -> d-pad, left stick -> d-pad with
 * deadzone 0.5 and hysteresis 0.1. */
void od_pad_map_default(od_pad_map *m);
void od_key_map_default(od_key_map *m);
void od_input_config_default(od_input_config *c);

/* Resolve the left stick into `s->axis_dpad` (NES d-pad bits), applying
 * deadzone and hysteresis. Call once per frame before od_pad_to_nes. */
void od_pad_resolve_axes(od_pad_state *s, const od_pad_map *m);

/* The NES mask produced by a controller state and map. Only OD_BTN_* bits can
 * come out, so a controller can never feed anything else to the simulation. */
unsigned od_pad_to_nes(const od_pad_state *s, const od_pad_map *m);

/* The action bitmask (OD_ACT_BIT) currently down on the controller. */
unsigned od_pad_actions(const od_pad_state *s, const od_pad_map *m);

/* The interface meaning of a controller state (C6.32, UI-24): the d-pad and
 * the resolved left stick as OD_BTN_UP/DOWN/LEFT/RIGHT, "accept" as
 * OD_BTN_A (the bottom face button, or Start) and "back" as OD_BTN_B (the
 * right face button), as the platforms' own menus do. The NES mapping puts the
 * bottom button on NES B, which used to make "accept" go back. */
unsigned od_pad_ui_buttons(const od_pad_state *s);

/* Same, for a logical keyboard bitmask. */
unsigned od_keys_to_nes(unsigned keys, const od_key_map *m);
unsigned od_keys_actions(unsigned keys, const od_key_map *m);

/* Rising edges: set bits in `current` that are not in `previous`. Writes the
 * actions to `out` (at most `max`) and returns how many. */
int od_actions_flank(unsigned current, unsigned previous, od_action *out, int max);

/* ------------------------------------------------------------------------- */
/* Reassignment                                                             */
/* ------------------------------------------------------------------------- */
void od_binding_clear(od_binding *b);
int  od_binding_set_nes(od_binding *b, unsigned nes);      /* 0 if not a NES bit */
int  od_binding_set_action(od_binding *b, od_action a);   /* 0 if not an action */

/* Assign a destination to a controller button. The map stays a function: any
 * other button bound to the same NES function or action is cleared. */
void od_pad_rebind(od_pad_map *m, od_pad_button b, const od_binding *bind);
void od_key_rebind(od_key_map *m, od_key k, const od_binding *bind);

/* ------------------------------------------------------------------------- */
/* Names and persistence (settings.ini `[input]`)                           */
/* ------------------------------------------------------------------------- */
const char *od_pad_button_name(od_pad_button b);   /* "south", ...        */
int od_pad_button_from_name(const char *name);      /* index, or -1        */
const char *od_key_name(od_key k);                 /* "a", "menu", ...    */
int od_key_from_name(const char *name);             /* index, or -1        */
const char *od_nes_name(unsigned nes);              /* "nes_a".."nes_right"*/
unsigned od_nes_from_name(const char *name);        /* bit, or 0           */
const char *od_action_name(od_action a);           /* "menu", ...         */
od_action od_action_from_name(const char *name);   /* NONE if unknown     */
/* Serialize/parse one binding as "none" | "nes_*" | "<action>". */
int od_binding_serialize(const od_binding *b, char *out, size_t n);
od_status od_binding_parse(const char *text, od_binding *out);

/* Load/save the `[input]` keys of settings.ini. Loading a missing file leaves
 * the defaults in place and returns OD_OK. Saving preserves every other line
 * and section of the file, writes atomically and only touches the keys this
 * module owns. */
od_status od_input_config_load(od_input_config *c, const char *path);
od_status od_input_config_save(const od_input_config *c, const char *path);
/* C6.32 (UI-16): write `base_text` (the settings of settings.c) with the
 * [input] block of `c` merged in, as ONE atomic replace of `path`, so saving
 * the settings never drops the bindings, not even for a moment. */
od_status od_input_config_save_with(const od_input_config *c, const char *path,
                                      const char *base_text);

#ifdef __cplusplus
}
#endif

#endif /* OD_EXT_INPUT_MAP_H */
