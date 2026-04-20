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
 * Control Registers:
 *   0x00 CTRL   - Bit 0: XIP enable (always enabled in QEMU)
 *   0x04 STATUS - Bit 0: busy (always 0 in QEMU)
 *   0x08 SIZE   - Flash size in bytes (read-only)
 *
 * The flash content can be loaded from a block device (drive property)
 * for persistence across QEMU sessions.
 *
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/block/block.h"
#include "system/block-backend.h"

#define TYPE_PEBBLE_EXTFLASH "pebble-extflash"
OBJECT_DECLARE_SIMPLE_TYPE(PblExtFlash, PEBBLE_EXTFLASH)

/* Register offsets */
#define EXTFLASH_CTRL    0x00
#define EXTFLASH_STATUS  0x04
#define EXTFLASH_SIZE    0x08

/* Default size: 32 MB */
#define EXTFLASH_DEFAULT_SIZE  (32 * MiB)

struct PblExtFlash {
    SysBusDevice parent_obj;

    MemoryRegion iomem_regs;
    MemoryRegion iomem_xip;

    BlockBackend *blk;
    uint8_t *storage;
    uint32_t size;

    uint32_t ctrl;
};

static uint64_t pbl_extflash_regs_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    PblExtFlash *s = opaque;

    switch (offset) {
    case EXTFLASH_CTRL:
        return s->ctrl;
    case EXTFLASH_STATUS:
        return 0;  /* never busy */
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
    case EXTFLASH_CTRL:
        s->ctrl = value & 0x1;
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
        }
    }

    s->ctrl = 1;  /* XIP enabled by default */
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
    s->ctrl = 1;
}

static const Property pbl_extflash_properties[] = {
    DEFINE_PROP_DRIVE("drive", PblExtFlash, blk),
    DEFINE_PROP_UINT32("size", PblExtFlash, size, EXTFLASH_DEFAULT_SIZE),
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
