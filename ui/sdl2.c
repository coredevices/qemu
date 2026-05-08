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
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"
#include "ui/sdl2-decoration.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "system/system.h"
#include "qemu/log.h"
#include "qemu-main.h"

static int sdl2_num_outputs;
static struct sdl2_console *sdl2_console;

static SDL_Surface *guest_sprite_surface;
static bool alt_grab;
static bool ctrl_grab;

static int gui_fullscreen;
static int gui_grab_code = KMOD_LALT | KMOD_LCTRL;
static SDL_Cursor *sdl_cursor_normal;
static SDL_Cursor *sdl_cursor_hidden;
static int absolute_enabled;
static bool guest_cursor;
static int guest_x, guest_y;
static SDL_Cursor *guest_sprite;
static Notifier mouse_mode_notifier;

#define SDL2_REFRESH_INTERVAL_BUSY 10
#define SDL2_MAX_IDLE_COUNT (2 * GUI_REFRESH_INTERVAL_DEFAULT \
                             / SDL2_REFRESH_INTERVAL_BUSY + 1)

/* introduced in SDL 2.0.10 */
#ifndef SDL_HINT_RENDER_BATCHING
#define SDL_HINT_RENDER_BATCHING "SDL_RENDER_BATCHING"
#endif

static void sdl_update_caption(struct sdl2_console *scon);

static struct sdl2_console *get_scon_from_window(uint32_t window_id)
{
    int i;
    for (i = 0; i < sdl2_num_outputs; i++) {
        if (sdl2_console[i].real_window == SDL_GetWindowFromID(window_id)) {
            return &sdl2_console[i];
        }
    }
    return NULL;
}

static SDL_HitTestResult sdl_decoration_hit_test(SDL_Window *win,
                                                 const SDL_Point *pt,
                                                 void *data)
{
    struct sdl2_console *scon = data;
    if (!scon->decoration) {
        return SDL_HITTEST_NORMAL;
    }
    SDL_Rect dst = scon->decoration->dst_rect;
    if (pt->x >= dst.x && pt->x < dst.x + dst.w &&
        pt->y >= dst.y && pt->y < dst.y + dst.h) {
        return SDL_HITTEST_NORMAL;
    }
    if (sdl2_decoration_button_at(scon, pt->x, pt->y) >= 0) {
        return SDL_HITTEST_NORMAL;
    }
    if (sdl2_decoration_is_close(scon, pt->x, pt->y)) {
        return SDL_HITTEST_NORMAL;
    }
    return SDL_HITTEST_DRAGGABLE;
}

#include <SDL_shape.h>
#include <SDL_syswm.h>

/*
 * Defined in SDL3 / sdl2-compat (>= ~2.30); fall back to the SDL3 numeric
 * value when the header doesn't expose it. Ignored by libsdl.org SDL2 —
 * macOS gets per-pixel transparency via the dedicated Cocoa overlay path
 * in ui/sdl2-cocoa.m instead.
 */
#ifndef SDL_WINDOW_TRANSPARENT
# define SDL_WINDOW_TRANSPARENT 0x40000000
#endif

#ifndef __APPLE__
/*
 * Build a binary alpha mask (every pixel either fully opaque or fully
 * transparent) sized to the given window dimensions. Anti-aliased edge
 * pixels in the source are pushed to opaque or transparent depending on a
 * threshold so the resulting shape mask doesn't have a halo of partial-
 * alpha pixels around the watch silhouette.
 *
 * Linux/X11 only — macOS uses the Cocoa overlay path instead.
 */
static SDL_Surface *build_shape_mask(SDL_Surface *src, int w, int h,
                                     uint8_t alpha_threshold)
{
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                      SDL_PIXELFORMAT_ARGB8888);
    if (!dst) {
        return NULL;
    }
    SDL_Rect dst_rect = { 0, 0, w, h };
    if (SDL_BlitScaled(src, NULL, dst, &dst_rect) != 0) {
        SDL_FreeSurface(dst);
        return NULL;
    }

    if (SDL_LockSurface(dst) == 0) {
        const uint32_t opaque = SDL_MapRGBA(dst->format, 0, 0, 0, 255);
        const uint32_t clear  = SDL_MapRGBA(dst->format, 0, 0, 0, 0);
        for (int y = 0; y < dst->h; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)dst->pixels + y * dst->pitch);
            for (int x = 0; x < dst->w; x++) {
                uint8_t a;
                SDL_GetRGBA(row[x], dst->format,
                            &(uint8_t){0}, &(uint8_t){0}, &(uint8_t){0}, &a);
                row[x] = (a >= alpha_threshold) ? opaque : clear;
            }
        }
        SDL_UnlockSurface(dst);
    }
    return dst;
}
#endif /* !__APPLE__ */

