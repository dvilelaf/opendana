/* Music and sound-effect sequencer plus the sound-chip synthesis. The
 * sequences are musical data (notes, durations, loops, volume and pitch
 * programs), not machine instructions; they are read from the content package
 * and interpreted here. */
#include "opendana/audio.h"
#include "opendana/content_gfx.h"
#include "opendana/rom_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_SIZE (6 * 8192)
#define RING_SIZE 16384

/* Intra-frame placement of the sequencer. The sequencer runs once per frame
 * and its register writes land a median 2,644 CPU cycles after the frame edge
 * rather than on the edge. The value is measured once for the whole sequencer,
 * applied to every tick, and not tuned per note. */
#define DRIVER_OFFSET_DEFAULT 2644u
/* The first frame ends on scanline 241, so it is shorter than the others:
 * 241*341+1 dots = 25,681.875 CPU cycles (25,684 counted in whole cycles), and
 * it yields 741 samples, not 960. It is the default because the synthesis
 * cycle grid has to start where the machine's does, or every later event cycle
 * carries a constant 7,563.5-cycle offset. The value belongs to the machine,
 * not to a song; 0 keeps a full-length first tick. */
#define BOOT_CYCLES_DEFAULT 25684u
/* Start-up configures two sound registers (the sample-rate register and
 * the frame-counter register) at CPU cycles 114,796 and 114,802 from reset,
 * i.e. 22,617 and 22,623 cycles into the third frame, which starts at
 * 25,684 + 2 full frames = 92,179. They are the only start-up writes, are not
 * sequencer writes, and so skip the per-write placement and land at these
 * measured instants. */
#define RESET_4010_OFFSET 22617u
#define RESET_4017_OFFSET 22623u
typedef struct {
    uint8_t flags, flags2, slot, wait_lo, wait_hi, reload_lo, reload_hi;
    uint16_t cursor, volume_pattern;
    uint8_t volume_wait, volume_pos, attenuation, detune, pitch_wait, pitch_pos;
    uint8_t volume, low, high, fade_wait, fade_reload, stack_size;
} Track;
typedef struct { uint8_t start, divider, decay; } Envelope;
typedef struct {
    uint16_t period, timer;
    uint8_t step, sweep_wait, sweep_reload;
    Envelope env;
} Pulse;
typedef struct {
    uint8_t reg[24], length[4], len_reload[4], len_reload_value[4], len_enabled[4];
    uint8_t linear, linear_reload, tri_step, tri_out;
    uint16_t tri_period, tri_timer, noise_timer, lfsr;
    Pulse pulse[2];
    Envelope noise_env;
    uint32_t frame_cycle;
    uint64_t cycles;
    uint8_t five_step, frame_reset; /* frame-counter register: 5-step mode and post-write counter reset */
    uint16_t dmc_timer, dmc_address, dmc_left;
    uint8_t dmc_level, dmc_bits, dmc_shift, dmc_silent;
    double phase, sum, hp90_in, hp90_out, hp440_in, hp440_out, lp;
    unsigned count;
    float pulse_mix[31], tnd_mix[203];
} Synth;
struct od_audio_engine {
    uint8_t data[DATA_SIZE], present[DATA_SIZE];
    Track track[8];
    uint8_t stack[68]; /* eight 8-byte track stacks, laid out contiguously */
    uint8_t fading, locked, enable, sweep[2], priority;
    uint8_t queue[8], qread, qwrite, qcount;
    int music_gain, sfx_gain, fanfare;
    unsigned driver_offset, boot_cycles, frame_phase;
    /* Derived per-write placement. Each sequencer operation carries a
     * CPU-cycle cost fitted from recorded traces; a write lands at the
     * accumulated cost instead of the single driver_offset. place=1 is the
     * default; 0 restores driver_offset. */
    int place;
    int place_first;
    double place_budget, place_rel;
    int64_t place_entry, place_emit, place_emit0;
    uint64_t place_tick_start, place_tick_end;
    /* Scratch bytes the sequencer reads when a track has no volume-envelope
     * pattern: the envelope pointer is still followed, so a missing pattern
     * reads these bytes at volume_pos rather than being skipped. They hold 0x88
     * and 0x1e from the first frames on, which makes the idle track volume 0x1e
     * and the first triangle write 0x9f instead of 0x8f. The app may override
     * them with the game's own working bytes. */
    uint8_t driver_ram[256];
    Synth synth;
    int16_t ring[RING_SIZE];
    size_t head, tail, count;
    od_audio_write_fn observer;
    void *observer_context;
    od_audio_write_cycle_fn cycle_observer;
    void *cycle_observer_context;
    od_audio_trace_fn trace;
    void *trace_context;
};

/* Cycle costs fitted by non-negative least squares on 136 recorded writes
 * (frames 2300-2360) and validated on held-out frames 2600-2625 (relative
 * placement error mean 7.2, max 26 cycles). The frame entry is the
 * accumulated cost of the operations before the first write; the within-frame
 * spread is the per-register write cost plus the per-voice emit-slot step. */
static const double PLACE_BASE_MAIN = 316.9;  /* main loop: one track slot  */
static const double PLACE_BASE_CALL = 296.0;  /* sequence() call            */
static const double PLACE_BASE_DUR  = 186.7;  /* duration byte              */
static const double PLACE_BASE_CTRL =  21.5;  /* control byte               */
static const double PLACE_BASE_EMIT =  57.1;  /* emit loop slot             */
static const double PLACE_EMIT_STEP =  79.6;  /* per voice-channel step     */
static const double PLACE_WCOST[24] = {
    [0] = 125.4, [1] = -13.4, [2] = 81.2, [3] = 48.1,
    [4] = 127.6, [5] = 0.9, [6] = 56.1,
    [12] = 0.0 /* noise envelope, not selected */, [21] = 17.7 /* channel enable */
};

static void synth_cycle(od_audio *a);

