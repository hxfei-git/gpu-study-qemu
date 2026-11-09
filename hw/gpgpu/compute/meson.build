# Dispatch 寄存器、warp 调度、RV64 解释器与 lane 访存。
system_ss.add(when: 'CONFIG_GPGPU', if_true: files(
    'dispatch.c',
    'scheduler.c',
    'rv64.c',
    'lane_mem.c',
    'tensor.c',
))