void sdl2_window_create(struct sdl2_console *scon)
{
    int flags = 0;
    int win_w, win_h;
    bool decorated = scon->decoration != NULL;

    if (!scon->surface) {
        return;
    }
    assert(!scon->real_window);

    if (gui_fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    } else if (decorated) {
        flags |= SDL_WINDOW_BORDERLESS;
    } else {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (scon->hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
#ifdef CONFIG_OPENGL
    if (scon->opengl) {
        flags |= SDL_WINDOW_OPENGL;
    }
#endif

    if (decorated) {
        sdl2_decoration_compute_layout(scon,
                                       surface_width(scon->surface),
                                       surface_height(scon->surface));
        win_w = scon->decoration->win_w;
        win_h = scon->decoration->win_h;
    } else {
        win_w = surface_width(scon->surface);
        win_h = surface_height(scon->surface);
    }

    scon->real_window = NULL;
#ifndef __APPLE__
    if (decorated) {
        /* Linux/X11: SDL_CreateShapedWindow + a binary alpha mask clip
         * the watch silhouette at the WM level. Best-effort: if the host
         * WM doesn't support shape masks (Wayland, some compositors)
         * SDL_CreateShapedWindow returns NULL and we fall through to a
         * plain borderless window below. */
        scon->real_window = SDL_CreateShapedWindow(
            "", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            win_w, win_h, flags | SDL_WINDOW_TRANSPARENT);
        if (scon->real_window && scon->decoration_pending_surface) {
            /* The decoration surface was already binarized at load time
             * (sdl2-decoration.c). Any threshold > 0 will pull through the
             * same pixels here. Use 1 to be tolerant of scaling rounding. */
            SDL_Surface *mask = build_shape_mask(
                scon->decoration_pending_surface, win_w, win_h, 1);
            if (mask) {
                /* The PNG has alpha=0 over the watch screen area so that the
                 * guest framebuffer can be composited there. The shape mask
                 * must include the screen area or the shaped window will
                 * clip the framebuffer away. */
                SDL_Rect dst = scon->decoration->dst_rect;
                uint32_t opaque = SDL_MapRGBA(mask->format, 0, 0, 0, 255);
                /* Close button hot-zone: ensure the shape mask is opaque
                 * over the icon so the click reaches our handler. */
                const SDL_Rect *cr_src =
                    &scon->decoration->preset->close_rect;
                if (cr_src->w > 0 && cr_src->h > 0) {
                    SDL_Rect cr = {
                        .x = (int)(cr_src->x * scon->decoration->scale_x + 0.5),
                        .y = (int)(cr_src->y * scon->decoration->scale_y + 0.5),
                        .w = (int)(cr_src->w * scon->decoration->scale_x + 0.5),
                        .h = (int)(cr_src->h * scon->decoration->scale_y + 0.5),
                    };
                    SDL_FillRect(mask, &cr, opaque);
                }
                if (scon->decoration->preset->screen_round &&
                    SDL_LockSurface(mask) == 0) {
                    /* Fill the inscribed circle so the framebuffer's
                     * rectangular corners stay clipped by the shape. */
                    int cx = dst.x + dst.w / 2;
                    int cy = dst.y + dst.h / 2;
                    int r  = (dst.w < dst.h ? dst.w : dst.h) / 2;
                    int r2 = r * r;
                    for (int y = dst.y; y < dst.y + dst.h; y++) {
                        if (y < 0 || y >= mask->h) continue;
                        uint32_t *row = (uint32_t *)((uint8_t *)mask->pixels +
                                                     y * mask->pitch);
                        int dy = y - cy;
                        for (int x = dst.x; x < dst.x + dst.w; x++) {
                            if (x < 0 || x >= mask->w) continue;
                            int dx = x - cx;
                            if (dx * dx + dy * dy <= r2) {
                                row[x] = opaque;
                            }
                        }
                    }
                    SDL_UnlockSurface(mask);
                } else {
                    SDL_FillRect(mask, &dst, opaque);
                }
                SDL_WindowShapeMode mode = {
                    .mode = ShapeModeBinarizeAlpha,
                    .parameters.binarizationCutoff = 1,
                };
                SDL_SetWindowShape(scon->real_window, mask, &mode);
                SDL_FreeSurface(mask);
            }
        }
    }
#endif /* !__APPLE__ */
    if (!scon->real_window) {
        Uint32 fb_flags = decorated ? (flags | SDL_WINDOW_TRANSPARENT) : flags;
        scon->real_window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED,
                                             SDL_WINDOWPOS_UNDEFINED,
                                             win_w, win_h, fb_flags);
    }
    if (scon->opengl) {
        const char *driver = "opengl";

        if (scon->opts->gl == DISPLAY_GL_MODE_ES) {
            driver = "opengles2";
        }

        SDL_SetHint(SDL_HINT_RENDER_DRIVER, driver);
        SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");

        scon->winctx = SDL_GL_CreateContext(scon->real_window);
        SDL_GL_SetSwapInterval(0);
    } else {
        bool skip_renderer = false;
#ifdef __APPLE__
        /* macOS decorated path drives the contentView's CALayer directly
         * (see ui/sdl2-cocoa.m); creating an SDL renderer would attach a
         * CAMetalLayer that obscures our layer's alpha. */
        skip_renderer = decorated;
#endif
        if (!skip_renderer) {
            /* The SDL renderer is only used by sdl2-2D, when OpenGL is
             * disabled. */
            scon->real_renderer =
                SDL_CreateRenderer(scon->real_window, -1, 0);
        }
    }
    if (decorated) {
        SDL_SetWindowHitTest(scon->real_window, sdl_decoration_hit_test, scon);
#ifdef __APPLE__
        if (!sdl2_cocoa_install(scon->real_window)) {
            error_report("decoration: Cocoa overlay install failed");
        }
#endif
    }
    sdl_update_caption(scon);
}

void sdl2_window_destroy(struct sdl2_console *scon)
{
    if (!scon->real_window) {
        return;
    }

#ifdef __APPLE__
    if (scon->decoration) {
        /* Restore the original SDL contentView before SDL_DestroyWindow
         * tears it down, otherwise we'd leak our overlay view. */
        sdl2_cocoa_uninstall(scon->real_window);
    }
#endif
    if (scon->winctx) {
        SDL_GL_DeleteContext(scon->winctx);
        scon->winctx = NULL;
    }
    if (scon->real_renderer) {
        SDL_DestroyRenderer(scon->real_renderer);
        scon->real_renderer = NULL;
    }
    SDL_DestroyWindow(scon->real_window);
    scon->real_window = NULL;
}

void sdl2_window_resize(struct sdl2_console *scon)
{
    if (!scon->real_window) {
        return;
    }

    if (scon->decoration) {
        /* Window dimensions follow the framebuffer; the PNG re-stretches. */
        sdl2_decoration_compute_layout(scon,
                                       surface_width(scon->surface),
                                       surface_height(scon->surface));
        SDL_SetWindowSize(scon->real_window,
                          scon->decoration->win_w,
                          scon->decoration->win_h);
        return;
    }

    SDL_SetWindowSize(scon->real_window,
                      surface_width(scon->surface),
                      surface_height(scon->surface));
}

static void sdl2_redraw(struct sdl2_console *scon)
{
    if (scon->opengl) {
#ifdef CONFIG_OPENGL
        sdl2_gl_redraw(scon);
#endif
    } else {
        sdl2_2d_redraw(scon);
    }
}

static void sdl_update_caption(struct sdl2_console *scon)
{
    char win_title[1024];
    char icon_title[1024];
    const char *status = "";

    if (!runstate_is_running()) {
        status = " [Stopped]";
    }

    if (qemu_name) {
        snprintf(win_title, sizeof(win_title), "QEMU (%s-%d)%s", qemu_name,
                 scon->idx, status);
        snprintf(icon_title, sizeof(icon_title), "QEMU (%s)", qemu_name);
    } else {
        snprintf(win_title, sizeof(win_title), "QEMU%s", status);
        snprintf(icon_title, sizeof(icon_title), "QEMU");
    }

    if (scon->real_window) {
        SDL_SetWindowTitle(scon->real_window, win_title);
    }
}

static void sdl_show_cursor(struct sdl2_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
    }

    if (guest_cursor &&
        (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    } else {
        SDL_SetCursor(sdl_cursor_normal);
    }

    SDL_ShowCursor(SDL_ENABLE);
}

static void absolute_mouse_grab(struct sdl2_console *scon)
{
    int mouse_x, mouse_y;
    int scr_w, scr_h;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
}

static void sdl_mouse_mode_change(Notifier *notify, void *data)
{
    if (qemu_input_is_absolute(sdl2_console[0].dcl.con)) {
        if (!absolute_enabled) {
            absolute_enabled = 1;
            SDL_SetRelativeMouseMode(SDL_FALSE);
            absolute_mouse_grab(&sdl2_console[0]);
        }
    } else if (absolute_enabled) {
        absolute_enabled = 0;
    }
}

static void sdl_send_mouse_event(struct sdl2_console *scon, int dx, int dy,
                                 int x, int y, int state)
{
    static uint32_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]       = SDL_BUTTON(SDL_BUTTON_LEFT),
        [INPUT_BUTTON_MIDDLE]     = SDL_BUTTON(SDL_BUTTON_MIDDLE),
        [INPUT_BUTTON_RIGHT]      = SDL_BUTTON(SDL_BUTTON_RIGHT),
        [INPUT_BUTTON_SIDE]       = SDL_BUTTON(SDL_BUTTON_X1),
        [INPUT_BUTTON_EXTRA]      = SDL_BUTTON(SDL_BUTTON_X2)
    };
    static uint32_t prev_state;

    if (prev_state != state) {
        qemu_input_update_buttons(scon->dcl.con, bmap, prev_state, state);
        prev_state = state;
    }

    if (qemu_input_is_absolute(scon->dcl.con)) {
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_X,
                             x, 0, surface_width(scon->surface));
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_Y,
                             y, 0, surface_height(scon->surface));
    } else {
        if (guest_cursor) {
            x -= guest_x;
            y -= guest_y;
            guest_x += x;
            guest_y += y;
            dx = x;
            dy = y;
        }
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_Y, dy);
    }
    qemu_input_event_sync();
}

