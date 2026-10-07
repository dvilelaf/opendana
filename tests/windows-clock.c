#include "opendana/clock.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint64_t frequency = od_host_frequency();
    uint64_t first = od_host_counter();
    assert(frequency > 0);
    uint64_t last = first;
    for (int i = 0; i < 10000; ++i) {
        uint64_t now = od_host_counter();
        assert(now >= last);
        last = now;
    }
    assert(last > first);
    od_clock c;
    od_clock_init(&c, frequency, first);
    assert(od_clock_elapsed_ticks(od_clock_tick_time(&c, 1000) - first,
                                   frequency) == 1000);
    puts("host-clock: monotonic counter and rational mapping passed");
    return 0;
}
