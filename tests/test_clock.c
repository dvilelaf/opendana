/* test_clock.c - rational clock: fixed step, no drift, catch-up, sleep.
 * Pure C11, no SDL and no host sleeps (the clock is driven with synthetic
 * counter values so the measurements are exact and reproducible). */
#include "test.h"

#include "opendana/clock.h"

#include <stdio.h>

/* Sizes of the clock's accumulator in host-counter units: at a 1 GHz counter
 * one tick is 531960/26601712 s = 19997209.5... ns, i.e. period 19997210. */
static void test_clock_period(void)
{
    printf("- clock period is the exact 50.006978 Hz rational\n");
    od_clock c;
    od_clock_init(&c, 1000000000ull, 0);
    CHECK_EQ_U32(c.period, 19997210ull);
    /* The rounded period must never be shorter than the real one (a 16.67 kHz
     * NTSC period would be 16639.7; a correctly rounded PAL period is 19998). */
    CHECK(c.period * OD_CLOCK_PERIOD_DEN >= OD_CLOCK_PERIOD_NUM * 1000000000ull);
    /* 50.006978 Hz to six decimals. */
    CHECK(OD_CLOCK_HZ > 50.006977 && OD_CLOCK_HZ < 50.006979);
    /* The period is exact: 531960 master clocks of 26601712 Hz. */
    CHECK_EQ_U32(OD_CLOCK_PERIOD_DEN, 21477272ull + 5124440ull);
}

/* A whole number of ticks advanced at the exact deadline must produce exactly
 * that many ticks and zero leftover accumulator (no loss, no gain). */
static void test_clock_exact_schedule(void)
{
    printf("- clock: N ticks at the exact Euler deadline == N ticks\n");
    od_clock c;
    od_clock_init(&c, 1000000000ull, 0);
    long total = 0;
    for (long i = 1; i <= 100000; i++) {
        uint64_t t = od_clock_tick_time(&c, i);
        long due = od_clock_advance(&c, t, 0);
        total += due;
        if (due != 1) {
            printf("  FAIL tick %ld produced %ld ticks\n", i, due);
            od_test_failures++;
            break;
        }
    }
    od_test_checks++;
    CHECK_EQ_U32(c.tick, 100000);
    CHECK_EQ_U32(c.tick, (long)total);
    CHECK_EQ_U32(c.accum, 0);
    CHECK_EQ_U32(c.dropped, 0);
}

/* A long session sampled on a coarse, irregular grid (as a 60/120/144 Hz loop
 * does) must reach the same tick count as the ideal schedule within one tick,
 * and never accumulate a systematic offset. */
static void test_clock_no_drift_coarse_sampling(void)
{
    printf("- clock: no accumulated drift under coarse sampling\n");
    /* Sample every 1.5 ms, an awkward period unrelated to 19.997 ms. */
    const uint64_t hz = 1000000000ull;
    const uint64_t step = 1500000ull; /* 1.5 ms */
    od_clock c;
    od_clock_init(&c, hz, 0);
    uint64_t now = 0;
    uint64_t elapsed = 0;
    long last = 0;
    for (int i = 0; i < 200000; i++) {
        now += step;
        elapsed += step;
        long due = od_clock_advance(&c, now, 0);
        last = c.tick;
        (void)due;
    }
    uint64_t ideal = od_clock_elapsed_ticks(elapsed, hz);
    /* With the rational clock the realised tick count equals the exact position
     * at every sample: there is no accumulated drift at all. */
    long diff = (long)ideal - last;
    CHECK_EQ_U32(diff, 0);
    printf("  drift: modelled=%ld ideal=%llu diff=%ld\n",
           last, (unsigned long long)ideal, diff);
    CHECK_EQ_U32(c.dropped, 0);
}

/* A long frame (host stall) must run every overdue tick rather than one tick
 * of simulation per rendered frame. */
static void test_clock_slow_frames_catch_up(void)
{
    printf("- clock: a slow frame runs all due ticks\n");
    od_clock c;
    od_clock_init(&c, 1000000000ull, 0);
    /* Jump straight to 500 ms: 500/19.99721 = 25.003 ticks. */
    long due = od_clock_advance(&c, 500000000ull, 64);
    CHECK_EQ_U32(due, 25);
    CHECK_EQ_U32(c.tick, 25);
    CHECK_EQ_U32(c.dropped, 0);
    /* The host timeline still owns the schedule: the next deadline is the 26th
     * tick, not 500 ms plus another period. */
    CHECK(od_clock_tick_time(&c, 26) > 500000000ull);
    CHECK(od_clock_tick_time(&c, 26) < 520000000ull);
}

/* The catch-up cap bounds a single stall, but it consumes the host time so the
 * discarded ticks never become permanent lag. */
