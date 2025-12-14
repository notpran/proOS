#include <stddef.h>

#include "module_api.h"

#include "devmgr.h"
#include "klog.h"
#include "ramfs.h"

MODULE_METADATA("ps2mouse", "0.1.0", MODULE_FLAG_AUTOSTART);

static size_t local_strlen(const char *s)
{
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        ++len;
    return len;
}

static const struct device_ops ps2mouse_ops = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

int module_init(void)
{
    if (!devmgr_find("ps2ctrl0"))
    {
        struct device_descriptor ctrl_desc = {
            "ps2ctrl0",
            "bus.ps2",
            "platform0",
            NULL,
            DEVICE_FLAG_INTERNAL,
            NULL
        };
        if (devmgr_register_device(&ctrl_desc, NULL) < 0)
        {
            klog_warn("ps2mouse.driver: controller registration failed");
            return -1;
        }
    }

    struct device_descriptor desc = {
        "ps2mouse0",
        "input.mouse",
        "ps2ctrl0",
        &ps2mouse_ops,
        DEVICE_FLAG_PUBLISH,
        NULL
    };

    if (devmgr_register_device(&desc, NULL) < 0)
    {
        klog_warn("ps2mouse.driver: device registration failed");
        return -1;
    }

    klog_info("ps2mouse.driver: registered (stub)");

    const char *status = "ps2mouse: stub driver\n";
    ramfs_write_file("/dev/ps2mouse0.status", status, local_strlen(status));
    return 0;
}

void module_exit(void)
{
    devmgr_unregister_device("ps2mouse0");
    ramfs_remove("/dev/ps2mouse0.status");
}
