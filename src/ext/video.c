/* video.c - video presentation geometry (C5.10).
 *
 * See include/opendana/ext/video.h. Pure integer arithmetic: no SDL, no floating
 * point, so the same rectangle is produced on every compiler and the unit
 * tests can pin the exact values. The geometric contract:
 *
 *   square pixels, integer k :  w = 256*k, h = 240*k
 *   8:7 pixels,    integer k :  w = round(256*k*8/7), h = 240*k
 *
 * Everything is centred; the leftover window is black (the renderer clears
 * black before copying). */
#include "opendana/ext/video.h"

/* round(v * 8 / 7) for the 8:7 horizontal widening. */
static int round8_7(int v)
{
    return (v * 8 + 3) / 7;
}

int od_video_scaled_width(int k, int aspect_87)
{
    if (k < 1)
        k = 1;
    if (!aspect_87)
        return OD_SCREEN_W * k;
    return round8_7(OD_SCREEN_W * k);
}

/* Largest non-integer fit: scale down until both dimensions fit, keeping the
 * aspect ratio (with the 8:7 widening when requested). Used when integer
 * scaling is off or when even one whole multiple is larger than the window. */
static void fit_viewport(int win_w, int win_h, int aspect_87, int *ow, int *oh)
{
    /* Start from the height and derive the square width, then the display
     * width. The height bound comes from the width constraint:
     *   square pixels: sw = sh*256/240 = sh*16/15 <= win_w -> sh <= win_w*15/16
     *   8:7 pixels:    dw = sw*8/7 <= win_w              -> sh <= win_w*105/128
     */
    int max_sh;
    if (aspect_87)
        max_sh = (win_w * 105) / 128;
    else
        max_sh = (win_w * 15) / 16;
    int sh = win_h < max_sh ? win_h : max_sh;
    if (sh < 1)
        sh = 1;

    int sw = (sh * OD_SCREEN_W + OD_SCREEN_H / 2) / OD_SCREEN_H;
    if (sw < 1)
        sw = 1;

    int dw = aspect_87 ? round8_7(sw) : sw;
    /* Round-off safety: shrink until the width really fits. */
    while (dw > win_w && sw > 1) {
        sw--;
        dw = aspect_87 ? round8_7(sw) : sw;
    }
    if (dw > win_w)
        dw = win_w;
    if (dw < 1)
        dw = 1;

    *ow = dw;
    *oh = sh;
}

void od_video_viewport(int win_w, int win_h, int integer, int aspect_87,
                        od_video_rect *out)
{
    if (out == NULL)
        return;
    if (win_w < 1)
        win_w = 1;
    if (win_h < 1)
        win_h = 1;

    int w = 0, h = 0;
    if (integer) {
        /* Largest whole multiple of the frame height that fits. The width is
         * monotonic in k, so a single downward scan finds it. */
        int k_max = win_h / OD_SCREEN_H + 1;
        for (int k = k_max; k >= 1; k--) {
            int cw = od_video_scaled_width(k, aspect_87);
            int ch = OD_SCREEN_H * k;
            if (cw <= win_w && ch <= win_h) {
                w = cw;
                h = ch;
                break;
            }
        }
    }
    if (w == 0) {
        /* Integer scaling off, or not even one multiple fits: largest fit. */
        fit_viewport(win_w, win_h, aspect_87, &w, &h);
    }

    out->w = w;
    out->h = h;
    out->x = (win_w - w) / 2;
    out->y = (win_h - h) / 2;
}