static void test_clock_catchup_cap(void)
{
    printf("- clock: catch-up cap bounds one stall without lag\n");
    od_clock c;
    od_clock_init(&c, 1000000000ull, 0);
    long due = od_clock_advance(&c, 500000000ull, 8);
    CHECK_EQ_U32(due, 8);
    CHECK_EQ_U32(c.tick, 8);
    CHECK_EQ_U32(c.dropped, 17);      /* 25 overdue - 8 run */
    CHECK_EQ_U32(c.accum, 0);         /* no lag carried into the next loop */
    /* Immediately after, a single normal frame produces exactly one tick. */
    uint64_t next = 500000000ull + c.period;
    CHECK_EQ_U32(od_clock_advance(&c, next, 8), 1);
    CHECK_EQ_U32(c.tick, 9);
}

/* Presentation deadlines for the 60/120/144 Hz characterisation land on exact
 * host instants and always stay at or after the elapsed time. */
static void test_clock_presentation_grid(void)
{
    printf("- clock: 60/120/144 Hz presentation deadlines\n");
    const unsigned rates[3] = { 60, 120, 144 };
    for (int r = 0; r < 3; r++) {
        od_clock c;
        od_clock_init(&c, 1000000000ull, 0);
        uint64_t now = 1000000000ull / (6u * rates[r]); /* ~1/6 of a period */
        (void)od_clock_advance(&c, now, 0);
        uint64_t next = od_clock_next_time(&c, rates[r]);
        /* A presentation happens at the simulation deadline at the earliest. */
        CHECK(next >= c.next_deadline);
        CHECK(next >= now);
        uint64_t sim_next = c.next_deadline;
        /* At 60 Hz the simulation (50.007 Hz) is the faster of the two, so the
         * simulation deadline wins; at 120/144 Hz the presentation deadline is
         * later or equal. */
        if (rates[r] == 60)
            CHECK_EQ_U32(next, c.next_deadline);
        else
            CHECK(next >= sim_next);
    }
}

/* Sleep is a hint only: it is never negative and is bounded by the next tick. */
static void test_clock_sleep_hint(void)
{
    printf("- clock: sleep hint is bounded and non-negative\n");
    od_clock c;
    od_clock_init(&c, 1000000000ull, 0);
    /* Tick 1 is not due yet at tick 0; the hint is the whole first period. */
    CHECK_EQ_U32(od_clock_sleep_ms(&c, 0), 19);
    CHECK_EQ_U32(od_clock_sleep_ms(&c, 1000000ull), 18); /* 18.997 ms -> 18 */
    CHECK_EQ_U32(od_clock_sleep_ms(&c, 999999999ull), 0);
    /* A sub-millisecond remainder returns a 1 ms yield, never a busy spin. */
    CHECK_EQ_U32(od_clock_sleep_ms(&c, c.next_deadline - 1), 1);
    CHECK_EQ_U32(od_clock_sleep_ms(&c, c.next_deadline), 0);
}

/* The deterministic source consumes exactly one tick per call and never reads
 * the host clock: N calls give exactly N ticks with no drop or accumulation, so
 * a batch run is a pure function of the input index. */
static void test_clock_fixed_step(void)
{
    printf("- clock: fixed source consumes one tick per call\n");
    od_clock c;
    od_clock_init(&c, 1000000000ull, 123456789ull);
    uint64_t first_deadline = c.next_deadline;
    for (long i = 0; i < 100000; i++) {
        if (od_clock_advance_fixed(&c) != 1) {
            printf("  FAIL fixed step %ld consumed no tick\n", i);
            od_test_failures++;
            break;
        }
    }
    od_test_checks++;
    CHECK_EQ_U32(c.tick, 100000);
    CHECK_EQ_U32(c.dropped, 0);
    CHECK_EQ_U32(c.accum, 0);
    /* The deadline tracks the tick index even though the host clock never ran. */
    CHECK_EQ_U32(c.next_deadline, od_clock_tick_time(&c, 100001));
    CHECK(c.next_deadline != first_deadline);
    CHECK_EQ_U32(od_clock_advance_fixed(NULL), 0);
}

/* Irrational host frequencies (e.g. a 10 MHz counter) keep exact ratios. */
static void test_clock_odd_frequency(void)
{
    printf("- clock: exact at an odd host frequency\n");
    od_clock c;
    od_clock_init(&c, 10000000ull, 0); /* 10 MHz: tick = 199972.095 units */
    long total = 0;
    for (long i = 1; i <= 50000; i++)
        total += od_clock_advance(&c, od_clock_tick_time(&c, i), 0);
    CHECK_EQ_U32(total, 50000);
    CHECK_EQ_U32(c.accum, 0);
    CHECK_EQ_U32(c.dropped, 0);
}

OD_TEST(test_clock_period)
OD_TEST(test_clock_exact_schedule)
OD_TEST(test_clock_no_drift_coarse_sampling)
OD_TEST(test_clock_slow_frames_catch_up)
OD_TEST(test_clock_catchup_cap)
OD_TEST(test_clock_presentation_grid)
OD_TEST(test_clock_sleep_hint)
OD_TEST(test_clock_fixed_step)
OD_TEST(test_clock_odd_frequency)
