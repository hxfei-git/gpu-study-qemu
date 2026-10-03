/*
 * G233 GPIO controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/gpio/g233_gpio.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

enum {
    GPIO_DIR = 0x00,
    GPIO_OUT = 0x04,
    GPIO_IN = 0x08,
    GPIO_IE = 0x0c,
    GPIO_IS = 0x10,
    GPIO_TRIG = 0x14,
    GPIO_POL = 0x18,
};

static uint32_t g233_gpio_input(G233GPIOState *s)
{
    return (s->out & s->dir) | (s->input & ~s->dir);
}

static void g233_gpio_update_outputs(G233GPIOState *s)
{
    uint32_t output = s->dir & s->out;

    for (unsigned int i = 0; i < G233_GPIO_PINS; i++) {
        qemu_set_irq(s->output[i], (output >> i) & 1);
    }
}

static uint64_t g233_gpio_read(void *opaque, hwaddr offset, unsigned int size)
{
    G233GPIOState *s = G233_GPIO(opaque);

    switch (offset) {
    case GPIO_DIR:
        return s->dir;
    case GPIO_OUT:
        return s->out;
    case GPIO_IN:
        return g233_gpio_input(s);
    case GPIO_IE:
    case GPIO_IS:
    case GPIO_TRIG:
    case GPIO_POL:
        /* Reserved until interrupt support is enabled. */
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void g233_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned int size)
{
    G233GPIOState *s = G233_GPIO(opaque);

    switch (offset) {
    case GPIO_DIR:
        s->dir = value;
        break;
    case GPIO_OUT:
        s->out = value;
        break;
    case GPIO_IN:
        /* The input register is read-only. */
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    if (offset == GPIO_DIR || offset == GPIO_OUT) {
        g233_gpio_update_outputs(s);
    }
}

static const MemoryRegionOps g233_gpio_ops = {
    .read = g233_gpio_read,
    .write = g233_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void g233_gpio_set_input(void *opaque, int line, int level)
{
    G233GPIOState *s = G233_GPIO(opaque);
    uint32_t mask = 1U << line;

    if (level > 0) {
        s->input |= mask;
    } else {
        s->input &= ~mask;
    }
}

static void g233_gpio_reset_hold(Object *obj, ResetType type)
{
    G233GPIOState *s = G233_GPIO(obj);

    s->dir = 0;
    s->out = 0;
    s->input = 0;

    g233_gpio_update_outputs(s);
}

static int g233_gpio_post_load(void *opaque, int version_id)
{
    G233GPIOState *s = G233_GPIO(opaque);

    g233_gpio_update_outputs(s);
    return 0;
}

static const VMStateDescription vmstate_g233_gpio = {
    .name = TYPE_G233_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = g233_gpio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dir, G233GPIOState),
        VMSTATE_UINT32(out, G233GPIOState),
        VMSTATE_UINT32(input, G233GPIOState),
        VMSTATE_END_OF_LIST()
    },
};

static void g233_gpio_init(Object *obj)
{
    G233GPIOState *s = G233_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &g233_gpio_ops, s,
                          TYPE_G233_GPIO, G233_GPIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    qdev_init_gpio_in(DEVICE(obj), g233_gpio_set_input, G233_GPIO_PINS);
    qdev_init_gpio_out_named(DEVICE(obj), s->output, "gpio", G233_GPIO_PINS);
}

static void g233_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "G233 GPIO controller";
    dc->vmsd = &vmstate_g233_gpio;
    rc->phases.hold = g233_gpio_reset_hold;
}

static const TypeInfo g233_gpio_info = {
    .name = TYPE_G233_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233GPIOState),
    .instance_init = g233_gpio_init,
    .class_init = g233_gpio_class_init,
};

static void g233_gpio_register_types(void)
{
    type_register_static(&g233_gpio_info);
}

type_init(g233_gpio_register_types)
