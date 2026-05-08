/*
 * SDL2 watch-style window decoration for Pebble machines.
 *
 * Loads a PNG frame from qemu_datadir/pebble-decorations/, renders it as
 * a borderless window background, and forwards mouse input from the inner
 * screen rect to the guest. Buttons drawn on the bezel are mapped to
 * Pebble button keypresses via QKeyCode events.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/datadir.h"
#include "qemu/cutils.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"
#include "ui/sdl2-decoration.h"

#include <png.h>

/* ============================================================ */
/* Preset table                                                  */
/* ============================================================ */

/*
 * Hot-zone coordinates and screen rect were measured against the bundled
 * PNG. If you replace the PNG, update the rects to match.
 */
static const Sdl2DecorationPreset s_presets[] = {
    {
        .name        = "pt2-sb",
        .file        = "pebble-decorations/pt2-sb.png",
        .width       = 293,
        .height      = 432,
        .screen_rect = { .x =  46, .y = 102, .w = 200, .h = 228 },
        .close_rect  = { .x = 265, .y =   5, .w =  24, .h =  24 },
        .num_buttons = 4,
        .buttons = {
            /* Bumps measured from the artwork: a single left bumper for
             * back, and three right-side bumpers separated by small
             * notches at y=176..180 and y=248..256. */
            { .rect = { .x =   0, .y = 112, .w =  16, .h =  60 },
              .qcode = Q_KEY_CODE_Q },     /* Back */
            { .rect = { .x = 277, .y = 112, .w =  16, .h =  60 },
              .qcode = Q_KEY_CODE_W },     /* Up */
            { .rect = { .x = 277, .y = 184, .w =  16, .h =  60 },
              .qcode = Q_KEY_CODE_S },     /* Select */
            { .rect = { .x = 277, .y = 260, .w =  16, .h =  60 },
              .qcode = Q_KEY_CODE_X },     /* Down */
        },
    },
    {
        .name        = "pt2-br",
        .file        = "pebble-decorations/pt2-br.png",
        .width       = 293,
        .height      = 432,
        .screen_rect = { .x =  46, .y = 102, .w = 200, .h = 228 },
        .close_rect  = { .x = 265, .y =   5, .w =  24, .h =  24 },
        .num_buttons = 4,
        .buttons = {
            { .rect = { .x =   0, .y = 112, .w =  16, .h =  60 },
              .qcode = Q_KEY_CODE_Q },     /* Back */
            { .rect = { .x = 277, .y = 112, .w =  16, .h =  60 },
              .qcode = Q_KEY_CODE_W },     /* Up */
            { .rect = { .x = 277, .y = 184, .w =  16, .h =  60 },
              .qcode = Q_KEY_CODE_S },     /* Select */
            { .rect = { .x = 277, .y = 260, .w =  16, .h =  60 },
              .qcode = Q_KEY_CODE_X },     /* Down */
        },
    },
    {
        .name        = "pr2-bk20",
        .file        = "pebble-decorations/pr2-bk20.png",
        .width       = 330,
        .height      = 460,
        /* Round screen bounding box (circle of diameter ~262 centered at
         * (~166, ~230)). The PNG has alpha=0 across the full bounding box
         * including the corners outside the round bezel, so the shape mask
         * needs to fill only the inscribed circle to clip the rectangular
         * framebuffer's corners. */
        .screen_rect  = { .x =  35, .y =  99, .w = 262, .h = 262 },
        .screen_round = true,
        .close_rect   = { .x = 302, .y =   5, .w =  24, .h =  24 },
        .num_buttons  = 4,
        /* Hot-zone bounding boxes were measured from the dark button-color
         * pixels in the PNG. The case edge follows a circle of radius ~163
         * centred at (165, 229), so the visible buttons sit at varying
         * x depending on y. Boxes are positioned over the visible button
         * pixels and extend a little further inside the case body for
         * click forgiveness. */
        .buttons = {
            { .rect = { .x =   0, .y = 212, .w =  18, .h =  36 },
              .qcode = Q_KEY_CODE_Q },     /* Back */
            { .rect = { .x = 298, .y = 148, .w =  22, .h =  32 },
              .qcode = Q_KEY_CODE_W },     /* Up */
            { .rect = { .x = 308, .y = 212, .w =  22, .h =  32 },
              .qcode = Q_KEY_CODE_S },     /* Select */
            { .rect = { .x = 290, .y = 276, .w =  26, .h =  34 },
              .qcode = Q_KEY_CODE_X },     /* Down */
        },
    },
    {
        .name        = "pr2-gd14",
        .file        = "pebble-decorations/pr2-gd14.png",
        .width       = 330,
        .height      = 460,
        .screen_rect  = { .x =  35, .y =  99, .w = 262, .h = 262 },
        .screen_round = true,
        .close_rect   = { .x = 302, .y =   5, .w =  24, .h =  24 },
        .num_buttons  = 4,
        .buttons = {
            { .rect = { .x =   0, .y = 212, .w =  18, .h =  36 },
              .qcode = Q_KEY_CODE_Q },     /* Back */
            { .rect = { .x = 298, .y = 148, .w =  22, .h =  32 },
              .qcode = Q_KEY_CODE_W },     /* Up */
            { .rect = { .x = 308, .y = 212, .w =  22, .h =  32 },
              .qcode = Q_KEY_CODE_S },     /* Select */
            { .rect = { .x = 290, .y = 276, .w =  26, .h =  34 },
              .qcode = Q_KEY_CODE_X },     /* Down */
        },
    },
    {
        .name        = "p2d-bk",
        .file        = "pebble-decorations/p2d-bk.png",
        .width       = 220,
        .height      = 403,
        /* Screen rect lines up exactly with flint's 144x168 framebuffer. */
        .screen_rect = { .x =  38, .y = 117, .w = 144, .h = 168 },
        .close_rect  = { .x = 192, .y =   5, .w =  24, .h =  24 },
        .num_buttons = 4,
        .buttons = {
            /* Single left bumper for back; three right bumpers separated
             * by notches at y=168..172 and y=232..236. */
            { .rect = { .x =   0, .y = 116, .w =  10, .h =  48 },
              .qcode = Q_KEY_CODE_Q },     /* Back */
            { .rect = { .x = 210, .y = 116, .w =  10, .h =  48 },
              .qcode = Q_KEY_CODE_W },     /* Up */
            { .rect = { .x = 210, .y = 176, .w =  10, .h =  52 },
              .qcode = Q_KEY_CODE_S },     /* Select */
            { .rect = { .x = 210, .y = 240, .w =  10, .h =  44 },
              .qcode = Q_KEY_CODE_X },     /* Down */
        },
    },
    {
        .name        = "p2d-wh",
        .file        = "pebble-decorations/p2d-wh.png",
        .width       = 221,
        .height      = 403,
        .screen_rect = { .x =  38, .y = 117, .w = 144, .h = 168 },
        .close_rect  = { .x = 193, .y =   5, .w =  24, .h =  24 },
        .num_buttons = 4,
        .buttons = {
            { .rect = { .x =   0, .y = 116, .w =  10, .h =  48 },
              .qcode = Q_KEY_CODE_Q },     /* Back */
            { .rect = { .x = 211, .y = 116, .w =  10, .h =  48 },
              .qcode = Q_KEY_CODE_W },     /* Up */
            { .rect = { .x = 211, .y = 176, .w =  10, .h =  52 },
              .qcode = Q_KEY_CODE_S },     /* Select */
            { .rect = { .x = 211, .y = 240, .w =  10, .h =  44 },
              .qcode = Q_KEY_CODE_X },     /* Down */
        },
    },
};

