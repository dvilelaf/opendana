#ifndef OD_CINEMATIC_H
#define OD_CINEMATIC_H
#include "opendana/common.h"
#include <stdint.h>
typedef struct od_cinematic_data od_cinematic_data;
/* Simulation events, never render events. Empty managed queue is NOT permission
 * to replay a legacy agenda. ID zero is an ordinary request, not a sentinel. */
typedef struct {
    uint8_t managed, count, ids[8];
    uint8_t overflow; /* fail closed: more than eight must not be truncated */
} od_cinematic_sounds;
typedef struct {
    uint8_t type, x, y, flags, pose, fx, fy, animation, phase, hold;
    uint8_t reverse_axes, reverse_clock; /* EU87E4/8B74, actor6's real period */
    int8_t acceleration_x, acceleration_y; /* EU880B/8BC2, actor3 after movement */
    int16_t vx, vy;
} od_cinematic_actor;
typedef struct {
    const od_cinematic_data *data;
    unsigned scene, pc, tick, delay, started, finished, flush, frame_tick, exit_age;
    unsigned sound, skip_enabled;
    /* Native presentation tasks; never CPU state or captured frame ages.
     * reset_sync counts the two vblank observations of the reset path.
     * entry_stage represents init / palette wait / cleared-display wait. */
    uint8_t reset_sync, entry_stage, render_enabled;
    uint8_t cleanup_stage; /* world-clear: 0off,1palette pending,2NT/OAM pending,3published */
    unsigned transfer_command;
    /* C6.17 EU8460 schedules0614/0615;89FD queues ALL decoded records.
     * Logical job index only, never an executable address/captured age. */
    unsigned vram_command;
    uint8_t vram_pending;
    od_cinematic_sounds sounds;
    uint8_t tiles[2048], palette[32], pending_palette[32], palettes[8], backdrop;
    uint8_t scroll_x, scroll_y, control, sprite_banks[2], bg_banks[4], split_banks[4];
    uint8_t band_row[4], band_x[4], band_control[4], band_origin[4], band_cut;
    int16_t band_speed[4];
    uint8_t band_fraction[4], band_mode, band_period, band_clock, band_delta[4];
    int16_t global_speed[2];
    int8_t global_acceleration[2];
    uint8_t global_fraction[2], palette_dirty;
    uint8_t cycle, cycle_hold, cycle_phase, cycle_frame;
    uint8_t fade, fade_hold, fade_speed, fade_brightness;
    uint8_t initial_flushes, chr_mode, next_scene, sprite_split;
    uint8_t backdrop_cycle, backdrop_frame, backdrop_phase, backdrop_hold;
    uint8_t text_active, text_region, text_wait, text_pause, text_speed, text_state;
    uint8_t text_font, text_x;
    unsigned text_pc;
    uint8_t shake, shake_phase, image, image_frame, image_count, image_phase, image_hold;
    uint16_t pending_oam[257];
    od_cinematic_actor actors[8];
} od_cinematic;
od_status od_cinematic_load(od_cinematic_data **out, const char *path);
#define OD_CINEMATIC_TEXT_API1 1
#define OD_CINEMATIC_TEXT_API 1
/* Pure parser for the exact pinned VFS bytes. size excludes a terminator;
 * no terminator is required in text, and the result owns all parsed data.
 * No I/O. On failure *out is NULL. */
od_status od_cinematic_load_text(od_cinematic_data **out,const char *text,size_t size);
void od_cinematic_free(od_cinematic_data *data);
/* Parsed semantic source index, not executable address or opaque layout. */
od_status od_cinematic_scene_index(const od_cinematic_data *data,unsigned scene,
                                    unsigned *source_index);
/* Execute program/yield boundaries without drawing, for caller timing records. */
od_status od_cinematic_duration(const od_cinematic_data *data,unsigned scene,unsigned *duration);
od_status od_cinematic_reset_duration(const od_cinematic_data *data,unsigned *duration);
void od_cinematic_begin(od_cinematic *s, const od_cinematic_data *data, unsigned world);
/* Cold product entry, distinct from re-entering the same scene program. */
void od_cinematic_begin_reset(od_cinematic *s,const od_cinematic_data *data);
od_status od_cinematic_tick(od_cinematic *s, uint16_t display[2382]);
/* Execute the native non-chained cleanup after a permitted START skip.
 * Does not seek a program/timeline; preserves requests already emitted. */
od_status od_cinematic_skip(od_cinematic *s);
/* Pure structural guard; trusted owner is selected by live screens identity.
 * Reject candidate.data != owner BEFORE accessing any opaque content. */
od_status od_cinematic_validate_candidate(const od_cinematic *candidate,
                                          const od_cinematic_data *owner);
/* Drain once after simulation advance, before the sequencer advance. Repeated
 * takes return managed=true/count=0. No timing/fidelity guarantee is implied
 * by managed: it identifies native ownership, not acceptance of a family. */
od_status od_cinematic_take_sounds(od_cinematic *s, od_cinematic_sounds *out);
#endif
