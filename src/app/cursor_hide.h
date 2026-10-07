/* C6.45: hide the mouse cursor in fullscreen after 3 s without mouse activity.
 * Presentation only (wall time, never the simulation). In a window the cursor
 * is always visible. Pure logic, no SDL: tests/cursor-hide.c exercises it. */
#ifndef OD_CURSOR_HIDE_H
#define OD_CURSOR_HIDE_H

#include <stdint.h>

#define OD_CURSOR_HIDE_MS 3000u

typedef struct {
    uint32_t last_activity;   /* wall ms of the last motion/click             */
    int      was_fullscreen;  /* fullscreen state seen by the previous step   */
    int      inited;
} od_cursor_hide;

/* One step per presented frame. `activity` is nonzero when a mouse motion or
 * button event arrived since the previous step. Returns 1 when the cursor
 * must be visible, 0 when it must be hidden. Entering or leaving fullscreen
 * restarts the timer (the cursor shows again until 3 s of stillness). */
static inline int od_cursor_hide_step(od_cursor_hide *c, uint32_t now_ms,
                                      int fullscreen, int activity)
{
    if (!c->inited) {
        c->inited = 1;
        c->last_activity = now_ms;
        c->was_fullscreen = fullscreen ? 1 : 0;
    }
    if (activity || (fullscreen ? 1 : 0) != c->was_fullscreen)
        c->last_activity = now_ms;
    c->was_fullscreen = fullscreen ? 1 : 0;
    if (!fullscreen)
        return 1;
    return (uint32_t)(now_ms - c->last_activity) < OD_CURSOR_HIDE_MS;
}

#endif