const Sdl2DecorationPreset *sdl2_decoration_lookup(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(s_presets); i++) {
        if (g_strcmp0(s_presets[i].name, name) == 0) {
            return &s_presets[i];
        }
    }
    return NULL;
}

/*
 * Threshold of 128 keeps anti-aliased edge pixels with alpha >= 128 as the
 * watch silhouette and drops the rest. We snap the texture's alpha to the
 * same binary threshold as the window shape mask: otherwise edge pixels
 * that are "in shape" (mask alpha >= 128) but partially transparent in
 * the texture get blended onto the black clear color, producing a dark
 * halo around the watch.
 */
#define SDL2_DECORATION_ALPHA_THRESHOLD 128

/* ============================================================ */
/* PNG loader                                                    */
/* ============================================================ */

static SDL_Surface *load_png_surface(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        error_report("decoration: cannot open %s: %s", path, strerror(errno));
        return NULL;
    }

    uint8_t header[8];
    if (fread(header, 1, sizeof(header), fp) != sizeof(header) ||
        png_sig_cmp(header, 0, sizeof(header)) != 0) {
        error_report("decoration: %s is not a PNG file", path);
        fclose(fp);
        return NULL;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return NULL;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return NULL;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, sizeof(header));
    png_read_info(png, info);

    /* Normalize to 8-bit RGBA. */
    int color_type = png_get_color_type(png, info);
    int bit_depth  = png_get_bit_depth(png, info);

    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if (bit_depth == 16) {
        png_set_strip_16(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    }
    png_read_update_info(png, info);

    uint32_t width  = png_get_image_width(png, info);
    uint32_t height = png_get_image_height(png, info);
    size_t   stride = png_get_rowbytes(png, info);

    uint8_t *pixels = g_malloc(stride * height);
    png_bytep *rows = g_new(png_bytep, height);
    for (uint32_t y = 0; y < height; y++) {
        rows[y] = pixels + y * stride;
    }
    png_read_image(png, rows);
    g_free(rows);

    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    /* Wrap raw RGBA pixels into an SDL_Surface (takes a copy). */
    SDL_Surface *raw = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, width, height, 32, stride, SDL_PIXELFORMAT_ABGR8888);
    if (!raw) {
        error_report("decoration: SDL surface creation failed: %s",
                     SDL_GetError());
        g_free(pixels);
        return NULL;
    }
    SDL_Surface *copy = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888,
                                                 0);
    SDL_FreeSurface(raw);
    g_free(pixels);
    if (!copy) {
        return NULL;
    }

    /* Snap alpha to binary so the texture has hard edges that match the
     * shape mask exactly: no anti-aliased pixels to be blended onto the
     * clear color and produce a dark halo. */
    if (SDL_LockSurface(copy) == 0) {
        for (int y = 0; y < copy->h; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)copy->pixels +
                                         y * copy->pitch);
            for (int x = 0; x < copy->w; x++) {
                uint8_t r, g, b, a;
                SDL_GetRGBA(row[x], copy->format, &r, &g, &b, &a);
                a = (a >= SDL2_DECORATION_ALPHA_THRESHOLD) ? 0xff : 0x00;
                row[x] = SDL_MapRGBA(copy->format, r, g, b, a);
            }
        }
        SDL_UnlockSurface(copy);
    }
    return copy;
}

