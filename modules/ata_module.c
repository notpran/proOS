#include <stddef.h>
#include <stdint.h>

#include "module_api.h"

#include "devmgr.h"
#include "klog.h"
#include "ramfs.h"

MODULE_METADATA("ata", "0.1.0", MODULE_FLAG_AUTOSTART);

static size_t local_strlen(const char *s)
{
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        ++len;
    return len;
}

static int ata_start(struct device_node *node)
{
    (void)node;
    klog_warn("ata.driver: probing skipped (stub)");
    return 0;
}

static int ata_read(struct device_node *node, void *buffer, size_t length, size_t *out_read)
{
    (void)node;
    (void)buffer;
    (void)length;
    if (out_read)
        *out_read = 0;
    return -1;
}

static int ata_ioctl(struct device_node *node, uint32_t request, void *arg)
{
    (void)node;
    (void)request;
    (void)arg;
    return -1;
}

static const struct device_ops ata_ops = {
    ata_start,
    NULL,
    ata_read,
    NULL,
    ata_ioctl
};

int module_init(void)
{
    const char *status = "ata: stub driver\n";
    struct device_descriptor desc = {
        "ata0",
        "storage.ata",
        "storage0",
        &ata_ops,
        DEVICE_FLAG_PUBLISH,
        NULL
    };

    if (devmgr_register_device(&desc, NULL) < 0)
    {
        klog_error("ata.driver: registration failed");
        return -1;
    }

    ramfs_write_file("/dev/ata0.status", status, local_strlen(status));

    return 0;
}

void module_exit(void)
{
    devmgr_unregister_device("ata0");
    ramfs_remove("/dev/ata0.status");
}
