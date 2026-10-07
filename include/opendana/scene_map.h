/* scene_map.h - the WORLD map as logic (C3.2).
 *
 * The original's WORLD map is a focus state, not a recorded clip. `od_scene_map`
 * owns that state: the WORLD/ROUND focus, the focus world and round, the
 * password's own progress image (`words[]`) and the measured map-opening length
 * (`ready`). It is pure C11 and drives the scene from the remapped direction
 * bits and press edges only.
 *
 * A5: the world step follows the advance/retreat tables *and* is bounded by the
 * world limit; the level is computed through `od_scene_round_level`, which
 * refuses out-of-range indices. */
#ifndef OD_SCENE_MAP_H
#define OD_SCENE_MAP_H

#include <stdint.h>

#include "opendana/compose.h"
#include "opendana/palette.h"
#include "opendana/scene_round.h"

/* Panel sprite bank: '0' is tile 0x40, '9' is 0x49 (the extracted map layout
 * resolves the panel band's sprite tiles through this bank). */
#define OD_SCENE_MAP_DIGIT_TILE 0x40u
#define OD_SCENE_MAP_WORLD_NUM_X 144
#define OD_SCENE_MAP_WORLD_NUM_Y 168

typedef enum {
    OD_MAP_EVENT_NONE = 0,
    OD_MAP_EVENT_ENTER,     /* ROUND focus `A`: enter the selected round */
    OD_MAP_EVENT_MENU,      /* WORLD focus `B`: back to the menu */
    OD_MAP_EVENT_DATA_ERROR /* scene-data unavailable; tick leaves state unchanged */
} od_map_event;

/* The map's sprite artwork and castle overlay (content/base/screens/map.json,
 * produced by the importer). Sprite records are the original's own
 * `(dy, tile, attr, dx)` order. */
#define OD_MAP_OBJECTS 3        /* the sprite pass draws objects 0..2 */
#define OD_MAP_OBJECT_FRAMES 8
#define OD_MAP_LIST_MAX 16
#define OD_MAP_OVERLAY_MAX 128

typedef struct {
    uint8_t dy, tile, attr, dx;
} od_map_sprite;

typedef struct {
    uint8_t duration;                      /* animation frames shown */
    uint8_t count;
    od_map_sprite sprites[OD_MAP_LIST_MAX];
} od_map_frame;

typedef struct {
    uint8_t y, x;                          /* sprite base */
    uint8_t frame_count;
    od_map_frame frames[OD_MAP_OBJECT_FRAMES];
} od_map_object;

typedef struct {
    int loaded;
    uint8_t world_x[OD_MAP_WORLD_COUNT];  /* landmark of each world */
    uint8_t world_y[OD_MAP_WORLD_COUNT];  /* landmark y */
    uint8_t panel_x[OD_MAP_ROUND_COUNT];  /* round x on the panel */
    uint8_t panel_y;                       /* round y on the panel */
    uint8_t poses[4][8][2];                /* Hero: (tile, attr) x 8 */
    uint8_t frame_tile, frame_attr;        /* landmark frame */
    uint8_t frame_pal_entry;               /* the frame's palette entry */
    uint8_t frame_pal_colors[3];           /* colour cycle */
    uint8_t digit_y, digit_x;              /* WORLD number */
    od_map_object objects[OD_MAP_OBJECTS];
    uint8_t castle_y, castle_x, castle_count; /* panel castle sprites */
    od_map_sprite castle[OD_MAP_LIST_MAX];
    uint8_t castle_banks[4];               /* the four banks of the castle map  */
    unsigned overlay_count;                /* castle name-table cells (castle map) */
    uint16_t overlay_index[OD_MAP_OVERLAY_MAX];
    uint8_t overlay_value[OD_MAP_OVERLAY_MAX];
    /* C6.18: the map's palette and PPU setup, imported into
     * screens/map.json ("static"). The v1 package carried them in
     * layouts.json:map; a v2 package must supply them here or the map inherits
     * the previous scene's palette. `static_loaded` is 0 for the v1 document. */
    int static_loaded;
    uint8_t palette[32];                   /* display words 0..31            */
    uint8_t mask;                          /* display word 2092              */
    uint8_t bands;                         /* display word 2093              */
    uint16_t band_table[12];               /* display words 2094..2105       */
    uint16_t split_banks[4];               /* display words 2084..2087       */
    uint16_t sprite_banks[4];              /* display words 2088..2091       */
} od_map_art;

