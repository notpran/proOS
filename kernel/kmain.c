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
#include "fatfs.h"
#include "gfx.h"
#include "klog.h"
#include "module.h"
#include "devmgr.h"
#include "ipc.h"
#include "pit.h"
#include "debug.h"
#include "sync.h"
#include "blockdev.h"
#include "partition.h"
#include "volmgr.h"
#include "bios_fallback.h"
#include "service.h"
#include "service_types.h"
#include "net.h"
#include "e1000.h"

extern void shell_run(void);
extern void user_init(void);
extern void user_fsd(void);
extern void user_netd(void);
extern void user_inputd(void);
extern void user_logd(void);

#define EXTRA_FAT_DISKS 2

static struct fatfs_volume extra_fat_volumes[EXTRA_FAT_DISKS];
static void *extra_fat_buffers[EXTRA_FAT_DISKS];

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
    vga_write_line("version: v0.8 b2");
    vga_set_color(0x7, 0x0);
    vga_write_line("Type 'help' to list commands.");
    vga_write_char('\n');
}

static void make_disk_label(char *buffer, size_t cap, unsigned int index)
{
    if (!buffer || cap == 0)
        return;

    const char prefix[] = "Disk";
    size_t pos = 0;
    while (prefix[pos] && pos + 1 < cap)
    {
        buffer[pos] = prefix[pos];
        ++pos;
    }

    char tmp[12];
    size_t len = 0;
    unsigned int value = index;
    if (value == 0)
    {
        tmp[len++] = '0';
    }
    else
    {
        while (value > 0 && len < sizeof(tmp))
        {
            tmp[len++] = (char)('0' + (value % 10u));
            value /= 10u;
        }
    }

    while (len > 0 && pos + 1 < cap)
        buffer[pos++] = tmp[--len];

    buffer[pos] = '\0';
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, size_t length)
{
    if (!dst || !src)
        return;
    for (size_t i = 0; i < length; ++i)
        dst[i] = src[i];
}

static size_t append_text(char *dst, size_t pos, size_t cap, const char *text)
{
    if (!text)
        return pos;
    while (*text && pos + 1 < cap)
    {
        dst[pos++] = *text++;
    }
    dst[pos] = '\0';
    return pos;
}

static size_t append_u32_dec(char *dst, size_t pos, size_t cap, uint32_t value)
{
    char temp[12];
    size_t idx = 0;
    if (value == 0)
        temp[idx++] = '0';
    else
    {
        while (value > 0 && idx < sizeof(temp))
        {
            temp[idx++] = (char)('0' + (value % 10u));
            value /= 10u;
        }
    }
    while (idx > 0 && pos + 1 < cap)
    {
        dst[pos++] = temp[--idx];
    }
    dst[pos] = '\0';
    return pos;
}

static size_t append_u32_hex(char *dst, size_t pos, size_t cap, uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    char temp[8];
    for (size_t i = 0; i < sizeof(temp); ++i)
    {
        temp[i] = hex[value & 0x0Fu];
        value >>= 4;
    }
    pos = append_text(dst, pos, cap, "0x");
    for (size_t i = 0; i < sizeof(temp) && pos + 1 < cap; ++i)
    {
        dst[pos++] = temp[sizeof(temp) - 1u - i];
    }
    dst[pos] = '\0';
    return pos;
}

static void log_vbe_bootinfo(void)
{
    const struct boot_info *info = boot_info_get();
    if (!info)
    {
        klog_warn("kernel: boot_info missing");
        return;
    }
    if (info->magic != BOOT_INFO_MAGIC)
    {
        klog_warn("kernel: boot_info magic invalid");
        return;
    }

    char line[128];
    size_t pos = 0;
    pos = append_text(line, pos, sizeof(line), "vbe fb_ptr=");
    pos = append_u32_hex(line, pos, sizeof(line), info->fb_ptr);
    pos = append_text(line, pos, sizeof(line), " pitch=");
    pos = append_u32_dec(line, pos, sizeof(line), info->fb_pitch);
    pos = append_text(line, pos, sizeof(line), " width=");
    pos = append_u32_dec(line, pos, sizeof(line), info->fb_width);
    pos = append_text(line, pos, sizeof(line), " height=");
    pos = append_u32_dec(line, pos, sizeof(line), info->fb_height);
    pos = append_text(line, pos, sizeof(line), " bpp=");
    pos = append_u32_dec(line, pos, sizeof(line), info->fb_bpp);
    klog_info(line);
}

