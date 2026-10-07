/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Run against the real Linux PCI driver and QEMU device, never a mock. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "gpgpu.h"
#include "builtin_index.h"

/* Driver and userspace compile this ABI independently on different machines. */
_Static_assert(sizeof(struct gpgpu_info) == 16, "INFO ABI size");
_Static_assert(sizeof(struct gpgpu_alloc) == 8, "ALLOC ABI size");
_Static_assert(sizeof(struct gpgpu_copy) == 16, "COPY ABI size");
_Static_assert(sizeof(struct gpgpu_launch) == 48, "LAUNCH ABI size");
_Static_assert(sizeof(struct gpgpu_stats) == 24, "STATS ABI size");
_Static_assert(offsetof(struct gpgpu_stats, dma_irqs) == 8, "DMA counter ABI");
_Static_assert(offsetof(struct gpgpu_stats, error_irqs) == 16, "error IRQ ABI");

static unsigned int passed, total;

static int call_ioctl(int fd, unsigned long command, void *arg)
{
    return ioctl(fd, command, arg) < 0 ? -errno : 0;
}

static void expect(const char *name, int actual, int expected)
{
    total++;
    if (actual == expected) {
        passed++;
        printf("PASS %s\n", name);
    } else {
        fprintf(stderr, "FAIL %s: got %d, expected %d\n",
                name, actual, expected);
    }
}

static void expect_mapping(int fd, size_t size, int flags, int prot,
                           off_t offset, const char *name)
{
    void *mapping = mmap(NULL, size, prot, flags, fd, offset);
    int rc = mapping == MAP_FAILED ? -errno : 0;

    expect(name, rc, -EINVAL);
    if (mapping != MAP_FAILED) {
        munmap(mapping, size);
    }
}

