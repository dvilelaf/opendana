/* hud.h - in-game status bar (ROUND / TIME) drawn in pure C11.
 *
 * The reference draws a 24-pixel band in the bottom rows (216..239) of the
 * 256x240 frame with the text
 *
 *     ROUND  1-1        TIME  00:00:16
 *
 * The font is the ASCII tile set: tile index == ASCII code, so '0' is tile 48,
 * 'A' is tile 65, ':' is 58 and '-' is 45 (verified against the captured
 * frame). The glyphs use pixel value 3 with background palette 0 (palette
 * entries 0x0F black and 0x30 white).
 *
 * Everything here is pure C11 with no SDL. `od_hud_draw` overwrites the bottom
 * band of the shared `od_index_frame` after the level has been composed. */
#ifndef OD_HUD_H
#define OD_HUD_H

#include "opendana/compose.h"
#include "opendana/palette.h"

/* Pixel geometry of the status band, matching the captured frame exactly. */
#define OD_HUD_TOP 216         /* first overwritten row (24 rows tall)     */
#define OD_HUD_TEXT_Y 224      /* tile row of the glyphs (row 224 blank)   */
#define OD_HUD_ROUND_COL 4     /* "ROUND" label, tile column               */
#define OD_HUD_ROUND_VAL_COL 11/* "w-r" value, tile column                 */
#define OD_HUD_TIME_COL 17     /* "TIME" label, tile column                */
#define OD_HUD_TIME_VAL_COL 22 /* "hh:mm:ss" value, tile column            */
#define OD_HUD_TIME_CHARS 8    /* digits in the reference time field          */
#define OD_HUD_BG_PAL 0        /* background palette used by the band      */

/* The four clock bytes, advanced only when the simulation runs the clock step.
 * The countdown's high bit freezes the saturated 99:59:59 clock. */
typedef struct {
    uint8_t countdown;
    uint8_t hours, minutes, seconds; /* packed decimal */
} od_hud_clock;

/* Returns 1 when the original queues a HUD write, 0 when gated by flags.
 * hero_flags and scene_flags are the two flag bytes the gate reads. Queue/presentation latency belongs
 * to the caller's single simulation clock, not to this renderer. */
int od_hud_clock_tick(od_hud_clock *clock, uint8_t hero_flags, uint8_t scene_flags);
void od_hud_clock_text(char *out, size_t cap, const od_hud_clock *clock);

/* Simulation-owned HUD queue. Call begin_frame at the display pass, then
 * clock_tick and queue after the clock step in the gameplay step. Drawing never
 * advances either latch. Initial clock/visible values must come from the
 * scene's initialisation. */
typedef struct {
    od_hud_clock queued, visible;
    int pending;
} od_hud_latch;
void od_hud_begin_frame(od_hud_latch *latch);
void od_hud_queue(od_hud_latch *latch, const od_hud_clock *clock);
void od_hud_draw_clock(od_index_frame *fb, int level_index,
                        const od_hud_clock *clock,
                        const uint8_t *chr, const od_palette_ram *pal);
/* The editor's user level: the value is the user level number + 1 written from
 * OD_HUD_ROUND_VAL_COL, its second column blanked over the template's '-' when
 * it has one digit ("ROUND 1"), never "w-r". `user_level` is 0-based. */
void od_hud_draw_clock_user(od_index_frame *fb, int user_level,
                             const od_hud_clock *clock,
                             const uint8_t *chr, const od_palette_ram *pal);
/* Status bar from a scene's own nametable text row (32 ASCII tile codes, 0 =
 * blank) with the live clock written over OD_HUD_TIME_VAL_COL. Used by the
 * Boss arena, whose bar is authored nametable text (od_arena_hud_row). */
void od_hud_draw_row(od_index_frame *fb, const uint8_t row[32],
                      const od_hud_clock *clock,
                      const uint8_t *chr, const od_palette_ram *pal);

/* The bar template the pause close writes: "ROUND", '-' and "TIME" in the
 * normal layout, the round value blank and a box glyph in the time
 * cells. `value` (NULL = still blank) is the round value once the round writer
 * has rewritten it, drawn from `val_col`; `clock` (NULL = still the template's
 * boxes) the digits once the clock writer has written after the template. */
void od_hud_draw_template(od_index_frame *fb, const char *value, int val_col,
                           const od_hud_clock *clock,
                           const uint8_t *chr, const od_palette_ram *pal);

/* The bar template of a round load (written at the play loop's start): "ROUND"
 * 00 00 00 '-' 00 00 00 00 "TIME" 00 20 20 ':' 20 20 ':'. The seconds cells are
 * not in it (they stay empty) and the round writer puts `value` in the same
 * frame, drawn from `val_col`. */
void od_hud_draw_level_template(od_index_frame *fb, const char *value,
                                 int val_col, const uint8_t *chr,
                                 const od_palette_ram *pal);

/* Draw one ASCII character (tile index == ASCII code) at pixel (px,py).
 * Characters outside 32..126 render as a space. Pixel value 0 is drawn as the
 * palette's entry 0 (opaque), so the glyph cell fully replaces what is behind. */
void od_font_putc(od_index_frame *fb, int px, int py, char c,
                   uint8_t bg_pal, const uint8_t *chr, const od_palette_ram *pal);

/* Draw a NUL-terminated string using the CHR ASCII font. `scale` >= 1 enlarges
 * each glyph by that integer factor (used for the world-map title). */
void od_font_text_scaled(od_index_frame *fb, int px, int py, const char *s,
                          int scale, uint8_t bg_pal,
                          const uint8_t *chr, const od_palette_ram *pal);
static inline void od_font_text(od_index_frame *fb, int px, int py, const char *s,
                                 const uint8_t *chr, const od_palette_ram *pal)
{
    od_font_text_scaled(fb, px, py, s, 1, OD_HUD_BG_PAL, chr, pal);
}

/* Format the ROUND value for a level index: "w-r" for the 100 normal rounds
 * (level 0..99) and a plain number for the hidden levels (100..149). */
void od_hud_round_text(char *out, size_t cap, int level_index);

/* Legacy convenience formatter for enabled ticks (60 ticks per second),
 * saturated at 99:59:59. Use od_hud_clock for the real gating/countdown;
 * elapsed level frames alone do not reproduce the reference's clock. */
void od_hud_time_text(char *out, size_t cap, long frames);

/* Compose the whole status bar into the bottom 24 rows of `fb`. Text is drawn
 * with background palette 0. `level_index` selects the ROUND label. */
void od_hud_draw(od_index_frame *fb, int level_index, long frames,
                  const uint8_t *chr, const od_palette_ram *pal);

#endif /* OD_HUD_H */
