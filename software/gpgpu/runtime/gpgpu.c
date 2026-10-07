/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone guest library: this file does not depend on QEMU headers. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "gpgpu.h"

struct GpgpuKernel {
    GpgpuDevicePtr address;
    uint32_t bytes;
    struct GpgpuKernel *next;
};

struct GpgpuContext {
    int fd;
    void *staging;
    pthread_mutex_t mutex;
    struct gpgpu_info info;
    GpgpuKernel *kernels;
};

static int request(GpgpuContext *ctx, unsigned long command, void *arg)
{
    int rc;

    do {
        rc = ioctl(ctx->fd, command, arg);
    } while (rc < 0 && errno == EINTR);
    return rc < 0 ? -errno : 0;
}

static int lock_context(GpgpuContext *ctx)
{
    return ctx ? -pthread_mutex_lock(&ctx->mutex) : -EINVAL;
}

static int allocate(GpgpuContext *ctx, GpgpuDevicePtr *out, size_t bytes)
{
    struct gpgpu_alloc allocation = { .size = bytes };
    int rc;

    if (!out || !bytes || bytes > UINT32_MAX) {
        return -EINVAL;
    }
    rc = request(ctx, GPGPU_IOC_ALLOC, &allocation);
    if (!rc) {
        *out = allocation.address;
    }
    return rc;
}

static int copy(GpgpuContext *ctx, GpgpuDevicePtr address, void *host,
                size_t bytes, GpgpuMemcpyKind kind)
{
    struct gpgpu_copy transfer = { .direction = kind };
    size_t offset = 0;
    int rc;

    if ((!host && bytes) || bytes > UINT32_MAX ||
        bytes > UINT32_MAX - address ||
        (kind != GPGPU_MEMCPY_HOST_TO_DEVICE &&
         kind != GPGPU_MEMCPY_DEVICE_TO_HOST)) {
        return -EINVAL;
    }
    while (offset < bytes) {
        size_t remaining = bytes - offset;

        transfer.address = address + offset;
        transfer.size = remaining < ctx->info.staging_size ?
                        remaining : ctx->info.staging_size;
        if (kind == GPGPU_MEMCPY_HOST_TO_DEVICE) {
            memcpy(ctx->staging, (char *)host + offset, transfer.size);
        }
        rc = request(ctx, GPGPU_IOC_COPY, &transfer);
        if (rc) {
            return rc;
        }
        if (kind == GPGPU_MEMCPY_DEVICE_TO_HOST) {
            memcpy((char *)host + offset, ctx->staging, transfer.size);
        }
        offset += transfer.size;
    }
    return 0;
}

int gpgpuInit(GpgpuContext **out, const char *device)
{
    GpgpuContext *ctx;
    int rc;

    if (!out) {
        return -EINVAL;
    }
    *out = NULL;
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -ENOMEM;
    }
    ctx->fd = open(device ? device : "/dev/gpgpu0", O_RDWR | O_CLOEXEC);
    if (ctx->fd < 0) {
        rc = -errno;
        goto fail;
    }
    rc = request(ctx, GPGPU_IOC_INFO, &ctx->info);
    if (rc) {
        goto fail_fd;
    }
    if (ctx->info.abi_version != GPGPU_ABI_VERSION ||
        ctx->info.staging_size != GPGPU_STAGING_SIZE) {
        rc = -EPROTO;
        goto fail_fd;
    }
    ctx->staging = mmap(NULL, ctx->info.staging_size,
                        PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, 0);
    if (ctx->staging == MAP_FAILED) {
        rc = -errno;
        goto fail_fd;
    }
    rc = pthread_mutex_init(&ctx->mutex, NULL);
    if (rc) {
        munmap(ctx->staging, ctx->info.staging_size);
        rc = -rc;
        goto fail_fd;
    }
    *out = ctx;
    return 0;

fail_fd:
    close(ctx->fd);
fail:
    free(ctx);
    return rc;
}

/* The caller must join other users of ctx before destroying it. */
void gpgpuDestroy(GpgpuContext *ctx)
{
    GpgpuKernel *kernel;

    if (!ctx) {
        return;
    }
    while ((kernel = ctx->kernels)) {
        ctx->kernels = kernel->next;
        free(kernel);
    }
    munmap(ctx->staging, ctx->info.staging_size);
    close(ctx->fd); /* Driver releases all allocations owned by this session. */
    pthread_mutex_destroy(&ctx->mutex);
    free(ctx);
}

int gpgpuMalloc(GpgpuContext *ctx, GpgpuDevicePtr *out, size_t bytes)
{
    int rc = lock_context(ctx);

    if (rc) {
        return rc;
    }
    rc = allocate(ctx, out, bytes);
    pthread_mutex_unlock(&ctx->mutex);
    return rc;
}

int gpgpuFree(GpgpuContext *ctx, GpgpuDevicePtr address)
{
    int rc = lock_context(ctx);

    if (rc) {
        return rc;
    }
    rc = request(ctx, GPGPU_IOC_FREE, &address);
    pthread_mutex_unlock(&ctx->mutex);
    return rc;
}

int gpgpuMemcpy(GpgpuContext *ctx, GpgpuDevicePtr device_address,
                void *host, size_t bytes, GpgpuMemcpyKind kind)
{
    int rc = lock_context(ctx);

    if (rc) {
        return rc;
    }
    rc = copy(ctx, device_address, host, bytes, kind);
    pthread_mutex_unlock(&ctx->mutex);
    return rc;
}

