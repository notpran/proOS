#include <stddef.h>
#include <stdint.h>

#include "module_api.h"

#include "devmgr.h"
#include "klog.h"
#include "pit.h"
#include "vfs.h"

MODULE_METADATA("pit", "0.1.0", MODULE_FLAG_AUTOSTART);

static size_t local_strlen(const char *s)
{
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        ++len;
    return len;
}

static int pit_start(struct device_node *node)
{
    (void)node;
    pit_init(100);
    klog_info("pit.driver: configured at 100 Hz");
    return 0;
}

static void pit_stop(struct device_node *node)
{
    (void)node;
    klog_info("pit.driver: stopped");
}

static int pit_read(struct device_node *node, void *buffer, size_t length, size_t *out_read)
{
    (void)node;
    if (!buffer || length < sizeof(uint64_t))
        return -1;

    uint64_t ticks = get_ticks();
    uint8_t *dst = (uint8_t *)buffer;
    for (size_t i = 0; i < sizeof(uint64_t); ++i)
        dst[i] = (uint8_t)((ticks >> (i * 8u)) & 0xFFu);

    if (out_read)
        *out_read = sizeof(uint64_t);
    return 0;
}

static const struct device_ops pit_ops = {
    pit_start,
    pit_stop,
    pit_read,
    NULL,
    NULL
};

int module_init(void)
{
    struct device_descriptor desc = {
        "pit0",
        "timer.pit",
        "platform0",
        &pit_ops,
        DEVICE_FLAG_PUBLISH,
        NULL
    };

    if (devmgr_register_device(&desc, NULL) < 0)
    {
        klog_error("pit.driver: registration failed");
        return -1;
    }

    const char *status = "pit: 100Hz\n";
    vfs_write_file("/Devices/pit0.status", status, local_strlen(status));

    return 0;
}

void module_exit(void)
{
    devmgr_unregister_device("pit0");
    vfs_remove("/Devices/pit0.status");
}
