/* video.h - video presentation geometry (C5.10).
 *
 * Pure C11, no SDL: the window/screen presentation of the 256x240 frame is a
 * function of the window size and two options, so it can be unit tested
 * without a display:
 *
 *   - integer scale: the frame is drawn at the largest whole multiple of
 *     256x240 that fits in the window, centred, with black borders. With
 *     integer scale off, the frame is enlarged to the largest size that fits
 *     keeping the aspect ratio.
 *   - 8:7 pixel aspect: widen horizontally by 8/7, as a PAL television showed
 *     the NES pixels. With integer scale the height is 240*k and the width is
 *     round(256*k*8/7); the scaling is always nearest neighbour (no
 *     interpolation), whatever the option.
 *
 * The presentation itself lives in src/render/render_sdl.c and the host of
 * src/app/main.c; the simulation never sees any of this. */
#ifndef OD_EXT_VIDEO_H
#define OD_EXT_VIDEO_H

#include "opendana/common.h"

typedef struct {
    int x, y;   /* top-left corner inside the window (pixels)            */
    int w, h;   /* size of the presented frame (pixels, nearest scaling) */
} od_video_rect;

/* Destination rectangle of the 256x240 frame inside a `win_w` x `win_h`
 * window. `integer` selects whole-multiple scaling (otherwise largest fit);
 * `aspect_87` applies the 8/7 horizontal widening. The rectangle is always
 * centred and never exceeds the window when any size fits; when the window is
 * smaller than one whole multiple, the integer mode degrades to the largest
 * fit so the frame is still fully visible. */
void od_video_viewport(int win_w, int win_h, int integer, int aspect_87,
                        od_video_rect *out);

/* Width of the presented frame at an `integer` vertical scale, for the
 * aspect options given. Exposed so the presentation and the tests agree on the
 * exact rounding rule: !aspect_87 -> OD_SCREEN_W*k ; aspect_87 ->
 * round(OD_SCREEN_W*k*8/7). */
int od_video_scaled_width(int k, int aspect_87);

#endif /* OD_EXT_VIDEO_H */
