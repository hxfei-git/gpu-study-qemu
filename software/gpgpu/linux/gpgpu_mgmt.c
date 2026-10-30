// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：KIQ / HIQ 管理队列
 *
 * 一次管理请求（gpgpu_mgmt_submit）：
 *
 *   1. 分配本次请求独占的结果区，初值 completion=0、status=PENDING
 *   2. 持 management_lock：在 Ring 中写入“命令 + WRITE_COMPLETION(序号)”，
 *      发布 W，敲 Doorbell
 *   3. 释放锁，轮询结果区的 completion 等于本次序号（不持 dev->lock）
 *   4. 读取 status；结果码超出协议范围视为协议错误
 *   5. 只有确认完成、或从未发布时才回收结果区；已发布但未确认的结果区
 *      永久保留，防止 CP 迟到的写回落到被复用的内存上
 */
#include <linux/delay.h>
#include <linux/slab.h>
#include "gpgpu_drv.h"

/* 管理 Ring 加一页 R/W 进度的总字节数。 */
#define MGMT_STORAGE_BYTES (GPGPU_MGMT_BYTES + PAGE_SIZE)
/* 管理 Ring 的槽数。 */
#define MGMT_SLOTS (GPGPU_MGMT_BYTES / sizeof(GpgpuManagementPacket))

/*
 * 为 KIQ 或 HIQ 分配 Ring、进度页与 MQD，并填好 MQD。
 *
 * Ring 之后紧跟一页进度：R 在偏移 GPGPU_MGMT_BYTES，W 在其后 8 字节。两条
 * 管理队列使用 Doorbell 第 0 页的入口 0、1（偏移 index * 8）。KIQ 由
 * gpgpu_load_kiq() 经选槽窗口直接写入 HQD，MQD 地址也写进 HQD，IH 记录
 * 据此识别 KIQ；HIQ 的 MQD 在启动第 6 步经 KIQ 的 MAP_QUEUES 交给 CP 装载。
 */
static int mgmt_ring_init(struct gpgpu_device *dev, unsigned int index)
{
	struct gpgpu_mgmt_ring *mq = &dev->mgmt[index];
	GpgpuMqd *mqd;
	int ret;

	ret = gpgpu_dma_buf_alloc(dev, &mq->storage, MGMT_STORAGE_BYTES, 1);
	if (!ret)
		ret = gpgpu_dma_buf_alloc(dev, &mq->mqd, sizeof(GpgpuMqd), 1);
	if (ret)
		return ret;

	mq->rptr = mq->storage.cpu + GPGPU_MGMT_BYTES;
	mq->wptr = mq->storage.cpu + GPGPU_MGMT_BYTES + 8;
	mq->doorbell = index * 8;
	mqd = mq->mqd.cpu;
	*mqd = (GpgpuMqd) {
		.ring_base = cpu_to_le64(mq->storage.addr),
		.rptr_report_addr = cpu_to_le64(mq->storage.addr + GPGPU_MGMT_BYTES),
		.wptr_poll_addr = cpu_to_le64(mq->storage.addr + GPGPU_MGMT_BYTES + 8),
		.pq_control = cpu_to_le32(GPGPU_MGMT_BYTES),
		.doorbell_control = cpu_to_le32(GPGPU_DOORBELL_ENABLE | mq->doorbell),
	};
	return 0;
}

/* 依次初始化 KIQ 与 HIQ；失败时已分配的部分由 gpgpu_mgmt_fini() 回收。 */
int gpgpu_mgmt_init(struct gpgpu_device *dev)
{
	int ret = mgmt_ring_init(dev, GPGPU_HQD_SLOT_KIQ);

	return ret ? ret : mgmt_ring_init(dev, GPGPU_HQD_SLOT_HIQ);
}

static void mgmt_request_free(struct gpgpu_device *dev, struct gpgpu_mgmt_request *req)
{
	gpgpu_dma_buf_free(dev, &req->result);
	list_del(&req->node);
	kfree(req);
}

/*
 * 设备引用归零时调用，此时设备已停止访问。回收两条管理 Ring
 * 和全部结果区，包括因超时而保留的结果区。
 */