static void place_event(od_audio *a, int event, int x)
{
    od_audio_engine *e = a->engine;
    if (event == OD_AUDIO_EV_EMIT_LOOP) ++e->place_emit;
    if (!e->place_first) return; /* only the frame entry is accumulated here */
    switch (event) {
    case OD_AUDIO_EV_MAIN_TRACK: e->place_budget += PLACE_BASE_MAIN; break;
    case OD_AUDIO_EV_SEQ_CALL:   e->place_budget += PLACE_BASE_CALL; break;
    case OD_AUDIO_EV_SEQ_BYTE:
        if (x >= 0x80 && x < 0xc0) e->place_budget += PLACE_BASE_DUR;
        else if (x >= 0xe0)        e->place_budget += PLACE_BASE_CTRL;
        break;
    case OD_AUDIO_EV_EMIT_LOOP:  e->place_budget += PLACE_BASE_EMIT; break;
    default: break;
    }
}
static void place_write(od_audio *a, int reg)
{
    od_audio_engine *e = a->engine;
    int64_t target;
    if (e->place_first) {
        e->place_entry = (int64_t)(e->place_budget + 0.5);
        e->place_emit0 = e->place_emit;
        e->place_rel = 0.0;
        e->place_first = 0;
        target = e->place_entry;
    } else {
        target = e->place_entry + (int64_t)(PLACE_EMIT_STEP * (double)(e->place_emit - e->place_emit0)
                                            + e->place_rel + 0.5);
    }
    uint64_t abs = e->place_tick_start + (uint64_t)(target < 0 ? 0 : target);
    if (abs > e->place_tick_end) abs = e->place_tick_end;
    while (a->engine->synth.cycles < abs) synth_cycle(a);
    if (e->trace) e->trace(e->trace_context, OD_AUDIO_EV_PLACED,
                           (int)(abs - e->place_tick_start), reg);
    e->place_rel += (reg >= 0 && reg < 24) ? PLACE_WCOST[reg] : 0.0;
}

static inline void driver_trace(od_audio *a, int event, int x, int y)
{
    if (a->engine->trace) a->engine->trace(a->engine->trace_context, event, x, y);
    if (a->engine->place) place_event(a, event, x);
}

/* Position in the sparse image of a 16-bit address stored in the sound data.
 * `slot` is the slot the caller has selected; a window that is bound to a
 * slot ignores it. */
static const od_audio_layout *layout_in_use(void)
{
    static const od_audio_layout none;
    const od_audio_layout *l = od_audio_layout_get();
    return l ? l : &none;
}
static int data_index(const od_audio_layout *l, unsigned slot, unsigned addr)
{
    for (unsigned i = 0; i < l->window_count; i++) {
        const od_audio_window *w = &l->window[i];
        if (addr < w->first || addr >= (unsigned)w->first + l->slot_size) continue;
        if (w->slot >= 0) slot = (unsigned)w->slot;
        if (slot < l->first_slot || slot >= (unsigned)l->first_slot + l->slot_count) return -1;
        return (int)((slot - l->first_slot) * l->slot_size + (addr - w->first));
    }
    return -1;
}
static uint8_t image_byte(od_audio *a, int i)
{
    if (i < 0 || i >= DATA_SIZE || !a->engine->present[i]) {
#ifdef OD_AUDIO_DIAGNOSTICS
        fprintf(stderr, "audio missing data at %d, tick %llu\n", i, (unsigned long long)a->ticks);
#endif
        a->error = OD_ERR_FORMAT; return 0;
    }
    return a->engine->data[i];
}
/* A byte named by a pointer inside the sound data. */
static uint8_t data(od_audio *a, unsigned slot, unsigned addr)
{
    return image_byte(a, data_index(layout_in_use(), slot, addr));
}
static uint16_t word(od_audio *a, unsigned slot, unsigned addr)
{
    uint16_t lo = data(a, slot, addr);
    return (uint16_t)(lo | (data(a, slot, addr + 1) << 8));
}
/* A byte or word of a table, named by its offset in the image. */
static uint8_t table_byte(od_audio *a, unsigned offset)
{
    return image_byte(a, (int)offset);
}
static uint16_t table_word(od_audio *a, unsigned offset)
{
    uint16_t lo = table_byte(a, offset);
    return (uint16_t)(lo | (table_byte(a, offset + 1) << 8));
}
static const uint8_t lengths[32] = {
    10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
    12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
};
static const uint16_t noise_period[16] = {
    4,8,14,30,60,88,118,148,188,236,354,472,708,944,1890,3778
};
static const uint16_t dmc_period[16] = {
    398,354,316,298,276,236,210,198,176,148,132,118,98,78,66,50
};

/* `place` is 0 only for the start-up writes (see od_audio_reset_init): the
 * sequencer does not emit them, so they are outside its intra-frame budget,
 * but they must still reach the observer and the synthesis state. */
static void apu_write_ex(od_audio *a, unsigned reg, uint8_t value, int place)
{
    od_audio_engine *e = a->engine;
    Synth *s = &e->synth;
    if (place && e->place) place_write(a, (int)reg);
    driver_trace(a, OD_AUDIO_EV_WRITE, (int)reg, value);
    /* The effective application instant is the synthesis cycle after the
     * placement advance; synth.cycles never decreases, so the reported cycle
     * is monotonic across writes. */
    if (e->cycle_observer)
        e->cycle_observer(e->cycle_observer_context, s->cycles,
                          (uint16_t)(0x4000 + reg), value);
    if (e->observer) e->observer(e->observer_context, (uint16_t)(0x4000 + reg), value);
    s->reg[reg] = value;
    if (reg < 8) {
        Pulse *p = &s->pulse[reg / 4];
        switch (reg & 3) {
        case 1: p->sweep_reload = 1; break;
        case 2: p->period = (uint16_t)((p->period & 0x700) | value); break;
        case 3: {
            /* Pulse high-period registers: the length is *queued* (reload),
             * not loaded here; the frame counter applies it on the next half
             * frame. A write on a half-frame clock only takes if the counter
             * was already 0. */
            unsigned ch = (unsigned)reg / 4;
            p->period = (uint16_t)((p->period & 255) | ((value & 7) << 8));
            p->step = 0; p->env.start = 1;
            if (s->len_enabled[ch]) {
                s->len_reload_value[ch] = lengths[value >> 3];
                s->len_reload[ch] = 1;
            }
            break;
        }
        default: break;
        }
    } else if (reg == 10) s->tri_period = (uint16_t)((s->tri_period & 0x700) | value);
    else if (reg == 11) {
        s->tri_period = (uint16_t)((s->tri_period & 255) | ((value & 7) << 8));
        s->linear_reload = 1;
        if (s->len_enabled[2]) {
            s->len_reload_value[2] = lengths[value >> 3];
            s->len_reload[2] = 1;
        }
    } else if (reg == 15) {
        s->noise_env.start = 1;
        if (s->len_enabled[3]) {
            s->len_reload_value[3] = lengths[value >> 3];
            s->len_reload[3] = 1;
        }
    } else if (reg == 17) s->dmc_level = value & 127;
    else if (reg == 23) { /* frame counter: the sequencer never writes it;
                           * start-up does, resetting the counter. */
        s->five_step = (value & 0x80) != 0;
        s->frame_reset = 4; /* the reset lands 3-4 APU cycles later */
    } else if (reg == 21) {
        for (int ch = 0; ch < 4; ch++) {
            s->len_enabled[ch] = (value >> ch) & 1;
            if (!(value & (1 << ch))) s->length[ch] = 0;
        }
        /* The sequencer builds the enable value from a read of the sound
         * status, so its sample bit reflects whether the sample still has
         * bytes. A plain latch would keep bit 4 set after the sample drains and
         * make the engine drop a write that is meant to happen. */
        if (!(value & 16)) s->dmc_left = 0;
        else if (!s->dmc_left) {
            s->dmc_address = (uint16_t)(layout_in_use()->sample_base + s->reg[18] * 64);
            s->dmc_left = (uint16_t)(s->reg[19] * 16 + 1);
        }
    }
}
static void apu_write(od_audio *a, unsigned reg, uint8_t value)
{
    apu_write_ex(a, reg, value, 1);
}
static uint8_t dmc_enabled(const od_audio *a)
{
    return a->engine->synth.dmc_left ? 16 : 0;
}
static int clamp(int n, int min, int max) { return n < min ? min : n > max ? max : n; }

