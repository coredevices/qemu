/*
 * Pebble Audio DAC
 *
 * Audio output device for Pebble generic machines.
 * Accepts PCM sample writes via MMIO DATA register, buffers them in a ring
 * buffer, and drains to host audio via QEMU's audio subsystem.
 *
 * Registers (0x1000 region):
 *   0x00 CTRL       - Bit 0: enable
 *   0x04 STATUS     - Bit 0: FIFO ready (always 1)
 *   0x08 SAMPLERATE - Sample rate in Hz
 *   0x0C DATA       - Write: push 16-bit PCM sample
 *   0x10 INTCTRL    - Bit 0: buffer-available IRQ enable
 *   0x14 INTSTAT    - Bit 0: buffer-available IRQ pending (write 1 to clear)
 *   0x18 BUFAVAIL   - Read: number of free samples in ring buffer
 *   0x1C VOLUME     - Volume 0-100
 *
 * Copyright (c) 2026 Core Devices LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "audio/audio.h"

#define TYPE_PEBBLE_AUDIO "pebble-audio"
OBJECT_DECLARE_SIMPLE_TYPE(PblAudio, PEBBLE_AUDIO)

/* Register offsets */
#define AUDIO_CTRL       0x00
#define AUDIO_STATUS     0x04
#define AUDIO_SAMPLERATE 0x08
#define AUDIO_DATA       0x0C
#define AUDIO_INTCTRL    0x10
#define AUDIO_INTSTAT    0x14
#define AUDIO_BUFAVAIL   0x18
#define AUDIO_VOLUME     0x1C

/* Interrupt bits */
#define INT_BUFAVAIL     (1 << 0)

/* Ring buffer size in samples (int16_t) */
#define RING_BUF_SAMPLES 4096

/* Drain timer interval: 10ms */
#define DRAIN_INTERVAL_NS (10 * 1000 * 1000)

struct PblAudio {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    /* Registers */
    uint32_t ctrl;
    uint32_t samplerate;
    uint32_t intctrl;
    uint32_t intstat;
    uint32_t volume;

    /* Audio subsystem */
    QEMUSoundCard card;
    SWVoiceOut *voice;

    /* Ring buffer of int16_t samples */
    int16_t ring_buf[RING_BUF_SAMPLES];
    uint32_t ring_wr;  /* write index */
    uint32_t ring_rd;  /* read index */
    uint32_t ring_count; /* number of samples buffered */

    /* Drain timer */
    QEMUTimer *drain_timer;
    bool running;
};

static void pbl_audio_update_irq(PblAudio *s)
{
    bool level = (s->intstat & s->intctrl) != 0;
    qemu_set_irq(s->irq, level);
}

static uint32_t pbl_audio_ring_free(PblAudio *s)
{
    return RING_BUF_SAMPLES - s->ring_count;
}

static void pbl_audio_ring_push(PblAudio *s, int16_t sample)
{
    if (s->ring_count < RING_BUF_SAMPLES) {
        s->ring_buf[s->ring_wr] = sample;
        s->ring_wr = (s->ring_wr + 1) % RING_BUF_SAMPLES;
        s->ring_count++;
    }
}

static void pbl_audio_drain_timer(void *opaque)
{
    PblAudio *s = opaque;

    if (!s->running) {
        return;
    }

    /* Drain buffered samples to host audio */
    if (s->voice && s->ring_count > 0) {
        /*
         * We need to write samples as bytes to AUD_write.
         * Write from ring_rd up to contiguous end, then wrap.
         */
        while (s->ring_count > 0) {
            uint32_t contiguous;
            if (s->ring_rd + s->ring_count > RING_BUF_SAMPLES) {
                contiguous = RING_BUF_SAMPLES - s->ring_rd;
            } else {
                contiguous = s->ring_count;
            }

            int written_bytes = AUD_write(s->voice,
                                          &s->ring_buf[s->ring_rd],
                                          contiguous * sizeof(int16_t));
            uint32_t written_samples = written_bytes / sizeof(int16_t);
            if (written_samples == 0) {
                break;  /* host audio backend can't accept more right now */
            }
            s->ring_rd = (s->ring_rd + written_samples) % RING_BUF_SAMPLES;
            s->ring_count -= written_samples;
        }
    }

    /* Fire IRQ to request more data from firmware */
    s->intstat |= INT_BUFAVAIL;
    pbl_audio_update_irq(s);

    /* Reschedule */
    timer_mod_anticipate_ns(s->drain_timer,
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                            + DRAIN_INTERVAL_NS);
}

static void pbl_audio_out_cb(void *opaque, int free)
{
    /* Intentionally empty — we use timer-based draining instead */
    (void)opaque;
    (void)free;
}

