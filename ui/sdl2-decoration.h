/*
 * SDL2 watch-style window decoration for Pebble machines.
 *
 * Wraps the guest framebuffer in a borderless window that renders a watch
 * frame (PNG) around it. The frame area drags the window via the platform
 * window manager; configured button hot-zones synthesize Pebble button
 * keypresses.
 */
#ifndef SDL2_DECORATION_H
#define SDL2_DECORATION_H

#include "qapi/qapi-types-ui.h"
#include <SDL.h>

struct sdl2_console;

/* Maximum number of button hot-zones per decoration preset. */
#define SDL2_DECORATION_MAX_BUTTONS 8

typedef struct Sdl2DecorationButton {
    SDL_Rect rect;          /* in PNG/window coordinates */
    QKeyCode qcode;         /* key code synthesized on click */
} Sdl2DecorationButton;

typedef struct Sdl2DecorationPreset {
    const char *name;       /* CLI value, e.g. "pt2" */
    const char *file;       /* path under qemu_datadir */
    int width, height;      /* expected PNG dimensions */
    SDL_Rect screen_rect;   /* where the guest framebuffer renders */
    bool screen_round;      /* shape mask uses inscribed circle, not rect */
    SDL_Rect close_rect;    /* host close button hot-zone; w==0 disables */
    Sdl2DecorationButton buttons[SDL2_DECORATION_MAX_BUTTONS];
    int num_buttons;
} Sdl2DecorationPreset;

typedef struct Sdl2Decoration {
    const Sdl2DecorationPreset *preset;
    SDL_Texture *texture;   /* decoration overlay (RGBA), SDL-renderer path */
    /* The window is sized so that the screen_rect inside the PNG maps to
     * the guest framebuffer at native pixels. The PNG is stretched per
     * axis to fit the window. */
    int win_w, win_h;       /* scaled window dimensions */
    double scale_x, scale_y; /* PNG -> window scale factors */
    SDL_Rect dst_rect;      /* guest framebuffer rect in window coords */
    int active_button;      /* index in preset->buttons or -1 */
    /*
     * Software-compositing path (used on macOS where SDL2 can't honor
     * per-pixel window alpha; see ui/sdl2-cocoa.m). Allocated lazily on
     * first compose; freed in fini. Both surfaces are RGBA32 (memory order
     * R,G,B,A) so the buffer can be handed straight to CGImageCreate.
     */
    SDL_Surface *compose_buf;   /* per-frame composite, win_w * win_h */
    SDL_Surface *deco_scaled;   /* decoration PNG pre-scaled to window size */
} Sdl2Decoration;

/* Look up a decoration preset by name. Returns NULL if unknown. */
const Sdl2DecorationPreset *sdl2_decoration_lookup(const char *name);

/* Locate the decoration PNG on disk. Caller frees the returned string. */
char *sdl2_decoration_find_file(const Sdl2DecorationPreset *p);

/* Initialise the decoration on a console. Returns true on success. */
bool sdl2_decoration_init(struct sdl2_console *scon, const char *name);

/* Tear down decoration state. */
void sdl2_decoration_fini(struct sdl2_console *scon);

/*
 * Compute the per-axis scale factor and window dimensions from the guest
 * framebuffer size. Idempotent; safe to call repeatedly when the surface
 * geometry changes.
 */
void sdl2_decoration_compute_layout(struct sdl2_console *scon,
                                    int surface_w, int surface_h);

/* Render decoration overlay on top of the cleared window. */
void sdl2_decoration_render(struct sdl2_console *scon);

/*
 * Software-composite the framebuffer + decoration + close button into the
 * decoration's compose_buf. Used on the macOS Cocoa overlay path where we
 * skip SDL's renderer and feed the result to a CALayer ourselves. Returns
 * the compose_buf surface on success (caller does not free), or NULL on
 * failure / unsupported framebuffer pixel format.
 */
SDL_Surface *sdl2_decoration_compose(struct sdl2_console *scon);

/* Returns a button index if (x, y) is inside a hot-zone, else -1. */
int sdl2_decoration_button_at(struct sdl2_console *scon, int x, int y);

/* True if (x, y) is inside the host close button hot-zone (if any). */
bool sdl2_decoration_is_close(struct sdl2_console *scon, int x, int y);

/*
 * Translate window-coordinate mouse position into guest surface coordinates.
 * Returns false if the point lies outside the screen rect.
 */
bool sdl2_decoration_map_mouse(struct sdl2_console *scon,
                               int win_x, int win_y, int win_xrel, int win_yrel,
                               int surf_w, int surf_h,
                               int *out_x, int *out_y,
                               int *out_dx, int *out_dy);

/* Send a key event (down/up) for a given button index. */
void sdl2_decoration_send_button(struct sdl2_console *scon,
                                 int button_index, bool down);

#endif /* SDL2_DECORATION_H */
