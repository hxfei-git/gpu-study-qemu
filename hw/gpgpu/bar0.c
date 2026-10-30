// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BAR0：寄存器组子区域与 CP 本地寄存器视图
 */
#include "qemu/osdep.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "bar0.h"

/* 每个寄存器组的字节数。 */
#define REG_GROUP_SIZE 0x100

/**
 * struct Bar0Group - 一个 BAR0 寄存器组的静态描述
 * @name: 子区域名。
 * @base: 组在 BAR0 内的起始偏移。
 * @size: 组的字节数。
 * @read: 读取组内寄存器，reg 为完整 BAR0 偏移；为 NULL 时读零。
 * @write: 写入组内寄存器；为 NULL 时忽略写入。
 * @ih: 组属于 IH；访问形式非法时另锁存 IH 协议故障。
 */
struct Bar0Group {
	const char *name;
	hwaddr base;
	hwaddr size;
	uint32_t (*read)(GPGPUState *s, hwaddr reg);
	void (*write)(GPGPUState *s, hwaddr reg, uint32_t value);
	bool ih;
};

/* Dispatch 寄存器只允许 CP 经本地窗口写入。 */
static void dispatch_host_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

static const Bar0Group bar0_groups[] = {
	{ "gpgpu-info", 0x000, REG_GROUP_SIZE, gpgpu_info_read, NULL },
	{ "gpgpu-global", 0x100, REG_GROUP_SIZE, gpgpu_global_read, gpgpu_global_write },
	{ "gpgpu-irq", 0x200, REG_GROUP_SIZE, gpgpu_irq_read, gpgpu_irq_write },
	{ "gpgpu-dispatch", 0x300, REG_GROUP_SIZE, gpgpu_dispatch_read, dispatch_host_write },
	{ "gpgpu-dma", 0x400, REG_GROUP_SIZE, gpgpu_dma_read, gpgpu_dma_write },
	{ "gpgpu-cp", 0x500, REG_GROUP_SIZE, gpgpu_cp_read, gpgpu_cp_write },
	{ "gpgpu-hqd-select", 0x600, REG_GROUP_SIZE, gpgpu_hqd_select_read,
	  gpgpu_hqd_select_write },
	{ "gpgpu-ih", 0x800, REG_GROUP_SIZE, gpgpu_ih_read, gpgpu_ih_write, true },
	{ "gpgpu-vm", 0x900, 2 * REG_GROUP_SIZE, gpgpu_vm_read, gpgpu_vm_write },
	{ "gpgpu-outbound", 0xb00, REG_GROUP_SIZE, gpgpu_outbound_read, gpgpu_outbound_write },
};

/* HQD 窗口的寄存器偏移相对所选槽，单独处理。 */
static uint32_t hqd_window_read(GPGPUState *s, hwaddr reg)
{
	return gpgpu_hqd_window_read(s, reg - GPGPU_REG_HQD_WINDOW);
}

static void hqd_window_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	gpgpu_hqd_window_write(s, reg - GPGPU_REG_HQD_WINDOW, value);
}

static const Bar0Group bar0_hqd_window = {
	"gpgpu-hqd-window", GPGPU_REG_HQD_WINDOW, REG_GROUP_SIZE, hqd_window_read, hqd_window_write,
};

/* 覆盖整个 BAR0 的背景区：未定义偏移读零、写入忽略，宽度规则相同。 */
static const Bar0Group bar0_background = {
	"gpgpu-bar0-reserved", 0, GPGPU_CTRL_BAR_SIZE, NULL, NULL,
};

QEMU_BUILD_BUG_ON(ARRAY_SIZE(bar0_groups) + 2 != GPGPU_BAR0_REGIONS);

/* 只接受对齐的 32 位访问；否则锁存 INVALID_CMD，IH 组另记协议故障。 */
static bool access_ok(GPGPUState *s, const Bar0Group *g, hwaddr off, unsigned int size)
{
	if (size == 4 && !(off & 3))
		return true;
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
	if (g->ih)
		gpgpu_ih_protocol_fault(s);
	return false;
}

/* off 是组内偏移，加上组基址后交给模块，模块看到的是完整 BAR0 偏移。 */
static uint64_t group_read(void *opaque, hwaddr off, unsigned int size)
{
	GPGPUBar0Region *r = opaque;

	if (!access_ok(r->s, r->group, off, size) || !r->group->read)
		return 0;
	return r->group->read(r->s, r->group->base + off);
}

static void group_write(void *opaque, hwaddr off, uint64_t value, unsigned int size)
{
	GPGPUBar0Region *r = opaque;

	if (!access_ok(r->s, r->group, off, size) || !r->group->write)
		return;
	r->group->write(r->s, r->group->base + off, value);
}

