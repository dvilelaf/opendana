/* input_pad.c - SDL2 game controller front end (C5.2).
 *
 * Opens up to OD_PAD_MAX_PLAYERS controllers, follows hot-plug events and
 * samples them once per frame. Everything is translated by the pure model in
 * `src/ext/input_map.c`, so the only thing that can reach the simulation is the
 * 8-bit NES mask. The keyboard keeps its original bindings and goes through the
 * same logical key map (`[input]` of settings.ini).
 */
#include "opendana/input_pad.h"

#include "opendana/game.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Physical position -> SDL controller button. The positions are the meaning
 * (D2): SOUTH is the bottom button, EAST the right-hand one. */
static const SDL_GameControllerButton PAD_TO_SDL[OD_PAD_COUNT] = {
    [OD_PAD_SOUTH] = SDL_CONTROLLER_BUTTON_A,
    [OD_PAD_EAST] = SDL_CONTROLLER_BUTTON_B,
    [OD_PAD_WEST] = SDL_CONTROLLER_BUTTON_X,
    [OD_PAD_NORTH] = SDL_CONTROLLER_BUTTON_Y,
    [OD_PAD_BACK] = SDL_CONTROLLER_BUTTON_BACK,
    [OD_PAD_GUIDE] = SDL_CONTROLLER_BUTTON_GUIDE,
    [OD_PAD_START] = SDL_CONTROLLER_BUTTON_START,
    [OD_PAD_LSTICK] = SDL_CONTROLLER_BUTTON_LEFTSTICK,
    [OD_PAD_RSTICK] = SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    [OD_PAD_LB] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    [OD_PAD_RB] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    [OD_PAD_DPAD_UP] = SDL_CONTROLLER_BUTTON_DPAD_UP,
    [OD_PAD_DPAD_DOWN] = SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    [OD_PAD_DPAD_LEFT] = SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    [OD_PAD_DPAD_RIGHT] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
};

/* The game's original keyboard bindings. Several keys can
 * share a logical key (Z and Space are both NES A). */
static const struct {
    SDL_Scancode sc;
    od_key key;
} KEY_TABLE[] = {
    { SDL_SCANCODE_UP, OD_KEY_UP },
    { SDL_SCANCODE_DOWN, OD_KEY_DOWN },
    { SDL_SCANCODE_LEFT, OD_KEY_LEFT },
    { SDL_SCANCODE_RIGHT, OD_KEY_RIGHT },
    { SDL_SCANCODE_Z, OD_KEY_A },
    { SDL_SCANCODE_SPACE, OD_KEY_A },
    { SDL_SCANCODE_X, OD_KEY_B },
    { SDL_SCANCODE_RETURN, OD_KEY_START },
    { SDL_SCANCODE_RSHIFT, OD_KEY_SELECT },
    { SDL_SCANCODE_ESCAPE, OD_KEY_MENU },
    { SDL_SCANCODE_F5, OD_KEY_STATE_SAVE },
    { SDL_SCANCODE_F9, OD_KEY_STATE_LOAD },
    { SDL_SCANCODE_F6, OD_KEY_SLOT_PREV },
    { SDL_SCANCODE_F7, OD_KEY_SLOT_NEXT },
    { SDL_SCANCODE_M, OD_KEY_MUTE },
    { SDL_SCANCODE_F2, OD_KEY_RESET },
    { SDL_SCANCODE_F11, OD_KEY_FULLSCREEN },
    { SDL_SCANCODE_F12, OD_KEY_SCREENSHOT },
};

int od_key_from_scancode(int scancode)
{
    for (size_t i = 0; i < sizeof KEY_TABLE / sizeof KEY_TABLE[0]; i++)
        if ((int)KEY_TABLE[i].sc == scancode)
            return (int)KEY_TABLE[i].key;
    return -1;
}

const char *od_scancode_name(int scancode)
{
    if (scancode < 0)
        return "?";
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    return (name != NULL && name[0] != '\0') ? name : "?";
}

static unsigned read_keyboard(const od_key_map *m)
{
    (void)m;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    unsigned keys = 0;
    if (ks == NULL)
        return 0;
    for (size_t i = 0; i < sizeof KEY_TABLE / sizeof KEY_TABLE[0]; i++)
        if (ks[KEY_TABLE[i].sc])
            keys |= OD_KEY_BIT(KEY_TABLE[i].key);
    return keys;
}

static int open_device(od_pads *pads, int device_index)
{
    if (!pads->ready || pads->n_slots >= OD_PAD_MAX_PLAYERS)
        return 0;
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    for (int i = 0; i < pads->n_slots; ++i)
        if (pads->slots[i].id == id) return 0;
    SDL_GameController *ctrl = SDL_GameControllerOpen(device_index);
    if (ctrl == NULL)
        return 0;
    SDL_Joystick *js = SDL_GameControllerGetJoystick(ctrl);
    int slot = pads->n_slots;
    pads->slots[slot].ctrl = ctrl;
    pads->slots[slot].id = SDL_JoystickInstanceID(js);
    pads->n_slots++;
    if (pads->active < 0)
        pads->active = slot;
    return 1;
}

