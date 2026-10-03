/*
 * G233 watchdog timer
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/watchdog/g233_wdt.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/watchdog.h"

#define WDT_CTRL_EN      (1U << 0)
#define WDT_CTRL_INTEN   (1U << 1)
#define WDT_CTRL_RSTEN   (1U << 2)
#define WDT_CTRL_LOCK    (1U << 3)
#define WDT_CTRL_MASK    0x7
#define WDT_TIMEOUT      (1U << 0)
#define WDT_KEY_FEED     0x5a5a5a5a
#define WDT_KEY_LOCK     0x1acce551

/* The camp specification has no clock register; use a fixed 1 MHz clock. */
#define WDT_TICK_NS 1000

static void g233_wdt_update_irq(G233WDTState *s)
{
    qemu_set_irq(s->irq, (s->ctrl & WDT_CTRL_INTEN) &&
                 (s->status & WDT_TIMEOUT));
}

static void g233_wdt_sync(G233WDTState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ticks;

    if (!s->active) {
        return;
    }
    ticks = (now - s->last_ns) / WDT_TICK_NS;
    if (ticks < s->value) {
        s->value -= ticks;
        s->last_ns += ticks * WDT_TICK_NS;
        return;
    }

    s->value = 0;
    s->active = false;
    s->status |= WDT_TIMEOUT;
    timer_del(s->timer);
    g233_wdt_update_irq(s);
    if (s->ctrl & WDT_CTRL_RSTEN) {
        watchdog_perform_action();
    }
}

static void g233_wdt_schedule(G233WDTState *s)
{
    timer_del(s->timer);
    if (s->active) {
        timer_mod(s->timer, s->last_ns + (uint64_t)s->value * WDT_TICK_NS);
    }
}

static void g233_wdt_tick(void *opaque)
{
    G233WDTState *s = opaque;

    g233_wdt_sync(s);
    g233_wdt_schedule(s);
}

static uint64_t g233_wdt_read(void *opaque, hwaddr offset, unsigned size)
{
    G233WDTState *s = opaque;

    g233_wdt_sync(s);
    switch (offset) {
    case 0:
        return s->ctrl;
    case 4:
        return s->load;
    case 8:
        return s->value;
    case 0xc:
    case 0x10:
        /*
         * The published datasheet swaps SR/KEY relative to the camp tests.
         * Alias the status reads to let software use either register layout.
         */
        return s->status;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233-wdt: invalid read at 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void g233_wdt_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    G233WDTState *s = opaque;
    uint32_t old_ctrl;

    g233_wdt_sync(s);
    switch (offset) {
    case 0:
        if (s->ctrl & WDT_CTRL_LOCK) {
            return;
        }
        old_ctrl = s->ctrl;
        s->ctrl = value & WDT_CTRL_MASK;
        if (!(s->ctrl & WDT_CTRL_EN)) {
            s->active = false;
        } else if (!(old_ctrl & WDT_CTRL_EN)) {
            s->value = s->load;
            s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->active = true;
        }
        break;
    case 4:
        /* Changing LOAD affects the next enable/feed, not the running count. */
        s->load = value;
        if (!(s->ctrl & WDT_CTRL_EN)) {
            s->value = s->load;
        }
        break;
    case 8:
        /* Current value is read-only. */
        return;
    case 0xc:
    case 0x10:
        /* Accept both the camp-test and datasheet SR/KEY offsets. */
        switch ((uint32_t)value) {
        case WDT_KEY_FEED:
            s->value = s->load;
            s->status &= ~WDT_TIMEOUT;
            s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->active = !!(s->ctrl & WDT_CTRL_EN);
            break;
        case WDT_KEY_LOCK:
            s->ctrl |= WDT_CTRL_LOCK;
            break;
        default:
            s->status &= ~(value & WDT_TIMEOUT);
            break;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "g233-wdt: invalid write at 0x%" HWADDR_PRIx "\n",
                      offset);
        return;
    }
    g233_wdt_update_irq(s);
    g233_wdt_schedule(s);
}

static const MemoryRegionOps g233_wdt_ops = {
    .read = g233_wdt_read,
    .write = g233_wdt_write,
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

static void g233_wdt_reset(DeviceState *dev)
{
    G233WDTState *s = G233_WDT(dev);

    timer_del(s->timer);
    s->ctrl = 0;
    s->load = 0xffff;
    s->value = 0xffff;
    s->status = 0;
    s->active = false;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    g233_wdt_update_irq(s);
}

static int g233_wdt_pre_save(void *opaque)
{
    g233_wdt_sync(opaque);
    return 0;
}

static int g233_wdt_post_load(void *opaque, int version_id)
{
    G233WDTState *s = opaque;

    g233_wdt_update_irq(s);
    g233_wdt_schedule(s);
    return 0;
}

static const VMStateDescription vmstate_g233_wdt = {
    .name = TYPE_G233_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = g233_wdt_pre_save,
    .post_load = g233_wdt_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, G233WDTState),
        VMSTATE_UINT32(load, G233WDTState),
        VMSTATE_UINT32(value, G233WDTState),
        VMSTATE_UINT32(status, G233WDTState),
        VMSTATE_INT64(last_ns, G233WDTState),
        VMSTATE_BOOL(active, G233WDTState),
        VMSTATE_TIMER_PTR(timer, G233WDTState),
        VMSTATE_END_OF_LIST()
    },
};

static void g233_wdt_init(Object *obj)
{
    G233WDTState *s = G233_WDT(obj);

    memory_region_init_io(&s->mmio, obj, &g233_wdt_ops, s,
                          TYPE_G233_WDT, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, g233_wdt_tick, s);
}

static void g233_wdt_finalize(Object *obj)
{
    G233WDTState *s = G233_WDT(obj);

    timer_free(s->timer);
}

static void g233_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "G233 watchdog timer";
    dc->vmsd = &vmstate_g233_wdt;
    set_bit(DEVICE_CATEGORY_WATCHDOG, dc->categories);
    device_class_set_legacy_reset(dc, g233_wdt_reset);
}

static const TypeInfo g233_wdt_info = {
    .name = TYPE_G233_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233WDTState),
    .instance_init = g233_wdt_init,
    .instance_finalize = g233_wdt_finalize,
    .class_init = g233_wdt_class_init,
};

static void g233_wdt_register_types(void)
{
    type_register_static(&g233_wdt_info);
}

type_init(g233_wdt_register_types)
