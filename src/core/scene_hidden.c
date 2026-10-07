/* scene_hidden.c - hidden-level access scene (A13, C3.6).
 *
 * Wraps the measured hidden-menu model of `worldmap.c` (the title trigger and the
 * hidden-level selector) so the sequence machine can run it as one scene and
 * hand the chosen level to the ordinary level-entry path. No SDL, no ROM: the
 * caller supplies the tick's held-buttons mask.
 *
 * The press-edge rule is the original's: the title trigger counts B *presses*
 * (freshly pressed bits) while SELECT is held, and the menu steps on the A/B
 * press edge, not on a held direction. Both need edges, so this module keeps its
 * own `previous` mask instead of relying on the scene machine's. */
#include "opendana/scene_hidden.h"

void od_hidden_scene_reset(od_hidden_scene *hs)
{
    if (!hs)
        return;
    od_hidden_reset(&hs->menu);
    hs->open = 0;
    hs->previous = 0;
    hs->trigger_tick = -1;
}

int od_hidden_scene_title_tick(od_hidden_scene *hs, unsigned buttons)
{
    if (!hs || hs->open)
        return 0;
    unsigned pressed = buttons & ~hs->previous;
    hs->previous = buttons;
    if (od_hidden_title_tick(&hs->menu, buttons, pressed)) {
        hs->open = 1;
        return 1;
    }
    return 0;
}

int od_hidden_scene_menu_tick(od_hidden_scene *hs, unsigned buttons)
{
    if (!hs || !hs->open)
        return -1;
    unsigned pressed = buttons & ~hs->previous;
    hs->previous = buttons;
    return od_hidden_menu_tick(&hs->menu, buttons, pressed);
}

void od_hidden_scene_draw(const od_hidden_scene *hs, od_index_frame *fb,
                           const uint8_t *chr, const od_palette_ram *pal)
{
    od_hidden_menu_draw(fb, hs ? &hs->menu : NULL, chr, pal);
}

void od_hidden_scene_level_world_round(int level, int *world, int *round)
{
    /* The measurement path is the flow's own numbering: world = level/10 with
     * 0-based worlds, so 100 -> world 10 (ROUND 11), 149 -> world 14 (ROUND
     * 15); round = level%10. The level-entry path recombines them as
     * world*10 + round, which is exactly `level` again (flow.c). */
    if (world)
        *world = level / OD_WORLD_ROUNDS;
    if (round)
        *round = level % OD_WORLD_ROUNDS;
}