static void toggle_full_screen(struct sdl2_console *scon)
{
    gui_fullscreen = !gui_fullscreen;
    if (gui_fullscreen) {
        SDL_SetWindowFullscreen(scon->real_window,
                                SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(scon->real_window, 0);
    }
    sdl2_redraw(scon);
}

static int get_mod_state(void)
{
    SDL_Keymod mod = SDL_GetModState();

    if (alt_grab) {
        return (mod & (gui_grab_code | KMOD_LSHIFT)) ==
            (gui_grab_code | KMOD_LSHIFT);
    } else if (ctrl_grab) {
        return (mod & KMOD_RCTRL) == KMOD_RCTRL;
    } else {
        return (mod & gui_grab_code) == gui_grab_code;
    }
}

static void handle_keydown(SDL_Event *ev)
{
    int win;
    struct sdl2_console *scon = get_scon_from_window(ev->key.windowID);
    int gui_key_modifier_pressed = get_mod_state();

    if (!scon) {
        return;
    }

    scon->gui_keysym = false;

    if (!scon->ignore_hotkeys && gui_key_modifier_pressed && !ev->key.repeat) {
        switch (ev->key.keysym.scancode) {
        case SDL_SCANCODE_2:
        case SDL_SCANCODE_3:
        case SDL_SCANCODE_4:
        case SDL_SCANCODE_5:
        case SDL_SCANCODE_6:
        case SDL_SCANCODE_7:
        case SDL_SCANCODE_8:
        case SDL_SCANCODE_9:

            win = ev->key.keysym.scancode - SDL_SCANCODE_1;
            if (win < sdl2_num_outputs) {
                sdl2_console[win].hidden = !sdl2_console[win].hidden;
                if (sdl2_console[win].real_window) {
                    if (sdl2_console[win].hidden) {
                        SDL_HideWindow(sdl2_console[win].real_window);
                    } else {
                        SDL_ShowWindow(sdl2_console[win].real_window);
                    }
                }
                sdl2_release_modifiers(scon);
                scon->gui_keysym = true;
            }
            break;
        case SDL_SCANCODE_F:
            toggle_full_screen(scon);
            scon->gui_keysym = true;
            break;
        case SDL_SCANCODE_G:
            scon->gui_keysym = true;
            break;
        case SDL_SCANCODE_U:
            sdl2_window_resize(scon);
            if (!scon->opengl) {
                /* re-create scon->texture */
                sdl2_2d_switch(&scon->dcl, scon->surface);
            }
            scon->gui_keysym = true;
            break;
#if 0
        case SDL_SCANCODE_KP_PLUS:
        case SDL_SCANCODE_KP_MINUS:
            if (!gui_fullscreen) {
                int scr_w, scr_h;
                int width, height;
                SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);

                width = MAX(scr_w + (ev->key.keysym.scancode ==
                                     SDL_SCANCODE_KP_PLUS ? 50 : -50),
                            160);
                height = (surface_height(scon->surface) * width) /
                    surface_width(scon->surface);
                fprintf(stderr, "%s: scale to %dx%d\n",
                        __func__, width, height);
                sdl_scale(scon, width, height);
                sdl2_redraw(scon);
                scon->gui_keysym = true;
            }
#endif
        default:
            break;
        }
    }
    if (!scon->gui_keysym) {
        sdl2_process_key(scon, &ev->key);
    }
}