/* A request replaces only the tracks its header lists. */
static void request_start(od_audio *a, unsigned id)
{
    od_audio_engine *e = a->engine;
    if (id == 2) { /* fade the music out; no tempo change or restart */
        e->fading = 1;
        e->track[6].flags = 0;
        for (int i = 7; i >= 4; i--) {
            Track *t = &e->track[i];
            if (t->attenuation == 15) e->track[0].flags = 0; /* quirk kept on purpose */
            else t->fade_wait = t->fade_reload = table_byte(a, layout_in_use()->fade_steps + t->attenuation);
        }
        return;
    }
    if (e->locked) {
        if (id < 9) return;
        if (e->fading && id < e->priority) e->fading = 0;
    } else e->fading = 0;
    unsigned slot = table_byte(a, layout_in_use()->song_slot + id);
    unsigned p = table_word(a, layout_in_use()->song_start + id * 2);
    for (int n = 0; n <= 8; n++, p += 3) {
        unsigned ch = data(a, slot, p);
        if (ch == 255) return;
        if (ch > 7 || n == 8) { a->error = OD_ERR_FORMAT; return; }
        Track *t = &e->track[ch];
        t->cursor = word(a, slot, p + 1);
        t->flags = t->wait_lo = 1;
        t->wait_hi = t->stack_size = t->detune = 0;
        t->slot = (uint8_t)slot;
    }
}
static uint8_t next_byte(od_audio *a, Track *t, uint16_t *base, uint8_t *pos)
{
    uint8_t v = data(a, t->slot, *base + *pos);
    driver_trace(a, OD_AUDIO_EV_SEQ_BYTE, v, 0);
    ++*pos;
    return v;
}
static void stack_push(od_audio *a, Track *t, uint8_t value)
{
    size_t i = (size_t)(t - a->engine->track) * 8 + t->stack_size;
    if (i >= sizeof(a->engine->stack)) { a->error = OD_ERR_FORMAT; return; }
    a->engine->stack[i] = value; ++t->stack_size;
}
static uint8_t stack_pop(od_audio *a, Track *t)
{
    if (!t->stack_size) { a->error = OD_ERR_FORMAT; return 0; }
    return a->engine->stack[(t - a->engine->track) * 8 + --t->stack_size];
}

/* Consume controls and durations until a note, rest, noise hit or end. */
static void sequence(od_audio *a, int ch)
{
    od_audio_engine *e = a->engine;
    Track *t = &e->track[ch];
    uint16_t base = t->cursor;
    uint8_t pos = 0, old_high = t->high;
    for (int budget = 0; budget < 1024 && !a->error; budget++) {
        uint8_t op = next_byte(a, t, &base, &pos);
        if (op < 0x80) {
            if (op == ((ch & 3) == 3 ? 0x10 : 0x0c)) t->flags |= 2;
            else {
                if ((ch & 3) == 3) t->low = op;
                else {
                    uint16_t period = table_word(a, layout_in_use()->periods + (op & 15) * 2) >> (op >> 4);
                    int delta = t->detune & 127;
                    int low = (period & 255) + ((t->detune & 128) ? -delta : delta);
                    /* An overflowing sum takes the subtracting path. */
                    if (low > 255) low = (period & 255) - delta;
                    t->low = (uint8_t)clamp(low, 0, 255);
                    t->high = (uint8_t)((t->high & 0xf8) | (period >> 8));
                }
                int bits = 0x60;
                if ((ch & 3) != 3 && (t->flags & 4)) {
                    if (t->flags2 & 0x40) t->flags2 &= 0xbf;
                    else if (t->high == old_high) bits = 0x40;
                }
                t->flags = (uint8_t)((t->flags | bits) & 0xfd);
            }
            t->cursor = (uint16_t)(base + pos);
            t->wait_lo = t->reload_lo; t->wait_hi = t->reload_hi;
            return;
        }
        if (op < 0xc0) { /* additive duration, from the durations table */
            unsigned duration = table_word(a, layout_in_use()->durations + (op & 127) * 2);
            duration += t->wait_lo + t->wait_hi * 256;
            t->wait_lo = t->reload_lo = (uint8_t)duration;
            t->wait_hi = t->reload_hi = (uint8_t)(duration >> 8);
            continue;
        }
        if (op < 0xe0) {
            if (!e->fading) {
                unsigned p = layout_in_use()->effects + (op & 63) * 4;
                apu_write(a, 21, e->enable & 15);
                apu_write(a, 16, table_byte(a, p));
                uint8_t level = table_byte(a, p + 1);
                if (level != 255) apu_write(a, 17, level);
                apu_write(a, 18, table_byte(a, p + 2));
                apu_write(a, 19, table_byte(a, p + 3));
                apu_write(a, 21, e->enable | 16);
            }
            continue;
        }
        uint8_t arg;
        switch (op) {
        case 0xe0:
            arg = next_byte(a, t, &base, &pos);
            t->volume_pattern = table_word(a, layout_in_use()->volume_patterns + arg * 2);
            t->volume_wait = 1; t->volume_pos = 0; break;
        case 0xe1:
            arg = next_byte(a, t, &base, &pos);
            if (!e->fading) {
                int delta = arg & 127;
                t->attenuation = (uint8_t)clamp(t->attenuation + ((arg & 128) ? -delta : delta), 0, 15);
                if (arg & 128) t->flags |= 128;
            }
            break;
        case 0xe2:
            arg = next_byte(a, t, &base, &pos);
            t->volume = (t->volume & 63) | arg; break;
        case 0xe3:
            arg = next_byte(a, t, &base, &pos);
            if (!e->fading) { t->attenuation = arg; t->flags |= 128; } break;
        case 0xe4:
            arg = next_byte(a, t, &base, &pos);
            if (ch < 2) { e->sweep[ch] = arg; t->flags |= 32; } break;
        case 0xe5: t->detune = next_byte(a, t, &base, &pos); break;
        case 0xe8: case 0xe9: {
            uint16_t target = next_byte(a, t, &base, &pos);
            target |= (uint16_t)(next_byte(a, t, &base, &pos) << 8);
            if (op == 0xe9) {
                stack_push(a, t, pos); stack_push(a, t, (uint8_t)base);
                stack_push(a, t, (uint8_t)(base >> 8));
            } else t->cursor = target;
            base = target; pos = 0; break;
        }
        case 0xea:
            base = (uint16_t)(stack_pop(a, t) << 8);
            base |= stack_pop(a, t); pos = stack_pop(a, t); break;
        case 0xeb:
            arg = next_byte(a, t, &base, &pos);
            if (arg) {
                stack_push(a, t, pos); stack_push(a, t, (uint8_t)base);
                stack_push(a, t, (uint8_t)(base >> 8)); stack_push(a, t, arg);
            }
            break;
        case 0xec:
            arg = stack_pop(a, t); --arg;
            if (!arg) {
                (void)stack_pop(a, t); (void)stack_pop(a, t); (void)stack_pop(a, t);
            } else {
                stack_push(a, t, arg);
                if (t->stack_size < 4) { a->error = OD_ERR_FORMAT; return; }
                unsigned i = ch * 8 + t->stack_size - 4;
                pos = e->stack[i]; base = (uint16_t)(e->stack[i+1] | (e->stack[i+2] << 8));
            }
            break;
        case 0xed:
            t->pitch_pos = (uint8_t)(next_byte(a, t, &base, &pos) * 8);
            t->flags |= 8; t->pitch_wait = 1; break;
        case 0xef: t->flags &= 0xf7; break;
        case 0xf2: t->flags |= 16; break;
        case 0xf3: t->flags |= 4; t->flags2 |= 64; break;
        case 0xf4: t->flags &= 0xfb; break;
        case 0xfb: e->locked = 1; break;
        case 0xfc: e->locked = 0; break;
        case 0xf0: case 0xf9: case 0xfa:
            arg = op == 0xf0 ? next_byte(a, t, &base, &pos) : 0;
            if (ch != 3 && (e->track[3].flags & 1)) t->flags |= 2;
            else {
                if (op != 0xf0) {
                    e->enable = (e->enable & 15) | dmc_enabled(a) | 8;
                    apu_write(a, 21, e->enable);
                    apu_write(a, 12, op == 0xf9 ? 1 : 5);
                    apu_write(a, 14, op == 0xf9 ? 3 : 9);
                    apu_write(a, 15, op == 0xf9 ? 0x18 : 0x28);
                } else apu_write(a, 14, arg);
                t->flags &= 0xfd; t->flags2 &= 0x7f;
            }
            t->cursor = (uint16_t)(base + pos);
            t->wait_lo = t->reload_lo; t->wait_hi = t->reload_hi;
            t->flags2 |= 32; return;
        case 0xff:
            t->flags = 0;
            if (ch < 4) {
                e->track[ch + 4].flags |= 0xe0;
                if ((t->flags2 & 64) && ch == 3) {
                    e->enable = (e->enable & 7) | dmc_enabled(a);
                    apu_write(a, 21, e->enable);
                }
            }
            return;
        default: break; /* controls that have no effect */
        }
    }
    a->error = OD_ERR_FORMAT;
}