static void close_slot(od_pads *pads, int slot)
{
    if (slot < 0 || slot >= pads->n_slots)
        return;
    if (pads->slots[slot].ctrl != NULL)
        SDL_GameControllerClose(pads->slots[slot].ctrl);
    /* Compact: move the last slot into the hole. */
    int last = pads->n_slots - 1;
    if (slot != last)
        pads->slots[slot] = pads->slots[last];
    pads->slots[last].ctrl = NULL;
    pads->slots[last].id = -1;
    pads->n_slots--;
    if (pads->active == slot)
        pads->active = pads->n_slots > 0 ? 0 : -1;
    else if (pads->active == last)
        pads->active = slot;
}

static int file_exists(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0;
}

od_status od_pads_init(od_pads *pads, const char *config_path,
                         const char *mapping_file)
{
    if (pads == NULL)
        return OD_ERR_RANGE;
    memset(pads, 0, sizeof *pads);
    pads->active = -1;
    for (int i = 0; i < OD_PAD_MAX_PLAYERS; i++)
        pads->slots[i].id = -1;
    if (config_path != NULL) {
        snprintf(pads->config_path, sizeof pads->config_path, "%s", config_path);
    }
    /* missing file: defaults stand. */
    od_input_config_load(&pads->config, config_path);

    if (mapping_file != NULL && file_exists(mapping_file))
        SDL_GameControllerAddMappingsFromFile(mapping_file);

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        /* No controller backend (e.g. the dummy video driver): the keyboard
         * still works, so this is not fatal. */
        pads->ready = 0;
        return OD_ERR_IO;
    }
    pads->ready = 1;
    SDL_GameControllerEventState(SDL_ENABLE);
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++)
        if (SDL_IsGameController(i))
            open_device(pads, i);
    return OD_OK;
}

void od_pads_close(od_pads *pads)
{
    if (pads == NULL)
        return;
    for (int i = 0; i < pads->n_slots; i++) {
        if (pads->slots[i].ctrl != NULL)
            SDL_GameControllerClose(pads->slots[i].ctrl);
        pads->slots[i].ctrl = NULL;
        pads->slots[i].id = -1;
    }
    pads->n_slots = 0;
    pads->active = -1;
    if (pads->ready) {
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        pads->ready = 0;
    }
}

void od_pads_handle_event(od_pads *pads, const SDL_Event *ev)
{
    if (pads == NULL || ev == NULL)
        return;
    if (ev->type == SDL_CONTROLLERDEVICEADDED) {
        open_device(pads, ev->cdevice.which);   /* which is a device index */
    } else if (ev->type == SDL_CONTROLLERDEVICEREMOVED) {
        SDL_JoystickID id = (SDL_JoystickID)ev->cdevice.which;
        for (int i = 0; i < pads->n_slots; i++) {
            if (pads->slots[i].id == id) {
                close_slot(pads, i);
                break;
            }
        }
    }
}

static void poll_inputs(od_pads *pads, od_input_frame *frame, int keyboard)
{
    if (pads == NULL || frame == NULL)
        return;
    unsigned nes = frame->nes_buttons;
    unsigned actions = 0;

    for (int i = 0; i < pads->n_slots; i++) {
        SDL_GameController *ctrl = pads->slots[i].ctrl;
        if (ctrl == NULL)
            continue;
        od_pad_state s;
        memset(&s, 0, sizeof s);
        for (int b = 0; b < OD_PAD_COUNT; b++)
            if (SDL_GameControllerGetButton(ctrl, PAD_TO_SDL[b]))
                s.buttons |= OD_PAD_BIT((od_pad_button)b);
        s.axis_left_x = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTX);
        s.axis_left_y = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTY);
        s.axis_right_x = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_RIGHTX);
        s.axis_right_y = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_RIGHTY);
        /* Keep the previous axis resolution so the hysteresis is continuous. */
        s.axis_dpad = pads->slots[i].axis_dpad;
        od_pad_resolve_axes(&s, &pads->config.pad);
        pads->slots[i].axis_dpad = s.axis_dpad;
        nes |= od_pad_to_nes(&s, &pads->config.pad);
        actions |= od_pad_actions(&s, &pads->config.pad);
        frame->ui_buttons |= od_pad_ui_buttons(&s);
        if (s.buttons != 0 || s.axis_dpad != 0)
            pads->active = i;               /* the last controller touched */
    }

    if (keyboard) {
        unsigned keys = read_keyboard(&pads->config.keymap);
        nes |= od_keys_to_nes(keys, &pads->config.keymap);
        actions |= od_keys_actions(keys, &pads->config.keymap);
    }

    frame->nes_buttons = nes;
    int room = 8 - frame->n_actions;
    if (room > 0) {
        frame->n_actions += od_actions_flank(actions, pads->prev_actions,
                                              frame->actions + frame->n_actions,
                                              room);
    }
    pads->prev_actions = actions;
}

void od_pads_poll(od_pads *pads, od_input_frame *frame)
{ poll_inputs(pads,frame,1); }

void od_pads_poll_controllers(od_pads *pads, od_input_frame *frame)
{ poll_inputs(pads,frame,0); }

void od_pads_reload(od_pads *pads)
{
    if (pads == NULL)
        return;
    od_input_config_load(&pads->config,
                          pads->config_path[0] ? pads->config_path : NULL);
}

od_status od_pads_save(const od_pads *pads)
{
    if (pads == NULL || pads->config_path[0] == '\0')
        return OD_ERR_IO;
    return od_input_config_save(&pads->config, pads->config_path);
}
