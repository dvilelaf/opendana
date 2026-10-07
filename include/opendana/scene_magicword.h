/* scene_magicword.h - "Today's magicword", the `quit` of "What next?" (C3.3).
 *
 * The mode that follows the "What next?" close, one state per step:
 *
 *     state 0  draws the layout, the box and the magicword of the progress with
 *              the key taken from the frame counter of that frame;
 *     state 1  waits for A, then queues the "Will you continue the game?  Yes
 *              No" layout;
 *     state 2  RIGHT/LEFT pick No/Yes and the cursor sprite is drawn every frame;
 *              A on Yes returns to "What next?" (mode 3), A on No enters state 3;
 *     state 3  the good-night cinematic, then the colour loop forever (screens.c,
 *              OD_SCREEN_GOODNIGHT).
 *
 * Every VRAM record goes through the queue: it enters while its byte count plus
 * the queued length stays below the capacity (60), otherwise the routine waits
 * for the next frame, which is what staggers the drawing. The mode loop cycles
 * palette entries 5..7 (every 16 frames) and 19 (every 8 frames) once state 0
 * has returned. All the data comes from the package (screens/magicword.json). */
#ifndef OD_SCENE_MAGICWORD_H
#define OD_SCENE_MAGICWORD_H

#include "opendana/common.h"

#define OD_MW_RECORDS     32
#define OD_MW_RECORD_DATA 63
#define OD_MW_STEPS       8
#define OD_MW_CUES        16
#define OD_MW_WORD_FIELDS 24

typedef struct {
    uint8_t count;        /* count byte: low 6 bits = bytes, bit 7 = +32 */
    uint16_t addr;        /* PPU address */
    uint8_t data[OD_MW_RECORD_DATA];
} od_mw_record;

enum { OD_MW_OP_RECORDS = 0, OD_MW_OP_YIELD, OD_MW_OP_WORD };

typedef struct {
    uint8_t op, first, count; /* records [first, first + count) */
} od_mw_step;

typedef struct {
    int loaded;
    uint8_t palette[32], bg_banks[4], sprite_banks[2], mask;
    uint8_t queue_capacity, program_start, palette_age, song_id, song_age;
    od_mw_record records[OD_MW_RECORDS];
    unsigned record_count;
    od_mw_step screen[OD_MW_STEPS], question[OD_MW_STEPS];
    unsigned screen_steps, question_steps;
    uint16_t word_addr;
    uint8_t word_count, word_group, word_gap, glyphs[32];
    uint8_t cursor_y, cursor_tile, cursor_attr, cursor_x[2];
    uint8_t bg_mask, bg_entries[3], bg_tables[3][3];
    uint8_t sprite_mask, sprite_entry, sprite_table[5];
    /* The good-night cinematic (state 3). */
    unsigned night_duration, night_palette_tick;
    uint8_t night_palette[32];
    uint8_t night_mask, night_entry, night_table[4];
    struct { uint16_t tick; uint8_t id; } night_cues[OD_MW_CUES];
    unsigned night_cue_count;
} od_magicword_art;

typedef enum {
    OD_MAGICWORD_NONE = 0,
    OD_MAGICWORD_YES,   /* back to "What next?" (mode 3)   */
    OD_MAGICWORD_NO     /* the good-night cinematic        */
} od_magicword_event;

/* The scene keeps the PPU state its CPU frames build; what the display pass
 * uploads during frame N is what frame N + 1 shows (the display lags one frame). */
typedef struct {
    unsigned age;            /* CPU frames since the scene was entered      */
    unsigned state;          /* state & 3 */
    unsigned option;         /* cursor choice: 0 = Yes, 1 = No */
    const od_mw_step *prog; /* the running VRAM program, NULL when done    */
    unsigned prog_steps, step, rec, word_done;
    unsigned prev_buttons;
    int blocked;             /* the loop waits in the queue this frame */
    int pending_palette;     /* palette upload deferred by a full queue     */
    uint8_t field[OD_MW_WORD_FIELDS]; /* the 5-bit symbols of the word */
    uint8_t key;             /* the key byte the word was built with      */
    uint8_t palette[32];     /* the palette buffer */
    uint16_t ppu[2382];      /* the next frame's display (OD_SCENE_WORDS)  */
} od_scene_magicword;

/* Enter the scene: blank nametable, the mode's palette, banks and mask. */
void od_scene_magicword_begin(od_scene_magicword *m, const od_magicword_art *art);
/* Static v2 path borrows only typed immutable data. Existing POD is retained:
 * no raw static command pointers/synthetic legacy art are stored in states.
 * Preserve caller's LIVE PPU control/mask, never a captured PPU-mask field. */
od_status od_scene_magicword_begin_static(od_scene_magicword *m,const uint16_t live[2382]);
od_status od_scene_magicword_validate_static(const od_scene_magicword *m);
od_magicword_event od_scene_magicword_tick_static(od_scene_magicword *m,
    unsigned buttons,uint16_t frame_tick,const uint16_t words[10],uint8_t *cycle3,
    uint8_t *cycle5,uint16_t display[2382]);
od_status od_scene_magicword_take_static_sound(od_scene_magicword *m,unsigned *id,int *present);
void od_magicword_night_cycle_static(uint16_t frame_tick,uint8_t *cycle4,uint16_t display[2382]);

/* One CPU frame. `buttons` is the native OD_BTN_* mask of the frame,
 * `frame_tick` the frame counter the frame sees, `words` the progress (one round
 * bitmap per world). `cycle3` and `cycle5` are the two shared colour counters
 * (the map's and the editor's). Writes the display the frame shows into
 * `display`. */
od_magicword_event od_scene_magicword_tick(od_scene_magicword *m,
                                             const od_magicword_art *art,
                                             unsigned buttons, uint16_t frame_tick,
                                             const uint16_t words[10],
                                             uint8_t *cycle3, uint8_t *cycle5,
                                             uint16_t *display);

/* The magicword text of the field (24 characters + NUL), "" before state 1. */
void od_scene_magicword_text(const od_scene_magicword *m, char out[25]);

/* The step of the good-night loop on one CPU frame: every 8 frames palette
 * entry 3 from its table, counter mod 4. */
void od_magicword_night_cycle(const od_magicword_art *art, uint16_t frame_tick,
                               uint8_t *cycle4, uint16_t *display);

#endif /* OD_SCENE_MAGICWORD_H */
