/*
 * Pebble Generic Display
 *
 * Simple framebuffer display for Pebble generic machines.
 * Firmware writes pixels to a memory-mapped framebuffer region,
 * then sets UPDATE_REQUEST in the CTRL register. QEMU renders
 * the framebuffer to a QemuConsole and fires an interrupt.
 *
 * Two MMIO regions:
 *   Region 0 (4K): Control registers
 *   Region 1 (up to 128K): Framebuffer pixel data
 *
 * Supported formats:
 *   1bpp - monochrome (1 bit per pixel, MSB first, row-major)
 *   8bpp - ARGB2222 (2 bits A, 2 bits R, 3 bits G, 2 bits B)
 *
 * Control Registers:
 *   0x00 CTRL       - Bit 0: enable, Bit 1: update request (write-1-to-set)
 *   0x04 STATUS     - Bit 0: busy (always 0 in QEMU)
 *   0x08 WIDTH      - Display width in pixels
 *   0x0C HEIGHT     - Display height in pixels
 *   0x10 FORMAT     - Bits per pixel
 *   0x14 FLAGS      - Bit 0: round mask
 *   0x18 BRIGHTNESS - Backlight brightness 0-255
 *   0x1C INTSTAT    - Bit 0: update complete (write-1-to-clear)
 *   0x20 INTCTRL    - Bit 0: update complete IRQ enable
 *   0x24 BL_RED     - RGB backlight red channel 0-255 (default 255)
 *   0x28 BL_GREEN   - RGB backlight green channel 0-255 (default 255)
 *   0x2C BL_BLUE    - RGB backlight blue channel 0-255 (default 255)
 *   0x400 MASK      - Round mask: pixels hidden at each end of every row of
 *                     the top half, one byte per row, mirrored in the bottom
 *                     half (512 bytes, little-endian words)
 *
 * WIDTH, HEIGHT, FORMAT, FLAGS and MASK read the geometry in use, the
 * machine's size and format, unmasked, until the firmware sets its own: writes
 * made while the display is disabled take effect when it is enabled, if the
 * framebuffer fits the region, and are dropped otherwise. A reset restores the
 * machine's geometry.
 *
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "pebble_sunlight.h"

#define TYPE_PEBBLE_DISPLAY "pebble-display"
OBJECT_DECLARE_SIMPLE_TYPE(PblDisplay, PEBBLE_DISPLAY)

/* Register offsets */
#define DISP_CTRL       0x00
#define DISP_STATUS     0x04
#define DISP_WIDTH      0x08
#define DISP_HEIGHT     0x0C
#define DISP_FORMAT     0x10
#define DISP_FLAGS      0x14
#define DISP_BRIGHTNESS 0x18
#define DISP_INTSTAT    0x1C
#define DISP_INTCTRL    0x20
#define DISP_BL_RED     0x24
#define DISP_BL_GREEN   0x28
#define DISP_BL_BLUE    0x2C
#define DISP_MASK       0x400
#define MASK_SIZE       512

/* CTRL bits */
#define CTRL_ENABLE     (1 << 0)
#define CTRL_UPDATE     (1 << 1)

/* INTSTAT / INTCTRL bits */
#define INT_UPDATE_DONE (1 << 0)

/* FLAGS bits */
#define FLAG_ROUND      (1 << 0)

/* Display format constants */
#define FMT_1BPP  1
#define FMT_8BPP  8

/* Size of the framebuffer region */
#define FB_REGION_SIZE (128 * 1024)

/* Transflective LCD ambient-reflectance floor (0-255). Sets the visible
 * brightness when the RGB backlight is fully off, matching a Pebble's
 * readability in ambient light. */
#define PBL_DISPLAY_AMBIENT_FLOOR 100

struct PblDisplay {
    SysBusDevice parent_obj;

    MemoryRegion iomem_regs;
    MemoryRegion iomem_fb;
    QemuConsole *con;
    qemu_irq irq;

    /* Framebuffer backing store (written by guest, read by display update) */
    uint8_t *fb;
    uint32_t fb_size;

    /* Machine geometry (set via properties before realize) */
    uint32_t cfg_width;
    uint32_t cfg_height;
    uint32_t cfg_format;    /* bpp: 1 or 8 */

    /* Geometry in use */
    uint32_t width;
    uint32_t height;
    uint32_t format;
    bool round_mask;
    uint8_t mask[MASK_SIZE];

    /* Geometry the firmware set, applied on enable */
    uint32_t new_width;
    uint32_t new_height;
    uint32_t new_format;
    bool new_round_mask;
    uint8_t new_mask[MASK_SIZE];