static void handle_keyup(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->key.windowID);

    if (!scon) {
        return;
    }

    scon->ignore_hotkeys = false;
    sdl2_process_key(scon, &ev->key);
}

static void handle_textinput(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->text.windowID);
    QemuConsole *con = scon ? scon->dcl.con : NULL;

    if (!con) {
        return;
    }

    if (!scon->gui_keysym && QEMU_IS_TEXT_CONSOLE(con)) {
        qemu_text_console_put_string(QEMU_TEXT_CONSOLE(con), ev->text.text, strlen(ev->text.text));
    }
}

static void handle_mousemotion(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->motion.windowID);
    int scr_w, scr_h, surf_w, surf_h, x, y, dx, dy;

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    surf_w = surface_width(scon->surface);
    surf_h = surface_height(scon->surface);

    if (scon->decoration) {
        if (!sdl2_decoration_map_mouse(scon, ev->motion.x, ev->motion.y,
                                       ev->motion.xrel, ev->motion.yrel,
                                       surf_w, surf_h, &x, &y, &dx, &dy)) {
            return;
        }
    } else {
        SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
        x = (int64_t)ev->motion.x * surf_w / scr_w;
        y = (int64_t)ev->motion.y * surf_h / scr_h;
        dx = (int64_t)ev->motion.xrel * surf_w / scr_w;
        dy = (int64_t)ev->motion.yrel * surf_h / scr_h;
    }
    if (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        sdl_send_mouse_event(scon, dx, dy, x, y, ev->motion.state);
    }
}

