/* scene_whatnext.h - the "What next?" menu as logic (C3.3).
 *
 * After a valid magicword or after the editor's EXIT, the original shows the
 * "What next?" menu:
 *
 *     *What next?          cont.story
 *                          quit
 *                          edit mode
 *
 * over the same hearth illustration as the first menu. The illustration and the
 * box frames are the extracted reference layout (`assets/screens/whatnext.track`,
 * frozen at f11800); this module drives the three-option cursor, the press-edge
 * navigation and the selection, exactly like the game's first menu. Pure C11,
 * no SDL. The first menu does not accept START, and this one is the same
 * routine: only UP/DOWN/A are read. */
#ifndef OD_SCENE_WHATNEXT_H
#define OD_SCENE_WHATNEXT_H

#include "opendana/common.h"
#include "opendana/scene_map.h"

#define OD_WHATNEXT_OPTIONS 3

/* Measured menu-close hold. On the PAL password route the `cont.story` A is
 * read at reference f12200 and the settled "What next?" screen is still shown
 * through f12274; the map's transition starts at f12275. The confirm is latched
 * and delivered after this many ticks, so the screen stays for the same window. */
#define OD_WHATNEXT_EXIT_TICKS 75u

/* `edit mode` runs the same 60-frame close and 15-frame fade from either menu
 * (A +75: the round starts, +362: the editor; first menu A f1700 -> f1775,
 * f2062; "What next?" A f12200 -> f12275, f12562). The editor clip was
 * recorded from the first menu (f1701..): "What next?" has already played the
 * close and the fade, so its `edit mode` enters the clip at this tick. */
#define OD_WHATNEXT_EDITOR_CLIP_SKIP 75u

typedef enum {
    OD_WHATNEXT_NONE = 0,
    OD_WHATNEXT_CONT_STORY,  /* `cont.story`: enter the WORLD map          */
    OD_WHATNEXT_QUIT,        /* `quit`: "Today's magicword" (not in C3.3)  */
    OD_WHATNEXT_EDIT_MODE    /* `edit mode`: enter the level editor        */
} od_whatnext_event;

typedef struct {
    unsigned selection;          /* 0 = cont.story, 1 = quit, 2 = edit mode  */
    unsigned visible_selection;  /* drawn this tick (one-tick PPU latch)      */
    unsigned previous_buttons;   /* last frame's buttons, for press edges     */
    unsigned age;                /* ticks since the scene was entered         */
    unsigned leaving;            /* remaining ticks of the menu-close hold    */
    od_whatnext_event pending;  /* event latched at confirm, delivered later */
} od_scene_whatnext;

/* Reset the menu to `cont.story` with no held buttons. */
void od_scene_whatnext_reset(od_scene_whatnext *w);
/* Same, for the scene entry point. */
void od_scene_whatnext_begin(od_scene_whatnext *w);

/* Advance one tick with the native OD_BTN_* mask. The cursor moves on the
 * press edge and A confirms the visible selection. Returns the menu event. */
od_whatnext_event od_scene_whatnext_tick(od_scene_whatnext *w,
                                           unsigned buttons);

/* Patch the option cursor into the extracted maqueta (`display` is the
 * OD_SCENE_WORDS vector): the arrow tile sits at nametable column 17 of
 * the selected option's row (19, 21 or 23). */
void od_scene_whatnext_panel(const od_scene_whatnext *w, uint16_t *display,
                              unsigned display_words);

/* Option row (nametable) of `option` (0..2): 19, 21, 23. */
unsigned od_scene_whatnext_row(unsigned option);

/* ---- the hearth illustration (WHATNEXT) ---------------------------------
 *
 * "Where to begin from?" and "What next?" share one loop that calls the hearth
 * animation once per frame (the close variant in the 60-frame close after the
 * A). It draws the sprite groups (the close set after the A), steps and draws
 * animation object 3 (the fire) and, every 16 frames, swaps the background
 * bank. The artwork comes from the package (screens/hearth.json); the state
 * lives across scenes like the state it mirrors: the first menu leaves it to
 * "What next?", the map init clears it. The sprite writer follows a rotation
 * mode that advances once per loop iteration. */
#define OD_HEARTH_GROUPS 8

typedef struct {
    uint8_t y, x, count;
    od_map_sprite sprites[OD_MAP_LIST_MAX];
} od_hearth_group;

/* One VRAM record: the low six bits of `count` give the byte count from `addr`, +1 per byte, or +32 when
 * bit 7 is set (the box columns). */
#define OD_MENU_RECORD_BYTES 63
#define OD_MENU_LAYOUT_RECORDS 16
typedef struct {
    uint8_t count;
    uint16_t addr;
    uint8_t data[OD_MENU_RECORD_BYTES];
} od_menu_record;

/* A layout: the records of its lists, in order. */
typedef struct {
    uint8_t record_count;
    od_menu_record records[OD_MENU_LAYOUT_RECORDS];
} od_menu_layout;

#define OD_MENU_LAYOUT_BEGIN    0  /* "Where to begin from?" */
#define OD_MENU_LAYOUT_WHATNEXT 1  /* "What next?" */