    /* Registers */
    uint32_t ctrl;
    uint32_t brightness;
    uint32_t intstat;
    uint32_t intctrl;
    uint8_t  bl_r;
    uint8_t  bl_g;
    uint8_t  bl_b;

    /* Use sunlight-corrected palette */
    bool sunlight;

    bool redraw;
    bool resize;
    bool vibrating;
    int vibe_frame;
    QEMUTimer *vibe_timer;
};

static void pbl_display_update_irq(PblDisplay *s)
{
    qemu_set_irq(s->irq,
                 (s->intctrl & INT_UPDATE_DONE) &&
                 (s->intstat & INT_UPDATE_DONE));
}

/* Convert ARGB2222 byte to RGB888 components */
static void argb2222_to_rgb(uint8_t pixel, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* PebbleOS GColor8: A: bits 7-6, R: bits 5-4, G: bits 3-2, B: bits 1-0 */
    *r = (pixel >> 4) & 0x3;
    *r = (*r << 6) | (*r << 4) | (*r << 2) | *r;  /* expand 2->8 bits */
    *g = (pixel >> 2) & 0x3;
    *g = (*g << 6) | (*g << 4) | (*g << 2) | *g;
    *b = pixel & 0x3;
    *b = (*b << 6) | (*b << 4) | (*b << 2) | *b;
}

static bool pixel_in_round_mask(PblDisplay *s, int x, int y)
{
    int qx = x < (int)s->width / 2 ? x : (int)s->width - 1 - x;
    int qy = y < (int)s->height / 2 ? y : (int)s->height - 1 - y;

    return qx >= s->mask[qy];
}

static void pbl_display_update(void *opaque)
{
    PblDisplay *s = opaque;
    DisplaySurface *surface;
    int bpp, stride;
    uint8_t *dest;

    if (!s->con) {
        return;
    }

    if (s->resize) {
        s->resize = false;
        qemu_console_resize(s->con, s->width, s->height);
    }

    if (!(s->ctrl & CTRL_ENABLE)) {
        return;
    }

    if (!s->redraw) {
        return;
    }
    s->redraw = false;

    surface = qemu_console_surface(s->con);
    if (!surface) {
        return;
    }

    bpp = surface_bits_per_pixel(surface);
    stride = surface_stride(surface);
    dest = surface_data(surface);

    /* Vibration shake offset: alternates +/- 2 pixels horizontally */
    int shake_x = 0;
    if (s->vibrating) {
        static const int shake_pattern[] = { -1, 1, 0, -1, 1, 0 };
        shake_x = shake_pattern[s->vibe_frame % 6];
    }

    for (uint32_t y = 0; y < s->height; y++) {
        for (uint32_t x = 0; x < s->width; x++) {
            uint8_t r, g, b;

            /* Apply shake: sample from offset position in framebuffer */
            int src_x = (int)x - shake_x;
            if (src_x < 0 || src_x >= (int)s->width) {
                r = g = b = 0;  /* Black for pixels shaken off-screen */
            } else
            /* Check round mask */
            if (s->round_mask &&
                !pixel_in_round_mask(s, src_x, y)) {
                r = g = b = 0;
            } else if (s->format == FMT_1BPP) {
                /* 1bpp: LSB first, rows padded to 32-bit boundary */
                uint32_t row_bytes = ((s->width + 31) / 32) * 4;
                uint32_t byte_idx = y * row_bytes + src_x / 8;
                uint32_t bit_idx = src_x & 7;  /* LSB first */
                bool on = (s->fb[byte_idx] >> bit_idx) & 1;
                /* on=1 means white pixel, on=0 means black in PebbleOS 1bpp */
                uint8_t level = on ? (s->brightness ? s->brightness : 0xFF) : 0;
                r = g = b = level;
            } else {
                /* 8bpp ARGB2222 */
                uint32_t idx = y * s->width + src_x;
                uint8_t raw = s->fb[idx];
                if (s->sunlight) {
                    pebble_sunlight_correct((raw >> 4) & 0x3,
                                            (raw >> 2) & 0x3,
                                            raw & 0x3, &r, &g, &b);
                } else {
                    argb2222_to_rgb(raw, &r, &g, &b);
                }
            }

            /* Assume a "sunlit" display would always be fully lit */
            if (!s->sunlight) {
                /* Apply brightness scaling for 8bpp */
                if (s->format == FMT_8BPP && s->brightness < 255 && s->brightness > 0) {
                    r = (uint8_t)((uint32_t)r * s->brightness / 255);
                    g = (uint8_t)((uint32_t)g * s->brightness / 255);
                    b = (uint8_t)((uint32_t)b * s->brightness / 255);
                }

                /* RGB backlight on a transflective LCD: ambient light reflects
                 * off the panel (neutral white) while the backlight shines
                 * through it (colored). The backlight dominates when bright, so
                 * the ambient white contribution must fade out as any channel of
                 * the LED lights up — otherwise pure-blue (0,0,255) leaves R=G=
                 * AMBIENT and you get washed-out light blue instead of deep blue.
                 *
                 * Scale ambient by (255 - max(bl_r,bl_g,bl_b)) so:
                 *   bl=(0,0,0)     → full ambient white floor (readable in room light)
                 *   bl=(0,0,255)   → no ambient, pure deep blue
                 *   bl=(255,255,255) → no ambient, full white
                 *   bl=(128,0,0)   → half ambient + half red → muted red-orange */
                {
                    const uint32_t amb = PBL_DISPLAY_AMBIENT_FLOOR;
                    uint32_t bl_max = s->bl_r;
                    if (s->bl_g > bl_max) bl_max = s->bl_g;
                    if (s->bl_b > bl_max) bl_max = s->bl_b;
                    uint32_t amb_contrib = amb * (255U - bl_max) / 255U;
                    uint32_t eff_r = amb_contrib + s->bl_r;
                    uint32_t eff_g = amb_contrib + s->bl_g;
                    uint32_t eff_b = amb_contrib + s->bl_b;
                    if (eff_r > 255U) eff_r = 255U;
                    if (eff_g > 255U) eff_g = 255U;
                    if (eff_b > 255U) eff_b = 255U;
                    r = (uint8_t)((uint32_t)r * eff_r / 255U);
                    g = (uint8_t)((uint32_t)g * eff_g / 255U);
                    b = (uint8_t)((uint32_t)b * eff_b / 255U);
                }
            }

            /* Write pixel to host surface */
            uint8_t *pixel = dest + y * stride + x * (bpp / 8);
            switch (bpp) {
            case 32:
                *(uint32_t *)pixel = rgb_to_pixel32(r, g, b);
                break;
            case 16:
                *(uint16_t *)pixel = rgb_to_pixel16(r, g, b);
                break;
            case 15:
                *(uint16_t *)pixel = rgb_to_pixel15(r, g, b);
                break;
            case 8:
                *pixel = rgb_to_pixel8(r, g, b);
                break;
            default:
                break;
            }
        }
    }

    dpy_gfx_update(s->con, 0, 0, s->width, s->height);
}

