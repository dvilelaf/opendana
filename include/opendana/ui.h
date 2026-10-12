/* ui.h - modern overlay interface (C5.1).
 *
 * The overlay is a small software rasteriser: widgets paint into a 256x240
 * ARGB canvas (alpha 0 = transparent) that the SDL layer uploads as a texture
 * over the already-scaled NES frame. Every text is cut or wrapped to its box
 * (C6.32); the editor and the catalogue have their own 384x360 canvas
 * (C6.24). Keeping the pixels in our own buffer
 * makes the menu capture deterministic and independent of the renderer backend.
 *
 * The menu is modal: while od_ui_modal() is true the host does not advance the
 * simulation. The product's ESC opens the menu and Quit
 * lives inside it (D1). */
#ifndef OD_UI_H
#define OD_UI_H

#include "opendana/ext/settings.h"
#include "opendana/ext/state.h"
#include "opendana/render_sdl.h"
#include "opendana/ui_catalog.h"
#include "opendana/ui_editor.h"

#include <stdint.h>

#define OD_UI_SCREEN_W 256
#define OD_UI_SCREEN_H 240
/* Interface revision: 632 = C6.32 (confirmations, slot scrolling, catalogue
 * selection, text input); 640 = C6.40 (OD_UI_CONTROLS, OD_UI_KEY_CONTROLS,
 * Controls menu entry, settings controls_hint_shown); 646 = C6.46 (OD_UI_CONFIRM_DESKTOP,
 * desktop_action/desktop_installed, settings desktop_prompted). The UI tests build against older trees too, as
 * their positive control, and test this to know which API exists. */
#define OD_UI_REVISION 646

typedef enum {
    OD_UI_HIDDEN = 0,
    OD_UI_MENU,
    OD_UI_SETTINGS,
    OD_UI_STATES,
    OD_UI_EDITOR,       /* C5.5: mouse level editor                */
    OD_UI_CATALOG,      /* C5.5: custom-level browser              */
    OD_UI_CONTROLS      /* C6.40: the controls table (F1 or menu)  */
} od_ui_screen;

typedef enum {
    OD_UI_KEY_NONE = 0,
    OD_UI_KEY_UP,
    OD_UI_KEY_DOWN,
    OD_UI_KEY_LEFT,
    OD_UI_KEY_RIGHT,
    OD_UI_KEY_ACCEPT,   /* Enter / A                                  */
    OD_UI_KEY_BACK,     /* Esc / B                                    */
    OD_UI_KEY_MUTE,     /* M                                           */
    OD_UI_KEY_SAVE,     /* F5 / S: save the state to the current slot */
    OD_UI_KEY_LOAD,     /* F9: load the current slot                  */
    OD_UI_KEY_DELETE,   /* Del: delete the current slot               */
    /* C5.5 editor keys. */
    OD_UI_KEY_UNDO,       /* Ctrl+Z                                   */
    OD_UI_KEY_REDO,       /* Ctrl+Y                                   */
    OD_UI_KEY_EDITOR_SAVE,/* Ctrl+S                                   */
    OD_UI_KEY_EDITOR_TEST,/* F5 inside the editor                     */
    OD_UI_KEY_PALETTE,    /* P: show/hide the palette panel           */
    OD_UI_KEY_CONTROLS    /* C6.40: F1 opens/closes the controls screen */
} od_ui_key;

/* What the host must do after a key/click: none, or one state operation on
 * `state_slot`. The host owns the flow and performs it, then clears it. */
enum { OD_UI_STATE_NONE = 0, OD_UI_STATE_SAVE, OD_UI_STATE_LOAD,
       OD_UI_STATE_DELETE };
enum { OD_UI_CONFIRM_NONE = 0, OD_UI_CONFIRM_DELETE, OD_UI_CONFIRM_OVERWRITE,
       OD_UI_CONFIRM_NEW_GAME, OD_UI_CONFIRM_DESKTOP };
/* C6.46: what the host must do about the desktop launcher. */
enum { OD_UI_DESKTOP_NONE = 0, OD_UI_DESKTOP_INSTALL, OD_UI_DESKTOP_UNINSTALL };
/* Rows of the states list visible at once (the rest scroll). */
#define OD_UI_STATES_VISIBLE 7

