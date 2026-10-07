/* Pure PAL dot/half-dot clock research model; no game timeline defaults. */
#ifndef OD_SPLIT_H
#define OD_SPLIT_H
#include "opendana/common.h"

/* Caller-owned accesses, offsets in CPU cycles, ticks as hardware-clock
 * arguments converted by (n*8+1)/3. reg/value are metadata, not executed. */
typedef struct {
    unsigned offset;
    uint16_t reg;
    uint8_t value, ticks;
} od_split_access;

/* Supply synthetic inputs or an explicit private research fixture. Phase is
 * modulo10. Outputs up to cap dot-offset/delta pairs (-1/+1) over the finite
 * 161-cycle horizon. The last dot can be incomplete, not a visible-row defect.
 * Invalid pointers or cap0 return0. No file loading, CPU VM or game calendar.
 * Historical default-calendar and template APIs have been removed in C6.6. */
unsigned od_split_phase_perturbations_for(const od_split_access *calendar,
    unsigned calendar_count, unsigned phase, unsigned *offset,
    int8_t *delta, unsigned cap);
#endif
