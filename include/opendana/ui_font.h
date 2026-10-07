/* ui_font.h - 8x8 bitmap font for the modern interface.
 *
 * The interface uses its own font, never the game's
 * graphics. This is the public-domain 8x8 font by Daniel Hepper (based on
 * Marcel Sondaar / IBM VGA public-domain glyphs), drawn into an ARGB canvas.
 * Nothing here comes from the ROM. */
#ifndef OD_UI_FONT_H_INCLUDED
#define OD_UI_FONT_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#define OD_UI_FONT_W 8
#define OD_UI_FONT_H 8

/* Draw `text` into an ARGB canvas of `cw` x `ch` pixels. `scale` is a positive
 * integer pixel multiplier. Returns the advance width in pixels. */
int od_ui_font_text(uint32_t *canvas, int cw, int ch, int x, int y,
                     const char *text, uint32_t argb, int scale);

/* Width in pixels of `text` at `scale` (8*scale per character). */
int od_ui_font_width(const char *text, int scale);

/* C6.24: text never runs out of its box. Copy `text` into `out` (`cap`
 * bytes) cut to at most `max_w` pixels at `scale`; a cut text ends in "...".
 * Returns `out`. */
const char *od_ui_font_fit(const char *text, int max_w, int scale,
                            char *out, size_t cap);

/* Break `text` at spaces into at most `max_lines` lines no wider than `max_w`
 * pixels; a word longer than a line is split, and the last line ends in "..."
 * when the rest does not fit. Line i is written at out + i * line_cap.
 * Returns the number of lines. */
int od_ui_font_wrap(const char *text, int max_w, int scale, char *out,
                     size_t line_cap, int max_lines);

#endif /* OD_UI_FONT_H_INCLUDED */
