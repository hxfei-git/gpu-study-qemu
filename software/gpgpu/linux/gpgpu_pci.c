// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

static int __init gpgpu_init(void)
{
    pr_info("gpgpu_pci: module loaded\n");
    return 0;
}

static void __exit gpgpu_exit(void)
{
    pr_info("gpgpu_pci: module unloaded\n");
}

module_init(gpgpu_init);
module_exit(gpgpu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hxfei");
MODULE_DESCRIPTION("Minimal GPGPU module loading experiment");
