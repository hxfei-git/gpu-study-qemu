/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 访存入口：设备内部全部 GPU 业务访存的唯一入口
 *
 * 发起方给出 GPU 物理地址、操作和已确定的访问类型，本模块决定是否经过
 * dcache，并在旁路访问之前处理重叠的缓存行：
 *
 *   CP 用户窗口、执行器 ─GPUVM 翻译与权限检查─▶ 页属性 WB 或 DEVICE ─┐
 *   CP 管理窗口、页表遍历、IH、BAR2、DMA ─────▶ DEVICE ──────────────┤
 *                                                                    ▼
 *                                                       gpgpu_mem_access()
 *     ├─ WB 读写 ──▶ dcache：命中直接读写，未命中先回写旧行再回填
 *     ├─ DEVICE 读写 ─▶ 先回写并失效重叠的行，再读写后备存储，不分配行
 *     ├─ FETCH ───▶ 直接读后备存储，不经过 dcache（以后接 icache）
 *     └─ COPY ────▶ 两端都按 DEVICE 处理，再整段搬运
 *
 * 用户地址的翻译和权限检查在 mem/gpuvm.c 完成，本模块只接收物理地址。
 * 设备寄存器、初始化清零和原生 CP 的固件取指、全局变量与栈不经过这里。
 */
#ifndef HW_GPGPU_MEM_ACCESS_H
#define HW_GPGPU_MEM_ACCESS_H

#include "hw/gpgpu/gpgpu.h"

/**
 * enum GPGPUMemClient - 请求来源
 * @GPGPU_MEM_CP_USER: CP 用户域窗口，地址已由 GPUVM 处理。
 * @GPGPU_MEM_EXECUTOR: 计算执行器的取指与 load/store，地址已由 GPUVM 处理。
 * @GPGPU_MEM_CP_MGMT: CP 管理域窗口，访问 MQD、管理 Ring 与结果区。
 * @GPGPU_MEM_PTW: 页表遍历。
 * @GPGPU_MEM_IH: IH 读取固件记录、写 Host Ring。
 * @GPGPU_MEM_BAR: Host 经 BAR2 读写 VRAM。
 * @GPGPU_MEM_DMA: DMA 引擎在 VRAM 与 Host 内存之间搬运。
 *
 * 来源只用于检查请求是否合理，不决定能否缓存：只有经 GPUVM 的两类来源
 * 可以使用 WB，其余来源必须使用 DEVICE。
 */
enum GPGPUMemClient {
	GPGPU_MEM_CP_USER,
	GPGPU_MEM_EXECUTOR,
	GPGPU_MEM_CP_MGMT,
	GPGPU_MEM_PTW,
	GPGPU_MEM_IH,
	GPGPU_MEM_BAR,
	GPGPU_MEM_DMA,
};

/**
 * enum GPGPUMemOp - 访问操作
 * @GPGPU_MEM_READ: 数据读。
 * @GPGPU_MEM_WRITE: 数据写。
 * @GPGPU_MEM_FETCH: 取指；只读，不进入 dcache。
 * @GPGPU_MEM_COPY: 从 @addr 搬运到 @dst，一端在 VRAM，另一端在 Outbound。
 */
enum GPGPUMemOp {
	GPGPU_MEM_READ,
	GPGPU_MEM_WRITE,
	GPGPU_MEM_FETCH,
	GPGPU_MEM_COPY,
};

/**
 * enum GPGPUMemType - 已确定的访问类型
 * @GPGPU_MEM_INVALID: 未填写；请求直接失败，不会默认进入 dcache。
 * @GPGPU_MEM_WB: 写回缓存：数据访问经过 dcache。
 * @GPGPU_MEM_DEVICE: 不分配缓存行，按请求宽度直接读写后备存储。
 */
enum GPGPUMemType {
	GPGPU_MEM_INVALID,
	GPGPU_MEM_WB,
	GPGPU_MEM_DEVICE,
};

/**
 * struct GPGPUMemRequest - 一次物理事务
 * @client: 请求来源。
 * @op: 访问操作。
 * @type: 访问类型；用户访问取 GPUVM 给出的页属性，其余来源为 DEVICE。
 * @addr: GPU 物理起点；COPY 时为源地址。
 * @len: 字节数，非零。
 * @buf: 读写与取指的字节缓冲区；COPY 不使用。
 * @dst: COPY 的目的 GPU 物理地址；其他操作不使用。
 *
 * 请求只保存本次访问的值，不持有对象引用；调用返回后即可丢弃。
 */
typedef struct GPGPUMemRequest {
	enum GPGPUMemClient client;
	enum GPGPUMemOp op;
	enum GPGPUMemType type;
	uint64_t addr;
	uint64_t len;
	void *buf;
	uint64_t dst;
} GPGPUMemRequest;

/**
 * gpgpu_mem_access() - 执行一次物理事务
 * @s: 设备实例，调用者持有主锁。
 * @req: 请求；来源、操作与类型的组合必须合法，范围必须可路由。
 *
 * 整次事务计入在途物理访问，期间不释放主锁。DEVICE 与 COPY 先回写并
 * 失效重叠的缓存行，维护失败时不开始后备访问；失败前已写入的部分保留。
 *
 * Return: 成功为 true。请求非法、范围不可路由、维护失败或后备访问失败时
 * 为 false，故障由调用者按各自的规则记录。
 */
bool gpgpu_mem_access(GPGPUState *s, const GPGPUMemRequest *req);

/**
 * gpgpu_mem_sync() - 回写或失效与一段 GPU 物理地址重叠的缓存行
 * @s: 设备实例，调用者持有主锁。
 * @addr: 起始 GPU 物理地址；@op 含 GPGPU_DCACHE_ALL 时忽略。
 * @len: 字节数，非零，范围低于 4 GiB；@op 含 GPGPU_DCACHE_ALL 时忽略。
 * @op: GPGPU_DCACHE_CLEAN、GPGPU_DCACHE_INVALIDATE 或两者之和，可加
 *      GPGPU_DCACHE_ALL；调用者已检查编码。
 * @status: 不为 NULL 时写入 GpgpuDcacheStatus，供维护寄存器读回。
 *
 * 维护命令、任务收尾、Outbound 关闭和整体复位都经过这里。仍有在途物理
 * 访问时不执行，结果为 BUSY。维护本身不改变替换顺序，也不计命中与
 * 未命中。
 *
 * Return: 范围内的行全部维护完成时为 true。
 */
bool gpgpu_mem_sync(GPGPUState *s, uint64_t addr, uint64_t len, uint32_t op, uint32_t *status);

/*
 * 把 Outbound 窗口与 dcache 恢复为复位值。调用者已停止全部访问，并已
 * 回写脏行，或处在允许丢弃缓存内容的冷复位路径上。
 */
void gpgpu_mem_reset(GPGPUState *s);

#endif /* HW_GPGPU_MEM_ACCESS_H */
