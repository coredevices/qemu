/*
 * Pebble External Flash
 *
 * Simple external flash device with XIP (eXecute In Place) support
 * for Pebble generic machines. Provides a RAM-backed memory region
 * at the XIP address for direct reads, plus control registers.
 *
 * Two MMIO regions:
 *   Region 0 (4K): Control registers
 *   Region 1 (32MB): XIP memory (read/write RAM)
 *
 * Control Registers (must match firmware in qemu_flash_hal.c):
 *   0x00 CMD        - Write: 1=ERASE_SUBSECTOR, 2=ERASE_SECTOR, 3=WRITE_ENABLE
 *   0x04 ADDR       - Write: target XIP-base + offset for the next CMD
 *   0x08 STATUS     - Read:  bit 0 BUSY, bit 1 COMPLETE (we're synchronous, so 0)
 *   0x0C INT_CTRL   - (unused, accepts writes for forward-compat)
 *   0x10 INT_STATUS - (unused, accepts writes for forward-compat)
 *   0x14 SIZE       - Read:  flash size in bytes
 *
 * Erase semantics: writing CMD_ERASE_{SUBSECTOR,SECTOR} after staging an
 * address in ADDR memset()s the targeted region of the XIP storage to 0xFF,
 * matching real NOR-flash behavior.  Without this PFS / flash_logging see
 * stale bytes in regions they think are erased and the firmware silently
 * misbehaves over many install cycles.
 *
 * The flash content can be loaded from a block device (drive property)
 * for persistence across QEMU sessions.
 *
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/notify.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/block/block.h"
#include "system/block-backend.h"
#include "system/runstate.h"

#define TYPE_PEBBLE_EXTFLASH "pebble-extflash"
OBJECT_DECLARE_SIMPLE_TYPE(PblExtFlash, PEBBLE_EXTFLASH)

/* Register offsets — must match firmware (qemu_flash_hal.c) */
#define EXTFLASH_CMD         0x00
#define EXTFLASH_ADDR        0x04
#define EXTFLASH_STATUS      0x08
#define EXTFLASH_INT_CTRL    0x0C
#define EXTFLASH_INT_STATUS  0x10
#define EXTFLASH_SIZE        0x14

/* CMD values */
#define EXTFLASH_CMD_ERASE_SUBSECTOR  1
#define EXTFLASH_CMD_ERASE_SECTOR     2
#define EXTFLASH_CMD_WRITE_ENABLE     3

/* Erase geometry — must match firmware QEMU_{SUBSECTOR,SECTOR}_SIZE */
#define EXTFLASH_SUBSECTOR_SIZE       0x1000   /* 4 KB */
#define EXTFLASH_SECTOR_SIZE          0x10000  /* 64 KB */

/* Default size: 32 MB */
#define EXTFLASH_DEFAULT_SIZE  (32 * MiB)

/* Default XIP base (matches PBL_EXTFLASH_BASE in pebble_generic.h).  Firmware
 * passes erase addresses as XIP-mapped pointers; we subtract this to find
 * the offset within s->storage.  Overridable via the "xip-base" property. */
#define EXTFLASH_DEFAULT_XIP_BASE     0x10000000

struct PblExtFlash {
    SysBusDevice parent_obj;

    MemoryRegion iomem_regs;
    MemoryRegion iomem_xip;

    BlockBackend *blk;
    uint8_t *storage;
    uint32_t size;
    uint32_t backed_size;
    uint32_t xip_base;

    /* Last value written to EXTFLASH_ADDR.  Latched until a CMD consumes it,
     * matching the typical hardware register pattern. */
    uint32_t pending_addr;

    Notifier shutdown_notifier;
};

static void pbl_extflash_do_erase(PblExtFlash *s, uint32_t cmd)
{
    size_t erase_size;
    switch (cmd) {
    case EXTFLASH_CMD_ERASE_SUBSECTOR:
        erase_size = EXTFLASH_SUBSECTOR_SIZE;
        break;
    case EXTFLASH_CMD_ERASE_SECTOR:
        erase_size = EXTFLASH_SECTOR_SIZE;
        break;
    default:
        return;
    }

    /* The firmware stages XIP-mapped addresses (PBL_EXTFLASH_BASE + offset).
     * Subtract the XIP base, then align down to the requested erase
     * granularity — both real NOR flash and PFS expect that. */
    uint32_t addr = s->pending_addr;
    if (addr < s->xip_base) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-extflash: erase addr 0x%" PRIx32
                      " below xip_base 0x%" PRIx32 "\n",
                      addr, s->xip_base);
        return;
    }
    uint32_t offset = (addr - s->xip_base) & ~(uint32_t)(erase_size - 1);
    if (offset >= s->size || offset + erase_size > s->size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-extflash: erase out of range offset=0x%" PRIx32
                      " size=0x%zx\n",
                      offset, erase_size);
        return;
    }

    memset(s->storage + offset, 0xFF, erase_size);
}

