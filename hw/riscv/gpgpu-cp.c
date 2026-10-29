/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * 远端 GPGPU 与原生 RV64 命令处理器
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/pci/pci_host.h"
#include "hw/remote/machine.h"
#include "hw/gpgpu/gpgpu.h"
#include "hw/gpgpu/bar0.h"
#include "hw/gpgpu/cp/cp_ctrl.h"
#include "hw/gpgpu/cp/cp_window.h"
#include "hw/gpgpu/cp/hqd.h"
#include "hw/gpgpu/mem/vram.h"
#include "target/riscv/cpu.h"
#include "system/cpus.h"
#include "system/runstate.h"
#include "system/tcg.h"

#define TYPE_GPGPU_CP_MACHINE MACHINE_TYPE_NAME("gpgpu-cp")
OBJECT_DECLARE_SIMPLE_TYPE(GPGPUCPMachineState, GPGPU_CP_MACHINE)

struct GPGPUCPMachineState {
    RemoteMachineState parent_obj;
    RISCVCPU cp;
    GPGPUState *gpgpu;
    MemoryRegion cp_memory;
    MemoryRegion vram_alias;
    MemoryRegion host_window;
    MemoryRegion host_window_regs;
    MemoryRegion hqd_regs;
    MemoryRegion dispatch_regs;
    bool reset_asserted;
    VMChangeStateEntry *vm_state;
};

typedef struct GPGPUCPStateChange {
    GPGPUCPMachineState *machine;
    bool reset_hold;
    uint32_t reset_vector;
} GPGPUCPStateChange;

static void gpgpu_cp_set_state_on_cpu(CPUState *cs, run_on_cpu_data data)
{
    GPGPUCPStateChange *change = data.host_ptr;
    GPGPUCPMachineState *s = change->machine;

    if (change->reset_hold) {
        /* 原生复位恢复 CPU 状态和复位态 PC。 */
        if (s->reset_asserted) {
            resettable_release_reset(OBJECT(cs), RESET_TYPE_COLD);
        }
        s->cp.env.resetvec = change->reset_vector;
        resettable_assert_reset(OBJECT(cs), RESET_TYPE_COLD);
        gpgpu_cp_on_cpu_reset(s->gpgpu);
        s->reset_asserted = true;
        cs->halted = true;
        /* 暂停取指，防止复位前残留的 pending IRQ 唤醒 CPU。 */
        cpu_pause(cs);
    } else {
        resettable_release_reset(OBJECT(cs), RESET_TYPE_COLD);
        s->reset_asserted = false;
        cs->halted = false;
        if (runstate_is_running()) {
            cpu_resume(cs);
        }
    }
}

static void gpgpu_cp_keep_paused(CPUState *cs, run_on_cpu_data data)
{
    GPGPUCPMachineState *s = data.host_ptr;

    if (s->reset_asserted) {
        cpu_pause(cs);
    }
}

static void gpgpu_cp_vm_state_change(void *opaque, bool running, RunState state)
{
    GPGPUCPMachineState *s = opaque;

    if (running) {
        /* 全局 resume 后，CPU 先确认复位保持。 */
        async_run_on_cpu(CPU(&s->cp), gpgpu_cp_keep_paused,
                         RUN_ON_CPU_HOST_PTR(s));
    }
}

static void gpgpu_cp_set_state(void *opaque, bool reset_hold,
                              uint32_t reset_vector)
{
    GPGPUCPMachineState *s = opaque;
    GPGPUCPStateChange change = {
        .machine = s,
        .reset_hold = reset_hold,
        .reset_vector = reset_vector,
    };

    /* 同步完成后，Host 读回可确认复位。 */
    run_on_cpu(CPU(&s->cp), gpgpu_cp_set_state_on_cpu,
               RUN_ON_CPU_HOST_PTR(&change));
    if (reset_hold) {
        /* 系统复位随后可能 resume，再确认保持。 */
        async_run_on_cpu(CPU(&s->cp), gpgpu_cp_keep_paused,
                         RUN_ON_CPU_HOST_PTR(s));
    }
}

