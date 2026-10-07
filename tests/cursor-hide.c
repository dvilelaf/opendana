/* unit test of the fullscreen cursor auto-hide timer (no SDL). */
#include "../src/app/cursor_hide.h"

#include <stdio.h>

static int failures, checks;
#define EXPECT(c, msg) do { checks++; if (!(c)) { failures++; printf("FAIL: %s\n", msg); } } while (0)

int main(void)
{
    od_cursor_hide c = {0};
    /* Window: always visible, however long it stays still. */
    EXPECT(od_cursor_hide_step(&c, 1000, 0, 0) == 1, "window start visible");
    EXPECT(od_cursor_hide_step(&c, 60000, 0, 0) == 1, "window never hides");
    /* Entering fullscreen restarts the timer. */
    EXPECT(od_cursor_hide_step(&c, 61000, 1, 0) == 1, "fullscreen enter shows");
    EXPECT(od_cursor_hide_step(&c, 63999, 1, 0) == 1, "visible before 3 s");
    EXPECT(od_cursor_hide_step(&c, 64000, 1, 0) == 0, "hidden at 3 s");
    EXPECT(od_cursor_hide_step(&c, 90000, 1, 0) == 0, "stays hidden");
    /* Any motion or click brings it back and restarts the 3 s. */
    EXPECT(od_cursor_hide_step(&c, 90001, 1, 1) == 1, "motion shows");
    EXPECT(od_cursor_hide_step(&c, 92500, 1, 0) == 1, "visible 2.5 s after motion");
    EXPECT(od_cursor_hide_step(&c, 93001, 1, 0) == 0, "hidden 3 s after motion");
    EXPECT(od_cursor_hide_step(&c, 93002, 1, 1) == 1, "click shows");
    /* Leaving fullscreen shows it; re-entering restarts. */
    EXPECT(od_cursor_hide_step(&c, 99000, 0, 0) == 1, "window shows again");
    EXPECT(od_cursor_hide_step(&c, 99001, 1, 0) == 1, "re-enter restarts timer");
    EXPECT(od_cursor_hide_step(&c, 102001, 1, 0) == 0, "hidden 3 s after re-enter");
    /* Millisecond counter wrap. */
    od_cursor_hide w = {0};
    EXPECT(od_cursor_hide_step(&w, 0xFFFFFF00u, 1, 1) == 1, "wrap start");
    EXPECT(od_cursor_hide_step(&w, 0x00000100u, 1, 0) == 1, "wrap, 0.5 s");
    EXPECT(od_cursor_hide_step(&w, 0x00000C00u, 1, 0) == 0, "wrap, over 3 s");
    printf("cursor-hide: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