void gpgpu_mgmt_fini(struct gpgpu_device *dev)
{
	struct gpgpu_mgmt_request *req, *next;

	list_for_each_entry_safe(req, next, &dev->requests, node)
		mgmt_request_free(dev, req);
	for (unsigned int i = 0; i < ARRAY_SIZE(dev->mgmt); i++) {
		gpgpu_dma_buf_free(dev, &dev->mgmt[i].mqd);
		gpgpu_dma_buf_free(dev, &dev->mgmt[i].storage);
	}
}

/*
 * 第 1 步：分配 16 字节结果区，completion 已由 gpgpu_dma_buf_alloc()
 * 清零，status 置为 PENDING。请求挂到 dev->requests，未回收的
 * 结果区在设备释放时统一归还。调用者持 dev->lock。
 */
static struct gpgpu_mgmt_request *mgmt_request_alloc(struct gpgpu_device *dev)
{
	struct gpgpu_mgmt_request *req = kzalloc(sizeof(*req), GFP_KERNEL);
	GpgpuManagementResult *result;

	if (!req)
		return NULL;
	if (gpgpu_dma_buf_alloc(dev, &req->result, sizeof(*result), 8)) {
		kfree(req);
		return NULL;
	}

	result = req->result.cpu;
	result->status = cpu_to_le32(GPGPU_MANAGEMENT_PENDING);
	list_add_tail(&req->node, &dev->requests);
	return req;
}

/*
 * mgmt_publish() - 第 2 步：在 Ring 中写入命令对并发布
 *
 * 调用者持 management_lock，保证同一时刻只有一个线程推进 W。
 *
 * 一次请求占两个相邻槽：先是管理命令，后是写回本请求结果区的
 * WRITE_COMPLETION，CP 执行完前者再执行后者，所以后者写出的序号就代表
 * 前者已结束。发布顺序是“写两个包 → dma_wmb → 写 W → wmb → Doorbell”，
 * CP 读到新 W 时两个包已经可见。
 *
 * Ring 至少要有两个空槽（W - R 不超过槽数 - 2）；序号从 1 递增，
 * 回绕为 0 后不再发布，防止新旧请求序号相同。设备移除、设备故障
 * 或该管理队列出错时也不再发布。
 *
 * Return: 0 并写出 *sequence，或 -EIO / -EBUSY。
 */
static int mgmt_publish(struct gpgpu_device *dev, struct gpgpu_mgmt_ring *mq, u32 opcode,
			u32 kind, u64 mqd, u64 result_addr, u64 *sequence)
{
	GpgpuManagementPacket *packets = mq->storage.cpu;
	u32 w, r;

	if (!READ_ONCE(dev->present) || READ_ONCE(dev->fatal) || READ_ONCE(mq->failed) ||
	    !dev->next_sequence)
		return -EIO;
	w = le32_to_cpu(READ_ONCE(*mq->wptr));
	r = le32_to_cpu(READ_ONCE(*mq->rptr));
	dma_rmb();
	if (w - r > MGMT_SLOTS - 2)
		return -EBUSY;

	*sequence = dev->next_sequence++;
	packets[w % MGMT_SLOTS] = (GpgpuManagementPacket) {
		.opcode = cpu_to_le32(opcode),
		.queue_kind = cpu_to_le32(kind),
		.address = cpu_to_le64(mqd),
	};
	packets[(w + 1) % MGMT_SLOTS] = (GpgpuManagementPacket) {
		.opcode = cpu_to_le32(GPGPU_WRITE_COMPLETION),
		.address = cpu_to_le64(result_addr),
		.value = cpu_to_le64(*sequence),
	};

	dma_wmb();
	WRITE_ONCE(*mq->wptr, cpu_to_le32(w + 2));
	/* 共享 W 必须先于 Doorbell 对设备可见。 */
	wmb();
	writeq((u64)(u32)(w + 1), dev->doorbell + mq->doorbell);
	return 0;
}