/* ============================================================ */
/* File lookup                                                   */
/* ============================================================ */

char *sdl2_decoration_find_file(const Sdl2DecorationPreset *p)
{
    /* qemu_find_file searches all data dirs (-L plus CONFIG_QEMU_DATADIR). */
    return qemu_find_file(QEMU_FILE_TYPE_BIOS, p->file);
}

/* ============================================================ */
/* Lifecycle                                                     */
/* ============================================================ */

bool sdl2_decoration_init(struct sdl2_console *scon, const char *name)
{
    const Sdl2DecorationPreset *preset = sdl2_decoration_lookup(name);
    if (!preset) {
        error_report("decoration: unknown preset '%s'", name);
        return false;
    }

    char *path = sdl2_decoration_find_file(preset);
    if (!path) {
        error_report("decoration: cannot find %s (try -L pc-bios)",
                     preset->file);
        return false;
    }

    SDL_Surface *surface = load_png_surface(path);
    g_free(path);
    if (!surface) {
        return false;
    }
    if (surface->w != preset->width || surface->h != preset->height) {
        warn_report("decoration: %s dimensions %dx%d differ from preset "
                    "%dx%d; rendering may be off", preset->name,
                    surface->w, surface->h, preset->width, preset->height);
    }

    scon->decoration = g_new0(Sdl2Decoration, 1);
    scon->decoration->preset = preset;
    scon->decoration->active_button = -1;
    /* Layout is computed once the guest surface size is known; for now use
     * the PNG dimensions so window creation has something to size against. */
    scon->decoration->win_w = preset->width;
    scon->decoration->win_h = preset->height;
    scon->decoration->scale_x = 1.0;
    scon->decoration->scale_y = 1.0;
    scon->decoration->dst_rect = preset->screen_rect;

    /* Texture is created lazily once the renderer exists. Stash the
     * surface on the console so sdl2_decoration_render can promote it. */
    scon->decoration_pending_surface = surface;
    return true;
}

