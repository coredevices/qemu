/*
 * SDL2 + Cocoa overlay for transparent decorated windows on macOS.
 *
 * Real libsdl.org SDL 2.32 doesn't honor SDL_WINDOW_TRANSPARENT (an SDL3-only
 * flag): SDL's contentView blocks alpha at the compositor level even with
 * NSWindow.opaque = NO. This file works around that by replacing the
 * NSWindow's contentView with a plain layer-backed NSView whose `contents`
 * we drive directly with a CGImage. SDL's original SDLView is preserved as
 * a fullsize subview so its mouse/keyboard tracking remains in the
 * responder chain.
 *
 * Lifetime: install attaches our NSView and stashes it on the SDL window
 * via objc_setAssociatedObject; uninstall reverses both. blit creates a
 * new CGImage each frame from the caller's RGBA buffer (the buffer is
 * copied into CGImage-owned storage so the caller can free or reuse it).
 */
#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/sdl2.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <objc/runtime.h>

/*
 * Custom contentView. We hold the latest framebuffer as a CGImage and
 * stamp it via drawRect: rather than via layer.contents — layer-backed
 * `contents = CGImage` ends up being treated as opaque on macOS even when
 * the layer is configured with opaque=NO, but a plain NSView drawing
 * through NSGraphicsContext produces the per-pixel alpha the compositor
 * actually honors.
 */
@interface QEMUSdlDecoView : NSView {
    CGImageRef _image;
}
- (void)setFrameImage:(CGImageRef)image;
@end

@implementation QEMUSdlDecoView

- (void)dealloc
{
    if (_image) {
        CGImageRelease(_image);
    }
    [super dealloc];
}

- (BOOL)isOpaque { return NO; }
/*
 * isFlipped stays at NSView's default (NO). With a non-flipped view the
 * CG context's CTM is bottom-left, which is the orientation
 * CGContextDrawImage expects to render a top-left-origin CGImage right-
 * side-up. Setting isFlipped=YES would composite the image upside-down.
 */

- (void)setFrameImage:(CGImageRef)image
{
    if (_image == image) {
        return;
    }
    if (_image) {
        CGImageRelease(_image);
    }
    _image = image ? CGImageRetain(image) : NULL;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect
{
    if (!_image) {
        return;
    }
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    /* Clear to fully transparent first — the previous frame's pixels would
     * otherwise linger anywhere outside the dirty rect that the new image
     * leaves at alpha=0. */
    CGContextClearRect(ctx, NSRectToCGRect([self bounds]));
    CGContextDrawImage(ctx, NSRectToCGRect([self bounds]), _image);
}

@end

static const char k_assoc_view[] = "qemu-sdl-deco-view";
static const char k_assoc_sdlcv[] = "qemu-sdl-original-cv";

static NSWindow *ns_window_for(SDL_Window *win)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info)) {
        return nil;
    }
    if (info.subsystem != SDL_SYSWM_COCOA) {
        return nil;
    }
    return info.info.cocoa.window;
}

bool sdl2_cocoa_install(SDL_Window *win)
{
    NSWindow *ns = ns_window_for(win);
    if (!ns) {
        return false;
    }

    [ns setOpaque:NO];
    [ns setHasShadow:NO];
    [ns setBackgroundColor:[NSColor clearColor]];

    NSView *sdl_cv = [ns contentView];
    QEMUSdlDecoView *deco =
        [[QEMUSdlDecoView alloc] initWithFrame:[sdl_cv frame]];
    deco.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    /* Deliberately NOT setWantsLayer:YES — the drawRect: path through
     * NSGraphicsContext is the one we proved keeps the per-pixel alpha
     * intact on this macOS+SDL2 combination. */

    [ns setContentView:deco];

    /* Re-host SDL's contentView fullsize so SDL's NSResponder overrides
     * keep firing (mouse, keyboard, tracking areas — including the hits
     * routed by SDL_SetWindowHitTest). We can't simply hide it: hidden
     * views don't receive events. But we can set alphaValue=0 — that's
     * purely a compositing hint, so AppKit still hit-tests it normally
     * but SDLView's drawRect: never paints anything visible over our
     * deco view's contents. */
    [sdl_cv setFrame:[deco bounds]];
    sdl_cv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [sdl_cv setAlphaValue:0.0];
    [deco addSubview:sdl_cv];

    /* Stash both views on the NSWindow so install/uninstall/blit can find
     * them without us threading state through QEMU's struct. */
    objc_setAssociatedObject(ns, k_assoc_view, deco,
                             OBJC_ASSOCIATION_RETAIN);
    objc_setAssociatedObject(ns, k_assoc_sdlcv, sdl_cv,
                             OBJC_ASSOCIATION_RETAIN);
    return true;
}

void sdl2_cocoa_uninstall(SDL_Window *win)
{
    NSWindow *ns = ns_window_for(win);
    if (!ns) {
        return;
    }
    NSView *sdl_cv = objc_getAssociatedObject(ns, k_assoc_sdlcv);
    if (sdl_cv) {
        /* Restoring the original contentView puts SDL back in charge of
         * the window's drawing surface so it can be cleanly destroyed. */
        [ns setContentView:sdl_cv];
    }
    objc_setAssociatedObject(ns, k_assoc_view, nil, OBJC_ASSOCIATION_RETAIN);
    objc_setAssociatedObject(ns, k_assoc_sdlcv, nil, OBJC_ASSOCIATION_RETAIN);
}

void sdl2_cocoa_blit(SDL_Window *win, const void *rgba_pixels,
                     int w, int h, int stride_bytes)
{
    NSWindow *ns = ns_window_for(win);
    if (!ns) {
        return;
    }
    QEMUSdlDecoView *deco = objc_getAssociatedObject(ns, k_assoc_view);
    if (!deco || !rgba_pixels || w <= 0 || h <= 0) {
        return;
    }

    /* CGDataProviderCreateWithData with a NULL release callback would
     * require us to keep the buffer alive until CG drops the image.
     * Easier and safer: copy the pixels into CFData and let Core
     * Graphics own the lifetime. */
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)rgba_pixels,
                                  (CFIndex)stride_bytes * h);
    CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* Caller passes RGBA8888 byte order with straight (non-premultiplied)
     * alpha; CG handles premultiplication during composite. */
    CGImageRef img = CGImageCreate(w, h, 8, 32, stride_bytes, cs,
        kCGImageAlphaLast | kCGBitmapByteOrder32Big,
        dp, NULL, NO, kCGRenderingIntentDefault);

    [deco setFrameImage:img];

    CGImageRelease(img);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(dp);
    CFRelease(data);
}
