/*
 * G233 SPI controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/ssi/g233_spi.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define SPI_CR1 0x00
#define SPI_CR2 0x04
#define SPI_SR  0x08
#define SPI_DR  0x0c

#define CR1_SPE    (1U << 0)
#define CR1_MSTR   (1U << 2)
#define CR1_RXNEIE (1U << 6)
#define CR1_TXEIE  (1U << 7)
#define CR1_MASK   (CR1_SPE | CR1_MSTR | CR1_RXNEIE | CR1_TXEIE)

#define SR_RXNE    (1U << 0)
#define SR_TXE     (1U << 1)

static bool g233_spi_enabled(G233SPIState *s)
{
    return (s->cr1 & (CR1_SPE | CR1_MSTR)) == (CR1_SPE | CR1_MSTR);
}

static void g233_spi_update_irq(G233SPIState *s)
{
    bool pending = ((s->cr1 & CR1_TXEIE) && (s->sr & SR_TXE)) ||
                   ((s->cr1 & CR1_RXNEIE) && (s->sr & SR_RXNE));

    qemu_set_irq(s->irq, g233_spi_enabled(s) && pending);
}

static void g233_spi_update_cs(G233SPIState *s)
{
    int new_cs = g233_spi_enabled(s) ? (int)s->cr2 : -1;

    /* Rewriting the selected CS must not split an ongoing transaction. */
    if (new_cs == s->active_cs) {
        return;
    }

    /* End the old transaction before starting the new one. */
    if (s->active_cs >= 0) {
        qemu_set_irq(s->cs[s->active_cs], 1);
    }
    s->active_cs = new_cs;
    if (new_cs >= 0) {
        qemu_set_irq(s->cs[new_cs], 0);
    }
}

static uint64_t g233_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    G233SPIState *s = opaque;

    switch (offset) {
    case SPI_CR1:
        return s->cr1;
    case SPI_CR2:
        return s->cr2;
    case SPI_SR:
        return s->sr;
    case SPI_DR:
        s->sr &= ~SR_RXNE;
        g233_spi_update_irq(s);
        return s->rx_data;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid read at 0x%" HWADDR_PRIx "\n",
                      TYPE_G233_SPI, offset);
        return 0;
    }
}

static void g233_spi_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    G233SPIState *s = opaque;

    switch (offset) {
    case SPI_CR1:
        s->cr1 = value & CR1_MASK;
        g233_spi_update_cs(s);
        break;
    case SPI_CR2:
        s->cr2 = value & (G233_SPI_NUM_CS - 1);
        g233_spi_update_cs(s);
        break;
    case SPI_SR:
        break;
    case SPI_DR:
        if (!g233_spi_enabled(s)) {
            break;
        }

        /* Transfers finish synchronously: TXE remains set. */
        s->rx_data = ssi_transfer(s->spi, value & 0xff);
        s->sr |= SR_RXNE;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid write at 0x%" HWADDR_PRIx "\n",
                      TYPE_G233_SPI, offset);
        return;
    }

    g233_spi_update_irq(s);
}

static const MemoryRegionOps g233_spi_ops = {
    .read = g233_spi_read,
    .write = g233_spi_write,
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

static void g233_spi_reset(DeviceState *dev)
{
    G233SPIState *s = G233_SPI(dev);
    int i;

    s->cr1 = 0;
    s->cr2 = 0;
    s->sr = SR_TXE;
    s->rx_data = 0;
    s->active_cs = -1;
    for (i = 0; i < G233_SPI_NUM_CS; i++) {
        qemu_set_irq(s->cs[i], 1);
    }
    g233_spi_update_irq(s);
}

static int g233_spi_post_load(void *opaque, int version_id)
{
    G233SPIState *s = opaque;
    int i;

    if ((s->cr1 & ~CR1_MASK) || s->cr2 >= G233_SPI_NUM_CS ||
        (s->sr & ~(SR_TXE | SR_RXNE)) || !(s->sr & SR_TXE)) {
        return -EINVAL;
    }

    s->active_cs = g233_spi_enabled(s) ? (int)s->cr2 : -1;
    /* Restore levels without pulsing the selected peripheral's CS. */
    for (i = 0; i < G233_SPI_NUM_CS; i++) {
        qemu_set_irq(s->cs[i], i != s->active_cs);
    }
    g233_spi_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_g233_spi = {
    .name = TYPE_G233_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = g233_spi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr1, G233SPIState),
        VMSTATE_UINT32(cr2, G233SPIState),
        VMSTATE_UINT32(sr, G233SPIState),
        VMSTATE_UINT8(rx_data, G233SPIState),
        VMSTATE_END_OF_LIST()
    },
};

static void g233_spi_init(Object *obj)
{
    G233SPIState *s = G233_SPI(obj);
    DeviceState *dev = DEVICE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &g233_spi_ops, s,
                          TYPE_G233_SPI, 0x1000);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(dev, s->cs, G233_SPI_CS, G233_SPI_NUM_CS);
    s->spi = ssi_create_bus(dev, "spi");
    s->active_cs = -1;
}

static void g233_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, g233_spi_reset);
    dc->vmsd = &vmstate_g233_spi;
}

static const TypeInfo g233_spi_info = {
    .name = TYPE_G233_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233SPIState),
    .instance_init = g233_spi_init,
    .class_init = g233_spi_class_init,
};

static void g233_spi_register_types(void)
{
    type_register_static(&g233_spi_info);
}

type_init(g233_spi_register_types)
