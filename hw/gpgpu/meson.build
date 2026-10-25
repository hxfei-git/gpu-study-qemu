# 设备组装和寄存器分发；功能实现由各模块目录登记。
system_ss.add(when: 'CONFIG_GPGPU', if_true: files('gpgpu.c', 'gpgpu_mmio.c'))
subdir('cp')
subdir('compute')
subdir('memory')
subdir('interrupt')
