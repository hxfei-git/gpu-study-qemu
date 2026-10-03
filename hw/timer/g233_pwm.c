/*
 * G233 four-channel PWM controller
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/timer/g233_pwm.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define PWM_CTRL_EN      (1U << 0)
#define PWM_CTRL_POL     (1U << 1)
#define PWM_CTRL_INTIE   (1U << 2)
#define PWM_CTRL_MASK    0x7

/* The camp specification has no clock register; use a fixed 1 MHz clock. */
#define PWM_TICK_NS 1000

static void g233_pwm_update_irq(G233PWMState *s)
{
    bool pending = false;

    for (unsigned i = 0; i < G233_PWM_CHANNELS; i++) {
        pending |= (s->done & (1U << i)) &&
                   (s->channel[i].ctrl & PWM_CTRL_INTIE);
    }
    qemu_set_irq(s->irq, pending);
}

static void g233_pwm_update_output(G233PWMChannel *ch)
{
    bool level = false;

    if (ch->ctrl & PWM_CTRL_EN) {
        level = (ch->count < ch->duty) ^ !!(ch->ctrl & PWM_CTRL_POL);
    }
    qemu_set_irq(ch->controller->output[ch->index], level);
}

static void g233_pwm_sync_channel(G233PWMChannel *ch, int64_t now)
{
    uint64_t ticks;
    uint64_t count;
    uint64_t cycle = (uint64_t)ch->period + 1;

    if (!(ch->ctrl & PWM_CTRL_EN)) {
        return;
    }

    ticks = (now - ch->last_ns) / PWM_TICK_NS;
    count = (uint64_t)ch->count + ticks;
    if (count >= cycle) {
        ch->controller->done |= 1U << ch->index;
    }
    ch->count = count % cycle;
    ch->last_ns += ticks * PWM_TICK_NS;
    g233_pwm_update_output(ch);
}

static void g233_pwm_schedule(G233PWMChannel *ch)
{
    uint64_t cycle = (uint64_t)ch->period + 1;
    uint64_t ticks = cycle - ch->count;
    bool waveform = ch->duty && ch->duty < cycle;

    timer_del(ch->timer);
    if (!(ch->ctrl & PWM_CTRL_EN)) {
        return;
    }

    /* Constant output needs no further event while DONE is already latched. */
    if (!waveform && (ch->controller->done & (1U << ch->index))) {
        return;
    }
    if (ch->count < ch->duty && ch->duty < cycle) {
        ticks = MIN(ticks, (uint64_t)ch->duty - ch->count);
    }
    timer_mod(ch->timer, ch->last_ns + ticks * PWM_TICK_NS);
}

static void g233_pwm_tick(void *opaque)
{
    G233PWMChannel *ch = opaque;

    /* Compute elapsed cycles in one step, including large qtest clock jumps. */
    g233_pwm_sync_channel(ch, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    g233_pwm_update_irq(ch->controller);
    g233_pwm_schedule(ch);
}

static void g233_pwm_sync(G233PWMState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (unsigned i = 0; i < G233_PWM_CHANNELS; i++) {
        g233_pwm_sync_channel(&s->channel[i], now);
    }
    g233_pwm_update_irq(s);
}

static uint64_t g233_pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    G233PWMState *s = opaque;
    G233PWMChannel *ch;
    uint32_t value;

    g233_pwm_sync(s);
    if (offset == 0) {
        value = s->done << 4;
        for (unsigned i = 0; i < G233_PWM_CHANNELS; i++) {
            if (s->channel[i].ctrl & PWM_CTRL_EN) {
                value |= 1U << i;
            }
        }
        return value;
    }
    if (offset < 0x10 || offset >= 0x50) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233-pwm: invalid read at 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
    ch = &s->channel[(offset - 0x10) / 0x10];
    switch (offset & 0xf) {
    case 0:
        return ch->ctrl;
    case 4:
        return ch->period;
    case 8:
        return ch->duty;
    case 0xc:
        return ch->count;
    default:
        g_assert_not_reached();
    }
}