static void pbl_display_invalidate(void *opaque)
{
    PblDisplay *s = opaque;
    s->redraw = true;
}

static const GraphicHwOps pbl_display_ops = {
    .gfx_update = pbl_display_update,
    .invalidate = pbl_display_invalidate,
};

/* === Geometry === */

static uint32_t pbl_display_fb_size(uint32_t width, uint32_t height,
                                    uint32_t format)
{
    if (format == FMT_1BPP) {
        return ((width + 31) / 32) * 4 * height;
    }
    return width * height;  /* 8bpp = 1 byte per pixel */
}

static bool pbl_display_geometry_valid(uint32_t width, uint32_t height,
                                       uint32_t format)
{
    return (format == FMT_1BPP || format == FMT_8BPP) &&
           width > 0 && height > 0 && width <= 1024 &&
           height <= 2 * MASK_SIZE &&
           pbl_display_fb_size(width, height, format) <= FB_REGION_SIZE;
}

static void pbl_display_set_geometry(PblDisplay *s, uint32_t width,
                                     uint32_t height, uint32_t format,
                                     bool round_mask, const uint8_t *mask)
{
    memcpy(s->mask, mask, MASK_SIZE);
    memcpy(s->new_mask, mask, MASK_SIZE);
    s->width = s->new_width = width;
    s->height = s->new_height = height;
    s->format = s->new_format = format;
    s->round_mask = s->new_round_mask = round_mask;
    s->fb_size = pbl_display_fb_size(width, height, format);
    s->fb = g_realloc(s->fb, s->fb_size);
    memset(s->fb, 0, s->fb_size);
    /* The UI resizes the console, as it may only do it from its own thread */
    s->resize = true;
    s->redraw = true;
}

