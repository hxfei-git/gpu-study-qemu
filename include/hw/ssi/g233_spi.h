/*
 * G233 SPI controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_G233_SPI_H
#define HW_SSI_G233_SPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_G233_SPI "g233-spi"
OBJECT_DECLARE_SIMPLE_TYPE(G233SPIState, G233_SPI)

#define G233_SPI_NUM_CS 4
#define G233_SPI_CS "cs"

struct G233SPIState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq cs[G233_SPI_NUM_CS];
    SSIBus *spi;

    uint32_t cr1;
    uint32_t cr2;
    uint32_t sr;
    uint8_t rx_data;

    /* Derived output state, reconstructed from CR1 and CR2 on migration. */
    int active_cs;
};

#endif /* HW_SSI_G233_SPI_H */