typedef struct {
    int focus;                 /* 0 = WORLD, 1 = ROUND                    */
    int world;                 /* 0-based focus world, 0..9               */
    int round;                 /* 0-based focus round, 0..9               */
    od_scene_round sel;       /* the round cursor                        */
    uint16_t words[OD_MAP_WORLD_COUNT]; /* progress image, low 10 bits/world */
    unsigned ready;            /* ticks the opening ignores input for     */
    unsigned age;              /* ticks since the map opened              */
    unsigned prev_buttons;     /* for press edges                         */
    /* Sprite-pass state: map-loop frames drawn since the init (cleared by the map
     * init), Hero's pose counter and the animation object states (frame timer,
     * frame index). */
    unsigned loop_calls;
    unsigned hero_pose;
    unsigned frame_pal_steps;  /* colour steps of this visit (the colour is the palette's own until the first;
                                * the colour counter itself is od_screens.frame_cycle, kept across scenes) */
    uint8_t obj_timer[OD_MAP_OBJECTS];
    uint8_t obj_frame[OD_MAP_OBJECTS];
} od_scene_map;

void od_scene_map_reset(od_scene_map *m);
/* C6.18 typed v2 CIRAM/CHR bases only, preserves live palette/control/OAM.
 * Missing v2 provider rejects without mutating the destination. */
od_status od_scene_map_static_base(const od_scene_map *m,uint16_t *display,unsigned words);

/* Open the map on `world` (clamped) with the password's own progress image and
 * the measured opening length. */
void od_scene_map_open(od_scene_map *m, const uint16_t words[OD_MAP_WORLD_COUNT],
                        int world, unsigned ready);

/* B in ROUND focus: back to WORLD focus. The layout is untouched, so the
 * return is immediate and never paints an intermediate gray frame (A7). */
void od_scene_map_focus_world(od_scene_map *m);

/* The map init with the "return" bit set resumes directly in ROUND focus on the
 * world's first uncleared round, unless that is round 10 and the castle is not
 * open yet (the castle latch: rounds 1-9 cleared and the previous world
 * complete). */
void od_scene_map_resume_round(od_scene_map *m);

/* One tick. Returns ENTER when the ROUND focus `A` accepts the selection. */
od_map_event od_scene_map_tick(od_scene_map *m, unsigned buttons);

/* The world step's bound: 9 worlds, 10 once world 9's castle is cleared (set by
 * the map opening, and after the castle cinematic). Progress does not bound the
 * step. */
int od_scene_map_world_limit(const od_scene_map *m);

/* Cleared bit of one round in one world (0/1). */
int od_scene_map_round_cleared(const od_scene_map *m, int world, int round);

/* Validated level index 0..99 for the current focus, or -1. */
int od_scene_map_level(const od_scene_map *m);

/* The round Hero's cursor stands on: the focus round in ROUND focus, the first
 * uncleared round of the world in WORLD focus. */
int od_scene_map_cursor_round(const od_scene_map *m);

/* Patch the frozen map layout with the logic panel: the `WORLD n` number and
 * the cleared/uncleared palettes of the nine round markers. `display` is a
 * OD_SCENE_WORDS-word copy of the extracted map layout. */
void od_scene_map_panel(const od_scene_map *m, uint16_t *display,
                         unsigned display_words);

/* World 9's castle cleared (bit 7 of its high byte): the map opening runs the
 * castle map script instead of the ordinary one, which draws Boss's castle, and
 * the world limit becomes 10, opening WORLD 10. */
int od_scene_map_castle_open(const od_scene_map *m);

/* Apply the castle overlay to a display copy of the map layout (no-op unless
 * `od_scene_map_castle_open`). */
void od_scene_map_castle(const od_scene_map *m, const od_map_art *art,
                          uint16_t *display, unsigned display_words);

/* One sprite pass: Hero's pose step (on the frames whose frame counter is a
 * multiple of 8; `frame_tick` is the native counter, od_screens.frame_tick) and
 * the object timers, for the objects this frame draws. Returns 1 when the frame
 * also runs the frame-colour step that advances the colour counter
 * (od_screens.frame_cycle). */
int od_scene_map_step_sprites(od_scene_map *m, const od_map_art *art,
                               unsigned frame_tick);

/* The map's OAM as the sprite pass leaves it: Hero (at the world landmark in
 * WORLD focus, on the panel round in ROUND focus), the landmark frame, the WORLD
 * number, the objects and the panel castle. Replaces the sprite list of
 * `display`; nothing is drawn before the first call. */
void od_scene_map_sprites(const od_scene_map *m, const od_map_art *art,
                           unsigned frame_cycle, uint16_t *display,
                           unsigned display_words);

/* Draw Hero at the cursor marker on top of the composed frame. */
void od_scene_map_draw_cursor(const od_scene_map *m, od_index_frame *fb,
                               const uint8_t *chr, const od_palette_ram *pal);

#endif /* OD_SCENE_MAP_H */