static void pbl_display_apply_geometry(PblDisplay *s)
{
    if (s->new_width == s->width && s->new_height == s->height &&
        s->new_format == s->format && s->new_round_mask == s->round_mask &&
        memcmp(s->new_mask, s->mask, MASK_SIZE) == 0) {
        return;
    }

    if (!pbl_display_geometry_valid(s->new_width, s->new_height,
                                    s->new_format)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-display: unsupported geometry %ux%u, %u bpp\n",
                      s->new_width, s->new_height, s->new_format);
        s->new_width = s->width;
        s->new_height = s->height;
        s->new_format = s->format;
        s->new_round_mask = s->round_mask;
        memcpy(s->new_mask, s->mask, MASK_SIZE);
        return;
    }

    pbl_display_set_geometry(s, s->new_width, s->new_height, s->new_format,
                             s->new_round_mask, s->new_mask);
}

static void pbl_display_set_machine_geometry(PblDisplay *s)
{
    static const uint8_t no_mask[MASK_SIZE];

    pbl_display_set_geometry(s, s->cfg_width, s->cfg_height, s->cfg_format,
                             false, no_mask);
}

/* === Control register access === */

static uint64_t pbl_display_regs_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    PblDisplay *s = opaque;

    if (offset >= DISP_MASK && offset < DISP_MASK + MASK_SIZE) {
        return ldl_le_p(&s->mask[offset - DISP_MASK]);
    }

    switch (offset) {
    case DISP_CTRL:
        return s->ctrl;
    case DISP_STATUS:
        return 0;  /* never busy */
    case DISP_WIDTH:
        return s->width;
    case DISP_HEIGHT:
        return s->height;
    case DISP_FORMAT:
        return s->format;
    case DISP_FLAGS:
        return s->round_mask ? FLAG_ROUND : 0;
    case DISP_BRIGHTNESS:
        return s->brightness;
    case DISP_INTSTAT:
        return s->intstat;
    case DISP_INTCTRL:
        return s->intctrl;
    case DISP_BL_RED:
        return s->bl_r;
    case DISP_BL_GREEN:
        return s->bl_g;
    case DISP_BL_BLUE:
        return s->bl_b;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-display: bad read offset 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void pbl_display_regs_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    PblDisplay *s = opaque;

    if (offset >= DISP_MASK && offset < DISP_MASK + MASK_SIZE) {
        if (s->ctrl & CTRL_ENABLE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pebble-display: mask written while enabled\n");
            return;
        }
        stl_le_p(&s->new_mask[offset - DISP_MASK], value);
        return;
    }

    switch (offset) {
    case DISP_CTRL:
        if ((value & CTRL_ENABLE) && !(s->ctrl & CTRL_ENABLE)) {
            pbl_display_apply_geometry(s);
        }
        s->ctrl = (s->ctrl & ~CTRL_ENABLE) | (value & CTRL_ENABLE);
        if (value & CTRL_UPDATE) {
            /* Update request: trigger redraw and signal completion */
            s->redraw = true;
            s->intstat |= INT_UPDATE_DONE;
            pbl_display_update_irq(s);
        }
        break;
    case DISP_WIDTH:
    case DISP_HEIGHT:
    case DISP_FORMAT:
    case DISP_FLAGS:
        if (s->ctrl & CTRL_ENABLE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pebble-display: geometry written while enabled\n");
            break;
        }
        if (offset == DISP_WIDTH) {
            s->new_width = value;
        } else if (offset == DISP_HEIGHT) {
            s->new_height = value;
        } else if (offset == DISP_FORMAT) {
            s->new_format = value;
        } else {
            s->new_round_mask = value & FLAG_ROUND;
        }
        break;
    case DISP_BRIGHTNESS:
        s->brightness = value & 0xFF;
        s->redraw = true;
        break;
    case DISP_INTSTAT:
        /* Write 1 to clear */
        s->intstat &= ~value;
        pbl_display_update_irq(s);
        break;
    case DISP_INTCTRL:
        s->intctrl = value & INT_UPDATE_DONE;
        pbl_display_update_irq(s);
        break;
    case DISP_BL_RED:
        s->bl_r = value & 0xFF;
        s->redraw = true;
        break;
    case DISP_BL_GREEN:
        s->bl_g = value & 0xFF;
        s->redraw = true;
        break;
    case DISP_BL_BLUE:
        s->bl_b = value & 0xFF;
        s->redraw = true;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-display: bad write offset 0x%" HWADDR_PRIx "\n",
                      offset);
        break;
    }
}

static const MemoryRegionOps pbl_display_regs_ops = {
    .read = pbl_display_regs_read,
    .write = pbl_display_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* === Framebuffer memory access === */

static uint64_t pbl_display_fb_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    PblDisplay *s = opaque;

    if (offset + size <= s->fb_size) {
        switch (size) {
        case 1:
            return s->fb[offset];
        case 2:
            return lduw_le_p(&s->fb[offset]);
        case 4:
            return ldl_le_p(&s->fb[offset]);
        default:
            break;
        }
    }
    return 0;
}

