# GPGPU 设备：组装与 BAR0 转发在本目录，各功能模块在子目录中登记。
system_ss.add(when: 'CONFIG_GPGPU', if_true: files('gpgpu.c', 'bar0.c'))
subdir('cp')
subdir('compute')
subdir('mem')
subdir('irq')
