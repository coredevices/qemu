/*
 * Pebble Simple UART
 *
 * Minimal UART peripheral for Pebble generic machines.
 * Simple register interface — not tied to any specific MCU.
 *
 * Registers (0x1000 region):
 *   0x00 DATA    - Write: TX byte, Read: RX byte (clears RX ready)
 *   0x04 STATE   - Bit 0: TX ready, Bit 1: RX data available
 *   0x08 CTRL    - Bit 0: TX IRQ enable, Bit 1: RX IRQ enable
 *   0x0C INTSTAT - Interrupt status (write 1 to clear bits)
 *                  Bit 0: TX complete, Bit 1: RX ready
 *
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties-system.h"
#include "hw/arm/pebble_simple_uart.h"
#include "chardev/char-fe.h"

#define TYPE_PEBBLE_SIMPLE_UART "pebble-simple-uart"
OBJECT_DECLARE_SIMPLE_TYPE(PblSimpleUart, PEBBLE_SIMPLE_UART)

/* Register offsets */
#define UART_DATA       0x00
#define UART_STATE      0x04
#define UART_CTRL       0x08
#define UART_INTSTAT    0x0C

/* STATE bits */
#define STATE_TX_READY  (1 << 0)
#define STATE_RX_AVAIL  (1 << 1)

/* CTRL bits */
#define CTRL_TX_IRQ_EN  (1 << 0)
#define CTRL_RX_IRQ_EN  (1 << 1)

/* INTSTAT bits */
#define INT_TX_COMPLETE (1 << 0)
#define INT_RX_READY    (1 << 1)

/* RX FIFO size */
#define RX_FIFO_SIZE 256

/* Write handler callback — intercepts firmware TX data */
typedef int (*PblUartWriteHandler)(void *opaque, const uint8_t *buf, int len);

struct PblSimpleUart {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharBackend chr;
    qemu_irq irq;

    uint32_t state;
    uint32_t ctrl;
    uint32_t intstat;

    uint8_t rx_fifo[RX_FIFO_SIZE];
    uint32_t rx_head;  /* next read position */
    uint32_t rx_count; /* bytes in FIFO */

    /* Optional write interceptor (for pebble_control) */
    PblUartWriteHandler write_handler;
    void *write_handler_opaque;
};

static void pbl_uart_update_irq(PblSimpleUart *s)
{
    bool level = false;

    if ((s->ctrl & CTRL_TX_IRQ_EN) && (s->intstat & INT_TX_COMPLETE)) {
        level = true;
    }
    if ((s->ctrl & CTRL_RX_IRQ_EN) && (s->intstat & INT_RX_READY)) {
        level = true;
    }

    qemu_set_irq(s->irq, level);
}

static void pbl_uart_update_state(PblSimpleUart *s)
{
    s->state = STATE_TX_READY; /* TX always ready in QEMU */
    if (s->rx_count > 0) {
        s->state |= STATE_RX_AVAIL;
    }
}

static uint64_t pbl_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    PblSimpleUart *s = opaque;

    switch (offset) {
    case UART_DATA:
        if (s->rx_count > 0) {
            uint8_t val = s->rx_fifo[s->rx_head];
            s->rx_head = (s->rx_head + 1) % RX_FIFO_SIZE;
            s->rx_count--;
            pbl_uart_update_state(s);
            return val;
        }
        return 0;

    case UART_STATE:
        pbl_uart_update_state(s);
        return s->state;

    case UART_CTRL:
        return s->ctrl;

    case UART_INTSTAT:
        return s->intstat;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-simple-uart: bad read offset 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void pbl_uart_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    PblSimpleUart *s = opaque;

    switch (offset) {
    case UART_DATA:
    {
        uint8_t ch = (uint8_t)value;
        if (s->write_handler) {
            s->write_handler(s->write_handler_opaque, &ch, 1);
        } else {
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
        }
        /* TX is instant in QEMU — signal completion */
        s->intstat |= INT_TX_COMPLETE;
        pbl_uart_update_irq(s);
        break;
    }

    case UART_CTRL:
        s->ctrl = value & (CTRL_TX_IRQ_EN | CTRL_RX_IRQ_EN);
        pbl_uart_update_irq(s);
        break;

    case UART_INTSTAT:
        /* Write 1 to clear */
        s->intstat &= ~value;
        pbl_uart_update_irq(s);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-simple-uart: bad write offset 0x%" HWADDR_PRIx "\n",
                      offset);
        break;
    }
}