static void pbl_display_fb_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    PblDisplay *s = opaque;

    if (offset + size <= s->fb_size) {
        switch (size) {
        case 1:
            s->fb[offset] = value;
            break;
        case 2:
            stw_le_p(&s->fb[offset], value);
            break;
        case 4:
            stl_le_p(&s->fb[offset], value);
            break;
        default:
            break;
        }
    }
}

static const MemoryRegionOps pbl_display_fb_ops = {
    .read = pbl_display_fb_read,
    .write = pbl_display_fb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* === Device lifecycle === */

/* Global instance for external access */
static PblDisplay *s_display_instance;

#define VIBE_FRAME_MS 30

static void pbl_display_vibe_tick(void *opaque)
{
    PblDisplay *s = opaque;
    if (s->vibrating) {
        s->vibe_frame++;
        s->redraw = true;
        timer_mod(s->vibe_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + VIBE_FRAME_MS);
    } else {
        /* Final redraw with no shake to restore normal position */
        s->vibe_frame = 0;
        s->redraw = true;
    }
}

static void pbl_display_realize(DeviceState *dev, Error **errp)
{
    PblDisplay *s = PEBBLE_DISPLAY(dev);

    if (!pbl_display_geometry_valid(s->cfg_width, s->cfg_height,
                                    s->cfg_format)) {
        error_setg(errp, "unsupported geometry %ux%u, %u bpp", s->cfg_width,
                   s->cfg_height, s->cfg_format);
        return;
    }

    s_display_instance = s;
    s->vibe_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, pbl_display_vibe_tick, s);

    /* Create QemuConsole */
    s->con = graphic_console_init(dev, 0, &pbl_display_ops, s);
    pbl_display_set_machine_geometry(s);
    qemu_console_resize(s->con, s->width, s->height);
    s->resize = false;

    s->brightness = 0xFF;
    s->bl_r = 0xFF;
    s->bl_g = 0xFF;
    s->bl_b = 0xFF;
    s->redraw = true;
}

static void pbl_display_init(Object *obj)
{
    PblDisplay *s = PEBBLE_DISPLAY(obj);

    /* Region 0: control registers */
    memory_region_init_io(&s->iomem_regs, obj, &pbl_display_regs_ops, s,
                          "pebble-display-regs", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem_regs);

    /* Region 1: framebuffer (sized to max, actual usage depends on format) */
    memory_region_init_io(&s->iomem_fb, obj, &pbl_display_fb_ops, s,
                          "pebble-display-fb", FB_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem_fb);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void pbl_display_reset(DeviceState *dev)
{
    PblDisplay *s = PEBBLE_DISPLAY(dev);

    s->ctrl = 0;
    s->intstat = 0;
    s->intctrl = 0;
    s->brightness = 0xFF;
    s->bl_r = 0xFF;
    s->bl_g = 0xFF;
    s->bl_b = 0xFF;
    s->redraw = true;

    if (s->con) {
        pbl_display_set_machine_geometry(s);
    }
}

static const Property pbl_display_properties[] = {
    DEFINE_PROP_UINT32("width", PblDisplay, cfg_width, 200),
    DEFINE_PROP_UINT32("height", PblDisplay, cfg_height, 228),
    DEFINE_PROP_UINT32("format", PblDisplay, cfg_format, FMT_8BPP),
    DEFINE_PROP_BOOL("sunlight", PblDisplay, sunlight, false),
};

static void pbl_display_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pbl_display_realize;
    device_class_set_legacy_reset(dc, pbl_display_reset);
    device_class_set_props(dc, pbl_display_properties);
}

static const TypeInfo pbl_display_info = {
    .name          = TYPE_PEBBLE_DISPLAY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PblDisplay),
    .instance_init = pbl_display_init,
    .class_init    = pbl_display_class_init,
};

static void pbl_display_register_types(void)
{
    type_register_static(&pbl_display_info);
}

type_init(pbl_display_register_types)

/* Called from pebble_control.c when the vibe motor is toggled. */
void pbl_display_set_vibrating(bool on);

void pbl_display_set_vibrating(bool on)
{
    PblDisplay *s = s_display_instance;
    if (!s || s->vibrating == on) {
        return;
    }
    s->vibrating = on;
    if (on) {
        s->vibe_frame = 0;
        s->redraw = true;
        timer_mod(s->vibe_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + VIBE_FRAME_MS);
    } else {
        timer_del(s->vibe_timer);
        s->vibe_frame = 0;
        s->redraw = true;  /* Final redraw to restore position */
    }
}
