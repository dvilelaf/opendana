/* scene_hidden.h - the hidden-level access scene (A13, C3.6).
 *
 * The hidden rounds 100..149 (ROUND 11-1 .. 15-10) are not on the world map: on
 * the title screen, holding SELECT and pressing B ten times opens the original's
 * hidden menu ("ROUND 11-1 / SOUND 00"). A/B step the level, SELECT+A/B step a
 * whole world and START loads the selection.
 *
 * The measured model lives in `worldmap.c` (`od_hidden_menu` /
 * `od_hidden_menu_tick` / `od_hidden_menu_draw`); this module is the *scene*
 * wrapper that the sequence machine drives. It only carries state and forwards
 * to that model, so it stays independent of the flow's world/round numbering:
 * `od_hidden_scene_level_world_round()` is the single conversion (level
 * 100..149 -> world 10..14, round 0..9) the caller uses to hand the choice to
 * the existing level-entry path. Pure C11, no SDL. */
#ifndef OD_SCENE_HIDDEN_H
#define OD_SCENE_HIDDEN_H

#include "opendana/palette.h"
#include "opendana/worldmap.h"

/* The hidden-menu scene. `open` is 1 while the menu is the current scene (the
 * scene machine uses it to keep the menu state across ticks); `menu` is the
 * original's menu state (level / SOUND / the SELECT+B counter). */
typedef struct {
    od_hidden_menu menu;
    int open;
    /* Last held-buttons mask, kept so the scene computes its own press edges
     * (the flow's simulation clock is one tick per call). */
    unsigned previous;
    /* The tick the trigger fired, for the report/measurement output. */
    long trigger_tick;
} od_hidden_scene;

void od_hidden_scene_reset(od_hidden_scene *hs);

/* One tick while the title screen runs. Returns 1 on the tick the SELECT+B x10
 * trigger fires (the caller switches to the hidden scene), else 0. */
int od_hidden_scene_title_tick(od_hidden_scene *hs, unsigned buttons);

/* One tick of the open hidden menu. Returns the 100..149 level on the START
 * edge, else -1. */
int od_hidden_scene_menu_tick(od_hidden_scene *hs, unsigned buttons);

/* Draw the open hidden menu (black, "ROUND w-r" / "SOUND 00", blue cursor). */
void od_hidden_scene_draw(const od_hidden_scene *hs, od_index_frame *fb,
                           const uint8_t *chr, const od_palette_ram *pal);

/* Level 100..149 -> the flow's 0-based world (10..14) and round (0..9). */
void od_hidden_scene_level_world_round(int level, int *world, int *round);

#endif /* OD_SCENE_HIDDEN_H */