typedef struct {
    int loaded;
    od_hearth_group groups[2][OD_HEARTH_GROUPS]; /* [0] sprite groups, [1] close set */
    uint8_t group_count[2];
    od_map_object fire;                           /* animation object 3 */
    uint8_t bank_slot;                             /* the slot after the first bank */
    uint8_t bank_period;                           /* frames per bank step       */
    uint8_t bank_values[2];                        /* the two alternating values */
    /* WHATNEXT-FADE: the panel's layouts and the queue limit (0 when the package
     * has none: the panel is then shown settled). */
    uint8_t queue_capacity;                        /* queue limit: 60 */
    od_menu_layout layouts[2];
    /* PISTAS-B: the menu screen under the panel, from the imported data: the
     * illustration (16 blocks of 4x4 tiles), its attribute block, the palettes and
     * the screen's PPU words (tile banks, mask, the y=128 split) captured from the
     * reference. 0 when the package has none. */
    int has_screen;
    uint8_t palette[32];
    uint8_t illus_x, illus_y;
    uint8_t illus[16][16];
    uint8_t attr_fill, attr_x, attr_y;
    uint8_t attr[4][4];
    uint8_t ppu_count;
    uint16_t ppu_field[24], ppu_value[24];
} od_hearth_art;

/* Write one VRAM record (count|flags, PPU address, bytes; bit 7 = +32 per byte)
 * into an OD_SCENE_WORDS display's nametables. */
void od_menu_record_write(const od_menu_record *r, uint16_t *display,
                           unsigned display_words);

/* screens/password.json (PISTAS-B): the magicword box's message records,
 * "Enter your magicword." and "Error! try again." (produced by the importer). */
typedef struct {
    int loaded;
    od_menu_record prompt, error;
} od_password_art;

/* Build the settled "What next?" screen into an OD_SCENE_WORDS display: the menu
 * screen above and the whole panel. The OAM is left to the hearth's calls
 * (od_hearth_patch). Returns 0 without the screen data. */
int od_hearth_screen(const od_hearth_art *art, uint16_t *display,
                      unsigned display_words);

typedef struct {
    uint8_t fire_timer, fire_frame;  /* fire timer and frame */
    uint8_t bank_timer, bank_value;  /* bank timer and the published bank value */
    uint8_t rotation;                /* low two bits, used by the next call */
    uint8_t drawn_rotation;          /* low two bits the last call wrote with */
    uint8_t drawn_confirmed;         /* the last call drew the close set */
    uint8_t calls;                   /* nonzero once a call has drawn */
} od_hearth;

/* Power-on / map init: every counter back to zero. */
void od_hearth_reset(od_hearth *h);

/* One hearth call (the close variant when `confirmed`): fire object 3, then the
 * bank swap; the OAM it writes is shown on the next frame (od_hearth_patch). */
void od_hearth_call(od_hearth *h, const od_hearth_art *art, int confirmed);

/* Write the sprites of the last call (in the order of its rotation mode) and
 * the bank register it published into an OD_SCENE_WORDS display. No-op before
 * the first call or without the artwork. */
void od_hearth_patch(const od_hearth *h, const od_hearth_art *art,
                      uint16_t *display, unsigned display_words);

/* The menu loop's hearth calls before the A, counted in frames from the
 * reference's mode-3 frame (0): from frame 6 for 17 frames (the draw loop),
 * then, after the menu-text upload (`text_frames`), every frame. The init picks
 * the text: "What next?" when any progress byte is set, "Where to begin from?"
 * otherwise (measured: 2 and 3 frames). */
#define OD_HEARTH_TEXT_WHATNEXT 2u
#define OD_HEARTH_TEXT_BEGIN    3u
int od_hearth_menu_frame(int frame, unsigned text_frames);

/* ---- the menu's entry (WHATNEXT-FADE) ----------------------------------
 *
 * Counted, like od_hearth_menu_frame, in frames from the reference's mode-3
 * frame (0). The first call (frame 6) sets both brightness bytes to 0 and the
 * fade control to its start value, and the fade driver raises both brightness bytes by one
 * per frame up to 15 (reference brightness = 1 at f11766 .. 15 at f11780); the
 * palette reaches the screen one display pass later. Before that the mode's init
 * shows black. When the fade is done (the 17th call, frame 22) the panel's
 * layout is run through the VRAM queue, a CPU frame's records on the next frame
 * (reference f11783: the box edges; f11784: the rest of the boxes and "*What
 * next?"; f11785: the options). */
#define OD_MENU_FADE_FIRST 6
#define OD_MENU_LAYOUT_FRAME 22

/* Brightness of the frame shown at `frame` (0..15; 15 = the settled
 * palette): 0 through frame 6, then frame - 6, then 15. */
int od_menu_entry_brightness(int frame);

/* How many records of `layout` are on screen at `frame`: the records the queue
 * (limit `capacity`) took in the CPU frames OD_MENU_LAYOUT_FRAME .. frame - 1.
 * All of them once the layout is done. */
unsigned od_menu_layout_visible(const od_menu_layout *layout,
                                 unsigned capacity, int frame);

/* Draw the panel as it stands at `frame` into an OD_SCENE_WORDS display that
 * holds the settled layout: the tiles of the records not yet uploaded are
 * blank (tile 0, as the mode's init left the nametable; the attribute bytes
 * already hold the layout's values). Up to the frame that shows the last
 * records the option rows are the layout's own (no cursor yet). Returns 1
 * when it changed the display. */
int od_menu_layout_draw(const od_menu_layout *layout, unsigned capacity,
                         int frame, uint16_t *display, unsigned display_words);

#endif /* OD_SCENE_WHATNEXT_H */