static void handle_mousebutton(SDL_Event *ev)
{
    int buttonstate = SDL_GetMouseState(NULL, NULL);
    SDL_MouseButtonEvent *bev;
    struct sdl2_console *scon = get_scon_from_window(ev->button.windowID);
    int scr_w, scr_h, x, y, dx, dy;
    int surf_w, surf_h;

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    bev = &ev->button;
    surf_w = surface_width(scon->surface);
    surf_h = surface_height(scon->surface);

    if (scon->decoration && bev->button == SDL_BUTTON_LEFT) {
        /* Host close button: trigger the same shutdown path as the
         * window manager's close (X). */
        if (ev->type == SDL_MOUSEBUTTONDOWN &&
            sdl2_decoration_is_close(scon, bev->x, bev->y)) {
            if (qemu_console_is_graphic(scon->dcl.con) &&
                (!scon->opts->has_window_close || scon->opts->window_close)) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
            return;
        }
        /* Watch buttons drawn on the bezel: synthesize a Pebble keypress. */
        if (ev->type == SDL_MOUSEBUTTONDOWN) {
            int btn = sdl2_decoration_button_at(scon, bev->x, bev->y);
            if (btn >= 0) {
                scon->decoration->active_button = btn;
                sdl2_decoration_send_button(scon, btn, true);
                return;
            }
        } else if (ev->type == SDL_MOUSEBUTTONUP &&
                   scon->decoration->active_button >= 0) {
            sdl2_decoration_send_button(scon,
                                        scon->decoration->active_button,
                                        false);
            scon->decoration->active_button = -1;
            return;
        }
    }

    if (scon->decoration) {
        if (!sdl2_decoration_map_mouse(scon, bev->x, bev->y, 0, 0,
                                       surf_w, surf_h, &x, &y, &dx, &dy)) {
            return;
        }
    } else {
        SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
        x = (int64_t)bev->x * surf_w / scr_w;
        y = (int64_t)bev->y * surf_h / scr_h;
    }

    if (qemu_input_is_absolute(scon->dcl.con)) {
        if (ev->type == SDL_MOUSEBUTTONDOWN) {
            buttonstate |= SDL_BUTTON(bev->button);
        } else {
            buttonstate &= ~SDL_BUTTON(bev->button);
        }
        sdl_send_mouse_event(scon, 0, 0, x, y, buttonstate);
    }
}

