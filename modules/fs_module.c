#include <stddef.h>

#include "module_api.h"

#include "fat16.h"
#include "klog.h"
#include "vfs.h"
#include "vbe.h"

MODULE_METADATA("fs", "0.2.0", MODULE_FLAG_AUTOSTART);

static size_t local_strlen(const char *s)
{
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        ++len;
    return len;
}

static void publish_directory_listing(void)
{
    char listing[768];
    int written = fat16_ls(listing, sizeof(listing));
    if (written <= 0)
    {
        klog_warn("fs.module: fat16_ls failed");
        return;
    }

    if (vfs_write("/fat/list", listing, (size_t)written) < 0)
        klog_warn("fs.module: vfs_write fat16.dir failed");
}

static void try_load_font(void)
{
    if (!vbe_try_load_font_from_fat())
    {
        klog_warn("fs.module: font load skipped");
        return;
    }

    const char *note = "font: loaded from FAT16\n";
    if (vfs_write("/System/font.status", note, local_strlen(note)) < 0)
        klog_warn("fs.module: vfs_write font.status failed");
    else
        klog_info("fs.module: font loaded");
}

int module_init(void)
{
    klog_info("fs.module: init");

    if (!fat16_ready())
    {
        klog_warn("fs.module: FAT16 unavailable");
        return 0;
    }

    publish_directory_listing();
    try_load_font();
    return 0;
}

void module_exit(void)
{
    klog_info("fs.module: exit");
}
