/*
 * G233 GPIO controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_G233_GPIO_H
#define HW_GPIO_G233_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_G233_GPIO "g233-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(G233GPIOState, G233_GPIO)

#define G233_GPIO_PINS 32
#define G233_GPIO_SIZE 0x1000

struct G233GPIOState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq output[G233_GPIO_PINS];

    uint32_t dir;
    uint32_t out;
    uint32_t input;
    uint32_t ie;
    uint32_t status;
    uint32_t trig;
    uint32_t pol;
};

#endif /* HW_GPIO_G233_GPIO_H */
