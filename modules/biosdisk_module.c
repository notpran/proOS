#include <stddef.h>

#include "module_api.h"

#include "devmgr.h"
#include "klog.h"
#include "ramfs.h"

static size_t local_strlen(const char *s)
{
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        ++len;
    return len;
}

MODULE_METADATA("biosdisk", "0.1.0", MODULE_FLAG_AUTOSTART);

static int biosdisk_start(struct device_node *node)
{
    (void)node;
    klog_warn("biosdisk.driver: INT13 fallback not implemented");
    return 0;
}

static int biosdisk_read(struct device_node *node, void *buffer, size_t length, size_t *out_read)
{
    (void)node;
    (void)buffer;
    (void)length;
    if (out_read)
        *out_read = 0;
    return -1;
}

static const struct device_ops biosdisk_ops = {
    biosdisk_start,
    NULL,
    biosdisk_read,
    NULL,
    NULL
};

int module_init(void)
{
    struct device_descriptor desc = {
        "biosdisk0",
        "storage.bios",
        "storage0",
        &biosdisk_ops,
        DEVICE_FLAG_PUBLISH,
        NULL
    };

    if (devmgr_register_device(&desc, NULL) < 0)
    {
        klog_error("biosdisk.driver: registration failed");
        return -1;
    }

    const char *status = "biosdisk: stub driver\n";
    ramfs_write_file("/dev/biosdisk0.status", status, local_strlen(status));

    return 0;
}

void module_exit(void)
{
    devmgr_unregister_device("biosdisk0");
    ramfs_remove("/dev/biosdisk0.status");
}