static void pbl_audio_start(PblAudio *s)
{
    if (s->running) {
        return;
    }

    struct audsettings as = {
        .freq = s->samplerate ? s->samplerate : 16000,
        .nchannels = 1,
        .fmt = AUDIO_FORMAT_S16,
        .endianness = 0, /* little-endian */
    };

    s->voice = AUD_open_out(&s->card, s->voice, "pebble-audio",
                            s, pbl_audio_out_cb, &as);
    if (!s->voice) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-audio: failed to open audio output\n");
        return;
    }

    AUD_set_active_out(s->voice, 1);

    /* Reset ring buffer */
    s->ring_wr = 0;
    s->ring_rd = 0;
    s->ring_count = 0;
    s->running = true;

    /* Fire initial IRQ so firmware starts writing samples */
    s->intstat |= INT_BUFAVAIL;
    pbl_audio_update_irq(s);

    /* Start drain timer */
    timer_mod_anticipate_ns(s->drain_timer,
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                            + DRAIN_INTERVAL_NS);
}

static void pbl_audio_stop(PblAudio *s)
{
    if (!s->running) {
        return;
    }

    s->running = false;
    timer_del(s->drain_timer);

    if (s->voice) {
        AUD_set_active_out(s->voice, 0);
        AUD_close_out(&s->card, s->voice);
        s->voice = NULL;
    }

    s->ring_wr = 0;
    s->ring_rd = 0;
    s->ring_count = 0;
}

static uint64_t pbl_audio_read(void *opaque, hwaddr offset, unsigned size)
{
    PblAudio *s = opaque;

    switch (offset) {
    case AUDIO_CTRL:
        return s->ctrl;
    case AUDIO_STATUS:
        return 1;  /* FIFO always ready */
    case AUDIO_SAMPLERATE:
        return s->samplerate;
    case AUDIO_INTCTRL:
        return s->intctrl;
    case AUDIO_INTSTAT:
        return s->intstat;
    case AUDIO_BUFAVAIL:
        return pbl_audio_ring_free(s);
    case AUDIO_VOLUME:
        return s->volume;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-audio: bad read offset 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void pbl_audio_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    PblAudio *s = opaque;

    switch (offset) {
    case AUDIO_CTRL: {
        uint32_t old = s->ctrl;
        s->ctrl = value & 1;
        if ((s->ctrl & 1) && !(old & 1)) {
            pbl_audio_start(s);
        } else if (!(s->ctrl & 1) && (old & 1)) {
            pbl_audio_stop(s);
        }
        break;
    }
    case AUDIO_SAMPLERATE:
        s->samplerate = value;
        break;
    case AUDIO_DATA:
        if (s->running) {
            pbl_audio_ring_push(s, (int16_t)(value & 0xFFFF));
        }
        break;
    case AUDIO_INTCTRL:
        s->intctrl = value & INT_BUFAVAIL;
        pbl_audio_update_irq(s);
        break;
    case AUDIO_INTSTAT:
        s->intstat &= ~value;
        pbl_audio_update_irq(s);
        break;
    case AUDIO_VOLUME:
        s->volume = value > 100 ? 100 : value;
        /* Apply volume to the voice if active */
        if (s->voice) {
            /* QEMU volume is 0-255, scale from 0-100 */
            int vol = (s->volume * 255) / 100;
            AUD_set_volume_out(s->voice, 0, vol, vol);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pebble-audio: bad write offset 0x%" HWADDR_PRIx "\n",
                      offset);
        break;
    }
}

static const MemoryRegionOps pbl_audio_ops = {
    .read = pbl_audio_read,
    .write = pbl_audio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void pbl_audio_init(Object *obj)
{
    PblAudio *s = PEBBLE_AUDIO(obj);

    memory_region_init_io(&s->iomem, obj, &pbl_audio_ops, s,
                          "pebble-audio", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void pbl_audio_realize(DeviceState *dev, Error **errp)
{
    PblAudio *s = PEBBLE_AUDIO(dev);

    if (!AUD_register_card("pebble-audio", &s->card, errp)) {
        return;
    }

    s->drain_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  pbl_audio_drain_timer, s);
}

static void pbl_audio_reset(DeviceState *dev)
{
    PblAudio *s = PEBBLE_AUDIO(dev);

    if (s->running) {
        pbl_audio_stop(s);
    }

    s->ctrl = 0;
    s->samplerate = 16000;
    s->intctrl = 0;
    s->intstat = 0;
    s->volume = 100;
}

static const Property pbl_audio_properties[] = {
    DEFINE_AUDIO_PROPERTIES(PblAudio, card),
};

static void pbl_audio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, pbl_audio_reset);
    dc->realize = pbl_audio_realize;
    device_class_set_props(dc, pbl_audio_properties);
}

static const TypeInfo pbl_audio_info = {
    .name          = TYPE_PEBBLE_AUDIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PblAudio),
    .instance_init = pbl_audio_init,
    .class_init    = pbl_audio_class_init,
};

static void pbl_audio_register_types(void)
{
    type_register_static(&pbl_audio_info);
}

type_init(pbl_audio_register_types)