void sdl2_decoration_fini(struct sdl2_console *scon)
{
    if (!scon->decoration) {
        return;
    }
    if (scon->decoration->texture) {
        SDL_DestroyTexture(scon->decoration->texture);
    }
    if (scon->decoration->compose_buf) {
        SDL_FreeSurface(scon->decoration->compose_buf);
    }
    if (scon->decoration->deco_scaled) {
        SDL_FreeSurface(scon->decoration->deco_scaled);
    }
    g_free(scon->decoration);
    scon->decoration = NULL;
    if (scon->decoration_pending_surface) {
        SDL_FreeSurface(scon->decoration_pending_surface);
        scon->decoration_pending_surface = NULL;
    }
}

/* ============================================================ */
/* Layout                                                        */
/* ============================================================ */

void sdl2_decoration_compute_layout(struct sdl2_console *scon,
                                    int surface_w, int surface_h)
{
    if (!scon->decoration || surface_w <= 0 || surface_h <= 0) {
        return;
    }
    Sdl2Decoration *d = scon->decoration;
    const SDL_Rect *screen = &d->preset->screen_rect;

    /* Per-axis scale chosen so the guest framebuffer fills the inner
     * screen rect at exactly its native pixel size. The PNG (and all
     * configured hot-zones) stretch to match. */
    d->scale_x = (double)surface_w / screen->w;
    d->scale_y = (double)surface_h / screen->h;
    d->win_w = (int)(d->preset->width  * d->scale_x + 0.5);
    d->win_h = (int)(d->preset->height * d->scale_y + 0.5);
    d->dst_rect = (SDL_Rect){
        .x = (int)(screen->x * d->scale_x + 0.5),
        .y = (int)(screen->y * d->scale_y + 0.5),
        .w = surface_w,
        .h = surface_h,
    };

    /* Drop any cached compose/scaled surfaces — they were sized for the
     * old window dimensions. They'll be lazily re-allocated by
     * sdl2_decoration_compose on the next frame. */
    if (d->compose_buf) {
        SDL_FreeSurface(d->compose_buf);
        d->compose_buf = NULL;
    }
    if (d->deco_scaled) {
        SDL_FreeSurface(d->deco_scaled);
        d->deco_scaled = NULL;
    }
}

/* ============================================================ */
/* Rendering                                                     */
/* ============================================================ */

static void render_close_button(struct sdl2_console *scon)
{
    Sdl2Decoration *d = scon->decoration;
    const SDL_Rect *src = &d->preset->close_rect;
    if (src->w == 0 || src->h == 0) {
        return;
    }
    /* close_rect is in PNG-pixel coords; convert to window coords. */
    SDL_Rect cr = {
        .x = (int)(src->x * d->scale_x + 0.5),
        .y = (int)(src->y * d->scale_y + 0.5),
        .w = (int)(src->w * d->scale_x + 0.5),
        .h = (int)(src->h * d->scale_y + 0.5),
    };

    /* Filled red background. */
    SDL_SetRenderDrawColor(scon->real_renderer, 200, 40, 40, 255);
    SDL_RenderFillRect(scon->real_renderer, &cr);

    /* White X with 2px stroke. */
    SDL_SetRenderDrawColor(scon->real_renderer, 255, 255, 255, 255);
    int pad = 6;
    int x0 = cr.x + pad,                y0 = cr.y + pad;
    int x1 = cr.x + cr.w - 1 - pad,     y1 = cr.y + cr.h - 1 - pad;
    for (int dx = 0; dx <= 1; dx++) {
        for (int dy = 0; dy <= 1; dy++) {
            SDL_RenderDrawLine(scon->real_renderer,
                               x0 + dx, y0 + dy, x1 + dx, y1 + dy);
            SDL_RenderDrawLine(scon->real_renderer,
                               x0 + dx, y1 + dy, x1 + dx, y0 + dy);
        }
    }
}

