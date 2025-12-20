#include "module.h"

#include "klog.h"
#include "ramfs.h"
#include "vfs.h"
#include "proc.h"
#include "ipc.h"
#include "pit.h"
#include "fat16.h"
#include "keyboard.h"
#include "vbe.h"
#include "devmgr.h"
#include "interrupts.h"
#include "syscall.h"

static const struct kernel_symbol builtin_symbols[] = {
    { "klog_emit", (uintptr_t)&klog_emit },
    { "klog_emit_tagged", (uintptr_t)&klog_emit_tagged },
    { "ramfs_write", (uintptr_t)&ramfs_write },
    { "ramfs_read", (uintptr_t)&ramfs_read },
    { "ramfs_write_file", (uintptr_t)&ramfs_write_file },
    { "ramfs_remove", (uintptr_t)&ramfs_remove },
    { "vfs_write", (uintptr_t)&vfs_write },
    { "vfs_write_file", (uintptr_t)&vfs_write_file },
    { "vfs_read", (uintptr_t)&vfs_read },
    { "vfs_list", (uintptr_t)&vfs_list },
    { "vfs_remove", (uintptr_t)&vfs_remove },
    { "ipc_channel_create", (uintptr_t)&ipc_channel_create },
    { "ipc_channel_join", (uintptr_t)&ipc_channel_join },
    { "ipc_channel_leave", (uintptr_t)&ipc_channel_leave },
    { "ipc_channel_send", (uintptr_t)&ipc_channel_send },
    { "ipc_channel_receive", (uintptr_t)&ipc_channel_receive },
    { "ipc_get_service_channel", (uintptr_t)&ipc_get_service_channel },
    { "process_create", (uintptr_t)&process_create },
    { "process_create_kernel", (uintptr_t)&process_create_kernel },
    { "get_ticks", (uintptr_t)&get_ticks },
    { "pit_init", (uintptr_t)&pit_init },
    { "fat16_ready", (uintptr_t)&fat16_ready },
    { "fat16_ls", (uintptr_t)&fat16_ls },
    { "kb_init", (uintptr_t)&kb_init },
    { "kb_getchar", (uintptr_t)&kb_getchar },
    { "kb_dump_layout", (uintptr_t)&kb_dump_layout },
    { "process_yield", (uintptr_t)&process_yield },
    { "process_sleep", (uintptr_t)&process_sleep },
    { "vbe_try_load_font_from_fat", (uintptr_t)&vbe_try_load_font_from_fat },
    { "devmgr_register_device", (uintptr_t)&devmgr_register_device },
    { "devmgr_unregister_device", (uintptr_t)&devmgr_unregister_device },
    { "devmgr_enumerate", (uintptr_t)&devmgr_enumerate },
    { "devmgr_find", (uintptr_t)&devmgr_find },
    { "devmgr_refresh_ramfs", (uintptr_t)&devmgr_refresh_ramfs },
    { "irq_register_shared_handler", (uintptr_t)&irq_register_shared_handler },
    { "irq_unregister_shared_handler", (uintptr_t)&irq_unregister_shared_handler },
    { "irq_mailbox_init", (uintptr_t)&irq_mailbox_init },
    { "irq_mailbox_subscribe", (uintptr_t)&irq_mailbox_subscribe },
    { "irq_mailbox_unsubscribe", (uintptr_t)&irq_mailbox_unsubscribe },
    { "irq_mailbox_receive", (uintptr_t)&irq_mailbox_receive },
    { "irq_mailbox_peek", (uintptr_t)&irq_mailbox_peek },
    { "irq_mailbox_flush", (uintptr_t)&irq_mailbox_flush },
    { "syscall_register_handler", (uintptr_t)&syscall_register_handler },
    { "syscall_unregister_handler", (uintptr_t)&syscall_unregister_handler },
    { "syscall_validate_user_buffer", (uintptr_t)&syscall_validate_user_buffer },
    { "syscall_validate_user_pointer", (uintptr_t)&syscall_validate_user_pointer }
};

void module_register_builtin_symbols(void)
{
    module_register_kernel_symbols(builtin_symbols, sizeof(builtin_symbols) / sizeof(builtin_symbols[0]));
}
