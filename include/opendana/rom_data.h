/* C6.3: remaining EU content, never compiled into the engine. */
#ifndef OD_ROM_DATA_H
#define OD_ROM_DATA_H

#include "opendana/content.h"

#define OD_ROM_DATA_PATH "engine/rom-data.json"
#define OD_ROM_BOSS_IDLE_SIZE 50
#define OD_ROM_BOSS_HIT_SIZE 11
#define OD_ROM_BOSS_DEFEAT_SIZE 41
#define OD_ROM_PASSWORD_SYMBOLS 32

typedef struct {
    uint8_t boss_idle[OD_ROM_BOSS_IDLE_SIZE];
    uint8_t boss_hit[OD_ROM_BOSS_HIT_SIZE];
    uint8_t boss_defeat[OD_ROM_BOSS_DEFEAT_SIZE];
    uint8_t password_code[OD_ROM_PASSWORD_SYMBOLS];
    uint8_t password_glyph[OD_ROM_PASSWORD_SYMBOLS];
    char password_display[OD_ROM_PASSWORD_SYMBOLS + 1];
} od_rom_data;

/* Parse through the layered VFS without installing. No output mutation on
 * failure. err is optional and gets the first error. No ROM access. */
od_status od_rom_data_parse(od_rom_data *out, const od_content_vfs *vfs,
                             char *err, size_t cap);

/* Validate and install a COPY, atomically. NULL clears the installation.
 * Malformed input returns FORMAT and retains the previous installation.
 * Caller must serialize installation with simulation, as for engine tables. */
od_status od_rom_data_install(const od_rom_data *data);

/* Installed data, or a lazy load of the default package, or NULL. Explicit
 * startup must parse/install with the actual mod VFS (see C6.3.md). */
const od_rom_data *od_rom_data_get(void);

/* Where the sound data sits inside the sparse image the sequencer reads, and
 * the per-world music request. Imported with the package (the `audio_layout`
 * object of pack.json), so the engine holds no positions of its own.
 *
 * Slots are numbered as in audio/sequence.json (first_slot .. first_slot +
 * slot_count - 1) and hold slot_size bytes each. A pointer stored inside the
 * sound data is a 16-bit address: window[i] says which slot an address range
 * of slot_size bytes belongs to (slot >= 0), or that the caller names the slot
 * (slot == -1). The table fields are byte offsets into the image, counted from
 * the start of the first slot. */
#define OD_AUDIO_LAYOUT_PATH "pack.json"
#define OD_AUDIO_WINDOWS 4
#define OD_AUDIO_WORLD_MUSIC 10

typedef struct {
    uint16_t first;
    int16_t slot;
} od_audio_window;

typedef struct {
    uint16_t slot_size;
    uint8_t first_slot, slot_count, window_count;
    od_audio_window window[OD_AUDIO_WINDOWS];
    uint16_t fade_steps;       /* one byte per attenuation level */
    uint16_t song_slot;        /* one byte per request id: slot of its header */
    uint16_t song_start;       /* one word per request id: address of its header */
    uint16_t periods;          /* 16 words: note periods */
    uint16_t durations;        /* 128 words: note lengths */
    uint16_t effects;          /* 64 records of 4 bytes: effect parameters */
    uint16_t volume_patterns;  /* one word per pattern: address of its steps */
    uint16_t pitch_steps;      /* pairs: wait and delta */
    uint16_t sample_base;      /* address the sample channel counts from */
    uint8_t world_music[OD_AUDIO_WORLD_MUSIC];
} od_audio_layout;

od_status od_audio_layout_parse(od_audio_layout *out, const od_content_vfs *vfs,
                                char *err, size_t cap);
od_status od_audio_layout_install(const od_audio_layout *layout);
/* Installed layout, or a lazy load of the default package, or NULL. */
const od_audio_layout *od_audio_layout_get(void);

#endif
