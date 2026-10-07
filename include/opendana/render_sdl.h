/* render_sdl.h - SDL2 window/renderer wrapper. Only this layer needs SDL. */
#ifndef OD_RENDER_SDL_H
#define OD_RENDER_SDL_H

#include "opendana/common.h"
#include "opendana/ext/video.h"

#include <SDL.h>

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int has_vsync;
    /* C5.10 presentation options. The window size is
     * queried at present time, so a resize is honoured immediately. */
    int integer_scale;
    int aspect_87;
    SDL_Texture *overlay;       /* lazily created blended 256x240 texture   */
    SDL_Texture *native;        /* C6.48: window-sized texture of the first-run screen */
    /* C6.32: the pixel aspect the window size was last made for (UI-10), and
     * whether that size is ours to change (not when an explicit size was
     * requested). */
    int window_aspect;
    int window_sized;
} od_renderer;

/* `scale` is the initial integer window scale; `headless` hides the window
 * (used together with SDL_VIDEODRIVER=dummy for screenshot verification).
 * `vsync` requests SDL_RENDERER_PRESENTVSYNC; when it is 0 or unavailable the
 * software renderer is preferred so the presentation rate cannot gate the
 * simulation (H9). The simulation clock never depends on this choice. */
od_status od_renderer_init(od_renderer *r, int scale, int headless, int vsync);
void od_renderer_shutdown(od_renderer *r);

/* Ask the next od_renderer_init for a borderless desktop-fullscreen window and
 * raise it so it takes keyboard focus (otherwise the game paints but ignores
 * every key: main.c only reads the keyboard while the window has focus). */
void od_renderer_request_fullscreen(int on);

/* Ask the next od_renderer_init to maximise the window (window manager
 * maximise, keeps the title bar). */
void od_renderer_request_maximized(int on);

/* Ask the next od_renderer_init for an explicit window size instead of
 * 256*scale x 240*scale. Used by the C5.10 capture instrument to exercise the
 * viewport at unusual sizes. 0x0 restores the scale-based size. */
void od_renderer_request_size(int w, int h);

/* Toggle fullscreen at runtime (EWMH, never a video-mode change). */
void od_renderer_toggle_fullscreen(od_renderer *r);
/* C6.32 (UI-9): set fullscreen on or off (no toggling against a setting that
 * may already disagree with the window), and query it. */
void od_renderer_set_fullscreen(od_renderer *r, int on);
int od_renderer_is_fullscreen(const od_renderer *r);

/* C6.32 (UI-22): drawable size in pixels (larger than the window size on a
 * HiDPI display), and a window point (SDL mouse coordinates) in those pixels.
 * Every viewport is computed in drawable pixels. */
void od_renderer_output_size(const od_renderer *r, int *w, int *h);
void od_renderer_point_to_output(const od_renderer *r, int wx, int wy,
                                  int *ox, int *oy);

/* C5.10: set the presentation options (integer scaling and 8:7 pixels). Cheap;
 * call it every frame or when the settings change. C6.32 (UI-10): when the
 * pixel aspect changes and the window is neither maximised nor fullscreen,
 * its width follows (same vertical scale), so 8:7 never drops a scale step. */
void od_renderer_set_video(od_renderer *r, int integer_scale, int aspect_87);

/* Current destination rectangle of the 256x240 frame, from the live window
 * size and the options set with od_renderer_set_video(). */
void od_renderer_get_viewport(const od_renderer *r, od_video_rect *out);

/* True when the renderer actually got a vsync-capable presentation. */
int od_renderer_has_vsync(const od_renderer *r);

/* Upload an ARGB8888 256x240 buffer and present it. */
od_status od_renderer_present(od_renderer *r, const uint32_t *argb);

/* C6.48: present a w x h ARGB image over the whole drawable (the first-run
 * screen is drawn at the window's own resolution, not on the 256x240 frame). */
od_status od_renderer_present_native(od_renderer *r, const uint32_t *argb, int w, int h);

/* C6.45: call once per presented frame; in fullscreen the mouse cursor hides
 * after 3 s without mouse activity and returns on motion or click. */
void od_renderer_cursor_update(od_renderer *r);

/* Present the game frame with a 256x240 ARGB overlay on top (alpha 0 =
 * transparent). Both are scaled to the same viewport with nearest neighbour;
 * `overlay` may be NULL. This replaces the overlay path of src/app/ui.c so the
 * viewport and the option live in one place (C5.10). */
od_status od_renderer_present_overlay(od_renderer *r, const uint32_t *argb,
                                        const uint32_t *overlay);

/* Read back the current backbuffer (ARGB8888). `out` holds 256*240 pixels. */
od_status od_renderer_read(od_renderer *r, uint32_t *out);

/* Read the whole window backbuffer (ARGB8888) into a freshly malloc'd buffer;
 * the caller frees it. Used by the C5.10 capture instrument. */
od_status od_renderer_read_window(od_renderer *r, uint32_t **out,
                                    int *w, int *h);

#endif /* OD_RENDER_SDL_H */
