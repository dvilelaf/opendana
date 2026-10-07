/* split.c - pure PAL hardware-clock research model. C6.6 removed the sampled
 * game calendar and perturbation templates. No default timeline, runtime ROM,
 * captured content or CPU instruction execution. */
#include "opendana/split.h"

#include <string.h>

/* The runner clocks the PPU inside a CPU register access with
 * hw_clock_run_ticks(n): on PAL it runs (n*8+1)/3 master half ticks. */
static unsigned od_split_pal_ticks(unsigned n)
{
    return (n * 8u + 1u) / 3u;
}

unsigned od_split_phase_perturbations_for(const od_split_access *calendar,
    unsigned calendar_count, unsigned phase, unsigned *offset,
    int8_t *delta, unsigned cap)
{
    if ((!calendar && calendar_count) || !cap || !offset || !delta)
        return 0;
    phase %= 10u;
    /* Relative dots after the handler entry and how many background half dots
     * each saw. One is normal; zero means the shift/commit was clocked past
     * (delta -1), two means it ran twice (delta +1). 600 covers the row and a
     * little beyond. */
    unsigned char half[600];
    unsigned dots = 0;
    int tick = 1;
    /* 0xFF marks a dot the simulation has not reached (it must not be counted
     * as a missing half dot). A reached dot starts at 0 and counts its halves. */
    memset(half, 0xFF, sizeof half);

    /* A CPU cycle at index j (j = 0 at the handler entry) has
     *     q = (handler_cycle*32 + align + tick) mod 10
     *       = (phase + 2*j + tick) mod 10,
     * a PPU dot on q == 0 and its half dot on q == 5. */
    for (unsigned j = 0; j <= 160; j++) {
        /* cycle_start: run the remaining ticks of this CPU cycle. */
        while (tick < 32) {
            unsigned q = (phase + 2u * j + (unsigned)tick) % 10u;
            if (q == 0) { if (++dots < 600) half[dots] = 0; }
            else if (q == 5 && dots < 600) half[dots]++;
            tick++;
        }
        tick = 0;
        /* A PPU register access at this offset clocks the PPU itself. */
        unsigned n = 0;
        for (unsigned i = 0; i < calendar_count; i++)
            if (calendar[i].offset == j)
                n += od_split_pal_ticks(calendar[i].ticks);
        while (n--) {
            unsigned q = (phase + 2u * j + (unsigned)tick) % 10u;
            if (q == 0) { if (++dots < 600) half[dots] = 0; }
            else if (q == 5 && dots < 600) half[dots]++;
            tick++;
        }
        /* cycle_finish: one more tick, then the next cycle starts at tick 1
         * unless the access already carried the tick past 32. */
        unsigned next_base = phase + 2u * (j + 1u);
        if (tick == 0) {
            unsigned q = next_base % 10u;
            if (q == 0) { if (++dots < 600) half[dots] = 0; }
            else if (q == 5 && dots < 600) half[dots]++;
            tick = 1;
        } else {
            unsigned q = (next_base + (unsigned)tick) % 10u;
            if (q == 0) { if (++dots < 600) half[dots] = 0; }
            else if (q == 5 && dots < 600) half[dots]++;
            tick++;
        }
    }

    unsigned out = 0;
    for (unsigned d = 1; d < 600 && out < cap; d++) {
        if (half[d] == 0xFF || half[d] == 1)
            continue;
        /* A dot has one background half dot; an access can only make it run
         * once too few or once too many, never more. */
        int dl = (int)half[d] - 1;
        if (dl > 1) dl = 1;
        if (dl < -1) dl = -1;
        offset[out] = d;
        delta[out] = (int8_t)dl;
        out++;
    }
    return out;
}
