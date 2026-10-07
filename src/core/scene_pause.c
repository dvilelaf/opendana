/* scene_pause.c - the START pause and the level-failure menu (C3.4, A12).
 *
 * One screen serves both cases: the bottom HUD band is replaced by
 * `TRY AGAIN / NEW GAME`. Both are driven by the same per-frame code of the
 * level-state task (the button auto-repeat of `od_retry_input` plus the handler
 * below), translated here frame by frame:
 *
 *   if (block in flight or round clear) return;
 *   if (death delay running) { if (--delay) goto start; goto open; }
 *   if (dead && !paused) delay = 120;          death: arm 120 frames
 *   start: if (START pressed) {
 *              if (!paused) goto open;
 *              if (dead) return;               a death cannot be resumed
 *              close; }
 *          else if (paused) cursor;            (od_retry_select)
 *   open:  paused = 1; band; sound; delay = 0; cursor
 *   close: paused = 0; HUD; two frame yields
 *
 * The paused flag only blocks Hero's decisions: the rest of the level loop keeps
 * running under the band. Measured on the PAL reference: START at f2400 sets the
 * flag on the same frame and the band shows on f2401; a second START (f2470,
 * bug4 f37223) clears it on its own frame; START at f16230, during the 2-10
 * death delay, opens the band at once and the START of f16260 is ignored
 * because Hero is dead.
 *
 * One more state bit exists in the handler's test but nothing ever sets it, so it
 * is kept implicit. */
#include "opendana/screens.h"

#include <string.h>

#define OD_PAUSE_FAIL_FRAMES 0x78u  /* death delay in frames: 120 */
#define OD_PAUSE_SOUND       0x5cu  /* sound played when the band opens, moves or closes */
#define OD_PAUSE_RESUME_HOLD 2u     /* two frame yields on resume */

void od_pause_init(od_pause *p)
{
    if (!p)
        return;
    memset(p, 0, sizeof *p);
    od_retry_reset(&p->menu);
}

static void pause_open(od_pause *p)
{
    p->paused = 1;
    p->sound = OD_PAUSE_SOUND;
    p->delay = 0;
    p->menu.selection = 0;
    p->new_game = 0;
}

int od_pause_frame(od_pause *p, unsigned held, unsigned pressed,
                    unsigned game03)
{
    if (!p)
        return OD_PAUSE_NONE;
    /* the band and the cursor written last frame are on screen now. */
    p->active = p->paused;
    p->menu.visible_selection = p->menu.selection;
    p->sound = 0;
    if (p->hold) {                      /* the task is still yielding */
        --p->hold;
        return OD_PAUSE_NONE;
    }
    if (!(game03 & OD_PAUSE_GAME_EXIT))
        od_retry_input(&p->menu, held, pressed);
    if (game03 & (OD_PAUSE_GAME_FROZEN | OD_PAUSE_GAME_CLEAR))
        return OD_PAUSE_NONE;
    int dead = (game03 & OD_PAUSE_GAME_DEAD) != 0;
    if (p->delay) {
        if (--p->delay == 0) {
            pause_open(p);
            return OD_PAUSE_OPENED;
        }
    } else if (dead && !p->paused) {
        p->delay = OD_PAUSE_FAIL_FRAMES;
    }
    if (pressed & OD_BTN_START) {
        if (!p->paused) {
            pause_open(p);
            return OD_PAUSE_OPENED;
        }
        if (dead)
            return OD_PAUSE_NONE;
        p->paused = 0;
        p->menu.selection = 0;
        p->hold = OD_PAUSE_RESUME_HOLD;
        return OD_PAUSE_CLOSED;
    }
    if (!p->paused)
        return OD_PAUSE_NONE;
    int new_game = 0, moved = 0;
    if (od_retry_select(&p->menu, &new_game, &moved)) {
        p->new_game = new_game;
        return OD_PAUSE_CONFIRMED;
    }
    if (moved)
        p->sound = OD_PAUSE_SOUND;
    return OD_PAUSE_NONE;
}