void sdl2_decoration_render(struct sdl2_console *scon)
{
    Sdl2Decoration *d = scon->decoration;
    if (!d || !scon->real_renderer) {
        return;
    }
    if (!d->texture && scon->decoration_pending_surface) {
        d->texture = SDL_CreateTextureFromSurface(scon->real_renderer,
                                                  scon->decoration_pending_surface);
        SDL_FreeSurface(scon->decoration_pending_surface);
        scon->decoration_pending_surface = NULL;
        if (!d->texture) {
            error_report("decoration: SDL_CreateTextureFromSurface failed: %s",
                         SDL_GetError());
            return;
        }
        SDL_SetTextureBlendMode(d->texture, SDL_BLENDMODE_BLEND);
    }
    if (d->texture) {
        SDL_RenderCopy(scon->real_renderer, d->texture, NULL, NULL);
    }
    render_close_button(scon);
}

/* ============================================================ */
/* Software compositor (macOS Cocoa overlay path)                */
/* ============================================================ */

/*
 * Pixman pixel formats we accept on the compose path. These match the
 * formats sdl2_2d_check_format advertises; we mirror them as SDL formats
 * so SDL_BlitScaled can do the format conversion + scaling for us.
 */
static int sdl_pixfmt_for_pixman(pixman_format_code_t fmt)
{
    switch (fmt) {
    case PIXMAN_a8r8g8b8: return SDL_PIXELFORMAT_ARGB8888;
    case PIXMAN_x8r8g8b8: return SDL_PIXELFORMAT_XRGB8888;
    case PIXMAN_a8b8g8r8: return SDL_PIXELFORMAT_ABGR8888;
    case PIXMAN_x8b8g8r8: return SDL_PIXELFORMAT_XBGR8888;
    case PIXMAN_r8g8b8a8: return SDL_PIXELFORMAT_RGBA8888;
    case PIXMAN_r8g8b8x8: return SDL_PIXELFORMAT_RGBX8888;
    case PIXMAN_b8g8r8a8: return SDL_PIXELFORMAT_BGRA8888;
    case PIXMAN_b8g8r8x8: return SDL_PIXELFORMAT_BGRX8888;
    case PIXMAN_x1r5g5b5: return SDL_PIXELFORMAT_ARGB1555;
    case PIXMAN_r5g6b5:   return SDL_PIXELFORMAT_RGB565;
    default:              return 0;
    }
}

/*
 * Software equivalent of render_close_button(). Stamps a 24x24-style
 * filled red rect with a white X centred on it directly into compose_buf
 * (RGBA32 byte order: R,G,B,A).
 */