static void volume_step(od_audio *a, int ch)
{
    Track *t = &a->engine->track[ch];
    if (t->flags & 2) return;
    uint8_t wait_byte, volume_byte;
    if (t->volume_pattern) {
        unsigned ptr = t->volume_pattern + t->volume_pos;
        wait_byte = data(a, t->slot, ptr);
        volume_byte = data(a, t->slot, ptr + 1);
    } else {
        /* Without a pattern, the envelope is read from the scratch bytes at
         * volume_pos; it is not skipped. a->engine->driver_ram holds them. */
        wait_byte = a->engine->driver_ram[t->volume_pos];
        volume_byte = a->engine->driver_ram[(uint8_t)(t->volume_pos + 1)];
    }
    t->volume_wait = wait_byte;
    int volume = (int)volume_byte - t->attenuation;
    t->volume_pos += 2;
    if (volume <= 0) { t->flags2 |= 128; return; }
    t->volume = (uint8_t)((t->volume & 0xf0) | volume);
    if ((ch & 3) != 2) t->volume |= 0x30;
    t->flags |= 128;
    if (t->flags2 & 128) t->flags |= 32;
    t->flags2 &= 127;
}

static void driver_tick(od_audio *a)
{
    od_audio_engine *e = a->engine;
    if (e->qcount) {
        unsigned id = e->queue[e->qread]; e->qread = (e->qread + 1) & 7; --e->qcount;
        request_start(a, id);
    }
    if (e->fading) for (int ch = 7; ch >= 4; ch--) {
        Track *t = &e->track[ch];
        driver_trace(a, OD_AUDIO_EV_FADE_LOOP, ch, 0);
        if ((t->flags & 1) && --t->fade_wait == 0) {
            t->fade_wait = t->fade_reload;
            if (++t->attenuation == 15) {
                t->flags = 0;
                if (!(e->track[4].flags | e->track[5].flags | e->track[7].flags)) e->fading = 0;
            }
        }
    }
    for (int ch = 0; ch < 8; ch++) {
        Track *t = &e->track[ch];
        driver_trace(a, OD_AUDIO_EV_MAIN_TRACK, ch, 0);
        if (!(t->flags & 1)) continue;
        if (--t->wait_lo == 0) {
            if (t->wait_hi) --t->wait_hi;
            else {
                if (ch < 2) e->sweep[ch] = 8;
                if (!(t->flags & 4)) {
                    t->volume_pos = 0; t->volume_wait = 1;
                    t->pitch_pos &= 0xf8; t->pitch_wait = 1;
                }
                t->flags2 &= 0xdf;
                driver_trace(a, OD_AUDIO_EV_SEQ_CALL, ch, 0);
                sequence(a, ch);
            }
        }
        if (!(t->flags2 & 32)) {
            if (--t->volume_wait == 0) volume_step(a, ch);
            if ((t->flags & 8) && --t->pitch_wait == 0) t->flags |= 64;
        }
    }
    uint8_t enable = dmc_enabled(a);
    for (int ch = 0; ch < 4; ch++) {
        Track *t = &e->track[ch];
        driver_trace(a, OD_AUDIO_EV_ENABLE_LOOP, ch, 0);
        if (!(t->flags & 1)) t += 4;
        if ((t->flags & 3) == 1 && !(t->flags2 & 128)) enable |= (uint8_t)(1 << ch);
    }
    if (enable != e->enable) { e->enable = enable; apu_write(a, 21, enable); }
    for (int ch = 0; ch < 4; ch++) {
        int logical = ch;
        Track *t = &e->track[ch];
        driver_trace(a, OD_AUDIO_EV_EMIT_LOOP, ch, 0);
        if (!(t->flags & 1)) { t += 4; logical += 4; }
        if (!(t->flags & 1) || (t->flags & 2) || (t->flags2 & 32)) continue;
        if (t->flags & 128) {
            uint8_t v = t->volume;
            if (ch == 2) v |= 128;
            else if (t->flags & 16) { t->flags &= 0xef; v += 128; }
            apu_write(a, ch * 4, v); t->flags &= 127;
        }
        if (t->flags & 32) apu_write(a, ch * 4 + 1, logical < 2 ? e->sweep[logical] : 8);
        if (t->flags & 64) {
            uint8_t low = t->low;
            if ((t->flags & 8) && !t->pitch_wait) {
                unsigned p = layout_in_use()->pitch_steps + t->pitch_pos;
                t->pitch_wait = table_byte(a, p);
                unsigned delta = table_byte(a, p + 1);
                low = (uint8_t)clamp(low + ((delta & 128) ? -(int)(delta & 127) : (int)delta), 0, 255);
                t->pitch_pos = (t->pitch_pos & 0xf8) | ((t->pitch_pos + 2) & 7);
            }
            apu_write(a, ch * 4 + 2, low); t->flags &= 0xbf;
        }
        if (t->flags & 32) { apu_write(a, ch * 4 + 3, t->high); t->flags &= 0xdf; }
    }
}

