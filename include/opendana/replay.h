/* replay.h - scripted controller input, compatible with the reference format.
 *
 * A replay file has one event per line:
 *
 *     FRAME [port:]BUTTONS
 *
 * BUTTONS is `-` (release everything) or one or more names joined by `+`:
 * A B SELECT START UP DOWN LEFT RIGHT. Blank lines and `#` comments are
 * ignored. The controller state set by an event sticks until the next event.
 */
#ifndef OD_REPLAY_H
#define OD_REPLAY_H

#include "opendana/common.h"

typedef struct {
    long frame;
    unsigned buttons;
} od_replay_event;

typedef struct {
    od_replay_event *events;
    int count;
    long last_frame;
} od_replay;

od_status od_replay_parse(od_replay *out, const char *text, size_t size);
od_status od_replay_load_file(od_replay *out, const char *path);
void od_replay_free(od_replay *r);

/* Button state in effect at `frame` (0 before the first event). */
unsigned od_replay_buttons_at(const od_replay *r, long frame);

/* Map one button name to its bit, or 0 if unknown. */
unsigned od_replay_button_bit(const char *name);

#endif /* OD_REPLAY_H */