static void g233_pwm_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    G233PWMState *s = opaque;
    G233PWMChannel *ch;
    uint32_t old_ctrl;

    g233_pwm_sync(s);
    if (offset == 0) {
        s->done &= ~((value >> 4) & 0xf);
        for (unsigned i = 0; i < G233_PWM_CHANNELS; i++) {
            g233_pwm_schedule(&s->channel[i]);
        }
        g233_pwm_update_irq(s);
        return;
    }
    if (offset < 0x10 || offset >= 0x50) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233-pwm: invalid write at 0x%" HWADDR_PRIx "\n",
                      offset);
        return;
    }
    ch = &s->channel[(offset - 0x10) / 0x10];
    switch (offset & 0xf) {
    case 0:
        old_ctrl = ch->ctrl;
        ch->ctrl = value & PWM_CTRL_MASK;
        if (!(old_ctrl & PWM_CTRL_EN) && (ch->ctrl & PWM_CTRL_EN)) {
            ch->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        break;
    case 4:
        ch->period = value;
        ch->count %= (uint64_t)ch->period + 1;
        break;
    case 8:
        ch->duty = value;
        break;
    case 0xc:
        /* The current counter is read-only. */
        return;
    default:
        g_assert_not_reached();
    }
    g233_pwm_update_output(ch);
    g233_pwm_schedule(ch);
    g233_pwm_update_irq(s);
}

static const MemoryRegionOps g233_pwm_ops = {
    .read = g233_pwm_read,
    .write = g233_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void g233_pwm_reset(DeviceState *dev)
{
    G233PWMState *s = G233_PWM(dev);

    s->done = 0;
    for (unsigned i = 0; i < G233_PWM_CHANNELS; i++) {
        G233PWMChannel *ch = &s->channel[i];

        timer_del(ch->timer);
        ch->ctrl = 0;
        ch->period = 0;
        ch->duty = 0;
        ch->count = 0;
        ch->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        g233_pwm_update_output(ch);
    }
    g233_pwm_update_irq(s);
}

static int g233_pwm_pre_save(void *opaque)
{
    g233_pwm_sync(opaque);
    return 0;
}

static int g233_pwm_post_load(void *opaque, int version_id)
{
    G233PWMState *s = opaque;

    for (unsigned i = 0; i < G233_PWM_CHANNELS; i++) {
        g233_pwm_update_output(&s->channel[i]);
        g233_pwm_schedule(&s->channel[i]);
    }
    g233_pwm_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_g233_pwm_channel = {
    .name = "g233-pwm/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, G233PWMChannel),
        VMSTATE_UINT32(period, G233PWMChannel),
        VMSTATE_UINT32(duty, G233PWMChannel),
        VMSTATE_UINT32(count, G233PWMChannel),
        VMSTATE_INT64(last_ns, G233PWMChannel),
        VMSTATE_TIMER_PTR(timer, G233PWMChannel),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_g233_pwm = {
    .name = TYPE_G233_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = g233_pwm_pre_save,
    .post_load = g233_pwm_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(done, G233PWMState),
        VMSTATE_STRUCT_ARRAY(channel, G233PWMState, G233_PWM_CHANNELS, 0,
                             vmstate_g233_pwm_channel, G233PWMChannel),
        VMSTATE_END_OF_LIST()
    },
};

static void g233_pwm_init(Object *obj)
{
    G233PWMState *s = G233_PWM(obj);

    memory_region_init_io(&s->mmio, obj, &g233_pwm_ops, s,
                          TYPE_G233_PWM, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), s->output, "pwm", G233_PWM_CHANNELS);
    for (unsigned i = 0; i < G233_PWM_CHANNELS; i++) {
        G233PWMChannel *ch = &s->channel[i];

        ch->controller = s;
        ch->index = i;
        ch->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, g233_pwm_tick, ch);
    }
}

static void g233_pwm_finalize(Object *obj)
{
    G233PWMState *s = G233_PWM(obj);

    for (unsigned i = 0; i < G233_PWM_CHANNELS; i++) {
        timer_free(s->channel[i].timer);
    }
}

static void g233_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "G233 four-channel PWM controller";
    dc->vmsd = &vmstate_g233_pwm;
    device_class_set_legacy_reset(dc, g233_pwm_reset);
}

static const TypeInfo g233_pwm_info = {
    .name = TYPE_G233_PWM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233PWMState),
    .instance_init = g233_pwm_init,
    .instance_finalize = g233_pwm_finalize,
    .class_init = g233_pwm_class_init,
};

static void g233_pwm_register_types(void)
{
    type_register_static(&g233_pwm_info);
}

type_init(g233_pwm_register_types)