static int driver_tests(const char *device)
{
    struct gpgpu_info info;
    struct gpgpu_stats before, after;
    struct gpgpu_alloc data = { .size = 256 }, code = { .size = 4 };
    struct gpgpu_alloc invalid = { 0 };
    struct gpgpu_copy copy;
    struct gpgpu_launch launch;
    unsigned char pattern[256];
    unsigned char *staging = MAP_FAILED;
    uint32_t address;
    unsigned int i;
    int fd = open(device, O_RDWR | O_CLOEXEC), other, rc = -1;

    if (fd < 0) {
        perror("driver test open");
        return -1;
    }
    other = open(device, O_RDWR | O_CLOEXEC);
    expect("exclusive device session", other < 0 ? -errno : 0, -EBUSY);
    if (other >= 0) {
        close(other);
    }
    expect("unknown ioctl", call_ioctl(fd, _IO('G', 99), NULL), -ENOTTY);
    expect("invalid user pointer", call_ioctl(fd, GPGPU_IOC_INFO, NULL),
           -EFAULT);
    expect_mapping(fd, 4096, MAP_SHARED, PROT_READ | PROT_WRITE, 0,
                   "reject short mmap");
    expect_mapping(fd, GPGPU_STAGING_SIZE * 2, MAP_SHARED, PROT_READ, 0,
                   "reject oversized mmap");
    expect_mapping(fd, GPGPU_STAGING_SIZE, MAP_SHARED, PROT_READ, 4096,
                   "reject nonzero mmap offset");
    expect_mapping(fd, GPGPU_STAGING_SIZE, MAP_PRIVATE, PROT_READ, 0,
                   "reject private mmap");
    expect_mapping(fd, GPGPU_STAGING_SIZE, MAP_SHARED, PROT_READ | PROT_EXEC,
                   0, "reject executable mmap");
    staging = mmap(NULL, GPGPU_STAGING_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (staging == MAP_FAILED || call_ioctl(fd, GPGPU_IOC_INFO, &info) ||
        call_ioctl(fd, GPGPU_IOC_ALLOC, &data) ||
        call_ioctl(fd, GPGPU_IOC_ALLOC, &code)) {
        fprintf(stderr, "driver test setup failed: %s\n", strerror(errno));
        goto out;
    }
    expect("ABI and DMA mapping size",
           info.abi_version == GPGPU_ABI_VERSION &&
           info.staging_size == GPGPU_STAGING_SIZE ? 0 : -1, 0);
    expect("zero-byte allocation", call_ioctl(fd, GPGPU_IOC_ALLOC, &invalid),
           -EINVAL);
    invalid.size = info.vram_size;
    expect("allocation larger than usable VRAM",
           call_ioctl(fd, GPGPU_IOC_ALLOC, &invalid), -EINVAL);
    address = data.address + 4;
    expect("free requires allocation base",
           call_ioctl(fd, GPGPU_IOC_FREE, &address), -EINVAL);
    address = 0;
    expect("free rejects reserved address",
           call_ioctl(fd, GPGPU_IOC_FREE, &address), -EINVAL);
    copy = (struct gpgpu_copy) { .address = data.address + 252, .size = 8 };
    expect("DMA rejects allocation overrun",
           call_ioctl(fd, GPGPU_IOC_COPY, &copy), -EINVAL);
    copy.address = data.address;
    copy.size = 0;
    expect("DMA rejects empty descriptor",
           call_ioctl(fd, GPGPU_IOC_COPY, &copy), -EINVAL);
    copy.size = GPGPU_STAGING_SIZE + 1;
    expect("DMA rejects staging overrun",
           call_ioctl(fd, GPGPU_IOC_COPY, &copy), -EINVAL);
    copy.size = 4;
    copy.address = UINT32_MAX - 1;
    expect("DMA rejects wrapped VRAM range",
           call_ioctl(fd, GPGPU_IOC_COPY, &copy), -EINVAL);
    copy.address = data.address;
    copy.direction = 2;
    expect("DMA rejects invalid direction",
           call_ioctl(fd, GPGPU_IOC_COPY, &copy), -EINVAL);
    copy.direction = 0;
    copy.reserved = 1;
    expect("DMA rejects nonzero reserved field",
           call_ioctl(fd, GPGPU_IOC_COPY, &copy), -EINVAL);
    for (i = 0; i < sizeof(pattern); i++) {
        pattern[i] = i ^ 0xa5;
    }
    memcpy(staging, pattern, sizeof(pattern));
    copy = (struct gpgpu_copy) { .address = data.address, .size = 256 };
    expect("real PCI DMA H2D", call_ioctl(fd, GPGPU_IOC_COPY, &copy), 0);
    memset(staging, 0, sizeof(pattern));
    copy.direction = GPGPU_COPY_D2H;
    expect("real PCI DMA D2H", call_ioctl(fd, GPGPU_IOC_COPY, &copy), 0);
    expect("DMA byte-for-byte round trip",
           memcmp(staging, pattern, sizeof(pattern)) ? -1 : 0, 0);
    memcpy(staging, "\x73\x00\x10\x00", 4); /* RV32 ebreak. */
    copy = (struct gpgpu_copy) { .address = code.address, .size = 4 };
    if (call_ioctl(fd, GPGPU_IOC_COPY, &copy)) {
        goto out;
    }
    launch = (struct gpgpu_launch) {
        .code = code.address, .code_size = 4,
        .args = data.address, .args_size = 4,
        .grid = { 1, 1, 1 }, .block = { 1, 1, 1 },
    };
    for (i = 0; i < 3; i++) {
        char name[64];

        launch.grid[i] = 0;
        snprintf(name, sizeof(name), "ioctl rejects zero grid dimension %u", i);
        expect(name, call_ioctl(fd, GPGPU_IOC_LAUNCH, &launch), -EINVAL);
        launch.grid[i] = 1;
        launch.block[i] = 0;
        snprintf(name, sizeof(name), "ioctl rejects zero block dimension %u",
                 i);
        expect(name, call_ioctl(fd, GPGPU_IOC_LAUNCH, &launch), -EINVAL);
        launch.block[i] = 1;
    }
    launch.grid[0] = GPGPU_MAX_BLOCKS + 1;
    expect("ioctl limits block count",
           call_ioctl(fd, GPGPU_IOC_LAUNCH, &launch), -E2BIG);
    launch.grid[0] = 1;
    launch.block[0] = GPGPU_MAX_BLOCK_THREADS + 1;
    expect("ioctl limits threads per block",
           call_ioctl(fd, GPGPU_IOC_LAUNCH, &launch), -E2BIG);
    launch.block[0] = 1024;
    launch.grid[0] = 65;
    expect("ioctl limits total threads",
           call_ioctl(fd, GPGPU_IOC_LAUNCH, &launch), -E2BIG);
    launch.block[0] = launch.grid[0] = 1;
    launch.shared_size = 4;
    expect("ioctl rejects unsupported shared memory",
           call_ioctl(fd, GPGPU_IOC_LAUNCH, &launch), -EOPNOTSUPP);
    launch.shared_size = 0;
    launch.code++;
    expect("ioctl rejects unaligned kernel",
           call_ioctl(fd, GPGPU_IOC_LAUNCH, &launch), -EINVAL);
    launch.code--;
    launch.args_size = 260;
    expect("ioctl rejects args allocation overrun",
           call_ioctl(fd, GPGPU_IOC_LAUNCH, &launch), -EINVAL);
    launch.args_size = 4;
    expect("valid RV32 launch", call_ioctl(fd, GPGPU_IOC_LAUNCH, &launch), 0);
    if (call_ioctl(fd, GPGPU_IOC_STATS, &before)) {
        goto out;
    }
    memset(staging, 0, 4); /* Unsupported opcode, must report an error IRQ. */
    if (call_ioctl(fd, GPGPU_IOC_COPY, &copy)) {
        goto out;
    }
    expect("invalid RV32 kernel returns device error",
           call_ioctl(fd, GPGPU_IOC_LAUNCH, &launch), -EIO);
    if (call_ioctl(fd, GPGPU_IOC_STATS, &after)) {
        goto out;
    }
    expect("device error is delivered by MSI-X",
           after.error_irqs > before.error_irqs ? 0 : -1, 0);
    expect("driver reset", call_ioctl(fd, GPGPU_IOC_RESET, NULL), 0);
    memcpy(staging, "\x73\x00\x10\x00", 4);
    expect("DMA works after reset", call_ioctl(fd, GPGPU_IOC_COPY, &copy), 0);
    expect("kernel works after error and reset",
           call_ioctl(fd, GPGPU_IOC_LAUNCH, &launch), 0);
    address = data.address;
    expect("free owned VRAM", call_ioctl(fd, GPGPU_IOC_FREE, &address), 0);
    expect("double free is rejected",
           call_ioctl(fd, GPGPU_IOC_FREE, &address), -EINVAL);
    rc = 0;
out:
    if (staging != MAP_FAILED) {
        munmap(staging, GPGPU_STAGING_SIZE);
    }
    close(fd);
    return rc;
}

typedef struct CopyWorker {
    GpgpuContext *ctx;
    GpgpuDevicePtr address;
    unsigned char value;
    int rc;
} CopyWorker;

static void *copy_worker(void *opaque)
{
    CopyWorker *worker = opaque;
    unsigned char input[2048], output[2048];
    unsigned int i;

    memset(input, worker->value, sizeof(input));
    for (i = 0; i < 4; i++) {
        worker->rc = gpgpuMemcpy(worker->ctx, worker->address, input,
                                 sizeof(input), GPGPU_MEMCPY_HOST_TO_DEVICE);
        if (worker->rc) {
            break;
        }
        worker->rc = gpgpuMemcpy(worker->ctx, worker->address, output,
                                 sizeof(output), GPGPU_MEMCPY_DEVICE_TO_HOST);
        if (worker->rc || memcmp(input, output, sizeof(input))) {
            worker->rc = -EIO;
            break;
        }
    }
    return NULL;
}

static int runtime_tests(const char *device)
{
    GpgpuContext *ctx = NULL, *other = NULL;
    GpgpuKernel *kernel = NULL, *invalid_kernel = NULL;
    GpgpuKernel *index_kernel = NULL;
    GpgpuDevicePtr address = 0;
    const unsigned char code[] = { 0x73, 0x00, 0x10, 0x00 };
    const unsigned char bad_code[] = { 0, 0, 0, 0 };
    GpgpuDim3 one = { 1, 1, 1 }, zero_y = { 1, 0, 1 };
    GpgpuDim3 overflow = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
    size_t bytes = GPGPU_STAGING_SIZE + 37;
    unsigned char *input = malloc(bytes), *output = malloc(bytes);
    CopyWorker workers[2] = { { 0 }, { 0 } };
    pthread_t threads[2];
    size_t i;
    unsigned int started = 0;
    uint32_t indices[360], index_args[1];
    GpgpuDim3 index_grid = { 2, 2, 2 }, index_block = { 3, 3, 5 };
    int rc = -1;

    if (!input || !output || gpgpuInit(&ctx, device)) {
        fprintf(stderr, "runtime test setup failed\n");
        goto out;
    }
    expect("runtime preserves exclusive session", gpgpuInit(&other, device),
           -EBUSY);
    expect("runtime rejects zero allocation", gpgpuMalloc(ctx, &address, 0),
           -EINVAL);
    expect("runtime rejects null allocation result", gpgpuMalloc(ctx, NULL, 4),
           -EINVAL);
    if (gpgpuMalloc(ctx, &address, bytes)) {
        goto out;
    }
    for (i = 0; i < bytes; i++) {
        input[i] = (i * 17) ^ (i >> 8);
    }
    expect("runtime chunks H2D larger than staging",
           gpgpuMemcpy(ctx, address, input, bytes,
                       GPGPU_MEMCPY_HOST_TO_DEVICE), 0);
    expect("runtime chunks D2H larger than staging",
           gpgpuMemcpy(ctx, address, output, bytes,
                       GPGPU_MEMCPY_DEVICE_TO_HOST), 0);
    expect("runtime preserves chunk boundary and odd tail bytes",
           memcmp(input, output, bytes) ? -1 : 0, 0);
    expect("runtime rejects missing host buffer",
           gpgpuMemcpy(ctx, address, NULL, 4, GPGPU_MEMCPY_HOST_TO_DEVICE),
           -EINVAL);
    expect("runtime rejects wrapped device address",
           gpgpuMemcpy(ctx, UINT32_MAX - 1, input, 4,
                       GPGPU_MEMCPY_HOST_TO_DEVICE), -EINVAL);
    expect("runtime rejects invalid memcpy kind",
           gpgpuMemcpy(ctx, address, input, 4, (GpgpuMemcpyKind)2), -EINVAL);
    expect("runtime rejects incomplete RV32 instruction",
           gpgpuLoadKernel(ctx, &invalid_kernel, code, 3), -EINVAL);
    if (gpgpuLoadKernel(ctx, &kernel, code, sizeof(code))) {
        goto out;
    }
    expect("runtime rejects zero dim3 component",
           gpgpuLaunchKernel(ctx, kernel, zero_y, one, NULL, 0, 0), -EINVAL);
    expect("runtime rejects dimensions without integer overflow",
           gpgpuLaunchKernel(ctx, kernel, overflow, overflow, NULL, 0, 0),
           -EINVAL);
    expect("runtime reports unsupported shared memory",
           gpgpuLaunchKernel(ctx, kernel, one, one, NULL, 0, 4), -EOPNOTSUPP);
    expect("runtime launches kernel without explicit args",
           gpgpuLaunchKernel(ctx, kernel, one, one, NULL, 0, 0), 0);
    if (gpgpuLoadKernel(ctx, &invalid_kernel, bad_code, sizeof(bad_code))) {
        goto out;
    }
    expect("runtime returns hardware execution error",
           gpgpuLaunchKernel(ctx, invalid_kernel, one, one, NULL, 0, 0), -EIO);
    expect("runtime recovers on next valid launch",
           gpgpuLaunchKernel(ctx, kernel, one, one, NULL, 0, 0), 0);
    expect("runtime synchronization", gpgpuDeviceSynchronize(ctx), 0);
    memset(indices, 0xff, sizeof(indices));
    if (gpgpuLoadKernel(ctx, &index_kernel, builtin_index_code,
                        sizeof(builtin_index_code))) {
        goto out;
    }
    index_args[0] = address;
    expect("initialize xyz builtin regression output",
           gpgpuMemcpy(ctx, address, indices, sizeof(indices),
                       GPGPU_MEMCPY_HOST_TO_DEVICE), 0);
    expect("launch 3D grid and 45-thread block",
           gpgpuLaunchKernel(ctx, index_kernel, index_grid, index_block,
                              index_args, sizeof(index_args), 0), 0);
    expect("read xyz builtin regression output",
           gpgpuMemcpy(ctx, address, indices, sizeof(indices),
                       GPGPU_MEMCPY_DEVICE_TO_HOST), 0);
    for (i = 0; i < 360 && indices[i] == i + index_grid.z; i++) {
        /* Every output contains its xyz identity plus the gridDim.z builtin. */
    }
    expect("all xyz builtins and second-warp thread identities", i == 360 ?
           0 : -1, 0);
    expect("runtime unload releases loaded kernel",
           gpgpuUnloadKernel(ctx, index_kernel), 0);
    expect("runtime rejects null kernel object",
           gpgpuUnloadKernel(ctx, NULL), -EINVAL);
    for (i = 0; i < 2; i++) {
        workers[i].ctx = ctx;
        workers[i].value = i ? 0x39 : 0xc7;
        if (gpgpuMalloc(ctx, &workers[i].address, 2048) ||
            pthread_create(&threads[i], NULL, copy_worker, &workers[i])) {
            goto join;
        }
        started++;
    }
join:
    for (i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
    }
    expect("context mutex protects concurrent DMA staging",
           started == 2 && !workers[0].rc && !workers[1].rc ? 0 : -1, 0);
    expect("runtime frees allocation", gpgpuFree(ctx, address), 0);
    expect("runtime returns invalid free error", gpgpuFree(ctx, address),
           -EINVAL);
    rc = 0;
out:
    gpgpuDestroy(other);
    gpgpuDestroy(ctx);
    free(input);
    free(output);
    return rc;
}

int main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] : "/dev/gpgpu0";
    int rc;

    rc = driver_tests(device);
    if (!rc) {
        rc = runtime_tests(device);
    }
    printf("GPGPU runtime tests: %u/%u %s\n", passed, total,
           !rc && passed == total ? "PASS" : "FAIL");
    return rc || passed != total ? EXIT_FAILURE : EXIT_SUCCESS;
}