/* 宽度检查在 group_read/write 中完成，因此对 QEMU 声明接受任意宽度。 */
static const MemoryRegionOps group_ops = {
	.read = group_read,
	.write = group_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
	.impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void add_group(GPGPUState *s, GPGPUBar0Region *r, const Bar0Group *g, int priority)
{
	r->s = s;
	r->group = g;
	memory_region_init_io(&r->mr, OBJECT(s), &group_ops, r, g->name, g->size);
	memory_region_add_subregion_overlap(&s->bar0, g->base, &r->mr, priority);
}

/*
 * gpgpu_bar0_init() - 建立 BAR0 寄存器空间
 *
 * BAR0 是一个 1 MiB 的容器区域，本身不处理访问，访问按偏移落到其中的
 * 子区域，再由各模块的读写函数处理：
 *
 * 1. 建立容器，先铺一个覆盖整个 BAR0、优先级为 -1 的背景区，
 *    使没有定义寄存器的偏移也按统一宽度规则读零、忽略写入；
 * 2. 以优先级 0 挂入各寄存器组和 HQD 窗口，覆盖对应位置的背景区；
 * 3. 把容器注册为 64 位内存 BAR 0。
 *
 * MSI-X 表与 PBA 由 irq.c 随后挂在容器末尾，同样覆盖背景区。
 */
void gpgpu_bar0_init(GPGPUState *s)
{
	GPGPUBar0Region *regions = s->bar0_regions;

	/* 1. 1 MiB 容器，背景区放在 regions[0]，优先级最低。 */
	memory_region_init(&s->bar0, OBJECT(s), "gpgpu-bar0", GPGPU_CTRL_BAR_SIZE);
	add_group(s, &regions[0], &bar0_background, -1);

	/*
	 * 2. bar0_groups 依次放在 regions[1..]，HQD 窗口放在最后一项；
	 *    数量由文件前部的 QEMU_BUILD_BUG_ON 核对。
	 */
	for (unsigned int i = 0; i < ARRAY_SIZE(bar0_groups); i++)
		add_group(s, &regions[i + 1], &bar0_groups[i], 0);
	add_group(s, &regions[ARRAY_SIZE(bar0_groups) + 1], &bar0_hqd_window, 0);

	/* 3. 注册为 64 位内存 BAR，此后 Host 才能看到这段寄存器空间。 */
	pci_register_bar(PCI_DEVICE(s), 0,
			 PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar0);
}

/* ---- CP 本地寄存器窗口 ---- */

/*
 * CP 视图中 GPUVM 可读范围的结束偏移（不含）：0x900 起的版本、模式、
 * 握手、失效状态与上下文寄存器可读，0xa00 起的故障组不开放给 CP。
 */
#define CP_VM_READ_END 0x0a00

/* reg 所在寄存器组的起始偏移，例如 0x804 属于 0x800 起的 IH 组。 */
static hwaddr reg_group(hwaddr reg)
{
	return reg & ~(hwaddr)(REG_GROUP_SIZE - 1);
}

/* 与 BAR0 相同的宽度规则：只接受对齐的 32 位访问，IH 组另记协议故障。 */
static bool cp_access_ok(GPGPUState *s, hwaddr reg, unsigned int size)
{
	if (size == 4 && !(reg & 3))
		return true;
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
	if (reg_group(reg) == GPGPU_REG_IH_BASE_LO)
		gpgpu_ih_protocol_fault(s);
	return false;
}

/* CP 可读：VM 0x900～0x9ff、Dispatch、IH 与 ERROR_STATUS；其余锁存 INVALID_CMD。 */
static uint64_t cp_regs_read(void *opaque, hwaddr reg, unsigned int size)
{
	GPGPUState *s = opaque;

	if (!cp_access_ok(s, reg, size))
		return 0;

	if (reg >= GPGPU_REG_VM_VERSION && reg < CP_VM_READ_END)
		return gpgpu_vm_read(s, reg);
	if (reg_group(reg) == GPGPU_REG_KERNEL_ADDR_LO)
		return gpgpu_dispatch_read(s, reg);
	if (reg_group(reg) == GPGPU_REG_IH_BASE_LO)
		return gpgpu_ih_read(s, reg);
	if (reg == GPGPU_REG_ERROR_STATUS)
		return gpgpu_global_read(s, reg);

	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
	return 0;
}

/*
 * CP 可写：VM_FIRMWARE 握手、Dispatch 与 IH_SUBMIT。
 * 握手版本不符或其余偏移锁存 INVALID_CMD。
 */
static void cp_regs_write(void *opaque, hwaddr reg, uint64_t value, unsigned int size)
{
	GPGPUState *s = opaque;

	if (!cp_access_ok(s, reg, size))
		return;

	if (reg == GPGPU_REG_VM_FIRMWARE) {
		if (!gpgpu_vm_firmware_handshake(s, value))
			gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
	} else if (reg_group(reg) == GPGPU_REG_KERNEL_ADDR_LO) {
		gpgpu_dispatch_cp_write(s, reg, value);
	} else if (reg_group(reg) == GPGPU_REG_IH_BASE_LO) {
		gpgpu_ih_cp_write(s, reg, value);
	} else {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
	}
}

const MemoryRegionOps gpgpu_cp_regs_ops = {
	.read = cp_regs_read,
	.write = cp_regs_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
	.impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};
