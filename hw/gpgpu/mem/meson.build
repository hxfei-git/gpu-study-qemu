# 访存入口、VRAM、GPU 物理地址路由、GPUVM 翻译、数据缓存与 DMA。
system_ss.add(when: 'CONFIG_GPGPU', if_true: files(
    'access.c',
    'vram.c',
    'phys.c',
    'gpuvm.c',
    'dcache.c',
    'dma.c',
))
