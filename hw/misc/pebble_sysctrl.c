/*
 * Pebble System Control
 *
 * Simple system control peripheral for Pebble generic machines.
 * Provides board identification and feature flags.
 *
 * Registers (0x1000 region):
 *   0x00 BOARD_ID  - Board identifier (read-only)
 *   0x04 FEATURES  - Feature flags (read-only)
 *                    Bit 0: has touch
 *                    Bit 1: has audio
 *                    Bit 2: round display
 *   0x08 DISPLAY_W - Display width (read-only)
 *   0x0C DISPLAY_H - Display height (read-only)
 *   0x10 DISPLAY_F - Display format: bpp (read-only)
 *   0x14 RESET_REASON - Why the last reset happened (write 1 to clear)
 *                    Bit 0: power on
 *                    Bit 1: soft reset
 *                    Bit 2: watchdog
 *
 * GPIO input "wdog": the watchdog timeout line. Its level when a reset
 * begins decides between a soft and a watchdog reset.
 *
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "hw/resettable.h"

#define TYPE_PEBBLE_SYSCTRL "pebble-sysctrl"
OBJECT_DECLARE_SIMPLE_TYPE(PblSysCtrl, PEBBLE_SYSCTRL)

/* Register offsets */
#define SYSCTRL_BOARD_ID    0x00
#define SYSCTRL_FEATURES    0x04
#define SYSCTRL_DISPLAY_W   0x08
#define SYSCTRL_DISPLAY_H   0x0C
#define SYSCTRL_DISPLAY_F   0x10
#define SYSCTRL_RESET_REASON 0x14

/* Reset reason bits */
#define RESET_POR   (1 << 0)
#define RESET_SOFT  (1 << 1)
#define RESET_WDOG  (1 << 2)

/* Feature bits */
#define FEAT_TOUCH  (1 << 0)
#define FEAT_AUDIO  (1 << 1)
#define FEAT_ROUND  (1 << 2)

struct PblSysCtrl {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t board_id;
    uint32_t features;
    uint32_t display_w;
    uint32_t display_h;
    uint32_t display_fmt;

    uint32_t reset_reason;
    bool wdog_level;
    bool booted;
};

static uint64_t pbl_sysctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    PblSysCtrl *s = opaque;

    switch (offset) {
    case SYSCTRL_BOARD_ID:
        return s->board_id;
    case SYSCTRL_FEATURES:
        return s->features;
    case SYSCTRL_DISPLAY_W:
        return s->display_w;
    case SYSCTRL_DISPLAY_H:
        return s->display_h;
    case SYSCTRL_DISPLAY_F:
        return s->display_fmt;
    case SYSCTRL_RESET_REASON:
        return s->reset_reason;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-sysctrl: bad read offset 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void pbl_sysctrl_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    PblSysCtrl *s = opaque;

    switch (offset) {
    case SYSCTRL_RESET_REASON:
        s->reset_reason &= ~value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-sysctrl: write to read-only register 0x%" HWADDR_PRIx "\n",
                      offset);
    }
}

static void pbl_sysctrl_wdog(void *opaque, int n, int level)
{
    PblSysCtrl *s = opaque;

    s->wdog_level = level != 0;
}

/* Runs before any device's hold phase, so the watchdog's timeout line is
 * still asserted when its second expiry is what reset the machine. */
static void pbl_sysctrl_enter_reset(Object *obj, ResetType type)
{
    PblSysCtrl *s = PEBBLE_SYSCTRL(obj);

    if (!s->booted) {
        s->reset_reason = RESET_POR;
        s->booted = true;
    } else if (s->wdog_level) {
        s->reset_reason |= RESET_WDOG;
    } else {
        s->reset_reason |= RESET_SOFT;
    }
}

static const MemoryRegionOps pbl_sysctrl_ops = {
    .read = pbl_sysctrl_read,
    .write = pbl_sysctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void pbl_sysctrl_init(Object *obj)
{
    PblSysCtrl *s = PEBBLE_SYSCTRL(obj);

    memory_region_init_io(&s->iomem, obj, &pbl_sysctrl_ops, s,
                          "pebble-sysctrl", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_in_named(DEVICE(obj), pbl_sysctrl_wdog, "wdog", 1);
}

static const Property pbl_sysctrl_properties[] = {
    DEFINE_PROP_UINT32("board-id", PblSysCtrl, board_id, 0),
    DEFINE_PROP_UINT32("features", PblSysCtrl, features, 0),
    DEFINE_PROP_UINT32("display-width", PblSysCtrl, display_w, 0),
    DEFINE_PROP_UINT32("display-height", PblSysCtrl, display_h, 0),
    DEFINE_PROP_UINT32("display-format", PblSysCtrl, display_fmt, 0),
};

static void pbl_sysctrl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, pbl_sysctrl_properties);
    rc->phases.enter = pbl_sysctrl_enter_reset;
}

static const TypeInfo pbl_sysctrl_info = {
    .name          = TYPE_PEBBLE_SYSCTRL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PblSysCtrl),
    .instance_init = pbl_sysctrl_init,
    .class_init    = pbl_sysctrl_class_init,
};

static void pbl_sysctrl_register_types(void)
{
    type_register_static(&pbl_sysctrl_info);
}

type_init(pbl_sysctrl_register_types)