static uint64_t pbl_extflash_regs_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    PblExtFlash *s = opaque;

    switch (offset) {
    case EXTFLASH_CMD:
        return 0;  /* command register reads as 0 once consumed */
    case EXTFLASH_ADDR:
        return s->pending_addr;
    case EXTFLASH_STATUS:
        return 0;  /* synchronous: never busy, never reports complete bit */
    case EXTFLASH_INT_CTRL:
    case EXTFLASH_INT_STATUS:
        return 0;
    case EXTFLASH_SIZE:
        return s->size;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-extflash: bad read offset 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void pbl_extflash_regs_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    PblExtFlash *s = opaque;

    switch (offset) {
    case EXTFLASH_CMD:
        switch (value) {
        case EXTFLASH_CMD_ERASE_SUBSECTOR:
        case EXTFLASH_CMD_ERASE_SECTOR:
            pbl_extflash_do_erase(s, (uint32_t)value);
            break;
        case EXTFLASH_CMD_WRITE_ENABLE:
            /* No-op in QEMU: writes are unconditionally accepted via the
             * RAM-backed XIP region. */
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pebble-extflash: unknown CMD 0x%" PRIx64 "\n",
                          value);
            break;
        }
        break;
    case EXTFLASH_ADDR:
        s->pending_addr = (uint32_t)value;
        break;
    case EXTFLASH_INT_CTRL:
    case EXTFLASH_INT_STATUS:
        /* Accept and discard — firmware writes-1-to-clear style. */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-extflash: bad write offset 0x%" HWADDR_PRIx "\n",
                      offset);
        break;
    }
}

static const MemoryRegionOps pbl_extflash_regs_ops = {
    .read = pbl_extflash_regs_read,
    .write = pbl_extflash_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void pbl_extflash_shutdown_notify(Notifier *n, void *data)
{
    PblExtFlash *s = container_of(n, PblExtFlash, shutdown_notifier);

    if (!s->blk || !blk_is_writable(s->blk) || s->backed_size == 0) {
        return;
    }
    if (blk_pwrite(s->blk, 0, s->backed_size, s->storage, 0) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-extflash: failed to flush storage on shutdown\n");
    }
}

static void pbl_extflash_realize(DeviceState *dev, Error **errp)
{
    PblExtFlash *s = PEBBLE_EXTFLASH(dev);

    /* Load content from block device if provided */
    if (s->blk) {
        uint64_t perm = BLK_PERM_CONSISTENT_READ;
        if (blk_supports_write_perm(s->blk)) {
            perm |= BLK_PERM_WRITE;
        }
        if (!blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp)) {
            /* Continue anyway - try to read the data */
        }

        int64_t blk_size = blk_getlength(s->blk);
        if (blk_size < 0) {
            error_setg(errp, "pebble-extflash: failed to get drive size");
            return;
        }
        if (blk_size > s->size) {
            blk_size = s->size;
        }
        if (blk_size > 0) {
            if (blk_pread(s->blk, 0, blk_size, s->storage, 0) < 0) {
                error_setg(errp, "pebble-extflash: failed to read drive");
                return;
            }
            s->backed_size = blk_size;
        }

        /* Persist guest writes back to the file before bdrv_close_all()
         * tears the block layer down. Fires for graceful shutdown,
         * `(qemu) quit`, and SIGINT (Ctrl+C). */
        s->shutdown_notifier.notify = pbl_extflash_shutdown_notify;
        qemu_register_shutdown_notifier(&s->shutdown_notifier);
    }
}

static void pbl_extflash_init(Object *obj)
{
    PblExtFlash *s = PEBBLE_EXTFLASH(obj);

    /* Region 0: control registers */
    memory_region_init_io(&s->iomem_regs, obj, &pbl_extflash_regs_ops, s,
                          "pebble-extflash-regs", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem_regs);

    /* Region 1: XIP memory (allocated with default size, content loaded in realize) */
    s->storage = g_malloc(EXTFLASH_DEFAULT_SIZE);
    memset(s->storage, 0xFF, EXTFLASH_DEFAULT_SIZE);  /* Erased flash = 0xFF */
    memory_region_init_ram_ptr(&s->iomem_xip, obj,
                               "pebble-extflash-xip",
                               EXTFLASH_DEFAULT_SIZE, s->storage);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem_xip);
}

static void pbl_extflash_reset(DeviceState *dev)
{
    PblExtFlash *s = PEBBLE_EXTFLASH(dev);
    s->pending_addr = 0;
}

static const Property pbl_extflash_properties[] = {
    DEFINE_PROP_DRIVE("drive", PblExtFlash, blk),
    DEFINE_PROP_UINT32("size", PblExtFlash, size, EXTFLASH_DEFAULT_SIZE),
    DEFINE_PROP_UINT32("xip-base", PblExtFlash, xip_base,
                       EXTFLASH_DEFAULT_XIP_BASE),
};

static void pbl_extflash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pbl_extflash_realize;
    device_class_set_legacy_reset(dc, pbl_extflash_reset);
    device_class_set_props(dc, pbl_extflash_properties);
}

static const TypeInfo pbl_extflash_info = {
    .name          = TYPE_PEBBLE_EXTFLASH,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PblExtFlash),
    .instance_init = pbl_extflash_init,
    .class_init    = pbl_extflash_class_init,
};

static void pbl_extflash_register_types(void)
{
    type_register_static(&pbl_extflash_info);
}

type_init(pbl_extflash_register_types)