/*
 * mgmt_wait() - 第 3、4 步：等待完成序号并读取结果码
 *
 * 不持 dev->lock 轮询结果区的 completion，IH 工作项因此可以继续消费记录，
 * 并在管理队列出错时设置 mq->failed，让这里提前退出。读到本次序号后先
 * dma_rmb()，保证随后读到的 status 不早于 completion：CP 是先写 status、
 * 再写 completion 的。
 *
 * Return: 0 并写出 *status；-EIO（设备移除或通道故障）、-ETIMEDOUT，
 * 或 -EPROTO（结果码超出协议范围，无法判断设备是否仍在访问）。
 */
static int mgmt_wait(struct gpgpu_device *dev, struct gpgpu_mgmt_ring *mq,
		     GpgpuManagementResult *result, u64 sequence, u32 *status)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(GPGPU_TIMEOUT_MS);

	while (le64_to_cpu(READ_ONCE(result->completion)) != sequence) {
		if (!READ_ONCE(dev->present) || READ_ONCE(dev->fatal) || READ_ONCE(mq->failed))
			return -EIO;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		usleep_range(50, 100);
	}
	dma_rmb();
	*status = le32_to_cpu(READ_ONCE(result->status));
	return *status > GPGPU_MANAGEMENT_RECOVERY_REQUIRED ? -EPROTO : 0;
}

/**
 * gpgpu_mgmt_submit() - 发布一条管理命令并等待结果
 * @dev: 设备；调用者持 dev->lock，函数内会暂时释放并重新获取。
 * @ring: GPGPU_HQD_SLOT_KIQ 或 GPGPU_HQD_SLOT_HIQ。
 * @opcode: GPGPU_MAP_QUEUES 或 GPGPU_UNMAP_QUEUES。
 * @kind: 目标队列种类。
 * @mqd: 目标 MQD 的 GPU 物理地址。
 * @status: 输出结果码；未取得时为 GPGPU_MANAGEMENT_PENDING。
 * @published: 输出命令是否已发布给 CP。已发布但返回错误时，CP 可能仍在
 *             访问相关对象，调用者必须保留它们。
 *
 * Return: 0 表示取得了合法结果码（结果本身可能是失败）；负值见 mgmt_wait()，
 * 或 -ENOMEM、-EBUSY，以及等待期间设备被移除时的 -ENODEV。
 */
int gpgpu_mgmt_submit(struct gpgpu_device *dev, unsigned int ring, u32 opcode, u32 kind,
		      u64 mqd, u32 *status, bool *published)
{
	struct gpgpu_mgmt_ring *mq = &dev->mgmt[ring];
	struct gpgpu_mgmt_request *req;
	u64 sequence;
	int ret;

	*published = false;
	*status = GPGPU_MANAGEMENT_PENDING;

	/*
	 * 1. 持 dev->lock 分配本请求独占的结果区并登记，然后解锁：
	 *    发布与等待都不需要它，等待期间 IH 工作项要用它消费记录。
	 */
	req = mgmt_request_alloc(dev);
	if (!req)
		return -ENOMEM;
	mutex_unlock(&dev->lock);

	/* 2. 只在写 Ring、推进 W、敲 Doorbell 时持 management_lock。 */
	mutex_lock(&dev->management_lock);
	ret = mgmt_publish(dev, mq, opcode, kind, mqd, req->result.addr, &sequence);
	mutex_unlock(&dev->management_lock);

	/*
	 * 3. 4. 只有成功发布才等待。*published 置位后，即使后面失败，调用者
	 *    也知道 CP 可能已经开始处理这条命令。
	 */
	if (!ret) {
		*published = true;
		ret = mgmt_wait(dev, mq, req->result.cpu, sequence, status);
	}

	/*
	 * 5. 重新取 dev->lock。取得合法结果或从未发布时回收结果区。
	 *    已发布却没确认（超时、通道故障、结果码非法）时保留，
	 *    CP 迟到的写回仍会落在这块内存上，等设备释放时再归还。
	 */
	mutex_lock(&dev->lock);
	if (!ret || !*published)
		mgmt_request_free(dev, req);
	/* 等待期间设备可能被移除，调用者不能再访问 BAR。 */
	if (!dev->present)
		ret = -ENODEV;
	return ret;
}
