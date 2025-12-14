#include "module.h"

#include "klog.h"
#include "ramfs.h"
#include "proc.h"
#include "pit.h"
#include "fat16.h"
#include "keyboard.h"
#include "vbe.h"
#include "devmgr.h"

static const struct kernel_symbol builtin_symbols[] = {
    { "klog_emit", (uintptr_t)&klog_emit },
    { "ramfs_write", (uintptr_t)&ramfs_write },
    { "ramfs_read", (uintptr_t)&ramfs_read },
    { "ramfs_write_file", (uintptr_t)&ramfs_write_file },
    { "ramfs_remove", (uintptr_t)&ramfs_remove },
    { "ipc_send", (uintptr_t)&ipc_send },
    { "process_create", (uintptr_t)&process_create },
    { "get_ticks", (uintptr_t)&get_ticks },
    { "pit_init", (uintptr_t)&pit_init },
    { "fat16_ready", (uintptr_t)&fat16_ready },
    { "fat16_ls", (uintptr_t)&fat16_ls },
    { "kb_init", (uintptr_t)&kb_init },
    { "kb_getchar", (uintptr_t)&kb_getchar },
    { "kb_dump_layout", (uintptr_t)&kb_dump_layout },
    { "process_yield", (uintptr_t)&process_yield },
    { "vbe_try_load_font_from_fat", (uintptr_t)&vbe_try_load_font_from_fat },
    { "devmgr_register_device", (uintptr_t)&devmgr_register_device },
    { "devmgr_unregister_device", (uintptr_t)&devmgr_unregister_device },
    { "devmgr_enumerate", (uintptr_t)&devmgr_enumerate },
    { "devmgr_find", (uintptr_t)&devmgr_find },
    { "devmgr_refresh_ramfs", (uintptr_t)&devmgr_refresh_ramfs }
};

void module_register_builtin_symbols(void)
{
    module_register_kernel_symbols(builtin_symbols, sizeof(builtin_symbols) / sizeof(builtin_symbols[0]));
}