static void handle_mousewheel(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->wheel.windowID);
    SDL_MouseWheelEvent *wev = &ev->wheel;
    InputButton btn;

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (wev->y > 0) {
        btn = INPUT_BUTTON_WHEEL_UP;
    } else if (wev->y < 0) {
        btn = INPUT_BUTTON_WHEEL_DOWN;
    } else if (wev->x < 0) {
        btn = INPUT_BUTTON_WHEEL_RIGHT;
    } else if (wev->x > 0) {
        btn = INPUT_BUTTON_WHEEL_LEFT;
    } else {
        return;
    }

    qemu_input_queue_btn(scon->dcl.con, btn, true);
    qemu_input_event_sync();
    qemu_input_queue_btn(scon->dcl.con, btn, false);
    qemu_input_event_sync();
}

static void handle_windowevent(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->window.windowID);
    bool allow_close = true;

    if (!scon) {
        return;
    }

    switch (ev->window.event) {
    case SDL_WINDOWEVENT_RESIZED:
        {
            QemuUIInfo info;
            memset(&info, 0, sizeof(info));
            info.width = ev->window.data1;
            info.height = ev->window.data2;
            dpy_set_ui_info(scon->dcl.con, &info, true);
        }
        sdl2_redraw(scon);
        break;
    case SDL_WINDOWEVENT_EXPOSED:
        sdl2_redraw(scon);
        break;
    case SDL_WINDOWEVENT_FOCUS_GAINED:
        /* fall through */
    case SDL_WINDOWEVENT_ENTER:
        if ((qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
            absolute_mouse_grab(scon);
        }
        /* If a new console window opened using a hotkey receives the
         * focus, SDL sends another KEYDOWN event to the new window,
         * closing the console window immediately after.
         *
         * Work around this by ignoring further hotkey events until a
         * key is released.
         */
        scon->ignore_hotkeys = get_mod_state();
        break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
        break;
    case SDL_WINDOWEVENT_RESTORED:
        update_displaychangelistener(&scon->dcl, GUI_REFRESH_INTERVAL_DEFAULT);
        break;
    case SDL_WINDOWEVENT_MINIMIZED:
        update_displaychangelistener(&scon->dcl, 500);
        break;
    case SDL_WINDOWEVENT_CLOSE:
        if (qemu_console_is_graphic(scon->dcl.con)) {
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
        } else {
            SDL_HideWindow(scon->real_window);
            scon->hidden = true;
        }
        break;
    case SDL_WINDOWEVENT_SHOWN:
        scon->hidden = false;
        break;
    case SDL_WINDOWEVENT_HIDDEN:
        scon->hidden = true;
        break;
    }
}

void sdl2_poll_events(struct sdl2_console *scon)
{
    SDL_Event ev1, *ev = &ev1;
    bool allow_close = true;
    int idle = 1;

    if (scon->last_vm_running != runstate_is_running()) {
        scon->last_vm_running = runstate_is_running();
        sdl_update_caption(scon);
    }

    while (SDL_PollEvent(ev)) {
        switch (ev->type) {
        case SDL_KEYDOWN:
            idle = 0;
            handle_keydown(ev);
            break;
        case SDL_KEYUP:
            idle = 0;
            handle_keyup(ev);
            break;
        case SDL_TEXTINPUT:
            idle = 0;
            handle_textinput(ev);
            break;
        case SDL_QUIT:
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
            break;
        case SDL_MOUSEMOTION:
            idle = 0;
            handle_mousemotion(ev);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            idle = 0;
            handle_mousebutton(ev);
            break;
        case SDL_MOUSEWHEEL:
            idle = 0;
            handle_mousewheel(ev);
            break;
        case SDL_WINDOWEVENT:
            handle_windowevent(ev);
            break;
        default:
            break;
        }
    }

    if (idle) {
        if (scon->idle_counter < SDL2_MAX_IDLE_COUNT) {
            scon->idle_counter++;
            if (scon->idle_counter >= SDL2_MAX_IDLE_COUNT) {
                scon->dcl.update_interval = GUI_REFRESH_INTERVAL_DEFAULT;
            }
        }
    } else {
        scon->idle_counter = 0;
        scon->dcl.update_interval = SDL2_REFRESH_INTERVAL_BUSY;
    }
}