static void compose_close_button(SDL_Surface *dst, const SDL_Rect *cr)
{
    if (cr->w <= 0 || cr->h <= 0) {
        return;
    }
    /* Background: opaque red. */
    SDL_FillRect(dst, (SDL_Rect *)cr,
                 SDL_MapRGBA(dst->format, 200, 40, 40, 255));

    if (SDL_LockSurface(dst) != 0) {
        return;
    }
    const uint32_t white = SDL_MapRGBA(dst->format, 255, 255, 255, 255);
    int pad = 6;
    int x0 = cr->x + pad,            y0 = cr->y + pad;
    int x1 = cr->x + cr->w - 1 - pad, y1 = cr->y + cr->h - 1 - pad;
    int dx_total = x1 - x0;
    int dy_total = y1 - y0;
    int steps = (abs(dx_total) > abs(dy_total) ? abs(dx_total) : abs(dy_total));
    if (steps <= 0) {
        SDL_UnlockSurface(dst);
        return;
    }
    /* 2-px brush via the four (sx,sy) offset combinations gives the same
     * "thicker stroke" the renderer path produced. */
    for (int s = 0; s <= steps; s++) {
        double t = (double)s / steps;
        int xa = (int)(x0 + t * dx_total + 0.5);
        int ya = (int)(y0 + t * dy_total + 0.5);
        int xb = (int)(x0 + t * dx_total + 0.5);
        int yb = (int)(y1 - t * dy_total + 0.5);
        for (int sx = 0; sx <= 1; sx++) {
            for (int sy = 0; sy <= 1; sy++) {
                int px, py;
                px = xa + sx; py = ya + sy;
                if (px >= 0 && px < dst->w && py >= 0 && py < dst->h) {
                    uint32_t *row = (uint32_t *)((uint8_t *)dst->pixels +
                                                 py * dst->pitch);
                    row[px] = white;
                }
                px = xb + sx; py = yb + sy;
                if (px >= 0 && px < dst->w && py >= 0 && py < dst->h) {
                    uint32_t *row = (uint32_t *)((uint8_t *)dst->pixels +
                                                 py * dst->pitch);
                    row[px] = white;
                }
            }
        }
    }
    SDL_UnlockSurface(dst);
}

SDL_Surface *sdl2_decoration_compose(struct sdl2_console *scon)
{
    Sdl2Decoration *d = scon ? scon->decoration : NULL;
    if (!d || d->win_w <= 0 || d->win_h <= 0 || !scon->surface) {
        return NULL;
    }

    /* Lazy-allocate the compose target and the pre-scaled decoration in
     * RGBA32 (memory order R,G,B,A) — the same byte order Core Graphics
     * expects with kCGImageAlphaLast | kCGBitmapByteOrder32Big. */
    if (!d->compose_buf) {
        d->compose_buf = SDL_CreateRGBSurfaceWithFormat(
            0, d->win_w, d->win_h, 32, SDL_PIXELFORMAT_RGBA32);
        if (!d->compose_buf) {
            return NULL;
        }
        SDL_SetSurfaceBlendMode(d->compose_buf, SDL_BLENDMODE_NONE);
    }
    if (!d->deco_scaled && scon->decoration_pending_surface) {
        d->deco_scaled = SDL_CreateRGBSurfaceWithFormat(
            0, d->win_w, d->win_h, 32, SDL_PIXELFORMAT_RGBA32);
        if (d->deco_scaled) {
            SDL_Rect full = { 0, 0, d->win_w, d->win_h };
            /* Pre-binarized at load time, so a plain BlitScaled preserves
             * the binary alpha across the rescale. */
            SDL_SetSurfaceBlendMode(scon->decoration_pending_surface,
                                    SDL_BLENDMODE_NONE);
            SDL_BlitScaled(scon->decoration_pending_surface, NULL,
                           d->deco_scaled, &full);
            SDL_SetSurfaceBlendMode(d->deco_scaled, SDL_BLENDMODE_BLEND);
        }
    }
    if (!d->deco_scaled) {
        return NULL;
    }

    /* Wrap the guest framebuffer's pixels in a temporary SDL_Surface so we
     * can BlitScaled it (handles per-axis scaling + format conversion in
     * one call). The wrapper does not own the pixels. */
    pixman_format_code_t pf = surface_format(scon->surface);
    int sdl_fmt = sdl_pixfmt_for_pixman(pf);
    if (!sdl_fmt) {
        return NULL;
    }
    SDL_Surface *fb = SDL_CreateRGBSurfaceWithFormatFrom(
        surface_data(scon->surface),
        surface_width(scon->surface), surface_height(scon->surface),
        surface_bits_per_pixel(scon->surface),
        surface_stride(scon->surface), sdl_fmt);
    if (!fb) {
        return NULL;
    }
    /* X-byte formats report alpha as undefined; force opaque so the screen
     * rect ends up with alpha=255 rather than whatever junk was in that
     * byte. */
    SDL_SetSurfaceBlendMode(fb, SDL_BLENDMODE_NONE);

    /* Step 1: clear to fully transparent. */
    SDL_FillRect(d->compose_buf, NULL,
                 SDL_MapRGBA(d->compose_buf->format, 0, 0, 0, 0));

    /* Step 2: blit framebuffer into the screen rect (writes alpha=255). */
    SDL_Rect dst = d->dst_rect;
    SDL_BlitScaled(fb, NULL, d->compose_buf, &dst);
    SDL_FreeSurface(fb);

    /* Step 3: blend the watch silhouette over the framebuffer. Where the
     * decoration is opaque it overwrites; where it's alpha=0 the underlying
     * framebuffer (or transparent background) shows through. */
    SDL_BlitSurface(d->deco_scaled, NULL, d->compose_buf, NULL);

    /* Step 4: stamp the host close button. */
    const SDL_Rect *cr_src = &d->preset->close_rect;
    if (cr_src->w > 0 && cr_src->h > 0) {
        SDL_Rect cr = {
            .x = (int)(cr_src->x * d->scale_x + 0.5),
            .y = (int)(cr_src->y * d->scale_y + 0.5),
            .w = (int)(cr_src->w * d->scale_x + 0.5),
            .h = (int)(cr_src->h * d->scale_y + 0.5),
        };
        compose_close_button(d->compose_buf, &cr);
    }

    return d->compose_buf;
}