static void gpgpu_cp_machine_init(MachineState *machine)
{
    GPGPUCPMachineState *s = GPGPU_CP_MACHINE(machine);
    MachineClass *parent = MACHINE_CLASS(
        object_class_by_name(TYPE_REMOTE_MACHINE));
    RemoteMachineState *remote = REMOTE_MACHINE(machine);
    PCIHostState *pci_host;
    DeviceState *gpu;
    GPGPUState *gpgpu;

    if (!tcg_enabled()) {
        error_report("gpgpu-cp requires the TCG accelerator");
        exit(EXIT_FAILURE);
    }
    if (!remote->vfio_user) {
        error_report("gpgpu-cp requires vfio-user=on");
        exit(EXIT_FAILURE);
    }

    parent->init(machine);
    pci_host = PCI_HOST_BRIDGE(remote->host);
    gpu = qdev_new(TYPE_GPGPU);
    gpu->id = g_strdup("gpu0");
    object_property_add_child(OBJECT(machine), "gpu0", OBJECT(gpu));
    qdev_realize_and_unref(gpu, BUS(pci_host->bus), &error_fatal);
    gpgpu = GPGPU(gpu);
    s->gpgpu = gpgpu;

    /* RV64 CP 仍使用原有 4 GiB 物理布局；容器不复制 VRAM。 */
    memory_region_init(&s->cp_memory, OBJECT(machine), "gpgpu-cp-memory",
                       UINT64_C(1) << 32);
    memory_region_init_alias(&s->vram_alias, OBJECT(machine), "gpgpu-cp-vram",
                             gpgpu_vram_region(gpgpu), 0, gpgpu_vram_size(gpgpu));
    memory_region_add_subregion(&s->cp_memory, 0, &s->vram_alias);
    memory_region_init_io(&s->host_window, OBJECT(machine), &gpgpu_cp_window_ops,
                          gpgpu, "gpgpu-cp-host-window", GPGPU_CP_WINDOW_SIZE);
    memory_region_add_subregion(&s->cp_memory, GPGPU_CP_WINDOW_BASE,
                                &s->host_window);
    memory_region_init_io(&s->host_window_regs, OBJECT(machine),
                          &gpgpu_cp_window_regs_ops, gpgpu,
                          "gpgpu-cp-host-window-regs", GPGPU_CP_WINDOW_REGS_SIZE);
    memory_region_add_subregion(&s->cp_memory, GPGPU_CP_WINDOW_REGS,
                                &s->host_window_regs);

    memory_region_init_io(&s->hqd_regs, OBJECT(machine), &gpgpu_cp_hqd_ops,
                          gpgpu, "gpgpu-cp-hqd", GPGPU_HQD_SLOTS * GPGPU_HQD_STRIDE);
    memory_region_add_subregion(&s->cp_memory, GPGPU_CP_HQD_BASE,
                                &s->hqd_regs);
    memory_region_init_io(&s->dispatch_regs, OBJECT(machine),
                          &gpgpu_cp_regs_ops, gpgpu,
                          "gpgpu-cp-registers", GPGPU_CP_REG_SIZE);
    memory_region_add_subregion(&s->cp_memory, GPGPU_CP_REG_BASE,
                                &s->dispatch_regs);

    object_initialize_child(OBJECT(machine), "cp", &s->cp,
                            machine->cpu_type);
    object_property_set_link(OBJECT(&s->cp), "memory", OBJECT(&s->cp_memory),
                             &error_fatal);
    qdev_prop_set_uint64(DEVICE(&s->cp), "resetvec", 0);
    object_property_set_bool(OBJECT(&s->cp), "start-powered-off", true,
                             &error_fatal);
    qdev_realize(DEVICE(&s->cp), NULL, &error_fatal);
    s->vm_state = qemu_add_vm_change_state_handler(gpgpu_cp_vm_state_change, s);
    gpgpu_cp_attach(gpgpu, gpgpu_cp_set_state, s);
}

static void gpgpu_cp_machine_instance_finalize(Object *obj)
{
    GPGPUCPMachineState *s = GPGPU_CP_MACHINE(obj);

    if (s->vm_state) {
        qemu_del_vm_change_state_handler(s->vm_state);
    }
}

static void gpgpu_cp_machine_instance_init(Object *obj)
{
    REMOTE_MACHINE(obj)->vfio_user = true;
}

static void gpgpu_cp_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        TYPE_RISCV_CPU_BASE64, NULL,
    };

    mc->init = gpgpu_cp_machine_init;
    mc->desc = "Remote GPGPU with a native RV64 command processor";
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE64;
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->no_serial = true;
    mc->no_parallel = true;
}

static const TypeInfo gpgpu_cp_machine_type = {
    .name = TYPE_GPGPU_CP_MACHINE,
    .parent = TYPE_REMOTE_MACHINE,
    .instance_size = sizeof(GPGPUCPMachineState),
    .instance_init = gpgpu_cp_machine_instance_init,
    .instance_finalize = gpgpu_cp_machine_instance_finalize,
    .class_init = gpgpu_cp_machine_class_init,
};

static void gpgpu_cp_machine_register_types(void)
{
    type_register_static(&gpgpu_cp_machine_type);
}

type_init(gpgpu_cp_machine_register_types);