static void envelope_step(Envelope *e, uint8_t reg)
{
    if (e->start) { e->start = 0; e->decay = 15; e->divider = reg & 15; }
    else if (e->divider) --e->divider;
    else {
        e->divider = reg & 15;
        if (e->decay) --e->decay;
        else if (reg & 32) e->decay = 15;
    }
}
static int sweep_target(const Synth *s, int ch)
{
    const Pulse *p = &s->pulse[ch];
    unsigned reg = s->reg[ch * 4 + 1];
    int delta = p->period >> (reg & 7);
    return (reg & 8) ? clamp(p->period - delta - (ch == 0), 0, 65535) : p->period + delta;
}
/* Frame-counter length clocking: a half-frame clock applies a queued length
 * reload only when the counter is already 0, while a reload queued just after
 * still lands on the next clock. */
static void clock_length_counters(Synth *s, int half)
{
    for (int ch = 0; ch < 4; ch++) {
        if (half) {
            if (s->len_reload[ch] && s->length[ch] == 0) s->length[ch] = s->len_reload_value[ch];
            else s->len_reload[ch] = 0;
            if (!s->len_enabled[ch]) s->length[ch] = 0;
            int halt = s->reg[ch * 4] & (ch == 2 ? 128 : 32);
            if (s->length[ch] && !halt && !s->len_reload[ch]) --s->length[ch];
        } else {
            if (s->len_reload[ch]) s->length[ch] = s->len_reload_value[ch];
            s->len_reload[ch] = 0;
        }
    }
}
static void frame_clock(Synth *s, int half)
{
    envelope_step(&s->pulse[0].env, s->reg[0]);
    envelope_step(&s->pulse[1].env, s->reg[4]);
    envelope_step(&s->noise_env, s->reg[12]);
    if (s->linear_reload) s->linear = s->reg[8] & 127;
    else if (s->linear) --s->linear;
    if (!(s->reg[8] & 128)) s->linear_reload = 0;
    if (half) {
        for (int ch = 0; ch < 2; ch++) {
            Pulse *p = &s->pulse[ch];
            unsigned r = s->reg[ch * 4 + 1];
            int target = sweep_target(s, ch);
            if (!p->sweep_wait && (r & 128) && (r & 7) && p->period >= 8 && target <= 2047)
                p->period = (uint16_t)target;
            if (!p->sweep_wait || p->sweep_reload) { p->sweep_wait = (r >> 4) & 7; p->sweep_reload = 0; }
            else --p->sweep_wait;
        }
    }
}
static int env_volume(const Envelope *e, uint8_t reg) { return reg & 16 ? reg & 15 : e->decay; }
static void emit(od_audio *a, double level)
{
    od_audio_engine *e = a->engine;
    Synth *s = &e->synth;
    const double pi = 3.14159265358979;
    double hp90 = 1.0 / (1.0 + 2.0 * pi * 90.0 / OD_AUDIO_RATE);
    double hp440 = 1.0 / (1.0 + 2.0 * pi * 440.0 / OD_AUDIO_RATE);
    double lp = (2.0 * pi * 14000.0) / (OD_AUDIO_RATE + 2.0 * pi * 14000.0);
    double v = hp90 * (s->hp90_out + level - s->hp90_in);
    s->hp90_in = level; s->hp90_out = v;
    double w = hp440 * (s->hp440_out + v - s->hp440_in);
    s->hp440_in = v; s->hp440_out = w;
    s->lp += lp * (w - s->lp);
    if (e->count == RING_SIZE) { e->tail = (e->tail + 1) % RING_SIZE; --e->count; ++a->overruns; }
    e->ring[e->head] = (int16_t)clamp((int)(s->lp * 30000.0), -32768, 32767);
    e->head = (e->head + 1) % RING_SIZE; ++e->count; ++a->samples;
}
static void synth_cycle(od_audio *a)
{
    od_audio_engine *e = a->engine;
    Synth *s = &e->synth;
    ++s->cycles;
    /* A frame-counter write resets the counter 3-4 APU cycles later. */
    if (s->frame_reset && --s->frame_reset == 0) s->frame_cycle = 0;
    ++s->frame_cycle;
    if (s->frame_cycle == 8313 || s->frame_cycle == 24939) frame_clock(s, 0);
    if (s->frame_cycle == 16627 || s->frame_cycle == 33253) frame_clock(s, 1);
    if (s->five_step && s->frame_cycle == 41565) frame_clock(s, 1);
    /* The length counters clock on every half frame, and on any cycle a
     * queued length reload is waiting (a quarter-frame clock clears it). */
    int half_step = s->frame_cycle == 16627 || s->frame_cycle == 33253 ||
                    (s->five_step && s->frame_cycle == 41565);
    if (half_step || (s->len_reload[0] | s->len_reload[1] | s->len_reload[2] | s->len_reload[3]))
        clock_length_counters(s, half_step);
    if (s->frame_cycle == (s->five_step ? 41566u : 33254u)) s->frame_cycle = 0;
    if (!(s->cycles & 1)) for (int ch = 0; ch < 2; ch++) {
        Pulse *p = &s->pulse[ch];
        if (p->timer) --p->timer;
        else { p->timer = p->period; p->step = (p->step + 1) & 7; }
    }
    if (s->noise_timer) --s->noise_timer;
    else {
        s->noise_timer = noise_period[s->reg[14] & 15] - 1;
        unsigned tap = s->reg[14] & 128 ? 6 : 1;
        unsigned bit = (s->lfsr ^ (s->lfsr >> tap)) & 1;
        s->lfsr = (uint16_t)((s->lfsr >> 1) | (bit << 14));
    }
    if (s->tri_timer) --s->tri_timer;
    else {
        s->tri_timer = s->tri_period;
        if (s->length[2] && s->linear) s->tri_step = (s->tri_step + 1) & 31;
    }
    if (s->tri_period >= 2) s->tri_out = s->tri_step < 16 ? 15 - s->tri_step : s->tri_step - 16;
    if (s->dmc_timer) --s->dmc_timer;
    else {
        s->dmc_timer = dmc_period[s->reg[16] & 15] - 1;
        if (!s->dmc_silent) {
            if (s->dmc_shift & 1) { if (s->dmc_level <= 125) s->dmc_level += 2; }
            else if (s->dmc_level >= 2) s->dmc_level -= 2;
        }
        s->dmc_shift >>= 1;
        if (--s->dmc_bits == 0) {
            s->dmc_bits = 8;
            s->dmc_silent = s->dmc_left == 0;
            if (s->dmc_left) {
                s->dmc_shift = data(a, 0, s->dmc_address++); --s->dmc_left;
                if (!s->dmc_left && (s->reg[16] & 64)) {
                    s->dmc_address = (uint16_t)(layout_in_use()->sample_base + s->reg[18] * 64);
                    s->dmc_left = (uint16_t)(s->reg[19] * 16 + 1);
                }
            }
        }
    }
    static const uint8_t duty[4] = {0x40, 0x60, 0x78, 0x9f};
    int levels[4] = {0, 0, s->tri_out, 0};
    for (int ch = 0; ch < 2; ch++) {
        Pulse *p = &s->pulse[ch];
        unsigned r = s->reg[ch * 4];
        if (((duty[r >> 6] >> (7 - p->step)) & 1) && s->length[ch] && p->period >= 8 && sweep_target(s, ch) <= 2047)
            levels[ch] = env_volume(&p->env, (uint8_t)r);
    }
    if (s->length[3] && !(s->lfsr & 1)) levels[3] = env_volume(&s->noise_env, s->reg[12]);
    for (int ch = 0; ch < 4; ch++) {
        int gain = e->track[ch].flags & 1 ? e->sfx_gain : e->music_gain;
        levels[ch] = levels[ch] * gain / 100;
    }
    s->sum += s->pulse_mix[levels[0] + levels[1]] + s->tnd_mix[3 * levels[2] + 2 * levels[3] + s->dmc_level * e->music_gain / 100];
    ++s->count;
    s->phase += 1.0;
    if (s->phase >= 1662607.0 / OD_AUDIO_RATE) {
        s->phase -= 1662607.0 / OD_AUDIO_RATE;
        emit(a, s->sum / s->count); s->sum = 0; s->count = 0;
    }
}