/* ============================================================ */
/* Hit-testing / mouse mapping                                   */
/* ============================================================ */

static bool point_in_rect(const SDL_Rect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w &&
           y >= r->y && y < r->y + r->h;
}

int sdl2_decoration_button_at(struct sdl2_console *scon, int x, int y)
{
    if (!scon->decoration) {
        return -1;
    }
    /* Hot-zone rects are stored in PNG-pixel coords; window coords are
     * scaled, so undo the scale before comparing. */
    Sdl2Decoration *d = scon->decoration;
    if (d->scale_x <= 0 || d->scale_y <= 0) {
        return -1;
    }
    int px = (int)(x / d->scale_x);
    int py = (int)(y / d->scale_y);
    const Sdl2DecorationPreset *p = d->preset;
    for (int i = 0; i < p->num_buttons; i++) {
        if (point_in_rect(&p->buttons[i].rect, px, py)) {
            return i;
        }
    }
    return -1;
}

bool sdl2_decoration_is_close(struct sdl2_console *scon, int x, int y)
{
    Sdl2Decoration *d = scon->decoration;
    if (!d) {
        return false;
    }
    const SDL_Rect *cr = &d->preset->close_rect;
    if (cr->w == 0 || cr->h == 0) {
        return false;
    }
    if (d->scale_x <= 0 || d->scale_y <= 0) {
        return false;
    }
    /* (x, y) come in window coords; close_rect is PNG coords. */
    int px = (int)(x / d->scale_x);
    int py = (int)(y / d->scale_y);
    return point_in_rect(cr, px, py);
}

bool sdl2_decoration_map_mouse(struct sdl2_console *scon,
                               int win_x, int win_y, int win_xrel, int win_yrel,
                               int surf_w, int surf_h,
                               int *out_x, int *out_y,
                               int *out_dx, int *out_dy)
{
    if (!scon->decoration) {
        return false;
    }
    SDL_Rect dst = scon->decoration->dst_rect;
    if (dst.w <= 0 || dst.h <= 0 || surf_w <= 0 || surf_h <= 0) {
        return false;
    }
    if (!point_in_rect(&dst, win_x, win_y)) {
        return false;
    }
    *out_x  = (int64_t)(win_x - dst.x) * surf_w / dst.w;
    *out_y  = (int64_t)(win_y - dst.y) * surf_h / dst.h;
    *out_dx = (int64_t)win_xrel * surf_w / dst.w;
    *out_dy = (int64_t)win_yrel * surf_h / dst.h;
    return true;
}

void sdl2_decoration_send_button(struct sdl2_console *scon,
                                 int button_index, bool down)
{
    if (!scon->decoration || button_index < 0) {
        return;
    }
    const Sdl2DecorationPreset *p = scon->decoration->preset;
    if (button_index >= p->num_buttons) {
        return;
    }
    qemu_input_event_send_key_qcode(scon->dcl.con,
                                    p->buttons[button_index].qcode, down);
}