static void sdl_mouse_warp(DisplayChangeListener *dcl,
                           int x, int y, bool on)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    if (!qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (on) {
        if (!guest_cursor) {
            sdl_show_cursor(scon);
        }
        if (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
            SDL_SetCursor(guest_sprite);
            if (!qemu_input_is_absolute(scon->dcl.con) && !absolute_enabled) {
                SDL_WarpMouseInWindow(scon->real_window, x, y);
            }
        }
    }
    guest_cursor = on;
    guest_x = x, guest_y = y;
}

static void sdl_mouse_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{

    if (guest_sprite) {
        SDL_FreeCursor(guest_sprite);
    }

    if (guest_sprite_surface) {
        SDL_FreeSurface(guest_sprite_surface);
    }

    guest_sprite_surface =
        SDL_CreateRGBSurfaceFrom(c->data, c->width, c->height, 32, c->width * 4,
                                 0xff0000, 0x00ff00, 0xff, 0xff000000);

    if (!guest_sprite_surface) {
        fprintf(stderr, "Failed to make rgb surface from %p\n", c);
        return;
    }
    guest_sprite = SDL_CreateColorCursor(guest_sprite_surface,
                                         c->hot_x, c->hot_y);
    if (!guest_sprite) {
        fprintf(stderr, "Failed to make color cursor from %p\n", c);
        return;
    }
    if (guest_cursor &&
        (qemu_input_is_absolute(dcl->con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    }
}

static void sdl_cleanup(void)
{
    if (guest_sprite) {
        SDL_FreeCursor(guest_sprite);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static const DisplayChangeListenerOps dcl_2d_ops = {
    .dpy_name             = "sdl2-2d",
    .dpy_gfx_update       = sdl2_2d_update,
    .dpy_gfx_switch       = sdl2_2d_switch,
    .dpy_gfx_check_format = sdl2_2d_check_format,
    .dpy_refresh          = sdl2_2d_refresh,
    .dpy_mouse_set        = sdl_mouse_warp,
    .dpy_cursor_define    = sdl_mouse_define,
};

#ifdef CONFIG_OPENGL
static const DisplayChangeListenerOps dcl_gl_ops = {
    .dpy_name                = "sdl2-gl",
    .dpy_gfx_update          = sdl2_gl_update,
    .dpy_gfx_switch          = sdl2_gl_switch,
    .dpy_gfx_check_format    = console_gl_check_format,
    .dpy_refresh             = sdl2_gl_refresh,
    .dpy_mouse_set           = sdl_mouse_warp,
    .dpy_cursor_define       = sdl_mouse_define,

    .dpy_gl_scanout_disable  = sdl2_gl_scanout_disable,
    .dpy_gl_scanout_texture  = sdl2_gl_scanout_texture,
    .dpy_gl_update           = sdl2_gl_scanout_flush,
};

static bool
sdl2_gl_is_compatible_dcl(DisplayGLCtx *dgc,
                          DisplayChangeListener *dcl)
{
    return dcl->ops == &dcl_gl_ops;
}

static const DisplayGLCtxOps gl_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = sdl2_gl_is_compatible_dcl,
    .dpy_gl_ctx_create       = sdl2_gl_create_context,
    .dpy_gl_ctx_destroy      = sdl2_gl_destroy_context,
    .dpy_gl_ctx_make_current = sdl2_gl_make_context_current,
};
#endif

static void sdl2_display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_SDL);
    if (o->has_gl && o->gl) {
#ifdef CONFIG_OPENGL
        display_opengl = 1;
#endif
    }
}

static void sdl2_display_init(DisplayState *ds, DisplayOptions *o)
{
    uint8_t data = 0;
    int i;
    SDL_SysWMinfo info;
    SDL_Surface *icon = NULL;
    char *dir;

    assert(o->type == DISPLAY_TYPE_SDL);

    if (SDL_GetHintBoolean("QEMU_ENABLE_SDL_LOGGING", SDL_FALSE)) {
        SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);
    }

    if (SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Could not initialize SDL(%s) - exiting\n",
                SDL_GetError());
        exit(1);
    }
