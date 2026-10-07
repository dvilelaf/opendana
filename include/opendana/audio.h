/* EU/PAL sequencer and APU synthesis. Pure C11, no SDL or ROM dependency. */
#ifndef OD_AUDIO_H
#define OD_AUDIO_H
#include "opendana/common.h"

#define OD_AUDIO_RATE 48000
#define OD_AUDIO_SONGS 119
#define OD_MUSIC_NONE (-1)

typedef enum {
    OD_MUSIC_LEVEL, OD_MUSIC_MENU, OD_MUSIC_MAP, OD_MUSIC_BOSS,
    OD_MUSIC_COUNT
} od_music_id;
typedef enum {
    OD_SFX_ICE, OD_SFX_FIRE, OD_SFX_CLEAR, OD_SFX_GAMEOVER,
    OD_SFX_COUNT
} od_sfx_id;

typedef struct od_audio_engine od_audio_engine;
typedef struct {
    od_audio_engine *engine;
    int ready, rate, music_on;
    uint64_t ticks, samples, overruns;
    int error; /* malformed/missing data or request queue overflow */
} od_audio;

od_status od_audio_init(od_audio *a, const char *assets_dir);
void od_audio_shutdown(od_audio *a);
/* Exactly once per accepted simulation tick, even when output is disabled.
 * Generates 959 or 960 samples with persistent PAL rational remainder.
 * Rendering/callbacks NEVER advance the sequencer or its clock. */
void od_audio_tick(od_audio *a);
void od_audio_render(od_audio *a, int16_t *out, int frames);
size_t od_audio_available(const od_audio *a);
int od_audio_rate(const od_audio *a);

/* Sound request ids 1..119; consumed one per simulation tick. */
od_status od_audio_request(od_audio *a, unsigned id);

/* Intra-frame placement of the sequencer, in CPU cycles after the tick edge.
 *
 * The sequencer runs once per frame, a fixed number of cycles after the frame
 * edge, rather than on the edge itself (offset 0). The value is measured, not
 * tuned per note, and is applied to every tick and every song. 0 restores the
 * edge placement. */
void od_audio_set_driver_offset(od_audio *a, unsigned cycles);
unsigned od_audio_driver_offset(const od_audio *a);

/* Derived per-write placement (default on). When on, each sequencer register
 * write is applied at the cycle position its operation reached inside the
 * frame (per-write costs fitted from recorded traces), instead of every write
 * landing at driver_offset. Turning it off restores the single-offset
 * behaviour. Neither mode changes the sequencer state or the sample clock. */
void od_audio_set_write_placement(od_audio *a, int on);
int od_audio_write_placement(const od_audio *a);

/* The first frame ends on scanline 241, so its audio is short (it does not
 * cover the 240 visible lines before the blanking interval): 25,684 CPU
 * cycles, 741 samples, not 960. The engine defaults boot_cycles to that
 * measured value so its synthesis cycle grid starts where the machine's does
 * (the third frame starts at 92,179, not 99,742); 0 keeps all ticks full
 * length. The value belongs to the machine, not to a song. */
void od_audio_set_boot_cycles(od_audio *a, unsigned cycles);
unsigned od_audio_boot_cycles(const od_audio *a);

/* Initial frame-counter phase, in CPU cycles into the frame. Start-up resets
 * the counter 3-4 cycles after writing its register; this sets the phase the
 * engine starts from. */
void od_audio_set_frame_phase(od_audio *a, unsigned cycles);
unsigned od_audio_frame_phase(const od_audio *a);

/* Start-up sequence, not the music sequencer: two registers are written in
 * the third frame, at the measured cycles 114,796 and 114,802 from reset. The
 * sequencer never writes either, so the app calls this once at that frame; the
 * engine applies them at their measured intra-frame instants (22,617 and
 * 22,623 cycles into the frame) so the synthesis clock, the frame counter and
 * the cycle observer all see the same start-up. It never consumes a queued
 * request. */
void od_audio_reset_init(od_audio *a);
/* Priority byte a request is compared against while the sequencer is locked. */
void od_audio_set_request_guard(od_audio *a, uint8_t guard);
/* Scratch bytes read when a track has no volume pattern (the pattern pointer
 * is still followed, so a missing pattern reads these bytes at the track's
 * volume position). The engine defaults to the measured bytes (0x88, 0x1e),
 * which hold the triangle's bit 4 and make its first write 0x9f; the app
 * supplies the game's own working bytes when it models them. addr counts
 * from 0. */
