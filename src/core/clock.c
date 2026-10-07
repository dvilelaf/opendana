/* clock.c - rational fixed-step simulation clock (H9). See opendana/clock.h. */

/* clock_gettime/CLOCK_MONOTONIC need POSIX visibility under strict C11. This
 * must precede every system header, including the ones opendana/clock.h pulls in. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "opendana/clock.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* Host counters are scaled with 128-bit intermediate products so the rational
 * mapping is exact for every frequency a desktop reports (SDL typically uses
 * 1e9 on Linux, 1e7 elsewhere). The multiplication cannot overflow 128 bits:
 * a long session counter is far smaller than 2^64 and small enough that
 * counter * period < 2^128. */
#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 od_u128;
#else
typedef unsigned long long od_u128; /* 64-bit fallback; see header limits */
#endif

void od_clock_init(od_clock *c, uint64_t frequency, uint64_t now)
{
    if (c == NULL)
        return;
    if (frequency == 0)
        frequency = 1;
    c->frequency = frequency;
    /* Round the tick period up to a whole host counter unit: a 1e9 Hz counter
     * sees 19997.08 units/tick and must wait 19998, never 19997. This value is
     * a *reference* period (the initial deadline); the authoritative position
     * always comes from the exact rational, not from this rounded value. */
    c->period = (OD_CLOCK_PERIOD_NUM * frequency + OD_CLOCK_PERIOD_DEN - 1) /
                OD_CLOCK_PERIOD_DEN;
    c->start = now;
    c->accum = 0;
    c->last_now = now;
    c->tick = 0;
    c->dropped = 0;
    c->resync_count = 0;
    c->next_deadline = now + c->period;
}

long od_clock_advance(od_clock *c, uint64_t now, long max_catchup)
{
    if (c == NULL || now < c->start)
        return 0;

    c->last_now = now;
    /* Authoritative position: the exact number of whole ticks the host clock
     * has covered since tick 0. Using the rational directly (not a rounded
     * per-tick period) means a run sampled at the exact deadlines reaches
     * exactly N ticks with no rounding gain or loss at any counter frequency. */
    uint64_t elapsed = now - c->start;
    uint64_t abs_due = od_clock_elapsed_ticks(elapsed, c->frequency);
    c->accum = (uint64_t)abs_due - (uint64_t)c->tick;

    long due = (long)abs_due - c->tick;
    if (due <= 0) {
        c->accum = 0;
        return 0;
    }

    long run = due;
    if (max_catchup > 0 && due > max_catchup) {
        /* A long host stall (window drag, debugger, suspended process). The
         * original console cannot skip frames; the host also cannot recover
         * real sleeping time. Drop the excess ticks so no deriva feeds the next
         * iteration. Re-anchoring `start` to the *cumulative* tick count makes
         * the skip permanent: the dropped ticks are never re-run, while ticks
         * already simulated stay accounted for. */
        c->dropped += (uint64_t)(due - max_catchup);
        run = max_catchup;
        c->tick += run;
        od_u128 num = (od_u128)(uint64_t)c->tick * OD_CLOCK_PERIOD_NUM * c->frequency;
        uint64_t off = (uint64_t)((num + OD_CLOCK_PERIOD_DEN - 1) / OD_CLOCK_PERIOD_DEN);
        c->start = now - off;
    } else {
        c->tick += run;
    }
    c->accum = 0;

    /* Deadline of the next tick, on the exact host timeline. */
    c->next_deadline = od_clock_tick_time(c, c->tick + 1);
    return run;
}

long od_clock_advance_fixed(od_clock *c)
{
    if (c == NULL)
        return 0;
    /* The tick index is the authority in this mode; the host fields are only
     * kept coherent for diagnostics and must never influence the next tick.
     * `start` is left untouched (it is still the tick-0 reference), only the
     * derived deadline is calculated again. */
    c->tick++;
    c->accum = 0;
    c->next_deadline = od_clock_tick_time(c, c->tick + 1);
    return 1;
}

uint64_t od_clock_tick_time(const od_clock *c, long tick)
{
    if (c == NULL || tick < 0)
        return 0;
    /* Smallest host instant at which `elapsed_ticks()` has reached `tick`:
     * ceil(tick * NUM * frequency / DEN). Rounding up (never down) guarantees
     * that a loop which sleeps exactly to this deadline observes `tick` ticks. */
    od_u128 num = (od_u128)(uint64_t)tick * OD_CLOCK_PERIOD_NUM * c->frequency;
    od_u128 den = OD_CLOCK_PERIOD_DEN;
    uint64_t off = (uint64_t)((num + den - 1) / den);
    return c->start + off;
}

uint64_t od_clock_next_time(const od_clock *c, unsigned max_sim_hz)
{
    if (c == NULL)
        return 0;
    if (max_sim_hz == 0)
        return c->next_deadline;
    uint64_t elapsed = c->last_now - c->start;
    /* Earliest multiple of (frequency / max_sim_hz) at or after the elapsed
     * host time, computed exactly in ticks of the host counter. */
    od_u128 k = (od_u128)elapsed * max_sim_hz;
    k = (k + c->frequency - 1) / c->frequency;
    uint64_t next = c->start +
        (uint64_t)((k * c->frequency) / max_sim_hz);
    if (next < c->next_deadline)
        return c->next_deadline;
    return next;
}

uint64_t od_clock_elapsed_ticks(uint64_t elapsed, uint64_t frequency)
{
    if (frequency == 0)
        return 0;
    return (uint64_t)(((od_u128)elapsed * OD_CLOCK_PERIOD_DEN) /
                      ((od_u128)OD_CLOCK_PERIOD_NUM * frequency));
}

void od_clock_resync(od_clock *c, uint64_t now)
{
    if (c == NULL)
        return;
    c->start = now - (uint64_t)(((od_u128)(uint64_t)c->tick *
                                 OD_CLOCK_PERIOD_NUM * c->frequency) /
                                OD_CLOCK_PERIOD_DEN);
    c->last_now = now;
    c->accum = 0;
    c->resync_count++;
    c->next_deadline = now;
}

unsigned od_clock_sleep_ms(const od_clock *c, uint64_t now)
{
    if (c == NULL || now >= c->next_deadline)
        return 0;
    uint64_t ticks_per_ms = c->frequency / 1000u;
    if (ticks_per_ms == 0)
        return 1;
    uint64_t ms = (c->next_deadline - now) / ticks_per_ms;
    if (ms == 0)
        return 1; /* sub-millisecond: yield for a moment, then re-check */
    return (unsigned)ms;
}

uint64_t od_host_counter(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter))
        return 0;
    return (uint64_t)counter.QuadPart;
#else
    struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t od_host_frequency(void)
{
#ifdef _WIN32
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return 0;
    return (uint64_t)frequency.QuadPart;
#else
    return 1000000000ull;
#endif
}