/* The first line of the stream: the current magic or the legacy one (C6.53). */
static int seq_magic_ok(const char *line)
{
    char word[16];
    size_t n = strcspn(line, "\n");
    if (n == 0 || n >= sizeof word || line[n] != '\n') return 0;
    memcpy(word, line, n);
    word[n] = 0;
    return strcmp(word, "ODSEQ1") == 0;
}

/* One line of the ODSEQ1 text, with the same discipline as the former fgets()
 * reader: the newline kept, at most cap-1 bytes. Returns 0 at the end. */
static int seq_next_line(const char **cur, const char *end, char *line, size_t cap)
{
    if (*cur >= end) return 0;
    size_t k = 0;
    while (*cur < end && k < cap - 1) {
        char c = *(*cur)++;
        line[k++] = c;
        if (c == '\n') break;
    }
    line[k] = 0;
    return 1;
}

/* Read the whole ODSEQ1 text of the bare-checkout fallback file. */
static char *read_sequence_file(const char *assets_dir, size_t *len, od_status *st)
{
    char path[1024];
    *st = OD_OK;
    if (snprintf(path, sizeof(path), "%s/audio/sequence.hex", assets_dir) >= (int)sizeof(path)) {
        *st = OD_ERR_RANGE;
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { *st = OD_ERR_IO; return NULL; }
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap + 1);
    while (buf) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap) break;
        char *grown = realloc(buf, cap * 2 + 1);
        if (!grown) { free(buf); buf = NULL; break; }
        buf = grown; cap *= 2;
    }
    if (!buf) *st = OD_ERR_NOMEM;
    else if (ferror(f)) { free(buf); buf = NULL; *st = OD_ERR_IO; }
    else { buf[n] = 0; *len = n; }
    fclose(f);
    return buf;
}