typedef struct od_ui {
    od_settings *settings;      /* borrowed; the UI edits it in place      */
    od_ui_screen screen;
    int selected;                /* row of the current screen               */
    int quit;                    /* Quit was chosen: the app should exit    */
    int save_settings;           /* some value changed; persist settings.ini*/
    int apply_video;             /* fullscreen changed; host reapplies it   */

    char toast[128];
    unsigned toast_ms;

    /* States screen (C5.3): the slot list is peeked from disk when the screen
     * opens; the host performs the requested operation on the flow. */
    int slot_focus;
    od_state_info slot_info[OD_SETTINGS_STATES_MAX];
    int slot_occupied[OD_SETTINGS_STATES_MAX];
    /* UI-only cache: never serialized into a game state. Refresh invalidates
     * every slot after save/load/delete; pixels own no renderer resources. */
    struct { int w, h; uint32_t pixels[14 * 14]; }
        slot_thumbnails[OD_SETTINGS_STATES_MAX];
    int state_action;            /* OD_UI_STATE_* requested                */
    int state_slot;              /* slot of the requested action            */
    char state_status[128];       /* last operation result, shown in the UI  */
    /* C6.32: the list scrolls (up to OD_SETTINGS_STATES_MAX slots), the
     * keyboard or a pad can reach the buttons (-1 = the list), and deleting
     * or overwriting a slot waits for a confirmation (OD_UI_CONFIRM_*). */
    int slot_scroll;
    int states_button;
    int confirm;
    int confirm_slot;
    int confirm_yes;             /* the focused answer; No (0) by default   */

    /* "New game" was confirmed in the menu (owner decision, 2026-10-06): the
     * host starts a new game and clears this. Set only after Yes. */
    int new_game;

    /* C6.46: the Settings row asks the host to add or remove the launcher
     * (OD_UI_DESKTOP_*); the host owns the files and keeps the state below. */
    int desktop_action;
    int desktop_installed;

    /* C6.40: where the controls screen returns to (hidden = the game). */
    od_ui_screen controls_return;

    /* Pointer input (C5.3), in 256x240 canvas coordinates (the editor and the
     * catalogue use their own 384x360 canvas, C6.24). */
    int ptr_x, ptr_y, ptr_buttons, ptr_wheel, ptr_shift, ptr_ctrl;

    /* C5.5 screens: borrowed, owned by the host. */
    od_ui_editor *editor;
    od_ui_catalog *catalog;

    uint32_t canvas[OD_UI_SCREEN_W * OD_UI_SCREEN_H];
    int canvas_active;
    SDL_Texture *overlay;        /* lazily created on the renderer          */
} od_ui;

void od_ui_init(od_ui *ui, od_settings *settings);
void od_ui_shutdown(od_ui *ui);

/* Feed one logical key. Returns 1 when the UI consumed it. Delete acts only
 * in the states screen, on the focused slot, after a confirmation (C6.32). */
int od_ui_handle_key(od_ui *ui, od_ui_key key);

/* Move the pointer over the canvas and, when `click` is non-zero, activate
 * the item under it (C5.3: the menu and the states screen are mouse driven).
 * Coordinates are 256x240 canvas pixels. Returns 1 when a click was consumed. */
int od_ui_pointer(od_ui *ui, int x, int y, int click);

/* C5.5: the full pointer sample (held buttons, wheel, modifiers) used by the
 * editor and catalogue screens. `click` is 1 on the frame a button went down. */
int od_ui_pointer2(od_ui *ui, int x, int y, int buttons, int click,
                    int wheel, int shift, int ctrl);

/* Re-read the slot files (called when the states screen opens and after an
 * operation). `current` selects the focused slot. */
void od_ui_states_refresh(od_ui *ui, int current);

/* Set the status line of the states screen (the host reports save/load). */
void od_ui_states_status(od_ui *ui, const char *text, unsigned toast_ms);

/* 1 when a modal screen is open: the host must not step the simulation. */
int od_ui_modal(const od_ui *ui);

/* 1 when the canvas has something to draw (menu or toast). */
int od_ui_visible(const od_ui *ui);

/* Show a transient message (English, from the string table). */
void od_ui_toast(od_ui *ui, const char *text, unsigned ms);

/* Advance the toast timer by `elapsed_ms`. */
void od_ui_tick(od_ui *ui, unsigned elapsed_ms);

/* Rasterise the current screen into `canvas` (alpha 0 = transparent). */
void od_ui_draw(od_ui *ui);

/* Upload `game_argb` (256x240) and the overlay, then present through `r`. */
od_status od_ui_present(od_ui *ui, od_renderer *r, const uint32_t *game_argb);

#endif /* OD_UI_H */