static const MemoryRegionOps pbl_uart_ops = {
    .read = pbl_uart_read,
    .write = pbl_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static int pbl_uart_can_receive(void *opaque)
{
    PblSimpleUart *s = opaque;
    return RX_FIFO_SIZE - s->rx_count;
}

static void pbl_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    PblSimpleUart *s = opaque;

    uint32_t space = RX_FIFO_SIZE - s->rx_count;
    if ((uint32_t)size > space) {
        size = space;
    }

    bool was_empty = (s->rx_count == 0);

    for (int i = 0; i < size; i++) {
        uint32_t tail = (s->rx_head + s->rx_count) % RX_FIFO_SIZE;
        s->rx_fifo[tail] = buf[i];
        s->rx_count++;
    }

    if (size > 0) {
        pbl_uart_update_state(s);
        /* Only raise the interrupt when the FIFO transitions from empty to
         * non-empty.  Avoid re-asserting while the guest ISR is already
         * draining — re-assertion between QEMU translation blocks can
         * confuse the NVIC's active/pending state machine. */
        if (was_empty) {
            s->intstat |= INT_RX_READY;
            pbl_uart_update_irq(s);
        }
    }
}

static void pbl_uart_realize(DeviceState *dev, Error **errp)
{
    PblSimpleUart *s = PEBBLE_SIMPLE_UART(dev);

    qemu_chr_fe_set_handlers(&s->chr, pbl_uart_can_receive,
                              pbl_uart_receive, NULL, NULL,
                              s, NULL, true);
}

static void pbl_uart_init(Object *obj)
{
    PblSimpleUart *s = PEBBLE_SIMPLE_UART(obj);

    memory_region_init_io(&s->iomem, obj, &pbl_uart_ops, s,
                          "pebble-simple-uart", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    s->state = STATE_TX_READY;
}

static void pbl_uart_reset(DeviceState *dev)
{
    PblSimpleUart *s = PEBBLE_SIMPLE_UART(dev);

    s->ctrl = 0;
    s->intstat = 0;
    s->rx_head = 0;
    s->rx_count = 0;
    s->state = STATE_TX_READY;
}

static const Property pbl_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", PblSimpleUart, chr),
};

static void pbl_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pbl_uart_realize;
    device_class_set_legacy_reset(dc, pbl_uart_reset);
    device_class_set_props(dc, pbl_uart_properties);
}

/* === Public API for pebble_control integration === */

void pbl_uart_set_write_handler(DeviceState *dev, void *opaque,
                                PblUartWriteHandler handler)
{
    PblSimpleUart *s = PEBBLE_SIMPLE_UART(dev);
    s->write_handler = handler;
    s->write_handler_opaque = opaque;
}

void pbl_uart_get_rcv_handlers(DeviceState *dev,
                               IOCanReadHandler **can_read,
                               IOReadHandler **read,
                               IOEventHandler **event)
{
    (void)dev;
    *can_read = pbl_uart_can_receive;
    *read = pbl_uart_receive;
    *event = NULL;
}

void pbl_uart_inject_rx(DeviceState *dev, const uint8_t *buf, int size)
{
    PblSimpleUart *s = PEBBLE_SIMPLE_UART(dev);
    pbl_uart_receive(s, buf, size);
}

static const TypeInfo pbl_uart_info = {
    .name          = TYPE_PEBBLE_SIMPLE_UART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PblSimpleUart),
    .instance_init = pbl_uart_init,
    .class_init    = pbl_uart_class_init,
};

static void pbl_uart_register_types(void)
{
    type_register_static(&pbl_uart_info);
}

type_init(pbl_uart_register_types)
