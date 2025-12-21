#include <stdint.h>

#include "vga.h"
#include "vfs.h"
#include "interrupts.h"
#include "pic.h"
#include "proc.h"
#include "syscall.h"
#include "memory.h"
#include "vbe.h"
#include "fat16.h"
#include "gfx.h"
#include "klog.h"
#include "module.h"
#include "devmgr.h"
#include "ipc.h"
#include "pit.h"
#include "debug.h"
#include "blockdev.h"
#include "partition.h"
#include "volmgr.h"
#include "bios_fallback.h"

extern void shell_run(void);
extern void user_init(void);

static void shell_task(void)
{
    shell_run();
    process_exit(0);
}

static void print_banner(void)
{
    vga_set_color(0xF, 0x0);
    vga_write_line("proOS (Protected Mode)");
    vga_set_color(0xA, 0x0);
    vga_write_line("version: v0.5");
    vga_set_color(0x7, 0x0);
    vga_write_line("Type 'help' to list commands.");
    vga_write_char('\n');
}

void kmain(void)
{
    memory_init();
    blockdev_init();
    partition_init();

    const struct boot_info *info = boot_info_get();
    bios_fallback_init(0x80u);

    vbe_init();
    vga_init();
    vga_clear();
    klog_init();
    klog_info("kernel: video initialized");
    klog_info("kernel: memory initialized");
    if (vfs_init() < 0)
        klog_error("kernel: vfs initialization failed");
    else
        klog_info("kernel: vfs ready");

    int fat_ok = 0;
    if (info && info->fat_ptr && info->fat_size)
    {
        fat_ok = fat16_init((const void *)(uintptr_t)info->fat_ptr, (size_t)info->fat_size);
    }

    if (fat_ok)
    {
        klog_info("kernel: FAT16 image mounted");
        if (fat16_mount_volume("Disk0") == 0)
            klog_info("kernel: FAT16 volume available at /Volumes/Disk0");
        else
            klog_warn("kernel: failed to expose FAT16 volume");
    }
    else
    {
        klog_warn("kernel: FAT16 image unavailable");
    }

    idt_init();
    klog_info("kernel: IDT configured");
    pic_init();
    klog_info("kernel: PIC configured");
    pit_init(250);
    klog_info("kernel: PIT started");
    ipc_system_init();
    klog_info("kernel: IPC system ready");
    devmgr_init();
    klog_info("kernel: device manager ready");
    module_system_init();
    klog_info("kernel: module system online");
    volmgr_init();
    klog_info("kernel: volume manager ready");
    process_system_init();
    klog_info("kernel: process system initialized");
    syscall_init();
    klog_info("kernel: syscall layer ready");
    if (process_create(user_init, PROC_STACK_SIZE) < 0)
    {
        vga_write_line("init process failed");
        klog_error("kernel: failed to create init process");
    }
    else
    {
        klog_info("kernel: init process spawned");
    }
    if (process_create_kernel(shell_task, PROC_STACK_SIZE) < 0)
        klog_error("kernel: failed to create shell thread");
    else
        klog_info("kernel: shell thread spawned");

    debug_publish_all();
    print_banner();
    __asm__ __volatile__("sti");
    klog_info("kernel: interrupts enabled");
    process_schedule();
}
