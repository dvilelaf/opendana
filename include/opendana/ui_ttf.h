/* ui_ttf.h - anti-aliased TrueType text for the modern screens (C6.48).
 *
 * Inter (SIL OFL 1.1, assets/fonts/) rasterised by stb_truetype (external/stb).
 * The fonts are packaged data loaded at run time, never compiled in. Drawing
 * is into any ARGB buffer, at its own resolution, so a screen can follow the
 * window size and the HiDPI drawable scale. SDL-free. */
#ifndef OD_UI_TTF_H
#define OD_UI_TTF_H

#include <stddef.h>
#include <stdint.h>

typedef struct od_ttf od_ttf;

enum { OD_TTF_REGULAR = 0, OD_TTF_SEMIBOLD = 1 };

/* Directory that holds Inter-Regular.ttf and Inter-SemiBold.ttf: the packaged
 * <exe>/../share/opendana/fonts, else the source tree <exe>/../assets/fonts
 * (build/opendana). The environment variable OD_FONT_DIR overrides it. 1 when
 * found. */
int od_ttf_find_dir(char *out, size_t cap);

/* Load both faces from `dir`; NULL when a file is missing or unreadable. */
od_ttf *od_ttf_open(const char *dir);
void od_ttf_close(od_ttf *f);

/* Width in pixels of `utf8` at a pixel height of `px` (em size). */
int od_ttf_width(od_ttf *f, int face, int px, const char *utf8);

/* Distance between baselines and the ascent above the baseline, at `px`. */
int od_ttf_line_height(od_ttf *f, int face, int px);
int od_ttf_ascent(od_ttf *f, int face, int px);

/* Draw `utf8` with its pen origin at (x, baseline_y), blending `argb` (its
 * alpha scales the coverage) over the buffer. */
void od_ttf_draw(od_ttf *f, int face, int px, uint32_t *buf, int bw, int bh,
                 int x, int baseline_y, const char *utf8, uint32_t argb);

/* Copy `utf8` to `out` cut with "..." so that it is at most `max_w` wide. */
const char *od_ttf_fit(od_ttf *f, int face, int px, const char *utf8, int max_w,
                       char *out, size_t cap);

/* Word-wrap `utf8` into at most `max_lines` lines of `max_w` pixels, each in
 * out + i * line_cap; the last line is cut with "..." when text is left.
 * Returns the number of lines. */
int od_ttf_wrap(od_ttf *f, int face, int px, const char *utf8, int max_w,
                char *out, size_t line_cap, int max_lines);

#endif /* OD_UI_TTF_H */