od_status od_audio_init(od_audio *a, const char *assets_dir)
{
    if (!a) return OD_ERR_FORMAT;
    memset(a, 0, sizeof(*a)); a->music_on = -1; a->rate = OD_AUDIO_RATE;
    if (!assets_dir || !*assets_dir) return OD_ERR_FORMAT;
    /* The sequencer stream comes from the content package
     * (audio/sequence.json, rebuilt as ODSEQ1 text by content_gfx.c and
     * layered through the VFS, so a mod can replace the music). The extracted
     * assets/ file is only the bare-checkout fallback, the same policy as the
     * CHR (src/import/assets.c). */
    if (!od_gfx_loaded()) (void)od_gfx_load_default(NULL);
    size_t len = 0;
    const char *text = od_gfx_audio_hex(&len);
    char *owned = NULL;
    if (!text || !len) {
        od_status st;
        owned = read_sequence_file(assets_dir, &len, &st);
        if (!owned) return st;
        text = owned;
    }
    if (!od_audio_layout_get()) { free(owned); return OD_ERR_IO; }
    a->engine = calloc(1, sizeof(*a->engine));
    if (!a->engine) { free(owned); return OD_ERR_NOMEM; }
    const char *cur = text, *end = text + len;
    char line[128];
    if (!seq_next_line(&cur, end, line, sizeof line) || !seq_magic_ok(line)) a->error = OD_ERR_FORMAT;
    while (!a->error && seq_next_line(&cur, end, line, sizeof line)) {
        unsigned slot, addr; char hex[65], extra;
        if (sscanf(line, "%x %x %64s %c", &slot, &addr, hex, &extra) != 3 || strlen(hex) % 2) { a->error = OD_ERR_FORMAT; break; }
        for (size_t j = 0; j < strlen(hex); j += 2) {
            unsigned byte;
            char pair[3] = {hex[j], hex[j+1], 0};
            int i = data_index(layout_in_use(), slot, addr++);
            if (i < 0 || sscanf(pair, "%2x%c", &byte, &extra) != 1 || a->engine->present[i]) { a->error = OD_ERR_FORMAT; break; }
            a->engine->data[i] = (uint8_t)byte; a->engine->present[i] = 1;
        }
    }
    free(owned);
    /* All requests must have a header before accepting the package. */
    for (unsigned id = 1; id <= OD_AUDIO_SONGS && !a->error; id++) {
        unsigned slot = table_byte(a, layout_in_use()->song_slot + id);
        unsigned p = table_word(a, layout_in_use()->song_start + id * 2);
        (void)data(a, slot, p);
    }
    if (a->error) { od_status st = (od_status)a->error; od_audio_shutdown(a); return st; }
    od_audio_engine *e = a->engine;
    e->music_gain = e->sfx_gain = 100;
    e->driver_offset = DRIVER_OFFSET_DEFAULT;
    e->boot_cycles = BOOT_CYCLES_DEFAULT; /* console's short first frame */
    e->frame_phase = 0;
    e->place = 1; /* derived per-write placement */
    /* Power-on state of the circuits the mixer runs: the noise shift register
     * seeds to 1, the triangle output rests at 15 while its period is 0, the
     * sample timer starts at 1022 CPU cycles and the shifter is empty. The
     * sequencer state is defined separately by the stream data. */
    e->synth.lfsr = 1;
    e->synth.tri_out = 15;
    e->synth.dmc_timer = 1022;
    e->synth.dmc_bits = 8;
    e->synth.dmc_silent = 1;
    /* Scratch bytes read when a track has no volume pattern; with these the
     * idle track volume and the first triangle write come out as measured,
     * instead of as from zeroed memory. */
    e->driver_ram[0] = 0x88;
    e->driver_ram[1] = 0x1e;
    for (int i = 1; i < 31; i++) e->synth.pulse_mix[i] = (float)(95.52 / (8128.0 / i + 100));
    for (int i = 1; i < 203; i++) e->synth.tnd_mix[i] = (float)(163.67 / (24329.0 / i + 100));
    a->ready = 1;
    return OD_OK;
}
void od_audio_shutdown(od_audio *a)
{
    if (!a) return;
    free(a->engine); a->engine = NULL; a->ready = 0;
}

/* C5.3: export/import the sequencer's mutable state (see audio.h). */
size_t od_audio_state_size(void)
{
    return sizeof(struct od_audio_engine);
}

void od_audio_state_save(const od_audio *a, void *out)
{
    if (!out) return;
    if (!a || !a->engine) {
        memset(out, 0, sizeof(struct od_audio_engine));
        return;
    }
    /* Observers belong to the live process, not to a resumable sequencer.
     * Sanitize a copy: exporting must never disconnect the running engine's
     * callbacks, and ext/state must not duplicate this private layout.
     * memcpy also preserves initialized padding for byte-stable snapshots. */
    struct od_audio_engine copy;
    memcpy(&copy, a->engine, sizeof copy);
    copy.observer = NULL;
    copy.observer_context = NULL;
    copy.cycle_observer = NULL;
    copy.cycle_observer_context = NULL;
    copy.trace = NULL;
    copy.trace_context = NULL;
    memcpy(out, &copy, sizeof copy);
}

void od_audio_state_load(od_audio *a, const void *in)
{
    if (!a || !a->engine || !in) return;
    od_audio_engine *e = a->engine;
    od_audio_write_fn observer = e->observer;
    void *observer_context = e->observer_context;
    od_audio_write_cycle_fn cycle_observer = e->cycle_observer;
    void *cycle_observer_context = e->cycle_observer_context;
    od_audio_trace_fn trace = e->trace;
    void *trace_context = e->trace_context;
    memcpy(e, in, sizeof *e);
    e->observer = observer;
    e->observer_context = observer_context;
    e->cycle_observer = cycle_observer;
    e->cycle_observer_context = cycle_observer_context;
    e->trace = trace;
    e->trace_context = trace_context;
}

void od_audio_tick(od_audio *a)
{
    if (!a || !a->ready || a->error) return;
    od_audio_engine *e = a->engine;
    Synth *s = &e->synth;
    uint64_t start = s->cycles;
    /* 341 dots * 312 lines * 5/16 CPU cycles/dot = 66495/2 per full frame.
     * With boot_cycles the first tick is the machine's partial first frame
     * and every later boundary is that base plus whole frames. */
    uint64_t end;
    if (e->boot_cycles) {
        end = a->ticks == 0 ? e->boot_cycles
                            : (uint64_t)e->boot_cycles + a->ticks * 66495 / 2;
    } else {
        end = (a->ticks + 1) * 66495 / 2;
    }
    /* The sequencer's register writes are applied inside the frame. With
     * placement on, each write lands at its own derived cycle position (the
     * sequencer advances the synthesis as it emits); off, all writes land at
     * the single derived driver_offset. */
    if (e->place) {
        e->place_tick_start = start;
        e->place_tick_end = end;
        e->place_first = 1;
        e->place_budget = 0.0;
        e->place_rel = 0.0;
        e->place_emit = 0;
        driver_tick(a);
        while (s->cycles < end && !a->error) synth_cycle(a);
    } else {
        uint64_t driver = start + e->driver_offset;
        if (driver > end) driver = end;
        while (s->cycles < driver && !a->error) synth_cycle(a);
        driver_tick(a);
        while (s->cycles < end && !a->error) synth_cycle(a);
    }
    ++a->ticks;
}

void od_audio_set_driver_offset(od_audio *a, unsigned cycles)
{
    if (a && a->ready) a->engine->driver_offset = cycles;
}
unsigned od_audio_driver_offset(const od_audio *a)
{
    return a && a->ready ? a->engine->driver_offset : 0;
}
/* Derived per-write placement. On (default), each sequencer write lands at
 * the cycle position its driver operation reached, instead of every write
 * landing at driver_offset. Off restores the single-offset behaviour. */
