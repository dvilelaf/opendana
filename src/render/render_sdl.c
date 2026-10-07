#include "opendana/render_sdl.h"

#include <stdio.h>
#include <stdlib.h>

#include "icon_data.h"
#include "../app/cursor_hide.h"

static int g_fullscreen = 0;
static int g_maximized = 0;
static int g_req_w = 0;
static int g_req_h = 0;

/* C6.45: cursor auto-hide in fullscreen (presentation only, wall time). Every
 * frame the host presents goes through od_renderer_cursor_update. */
static volatile int g_cursor_activity;
static od_cursor_hide g_cursor;
static int g_cursor_shown = 1;
static int g_cursor_watch_on;

static int cursor_event_watch(void *ud, SDL_Event *e)
{
    (void)ud;
    if (e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEBUTTONDOWN ||
        e->type == SDL_MOUSEBUTTONUP || e->type == SDL_MOUSEWHEEL)
        g_cursor_activity = 1;
    return 0;
}

void od_renderer_cursor_update(od_renderer *r)
{
    if (!g_cursor_watch_on) {
        SDL_AddEventWatch(cursor_event_watch, NULL);
        g_cursor_watch_on = 1;
    }
    int act = g_cursor_activity;
    g_cursor_activity = 0;
    int visible = od_cursor_hide_step(&g_cursor, SDL_GetTicks(),
                                      od_renderer_is_fullscreen(r), act);
    if (visible != g_cursor_shown) {
        g_cursor_shown = visible;
        SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
        fprintf(stderr, "cursor: %s (shown=%d)\n", visible ? "visible" : "hidden",
                SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE);
    }
}

void od_renderer_request_fullscreen(int on) { g_fullscreen = on; }
void od_renderer_request_maximized(int on) { g_maximized = on; }
void od_renderer_request_size(int w, int h) { g_req_w = w; g_req_h = h; }

int od_renderer_is_fullscreen(const od_renderer *r)
{
    if (r == NULL || r->window == NULL)
        return 0;
    Uint32 f = SDL_GetWindowFlags(r->window);
    return (f & SDL_WINDOW_FULLSCREEN_DESKTOP) || (f & SDL_WINDOW_FULLSCREEN);
}

