#include <stdint.h>

#include "vga.h"
#include "ramfs.h"
#include "interrupts.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "proc.h"
#include "syscall.h"
#include "memory.h"
#include "vbe.h"
#include "fat16.h"
#include "gfx.h"
#include "klog.h"

extern void shell_run(void);
extern void user_init(void);

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
    vbe_init();
    vga_init();
    vga_clear();
    klog_init();
    klog_info("kernel: video initialized");
    memory_init();
    klog_info("kernel: memory initialized");
    ramfs_init();
    klog_info("kernel: ramfs ready");

    const struct boot_info *info = boot_info_get();
    int fat_ok = 0;
    if (info && info->fat_ptr && info->fat_size)
    {
        fat_ok = fat16_init((const void *)(uintptr_t)info->fat_ptr, (size_t)info->fat_size);
    }

    if (fat_ok)
    {
        vbe_try_load_font_from_fat();
        klog_info("kernel: FAT16 font loaded");
    }
    else
    {
        klog_warn("kernel: FAT16 image unavailable");
    }

    idt_init();
    klog_info("kernel: IDT configured");
    pic_init();
    klog_info("kernel: PIC configured");
    pit_init(100);
    klog_info("kernel: PIT started");
    kb_init();
    klog_info("kernel: keyboard ready");
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
    print_banner();
    __asm__ __volatile__("sti");
    klog_info("kernel: interrupts enabled");
    process_schedule();
    klog_info("kernel: scheduler relinquished");
    shell_run();

    for (;;)
    {
        __asm__ __volatile__("hlt");
    }
}