void od_audio_set_driver_ram(od_audio *a, uint16_t addr, uint8_t value);
uint8_t od_audio_driver_ram(const od_audio *a, uint16_t addr);
void od_audio_play_music(od_audio *a, int track);
void od_audio_stop_music(od_audio *a);
int od_audio_music_playing(const od_audio *a);
void od_audio_play_sfx(od_audio *a, int sfx);
void od_audio_set_music_volume(od_audio *a, int percent);
void od_audio_set_sfx_volume(od_audio *a, int percent);

/* Integration helpers: level 99 is the final boss; other worlds use the
 * per-world list imported with the package (`audio_layout` in pack.json).
 * The caller supplies confirmed action events, never raw button presses. */
unsigned od_audio_level_request(unsigned level);
void od_audio_accepted_sfx(od_audio *a, int sfx, int accepted);

/* C5.3: the sequencer's mutable state as an opaque byte block, so the
 * `.odstate` format can resume the audio engine exactly (the PCM hash of a
 * resumed run matches). `size` is the block length; `save` writes zeroes when
 * no engine is loaded. Export clears all three callback pointers AND their
 * context pointers in the output only, leaving live observers unchanged;
 * import keeps the destination's observers/trace installed in `a`.
 * The caller owns tick/render/callback synchronization (SDL device lock for
 * the frontend). Size/API are unchanged; no private layout escapes here. */
size_t od_audio_state_size(void);
void od_audio_state_save(const od_audio *a, void *out);
void od_audio_state_load(od_audio *a, const void *in);

/* Diagnostic dump of the sequencer state (a fixed 256-byte layout) and the
 * sound-register write observer. This is sequencer data, not a CPU emulator.
 * Observer runs on tick thread. */
void od_audio_snapshot(const od_audio *a, uint8_t out[256]);
typedef void (*od_audio_write_fn)(void *context, uint16_t reg, uint8_t value);
void od_audio_observe(od_audio *a, od_audio_write_fn fn, void *context);

/* Cycle-aware observer: the same ordered register writes, but carrying the
 * **effective CPU cycle at which the write is applied to the synthesis**,
 * counted from the reset origin (the engine's own clock, which models the short
 * boot frame and the intra-frame placement). It is not a tick edge
 * reconstructed by the caller, so tools can compare the ordered
 * (register,value,cycle) list without a transport artifact. Setting this observer does not disable the value-only
 * one; both may run. Cycle is non-decreasing across writes. */
typedef void (*od_audio_write_cycle_fn)(void *context, uint64_t cycle,
                                         uint16_t reg, uint8_t value);
void od_audio_observe_cycles(od_audio *a, od_audio_write_cycle_fn fn,
                              void *context);

/* Development-only event trace: reports the ordered sequencer operations the
 * engine performs each tick so tools can fit their cycle costs against
 * recorded traces. It is an observer; it never changes sequencing. Event codes
 * are OD_AUDIO_EV_* and (a,b) hold small counters/register/value. */
enum {
    OD_AUDIO_EV_MAIN_TRACK = 1, /* main loop: examine one of the 8 tracks */
    OD_AUDIO_EV_SEQ_CALL,       /* reached sequence() for a track */
    OD_AUDIO_EV_SEQ_BYTE,       /* consumed one stream byte (a=byte) */
    OD_AUDIO_EV_WRITE,          /* sound register write (a=reg, b=value) */
    OD_AUDIO_EV_ENABLE_LOOP,    /* enable-value loop iteration        */
    OD_AUDIO_EV_EMIT_LOOP,      /* final emit loop iteration (a=channel)*/
    OD_AUDIO_EV_FADE_LOOP,      /* fade loop iteration (a=channel)      */
    OD_AUDIO_EV_PLACED,         /* write placed (a=cycle offset, b=register) */
};
typedef void (*od_audio_trace_fn)(void *context, int event, int a, int b);
void od_audio_set_trace(od_audio *a, od_audio_trace_fn fn, void *context);
#endif
