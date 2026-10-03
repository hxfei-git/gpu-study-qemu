/*
 * G233 PWM controller
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_G233_PWM_H
#define HW_TIMER_G233_PWM_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_G233_PWM "g233-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(G233PWMState, G233_PWM)

#define G233_PWM_CHANNELS 4

typedef struct G233PWMChannel {
    G233PWMState *controller;
    QEMUTimer *timer;
    unsigned index;
    uint32_t ctrl;
    uint32_t period;
    uint32_t duty;
    uint32_t count;
    int64_t last_ns;
} G233PWMChannel;

struct G233PWMState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq output[G233_PWM_CHANNELS];
    G233PWMChannel channel[G233_PWM_CHANNELS];
    uint32_t done;
};

#endif /* HW_TIMER_G233_PWM_H */