int gpgpuLoadKernel(GpgpuContext *ctx, GpgpuKernel **out,
                   const void *code, size_t bytes)
{
    GpgpuKernel *kernel;
    int rc;

    if (!out || !code || !bytes || bytes % 4 || bytes > UINT32_MAX) {
        return -EINVAL;
    }
    *out = NULL;
    kernel = calloc(1, sizeof(*kernel));
    if (!kernel) {
        return -ENOMEM;
    }
    rc = lock_context(ctx);
    if (rc) {
        free(kernel);
        return rc;
    }
    rc = allocate(ctx, &kernel->address, bytes);
    if (!rc) {
        rc = copy(ctx, kernel->address, (void *)code, bytes,
                  GPGPU_MEMCPY_HOST_TO_DEVICE);
        if (rc) {
            request(ctx, GPGPU_IOC_FREE, &kernel->address);
        }
    }
    if (!rc) {
        kernel->bytes = bytes;
        kernel->next = ctx->kernels;
        ctx->kernels = kernel;
        *out = kernel;
    } else {
        free(kernel);
    }
    pthread_mutex_unlock(&ctx->mutex);
    return rc;
}

static GpgpuKernel **find_kernel(GpgpuContext *ctx, GpgpuKernel *kernel)
{
    GpgpuKernel **cursor;

    for (cursor = &ctx->kernels; *cursor; cursor = &(*cursor)->next) {
        if (*cursor == kernel) {
            return cursor;
        }
    }
    return NULL;
}

int gpgpuUnloadKernel(GpgpuContext *ctx, GpgpuKernel *kernel)
{
    GpgpuKernel **cursor;
    int rc = lock_context(ctx);

    if (rc) {
        return rc;
    }
    cursor = find_kernel(ctx, kernel);
    if (!cursor) {
        rc = -EINVAL;
    } else {
        rc = request(ctx, GPGPU_IOC_FREE, &kernel->address);
        if (!rc) {
            *cursor = kernel->next;
            free(kernel);
        }
    }
    pthread_mutex_unlock(&ctx->mutex);
    return rc;
}

static int validate_dimensions(GpgpuDim3 grid, GpgpuDim3 block)
{
    uint32_t grids[] = { grid.x, grid.y, grid.z };
    uint32_t blocks[] = { block.x, block.y, block.z };
    uint64_t block_count = 1, thread_count = 1;
    unsigned int i;

    for (i = 0; i < 3; i++) {
        if (!grids[i] || !blocks[i] ||
            grids[i] > GPGPU_MAX_BLOCKS / block_count ||
            blocks[i] > GPGPU_MAX_BLOCK_THREADS / thread_count) {
            return -EINVAL;
        }
        block_count *= grids[i];
        thread_count *= blocks[i];
    }
    return block_count * thread_count > GPGPU_MAX_THREADS ? -EINVAL : 0;
}

int gpgpuLaunchKernel(GpgpuContext *ctx, GpgpuKernel *kernel,
                      GpgpuDim3 grid, GpgpuDim3 block,
                      const void *args, size_t args_bytes,
                      uint32_t shared_bytes)
{
    uint32_t empty_args = 0;
    size_t transfer_bytes = args_bytes ? args_bytes : sizeof(empty_args);
    struct gpgpu_launch launch = {
        .grid = { grid.x, grid.y, grid.z },
        .block = { block.x, block.y, block.z },
        .args_size = transfer_bytes,
        .shared_size = shared_bytes,
    };
    int rc = lock_context(ctx);
    int free_rc;

    if (rc) {
        return rc;
    }
    if (!find_kernel(ctx, kernel) || (!args && args_bytes) ||
        args_bytes > UINT32_MAX || args_bytes % 4) {
        rc = -EINVAL;
        goto out;
    }
    rc = validate_dimensions(grid, block);
    if (rc) {
        goto out;
    }
    if (shared_bytes) {
        rc = -EOPNOTSUPP;
        goto out;
    }
    launch.code = kernel->address;
    launch.code_size = kernel->bytes;
    rc = allocate(ctx, &launch.args, transfer_bytes);
    if (rc) {
        goto out;
    }
    rc = copy(ctx, launch.args, args_bytes ? (void *)args : &empty_args,
              transfer_bytes, GPGPU_MEMCPY_HOST_TO_DEVICE);
    if (!rc) {
        rc = request(ctx, GPGPU_IOC_LAUNCH, &launch);
    }
    free_rc = request(ctx, GPGPU_IOC_FREE, &launch.args);
    if (!rc) {
        rc = free_rc;
    }
out:
    pthread_mutex_unlock(&ctx->mutex);
    return rc;
}

int gpgpuDeviceSynchronize(GpgpuContext *ctx)
{
    int rc = lock_context(ctx);

    if (!rc) {
        /* DMA and launches complete before their ioctls return. */
        pthread_mutex_unlock(&ctx->mutex);
    }
    return rc;
}

int gpgpuGetInfo(GpgpuContext *ctx, struct gpgpu_info *info)
{
    int rc = lock_context(ctx);

    if (rc) {
        return rc;
    }
    rc = info ? request(ctx, GPGPU_IOC_INFO, info) : -EINVAL;
    pthread_mutex_unlock(&ctx->mutex);
    return rc;
}

int gpgpuGetStats(GpgpuContext *ctx, struct gpgpu_stats *stats)
{
    int rc = lock_context(ctx);

    if (rc) {
        return rc;
    }
    rc = stats ? request(ctx, GPGPU_IOC_STATS, stats) : -EINVAL;
    pthread_mutex_unlock(&ctx->mutex);
    return rc;
}