static void init_extra_fat_disks(const struct boot_info *info)
{
    if (!info || info->fat_ptr == 0u || info->fat_size == 0u)
        return;

    const uint8_t *source = (const uint8_t *)(uintptr_t)info->fat_ptr;
    size_t size = (size_t)info->fat_size;

    for (unsigned int i = 0; i < EXTRA_FAT_DISKS; ++i)
    {
        if (extra_fat_buffers[i])
            continue;

        uint8_t *buffer = (uint8_t *)kalloc(size);
        if (!buffer)
        {
            klog_warn("kernel: unable to allocate memory for extra FAT disk");
            break;
        }

        copy_bytes(buffer, source, size);

        int type = fatfs_init(&extra_fat_volumes[i], buffer, size);
        if (type == FATFS_TYPE_NONE)
        {
            klog_warn("kernel: extra FAT image unsupported");
            continue;
        }

        char label[16];
        make_disk_label(label, sizeof(label), i + 1u);
        if (fatfs_mount(&extra_fat_volumes[i], label) < 0)
        {
            klog_warn("kernel: failed to mount extra FAT volume");
            continue;
        }

        extra_fat_buffers[i] = buffer;
    }
}

void kmain(void)
{
    memory_init();
    blockdev_init();
    partition_init();

    const struct boot_info *info = boot_info_get();
    uint8_t boot_drive = 0x80u;
    if (info && info->boot_drive)
        boot_drive = (uint8_t)info->boot_drive;
    bios_fallback_init(boot_drive);

    vbe_init();
    vga_init();
    vga_clear();
    service_system_init();
    klog_init();
    klog_info("kernel: video initialized");
    log_vbe_bootinfo();
    klog_info("kernel: memory initialized");
    if (vfs_init() < 0)
        klog_error("kernel: vfs initialization failed");
    else
        klog_info("kernel: vfs ready");

    int fat_ok = 0;
    int fat_type = FATFS_TYPE_NONE;
    if (info && info->fat_ptr && info->fat_size)
    {
        fat_ok = fat16_init((const void *)(uintptr_t)info->fat_ptr, (size_t)info->fat_size);
        if (fat_ok)
        {
            fat16_configure_backing(info->fat_lba, info->fat_sectors);
            fat_type = fat16_type();
        }
    }

    if (fat_ok)
    {
        if (fat_type == FATFS_TYPE_FAT32)
            klog_info("kernel: FAT32 image detected");
        else
            klog_info("kernel: FAT16 image detected");
        if (fat16_mount_volume("Disk0") == 0)
        {
            klog_info("kernel: FAT volume available at /Volumes/Disk0");
            init_extra_fat_disks(info);
        }
        else
            klog_warn("kernel: failed to expose FAT volume");
    }
    else
    {
        klog_warn("kernel: FAT volume unavailable");
    }

    idt_init();
    debug_trap_init();
    klog_info("kernel: IDT configured");
    pic_init();
    klog_info("kernel: PIC configured");
    pit_init(250);
    klog_info("kernel: PIT started");
    klog_info("kernel: service manager ready");
    ipc_system_init();
    klog_info("kernel: IPC system ready");
    devmgr_init();
    klog_info("kernel: device manager ready");

    net_init();

    e1000_driver_init();
    module_system_init();
    klog_info("kernel: module system online");
    volmgr_init();
    klog_info("kernel: volume manager ready");
    process_system_init();
    klog_info("kernel: process system initialized");
    sync_init();
    klog_info("kernel: sync primitives ready");
    syscall_init();
    klog_info("kernel: syscall layer ready");

    uint32_t svc_rights = IPC_RIGHT_SEND | IPC_RIGHT_RECV;
    service_register(SYSTEM_SERVICE_FSD, "fsd", user_fsd, svc_rights);
    service_register(SYSTEM_SERVICE_NETD, "netd", user_netd, svc_rights);
    service_register(SYSTEM_SERVICE_INPUTD, "inputd", user_inputd, svc_rights);
    service_register(SYSTEM_SERVICE_LOGD, "logd", user_logd, svc_rights);
    service_bootstrap();
    klog_info("kernel: services launched");
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
