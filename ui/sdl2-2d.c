/*
 * QEMU SDL display driver
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/* Ported SDL 1.2 code to 2.0 by Dave Airlie. */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"
#include "ui/sdl2-decoration.h"

void sdl2_2d_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    DisplaySurface *surf = scon->surface;
    SDL_Rect rect;
    size_t surface_data_offset;
    assert(!scon->opengl);

#ifdef __APPLE__
    /* macOS decorated path: no SDL renderer/texture exist; sdl2-decoration
     * does the compositing in software and we hand the result to the
     * Cocoa overlay (see ui/sdl2-cocoa.m). The dirty rect is ignored —
     * we always recomposite the full window because the watch silhouette
     * overlaps the framebuffer at its rounded edges. */
    if (scon->decoration && !scon->real_renderer) {
        SDL_Surface *out = sdl2_decoration_compose(scon);
        if (out) {
            sdl2_cocoa_blit(scon->real_window, out->pixels,
                            out->w, out->h, out->pitch);
        }
        return;
    }
#endif

    if (!scon->texture) {
        return;
    }

    surface_data_offset = surface_bytes_per_pixel(surf) * x +
                          surface_stride(surf) * y;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;

    SDL_UpdateTexture(scon->texture, &rect,
                      surface_data(surf) + surface_data_offset,
                      surface_stride(surf));
    if (scon->decoration) {
        /* Clear to fully transparent so areas the PNG doesn't cover have
         * rendered alpha=0. Linux/Wayland's shape mask already clips the
         * alpha-zero regions, so this is just defensive. */
        SDL_SetRenderDrawColor(scon->real_renderer, 0, 0, 0, 0);
    }
    SDL_RenderClear(scon->real_renderer);
    if (scon->decoration) {
        SDL_Rect dst = scon->decoration->dst_rect;
        SDL_RenderCopy(scon->real_renderer, scon->texture, NULL, &dst);
        sdl2_decoration_render(scon);
    } else {
        SDL_RenderCopy(scon->real_renderer, scon->texture, NULL, NULL);
    }
    SDL_RenderPresent(scon->real_renderer);
}

void sdl2_2d_switch(DisplayChangeListener *dcl,
                    DisplaySurface *new_surface)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    DisplaySurface *old_surface = scon->surface;
    int format = 0;

    assert(!scon->opengl);

    scon->surface = new_surface;

    if (scon->texture) {
        SDL_DestroyTexture(scon->texture);
        scon->texture = NULL;
    }

    if (surface_is_placeholder(new_surface) && qemu_console_get_index(dcl->con)) {
        sdl2_window_destroy(scon);
        return;
    }

    if (!scon->real_window) {
        sdl2_window_create(scon);
    } else if (old_surface &&
               ((surface_width(old_surface)  != surface_width(new_surface)) ||
                (surface_height(old_surface) != surface_height(new_surface)))) {
        sdl2_window_resize(scon);
    }

    if (scon->decoration) {
        /* Coordinates are in window pixel space; the decoration PNG is
         * stretched to fit and the guest framebuffer renders 1:1 at
         * decoration->dst_rect. */
        sdl2_decoration_compute_layout(scon,
                                       surface_width(new_surface),
                                       surface_height(new_surface));
    } else {
        SDL_RenderSetLogicalSize(scon->real_renderer,
                                 surface_width(new_surface),
                                 surface_height(new_surface));
    }

    if (!scon->real_renderer) {
        /* Cocoa overlay path (macOS decorated): no SDL renderer/texture;
         * sdl2_decoration_compose pulls pixels straight from the surface. */
        sdl2_2d_redraw(scon);
        return;
    }

    switch (surface_format(scon->surface)) {
    case PIXMAN_x1r5g5b5:
        format = SDL_PIXELFORMAT_ARGB1555;
        break;
    case PIXMAN_r5g6b5:
        format = SDL_PIXELFORMAT_RGB565;
        break;
    case PIXMAN_a8r8g8b8:
        format = SDL_PIXELFORMAT_ARGB8888;
        break;
    case PIXMAN_x8r8g8b8:
        /* The X byte is undefined; QEMU's rgb_to_pixel32 leaves it as 0.
         * Use the X-variant format so SDL doesn't interpret it as alpha
         * and turn black framebuffer pixels transparent under a shaped
         * window. */
        format = SDL_PIXELFORMAT_XRGB8888;
        break;
    case PIXMAN_a8b8g8r8:
        format = SDL_PIXELFORMAT_ABGR8888;
        break;
    case PIXMAN_x8b8g8r8:
        format = SDL_PIXELFORMAT_XBGR8888;
        break;
    case PIXMAN_r8g8b8a8:
        format = SDL_PIXELFORMAT_RGBA8888;
        break;
    case PIXMAN_r8g8b8x8:
        format = SDL_PIXELFORMAT_RGBX8888;
        break;
    case PIXMAN_b8g8r8x8:
        format = SDL_PIXELFORMAT_BGRX8888;
        break;
    case PIXMAN_b8g8r8a8:
        format = SDL_PIXELFORMAT_BGRA8888;
        break;
    default:
        g_assert_not_reached();
    }
    scon->texture = SDL_CreateTexture(scon->real_renderer, format,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      surface_width(new_surface),
                                      surface_height(new_surface));
    sdl2_2d_redraw(scon);
}

void sdl2_2d_refresh(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(!scon->opengl);
    graphic_hw_update(dcl->con);
    sdl2_poll_events(scon);
}

void sdl2_2d_redraw(struct sdl2_console *scon)
{
    assert(!scon->opengl);

    if (!scon->surface) {
        return;
    }
    sdl2_2d_update(&scon->dcl, 0, 0,
                   surface_width(scon->surface),
                   surface_height(scon->surface));
}

bool sdl2_2d_check_format(DisplayChangeListener *dcl,
                          pixman_format_code_t format)
{
    /*
     * We let SDL convert for us a few more formats than,
     * the native ones. These are the ones I have tested.
     */
    return (format == PIXMAN_x8r8g8b8 ||
            format == PIXMAN_a8r8g8b8 ||
            format == PIXMAN_a8b8g8r8 ||
            format == PIXMAN_x8b8g8r8 ||
            format == PIXMAN_b8g8r8x8 ||
            format == PIXMAN_b8g8r8a8 ||
            format == PIXMAN_r8g8b8x8 ||
            format == PIXMAN_r8g8b8a8 ||
            format == PIXMAN_x1r5g5b5 ||
            format == PIXMAN_r5g6b5);
}
