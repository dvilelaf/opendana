/* clock.h - rational fixed-step simulation clock (H9).
 *
 * The game tick is the PAL frame: 341*312 PPU dots, 5/16 master clocks per dot,
 * on a 26.601.712 Hz PAL master clock.
 *
 *     tick period = 531960 master clocks / 26601712 Hz = 0.019997209... s
 *                  -> 50.006978... Hz
 *
 * 26601712 Hz is the PAL CPU/APU master frequency the reference uses (the NTSC
 * master is 21477272 Hz; PAL is 21477272 + 5124440). The period is kept as an
 * exact rational (num/den), never as a rounded double, so the simulation
 * position after N ticks is exactly N ticks. `od_clock_tick_time` maps a tick
 * index to a host performance-counter value through integer arithmetic only.
 *
 * The clock advances the simulation by whole ticks as a function of the
 * *monotonic host clock*, independent of how often the caller renders, of vsync
 * and of the refresh rate. Audio is produced inside those same ticks, so there
 * is a single clock for gameplay, presentation and sound.
 *
 * This module is pure C11: it neither opens a device nor calls SDL_Delay. The
 * app layer owns the waiting; the clock only says which ticks are due. */
#ifndef OD_CLOCK_H
#define OD_CLOCK_H

#include "opendana/common.h"

/* Rational tick period in seconds: 531960/26601712. Kept unreduced because
 * 531960 is the exact PPU-dot count of one frame (341 * 312 * 5). */
#define OD_CLOCK_PERIOD_NUM 531960ull
#define OD_CLOCK_PERIOD_DEN 26601712ull

/* Ticks per second as a double, for diagnostics only (never for stepping). */
#define OD_CLOCK_HZ ((double)OD_CLOCK_PERIOD_DEN / (double)OD_CLOCK_PERIOD_NUM)

typedef struct {
    uint64_t frequency;     /* host counter frequency used to scale       */
    uint64_t period;        /* reference tick period in host units (ceil) */
    uint64_t start;         /* host counter of the authoritative tick 0    */
    uint64_t accum;         /* ticks overdue at the last sample (diagnostic)*/
    uint64_t next_deadline; /* host counter of the next due tick          */
    uint64_t last_now;      /* last sampled host counter                  */
    long tick;              /* ticks simulated so far                     */
    long dropped;           /* whole ticks the catch-up cap discarded     */
    int resync_count;       /* host stalls that forced a resync           */
} od_clock;

/* Initialise for a host counter frequency (e.g. SDL_GetPerformanceFrequency).
 * `now` is the host counter value that corresponds to tick 0. */
void od_clock_init(od_clock *c, uint64_t frequency, uint64_t now);

/* Number of simulation ticks due at host instant `now`, capped by `max_catchup`
 * (0 = unlimited). The clock consumes that host time even when the cap discards
 * ticks, so it never accumulates unbounded lag. Read `c->dropped` for the number
 * discarded by this call. */
long od_clock_advance(od_clock *c, uint64_t now, long max_catchup);

/* Deterministic single-step counterpart of `od_clock_advance`: consume exactly
 * one tick with no reference to the host clock. Batch/harness runs drive this so
 * a run is a pure function of the input indexed by tick, identical byte for byte
 * regardless of wall-clock jitter, focus or vsync. The interactive session keeps
 * `od_clock_advance` for ~50 Hz pacing. Returns 1 when a tick was consumed. */
long od_clock_advance_fixed(od_clock *c);

/* Exact host-counter instant at which tick `tick` must have run (ceil). */
uint64_t od_clock_tick_time(const od_clock *c, long tick);

/* Exact host-counter instant of the next characterisation deadline, or the
 * instant of the next tick (`next_deadline`), whichever is later. `max_sim_hz`
 * is the rate the user configured (60/120/144...); the earliest multiple of
 * that interval at or after the elapsed host time is returned. Passing 0
 * disables the characterisation deadline and returns `next_deadline`. */
uint64_t od_clock_next_time(const od_clock *c, unsigned max_sim_hz);

/* Authoritative tick position implied by the host clock: the exact number of
 * whole ticks in `elapsed` host-counter units at `frequency`.
 *     floor(elapsed / frequency * DEN / NUM) */
uint64_t od_clock_elapsed_ticks(uint64_t elapsed, uint64_t frequency);

/* Drop all un-simulated host time and re-anchor tick 0 at `now`, counting a
 * resync. Used after a long host stall or a debugger break. */
void od_clock_resync(od_clock *c, uint64_t now);

/* Host wait, in milliseconds, until the next due tick. Never negative; returns
 * 0 when the tick is already due. This is only a hint: correctness comes from
 * the exact rational position, not from the sleep duration. */
unsigned od_clock_sleep_ms(const od_clock *c, uint64_t now);

/* Monotonic host counter and its frequency (SDL-free). */
uint64_t od_host_counter(void);
uint64_t od_host_frequency(void);

#endif /* OD_CLOCK_H */
