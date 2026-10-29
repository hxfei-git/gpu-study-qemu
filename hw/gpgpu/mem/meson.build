# VRAM、GPU 物理地址路由、GPUVM 翻译与 DMA。
system_ss.add(when: 'CONFIG_GPGPU', if_true: files(
    'vram.c',
    'phys.c',
    'gpuvm.c',
    'dma.c',
))