void od_renderer_set_fullscreen(od_renderer *r, int on)
{
    if (r == NULL || r->window == NULL || od_renderer_is_fullscreen(r) == !!on)
        return;
    /* SDL_WINDOW_FULLSCREEN_DESKTOP is the EWMH fullscreen state: it never
     * changes the video mode. SDL_WINDOW_FULLSCREEN is never used. */
    SDL_SetWindowFullscreen(r->window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void od_renderer_toggle_fullscreen(od_renderer *r)
{
    od_renderer_set_fullscreen(r, !od_renderer_is_fullscreen(r));
}

void od_renderer_set_video(od_renderer *r, int integer_scale, int aspect_87)
{
    if (r == NULL)
        return;
    r->integer_scale = integer_scale ? 1 : 0;
    r->aspect_87 = aspect_87 ? 1 : 0;
    if (r->window == NULL || !r->window_sized || r->window_aspect == r->aspect_87)
        return;
    Uint32 f = SDL_GetWindowFlags(r->window);
    if (!(f & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP |
               SDL_WINDOW_MAXIMIZED))) {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(r->window, &ww, &wh);
        int k = wh / OD_SCREEN_H;
        if (k < 1)
            k = 1;
        SDL_SetWindowSize(r->window, od_video_scaled_width(k, r->aspect_87),
                          OD_SCREEN_H * k);
    }
    r->window_aspect = r->aspect_87;
}

void od_renderer_output_size(const od_renderer *r, int *w, int *h)
{
    int ow = 0, oh = 0;
    if (r != NULL && r->renderer != NULL &&
        SDL_GetRendererOutputSize(r->renderer, &ow, &oh) == 0 && ow > 0 && oh > 0) {
        /* drawable pixels */
    } else if (r != NULL && r->window != NULL) {
        SDL_GetWindowSize(r->window, &ow, &oh);
    }
    if (w) *w = ow;
    if (h) *h = oh;
}

void od_renderer_point_to_output(const od_renderer *r, int wx, int wy,
                                  int *ox, int *oy)
{
    int ww = 0, wh = 0, pw = 0, ph = 0;
    if (r != NULL && r->window != NULL)
        SDL_GetWindowSize(r->window, &ww, &wh);
    od_renderer_output_size(r, &pw, &ph);
    if (ox) *ox = ww > 0 && pw > 0 ? (int)((long long)wx * pw / ww) : wx;
    if (oy) *oy = wh > 0 && ph > 0 ? (int)((long long)wy * ph / wh) : wy;
}

void od_renderer_get_viewport(const od_renderer *r, od_video_rect *out)
{
    if (out == NULL)
        return;
    out->x = out->y = 0;
    out->w = OD_SCREEN_W;
    out->h = OD_SCREEN_H;
    if (r == NULL || r->window == NULL)
        return;
    int ww = 0, wh = 0;
    od_renderer_output_size(r, &ww, &wh);
    od_video_viewport(ww, wh, r->integer_scale, r->aspect_87, out);
}

od_status od_renderer_init(od_renderer *r, int scale, int headless, int vsync)
{
    if (scale < 1)
        scale = 1;
    r->window = NULL;
    r->renderer = NULL;
    r->texture = NULL;
    r->has_vsync = 0;
    r->integer_scale = 1;
    r->aspect_87 = 0;
    r->overlay = NULL;
    r->native = NULL;
    r->window_aspect = 0;            /* the size below is for square pixels */
    r->window_sized = g_req_w <= 0 && g_req_h <= 0;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return OD_ERR_IO;
    }

    int win_w = g_req_w > 0 ? g_req_w : OD_SCREEN_W * scale;
    int win_h = g_req_h > 0 ? g_req_h : OD_SCREEN_H * scale;

    /* C6.32 (UI-22): a HiDPI display gets a full-resolution drawable. */
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (headless)
        flags |= SDL_WINDOW_HIDDEN;
    if (g_fullscreen && !headless)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP; /* EWMH: never a mode change */
    r->window = SDL_CreateWindow("OpenDana",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 win_w, win_h, flags);
    if (r->window == NULL) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return OD_ERR_IO;
    }

    /* C6.42: OpenDana's own icon (branding, not game content). */
    {
        SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormatFrom(
            (void *)od_icon_rgba, OD_ICON_W, OD_ICON_H, 32, OD_ICON_W * 4,
            SDL_PIXELFORMAT_RGBA32);
        if (icon != NULL) {
            SDL_SetWindowIcon(r->window, icon);
            SDL_FreeSurface(icon);
        }
    }

    /* Take keyboard focus. main.c reads the keyboard only while the window has
     * focus, so an unraised window paints but ignores every key (looks frozen). */
    if (!headless) {
        /* Keep the normal window (title bar, so the window manager can maximise
         * it). F11 toggles fullscreen at runtime; the video mode is never
         * changed (a mode change reset the owner's 4K desktop scale). */
        if (g_fullscreen)
            fprintf(stderr, "video: fullscreen window (EWMH, desktop mode kept)\n");
        if (g_maximized)
            SDL_MaximizeWindow(r->window);
        SDL_RaiseWindow(r->window);
    }

    /* Ask for vsync only when requested; otherwise software/accelerated without
     * present-vsync. The simulation clock is independent either way (H9). */
    Uint32 render_flags = SDL_RENDERER_ACCELERATED;
    if (vsync)
        render_flags |= SDL_RENDERER_PRESENTVSYNC;
    r->renderer = SDL_CreateRenderer(r->window, -1, render_flags);
    if (r->renderer == NULL)
        r->renderer = SDL_CreateRenderer(r->window, -1, SDL_RENDERER_SOFTWARE);
    if (r->renderer == NULL) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        od_renderer_shutdown(r);
        return OD_ERR_IO;
    }
    {
        SDL_RendererInfo info;
        if (SDL_GetRendererInfo(r->renderer, &info) == 0)
            r->has_vsync = (info.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
    }

    /* C5.10: the presentation computes its own destination rectangle
     * (od_video_viewport), so SDL's logical size/integer scale are not used.
     * Nearest neighbour is required: the frame is never interpolated. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    r->texture = SDL_CreateTexture(r->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, OD_SCREEN_W, OD_SCREEN_H);
    if (r->texture == NULL) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        od_renderer_shutdown(r);
        return OD_ERR_IO;
    }
    return OD_OK;
}

void od_renderer_shutdown(od_renderer *r)
{
    if (r->native)
        SDL_DestroyTexture(r->native);
    r->native = NULL;
    if (r->overlay)
        SDL_DestroyTexture(r->overlay);
    if (r->texture)
        SDL_DestroyTexture(r->texture);
    if (r->renderer)
        SDL_DestroyRenderer(r->renderer);
    if (r->window)
        SDL_DestroyWindow(r->window);
    r->overlay = NULL;
    r->texture = NULL;
    r->renderer = NULL;
    r->window = NULL;
    r->has_vsync = 0;
    if (SDL_WasInit(SDL_INIT_VIDEO))
        SDL_Quit();
}

int od_renderer_has_vsync(const od_renderer *r)
{
    return r ? r->has_vsync : 0;
}

od_status od_renderer_present(od_renderer *r, const uint32_t *argb)
{
    if (r == NULL || r->renderer == NULL || r->texture == NULL)
        return OD_ERR_RANGE;
    if (SDL_UpdateTexture(r->texture, NULL, argb,
                          OD_SCREEN_W * (int)sizeof(uint32_t)) != 0)
        return OD_ERR_IO;
    od_video_rect vp;
    od_renderer_get_viewport(r, &vp);
    SDL_Rect dst = { vp.x, vp.y, vp.w, vp.h };
    SDL_SetRenderDrawColor(r->renderer, 0, 0, 0, 255);
    SDL_RenderClear(r->renderer);
    if (SDL_RenderCopy(r->renderer, r->texture, NULL, &dst) != 0)
        return OD_ERR_IO;
    od_renderer_cursor_update(r);
    SDL_RenderPresent(r->renderer);
    return OD_OK;
}

od_status od_renderer_present_native(od_renderer *r, const uint32_t *argb, int w, int h)
{
    if (r == NULL || r->renderer == NULL || argb == NULL || w < 1 || h < 1)
        return OD_ERR_RANGE;
    int tw = 0, th = 0;
    if (r->native != NULL)
        SDL_QueryTexture(r->native, NULL, NULL, &tw, &th);
    if (r->native == NULL || tw != w || th != h) {
        if (r->native != NULL)
            SDL_DestroyTexture(r->native);
        r->native = SDL_CreateTexture(r->renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, w, h);
        if (r->native == NULL)
            return OD_ERR_IO;
    }
    if (SDL_UpdateTexture(r->native, NULL, argb, w * (int)sizeof(uint32_t)) != 0)
        return OD_ERR_IO;
    SDL_SetRenderDrawColor(r->renderer, 0, 0, 0, 255);
    SDL_RenderClear(r->renderer);
    if (SDL_RenderCopy(r->renderer, r->native, NULL, NULL) != 0)
        return OD_ERR_IO;
    od_renderer_cursor_update(r);
    SDL_RenderPresent(r->renderer);
    return OD_OK;
}

od_status od_renderer_present_overlay(od_renderer *r, const uint32_t *argb,
                                        const uint32_t *overlay)
{
    if (r == NULL || r->renderer == NULL || r->texture == NULL || argb == NULL)
        return OD_ERR_RANGE;
    if (SDL_UpdateTexture(r->texture, NULL, argb,
                          OD_SCREEN_W * (int)sizeof(uint32_t)) != 0)
        return OD_ERR_IO;

    if (overlay != NULL) {
        if (r->overlay == NULL) {
            r->overlay = SDL_CreateTexture(r->renderer, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           OD_SCREEN_W, OD_SCREEN_H);
            if (r->overlay == NULL)
                return OD_ERR_IO;
            SDL_SetTextureBlendMode(r->overlay, SDL_BLENDMODE_BLEND);
        }
        if (SDL_UpdateTexture(r->overlay, NULL, overlay,
                              OD_SCREEN_W * (int)sizeof(uint32_t)) != 0)
            return OD_ERR_IO;
    }

    od_video_rect vp;
    od_renderer_get_viewport(r, &vp);
    SDL_Rect dst = { vp.x, vp.y, vp.w, vp.h };
    SDL_SetRenderDrawColor(r->renderer, 0, 0, 0, 255);
    SDL_RenderClear(r->renderer);
    if (SDL_RenderCopy(r->renderer, r->texture, NULL, &dst) != 0)
        return OD_ERR_IO;
    if (overlay != NULL && SDL_RenderCopy(r->renderer, r->overlay, NULL, &dst) != 0)
        return OD_ERR_IO;
    od_renderer_cursor_update(r);
    SDL_RenderPresent(r->renderer);
    return OD_OK;
}

od_status od_renderer_read(od_renderer *r, uint32_t *out)
{
    if (r == NULL || r->renderer == NULL || out == NULL)
        return OD_ERR_RANGE;
    if (SDL_RenderReadPixels(r->renderer, NULL, SDL_PIXELFORMAT_ARGB8888, out,
                             OD_SCREEN_W * (int)sizeof(uint32_t)) != 0) {
        fprintf(stderr, "SDL_RenderReadPixels: %s\n", SDL_GetError());
        return OD_ERR_IO;
    }
    return OD_OK;
}

od_status od_renderer_read_window(od_renderer *r, uint32_t **out,
                                    int *w, int *h)
{
    if (r == NULL || r->renderer == NULL || r->window == NULL || out == NULL)
        return OD_ERR_RANGE;
    int ww = 0, wh = 0;
    od_renderer_output_size(r, &ww, &wh);
    if (ww < 1 || wh < 1)
        return OD_ERR_RANGE;
    uint32_t *buf = (uint32_t *)malloc((size_t)ww * (size_t)wh * sizeof(uint32_t));
    if (buf == NULL)
        return OD_ERR_NOMEM;
    if (SDL_RenderReadPixels(r->renderer, NULL, SDL_PIXELFORMAT_ARGB8888, buf,
                             ww * (int)sizeof(uint32_t)) != 0) {
        fprintf(stderr, "SDL_RenderReadPixels: %s\n", SDL_GetError());
        free(buf);
        return OD_ERR_IO;
    }
    if (w) *w = ww;
    if (h) *h = wh;
    *out = buf;
    return OD_OK;
}