void od_audio_set_write_placement(od_audio *a, int on)
{
    if (a && a->ready) a->engine->place = on ? 1 : 0;
}
int od_audio_write_placement(const od_audio *a)
{
    return a && a->ready ? a->engine->place : 0;
}
void od_audio_set_boot_cycles(od_audio *a, unsigned cycles)
{
    if (a && a->ready) a->engine->boot_cycles = cycles;
}
unsigned od_audio_boot_cycles(const od_audio *a)
{
    return a && a->ready ? a->engine->boot_cycles : 0;
}
void od_audio_set_frame_phase(od_audio *a, unsigned cycles)
{
    if (a && a->ready) {
        a->engine->frame_phase = cycles;
        a->engine->synth.frame_cycle = cycles;
    }
}
unsigned od_audio_frame_phase(const od_audio *a)
{
    return a && a->ready ? a->engine->frame_phase : 0;
}
void od_audio_reset_init(od_audio *a)
{
    if (!a || !a->ready) return;
    od_audio_engine *e = a->engine;
    /* Start-up writes two registers in the third frame: the sample register
     * disables its interrupt and selects rate 0, and the frame-counter register
     * selects the 4-step sequence with the interrupt inhibited (which also resets the
     * counter). Neither is a sequencer write, so they skip the placement, but
     * they still land at their measured instants (114,796 and 114,802 cycles
     * from reset = 22,617 and 22,623 into that frame). The synthesis and the
     * frame counter advance with them, so the sound state and the reported
     * cycle stay consistent. */
    uint64_t start = e->synth.cycles; /* start of the frame about to run */
    while (e->synth.cycles < start + RESET_4010_OFFSET) synth_cycle(a);
    apu_write_ex(a, 16, 0x00, 0);
    while (e->synth.cycles < start + RESET_4017_OFFSET) synth_cycle(a);
    apu_write_ex(a, 23, 0x40, 0);
}
void od_audio_render(od_audio *a, int16_t *out, int frames)
{
    if (!out || frames <= 0) return;
    for (int i = 0; i < frames; i++) {
        out[i] = 0;
        if (a && a->ready && a->engine->count) {
            od_audio_engine *e = a->engine;
            out[i] = e->ring[e->tail]; e->tail = (e->tail + 1) % RING_SIZE; --e->count;
        }
    }
}
size_t od_audio_available(const od_audio *a) { return a && a->ready ? a->engine->count : 0; }
int od_audio_rate(const od_audio *a) { (void)a; return OD_AUDIO_RATE; }
od_status od_audio_request(od_audio *a, unsigned id)
{
    if (!a || !a->ready) return OD_ERR_FORMAT;
    if (!id || id > OD_AUDIO_SONGS) return OD_ERR_RANGE;
    od_audio_engine *e = a->engine;
    if (e->qcount == 8) { a->error = OD_ERR_RANGE; return OD_ERR_RANGE; }
    e->queue[e->qwrite] = (uint8_t)id; e->qwrite = (e->qwrite + 1) & 7; ++e->qcount;
    return OD_OK;
}
void od_audio_set_request_guard(od_audio *a, uint8_t guard)
{
    if (a && a->ready) a->engine->priority = guard;
}
/* Scratch bytes the sequencer reads when a track has no volume pattern; addr
 * counts from 0. The engine defaults to the measured bytes (0x88, 0x1e); the
 * app supplies the game's own working bytes when it models them. */
void od_audio_set_driver_ram(od_audio *a, uint16_t addr, uint8_t value)
{
    if (a && a->ready && addr < 256) a->engine->driver_ram[addr] = value;
}
uint8_t od_audio_driver_ram(const od_audio *a, uint16_t addr)
{
    if (a && a->ready && addr < 256) return a->engine->driver_ram[addr];
    return 0;
}
unsigned od_audio_level_request(unsigned level)
{
    const od_audio_layout *layout = od_audio_layout_get();
    if (level < 100 && level % 10 == 9) return 21;
    return level < 100 && layout ? layout->world_music[level / 10] : 11;
}
void od_audio_play_music(od_audio *a, int track)
{
    static const uint8_t ids[OD_MUSIC_COUNT] = {11,8,26,21};
    if (!a || !a->ready) return;
    if (track < 0 || track >= OD_MUSIC_COUNT) { od_audio_stop_music(a); return; }
    if (a->engine->fanfare && track == a->music_on) return;
    a->engine->fanfare = 0; a->music_on = track;
    (void)od_audio_request(a, 1); (void)od_audio_request(a, ids[track]);
}
void od_audio_stop_music(od_audio *a)
{
    if (!a || !a->ready) return;
    a->music_on = -1; a->engine->fanfare = 0;
    (void)od_audio_request(a, 1);
}
int od_audio_music_playing(const od_audio *a) { return a && a->ready && a->music_on >= 0 && !a->engine->fanfare; }
void od_audio_play_sfx(od_audio *a, int sfx)
{
    /* The sound ids were checked against where the game requests them. */
    static const uint8_t ids[OD_SFX_COUNT] = {79,89,54,87};
    if (!a || !a->ready || sfx < 0 || sfx >= OD_SFX_COUNT) return;
    if (sfx == OD_SFX_CLEAR || sfx == OD_SFX_GAMEOVER) {
        a->engine->fanfare = 1;
        (void)od_audio_request(a, 1);
    }
    (void)od_audio_request(a, ids[sfx]);
}
void od_audio_accepted_sfx(od_audio *a, int sfx, int accepted) { if (accepted) od_audio_play_sfx(a, sfx); }
void od_audio_set_music_volume(od_audio *a, int percent) { if (a && a->ready) a->engine->music_gain = clamp(percent,0,100); }
void od_audio_set_sfx_volume(od_audio *a, int percent) { if (a && a->ready) a->engine->sfx_gain = clamp(percent,0,100); }
void od_audio_observe(od_audio *a, od_audio_write_fn fn, void *context)
{
    if (a && a->ready) { a->engine->observer = fn; a->engine->observer_context = context; }
}
void od_audio_observe_cycles(od_audio *a, od_audio_write_cycle_fn fn, void *context)
{
    if (a && a->ready) {
        a->engine->cycle_observer = fn;
        a->engine->cycle_observer_context = context;
    }
}
void od_audio_set_trace(od_audio *a, od_audio_trace_fn fn, void *context)
{
    if (a && a->ready) { a->engine->trace = fn; a->engine->trace_context = context; }
}
void od_audio_snapshot(const od_audio *a, uint8_t out[256])
{
    memset(out, 0, 256);
    if (!a || !a->ready) return;
    out[1] = a->engine->fading;
    memcpy(out + 178, a->engine->stack, 68);
    for (int i = 0; i < 8; i++) {
        const Track *t = &a->engine->track[i];
        out[2+i]=t->fade_wait; out[10+i]=t->fade_reload;
        out[18+i]=t->flags; out[26+i]=t->flags2;
        out[34+i]=(uint8_t)t->cursor; out[42+i]=(uint8_t)(t->cursor>>8);
        out[50+i]=t->wait_lo; out[58+i]=t->wait_hi;
        out[66+i]=t->reload_lo; out[74+i]=t->reload_hi;
        out[82+i]=t->volume_wait;
        out[90+i]=(uint8_t)t->volume_pattern; out[98+i]=(uint8_t)(t->volume_pattern>>8);
        out[106+i]=t->volume_pos; out[114+i]=t->attenuation; out[122+i]=t->detune;
        out[130+i]=t->pitch_wait; out[138+i]=t->pitch_pos; out[146+i]=t->volume;
        out[154+i]=t->low; out[162+i]=t->high; out[170+i]=t->stack_size;
        out[246+i]=t->slot;
    }
}
