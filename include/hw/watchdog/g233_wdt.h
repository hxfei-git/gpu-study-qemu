/*
 * G233 watchdog timer
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_WATCHDOG_G233_WDT_H
#define HW_WATCHDOG_G233_WDT_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_G233_WDT "g233-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(G233WDTState, G233_WDT)

struct G233WDTState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    QEMUTimer *timer;
    uint32_t ctrl;
    uint32_t load;
    uint32_t value;
    uint32_t status;
    int64_t last_ns;
    bool active;
};

#endif /* HW_WATCHDOG_G233_WDT_H */
