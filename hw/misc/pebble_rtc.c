/*
 * Pebble Generic RTC
 *
 * Simple RTC with backup registers for Pebble generic machines.
 * Provides host time and backup registers for QEMU settings.
 *
 * Registers (0x1000 region):
 *   0x00 TIME_LO  - Unix timestamp low 32 bits (read: current time)
 *   0x04 TIME_HI  - Unix timestamp high 32 bits
 *   0x08 ALARM    - Alarm time (low 32 bits)
 *   0x0C CTRL     - Bit 0: alarm enable, Bit 1: alarm IRQ pending (w1c)
 *   0x40..0x7F    - Backup registers (16 x 32-bit, read/write, persist across reset)
 *
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/sysbus.h"

#include <time.h>
#include <stdlib.h>

#define TYPE_PEBBLE_GENERIC_RTC "pebble-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(PblRtc, PEBBLE_GENERIC_RTC)

/* Register offsets */
#define RTC_TIME_LO     0x00
#define RTC_TIME_HI     0x04
#define RTC_ALARM       0x08
#define RTC_CTRL        0x0C
#define RTC_TICKS       0x10  /* Monotonic 1000Hz tick counter */

#define RTC_BACKUP_BASE 0x40
#define RTC_BACKUP_END  0x80
#define RTC_NUM_BACKUP  16

/* CTRL bits */
#define CTRL_ALARM_EN   (1 << 0)
#define CTRL_ALARM_IRQ  (1 << 1)

struct PblRtc {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t alarm;
    uint32_t ctrl;
    uint32_t backup[RTC_NUM_BACKUP];
};

static void pbl_rtc_update_irq(PblRtc *s)
{
    qemu_set_irq(s->irq, (s->ctrl & CTRL_ALARM_EN) &&
                          (s->ctrl & CTRL_ALARM_IRQ));
}

/* Return the local-time UTC offset in seconds, cached on first call.
 *
 * Honors the TZ_OFFSET_SEC environment variable (seconds east of UTC) when set
 * — this matches the stm32 Pebble RTC and is how the browser/JS build injects
 * the user's timezone. When unset, falls back to the host's detected local
 * offset via localtime_r(). */
static int64_t pbl_rtc_tz_offset(void)
{
    static int64_t offset_sec;
    static bool cached;

    if (!cached) {
        const char *env = getenv("TZ_OFFSET_SEC");
        if (env) {
            offset_sec = (int64_t)atoll(env);
        } else {
            time_t now = time(NULL);
            struct tm lt;
            if (localtime_r(&now, &lt)) {
                offset_sec = (int64_t)lt.tm_gmtoff;
            }
        }
        cached = true;
    }
    return offset_sec;
}

/* Return "local time as a unix timestamp". Pebble firmware stores wall-clock
 * time in this form, so the value read here is shifted by the local UTC
 * offset — a guest that applies no further timezone conversion will still
 * display the user's local wall-clock time. */
static uint64_t pbl_rtc_get_time(void)
{
    uint64_t utc = (uint64_t)qemu_clock_get_ms(QEMU_CLOCK_HOST) / 1000;
    return utc + pbl_rtc_tz_offset();
}

static uint64_t pbl_rtc_read(void *opaque, hwaddr offset, unsigned size)
{
    PblRtc *s = opaque;
    uint64_t now;

    switch (offset) {
    case RTC_TIME_LO:
        now = pbl_rtc_get_time();
        return (uint32_t)now;

    case RTC_TIME_HI:
        now = pbl_rtc_get_time();
        return (uint32_t)(now >> 32);

    case RTC_ALARM:
        return s->alarm;

    case RTC_CTRL:
        return s->ctrl;

    case RTC_TICKS:
        /* Monotonic millisecond counter (1000Hz ticks) */
        return (uint32_t)qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    default:
        if (offset >= RTC_BACKUP_BASE && offset < RTC_BACKUP_END) {
            int idx = (offset - RTC_BACKUP_BASE) / 4;
            return s->backup[idx];
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-rtc: bad read offset 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void pbl_rtc_write(void *opaque, hwaddr offset,
                           uint64_t value, unsigned size)
{
    PblRtc *s = opaque;

    switch (offset) {
    case RTC_ALARM:
        s->alarm = value;
        break;

    case RTC_CTRL:
        /* Write 1 to clear IRQ pending bit */
        if (value & CTRL_ALARM_IRQ) {
            s->ctrl &= ~CTRL_ALARM_IRQ;
        }
        /* Update enable bit */
        s->ctrl = (s->ctrl & CTRL_ALARM_IRQ) | (value & CTRL_ALARM_EN);
        pbl_rtc_update_irq(s);
        break;

    default:
        if (offset >= RTC_BACKUP_BASE && offset < RTC_BACKUP_END) {
            int idx = (offset - RTC_BACKUP_BASE) / 4;
            s->backup[idx] = value;
            return;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-rtc: bad write offset 0x%" HWADDR_PRIx "\n",
                      offset);
        break;
    }
}

static const MemoryRegionOps pbl_rtc_ops = {
    .read = pbl_rtc_read,
    .write = pbl_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void pbl_rtc_init(Object *obj)
{
    PblRtc *s = PEBBLE_GENERIC_RTC(obj);

    memory_region_init_io(&s->iomem, obj, &pbl_rtc_ops, s,
                          "pebble-rtc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void pbl_rtc_reset(DeviceState *dev)
{
    PblRtc *s = PEBBLE_GENERIC_RTC(dev);

    s->alarm = 0;
    s->ctrl = 0;
    /* Backup registers intentionally NOT cleared on reset */
}

static void pbl_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, pbl_rtc_reset);
}

static const TypeInfo pbl_rtc_info = {
    .name          = TYPE_PEBBLE_GENERIC_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PblRtc),
    .instance_init = pbl_rtc_init,
    .class_init    = pbl_rtc_class_init,
};

static void pbl_rtc_register_types(void)
{
    type_register_static(&pbl_rtc_info);
}

type_init(pbl_rtc_register_types)