#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR /* only available since SDL 2.0.8 */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
#ifdef SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
#endif
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "1");
    SDL_EnableScreenSaver();
    memset(&info, 0, sizeof(info));
    SDL_VERSION(&info.version);

    gui_fullscreen = o->has_full_screen && o->full_screen;

    if (o->u.sdl.has_grab_mod) {
        if (o->u.sdl.grab_mod == HOT_KEY_MOD_LSHIFT_LCTRL_LALT) {
            alt_grab = true;
        } else if (o->u.sdl.grab_mod == HOT_KEY_MOD_RCTRL) {
            ctrl_grab = true;
        }
    }

    for (i = 0;; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        if (!con) {
            break;
        }
    }
    sdl2_num_outputs = i;
    if (sdl2_num_outputs == 0) {
        return;
    }
    sdl2_console = g_new0(struct sdl2_console, sdl2_num_outputs);
    for (i = 0; i < sdl2_num_outputs; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        assert(con != NULL);
        if (!qemu_console_is_graphic(con) &&
            qemu_console_get_index(con) != 0) {
            sdl2_console[i].hidden = true;
        }
        sdl2_console[i].idx = i;
        sdl2_console[i].opts = o;
        if (i == 0 && o->u.sdl.decoration) {
            if (!sdl2_decoration_init(&sdl2_console[i],
                                       o->u.sdl.decoration)) {
                error_report("decoration: disabling for console 0");
            }
        }
#ifdef CONFIG_OPENGL
        sdl2_console[i].opengl = display_opengl;
        sdl2_console[i].dcl.ops = display_opengl ? &dcl_gl_ops : &dcl_2d_ops;
        sdl2_console[i].dgc.ops = display_opengl ? &gl_ctx_ops : NULL;
#else
        sdl2_console[i].opengl = 0;
        sdl2_console[i].dcl.ops = &dcl_2d_ops;
#endif
        sdl2_console[i].dcl.con = con;
        sdl2_console[i].kbd = qkbd_state_init(con);
        if (display_opengl) {
            qemu_console_set_display_gl_ctx(con, &sdl2_console[i].dgc);
        }
        register_displaychangelistener(&sdl2_console[i].dcl);

#if defined(SDL_VIDEO_DRIVER_WINDOWS) || defined(SDL_VIDEO_DRIVER_X11)
        if (SDL_GetWindowWMInfo(sdl2_console[i].real_window, &info)) {
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
            qemu_console_set_window_id(con, (uintptr_t)info.info.win.window);
#elif defined(SDL_VIDEO_DRIVER_X11)
            qemu_console_set_window_id(con, info.info.x11.window);
#endif
        }
#endif
    }

#ifdef CONFIG_SDL_IMAGE
    dir = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/128x128/apps/qemu.png");
    icon = IMG_Load(dir);
#else
    /* Load a 32x32x4 image. White pixels are transparent. */
    dir = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/32x32/apps/qemu.bmp");
    icon = SDL_LoadBMP(dir);
    if (icon) {
        uint32_t colorkey = SDL_MapRGB(icon->format, 255, 255, 255);
        SDL_SetColorKey(icon, SDL_TRUE, colorkey);
    }
#endif
    g_free(dir);
    if (icon) {
        SDL_SetWindowIcon(sdl2_console[0].real_window, icon);
    }

    mouse_mode_notifier.notify = sdl_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&mouse_mode_notifier);

    sdl_cursor_hidden = SDL_CreateCursor(&data, &data, 8, 1, 0, 0);
    sdl_cursor_normal = SDL_GetCursor();

    atexit(sdl_cleanup);

    /* SDL's event polling (in dpy_refresh) must happen on the main thread. */
    qemu_main = NULL;
}

static QemuDisplay qemu_display_sdl2 = {
    .type       = DISPLAY_TYPE_SDL,
    .early_init = sdl2_display_early_init,
    .init       = sdl2_display_init,
};

static void register_sdl1(void)
{
    qemu_display_register(&qemu_display_sdl2);
}

type_init(register_sdl1);

#ifdef CONFIG_OPENGL
module_dep("ui-opengl");
#endif
