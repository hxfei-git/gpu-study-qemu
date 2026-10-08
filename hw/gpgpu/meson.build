# PCI lifecycle, register banks and execution engines.
system_ss.add(when: 'CONFIG_GPGPU', if_true: files(
    'gpgpu.c',
    'gpgpu_regs.c',
    'gpgpu_irq.c',
    'gpgpu_dma.c',
    'gpgpu_dispatch.c',
    'gpgpu_doorbell.c',
    'gpgpu_core.c',
))
