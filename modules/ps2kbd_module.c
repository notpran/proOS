#include <stddef.h>

#include "module_api.h"

#include "devmgr.h"
#include "klog.h"
#include "keyboard.h"
#include "ramfs.h"

MODULE_METADATA("ps2kbd", "0.1.0", MODULE_FLAG_AUTOSTART);

static size_t local_strlen(const char *s)
{
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        ++len;
    return len;
}

static int keyboard_start(struct device_node *node)
{
    (void)node;
    kb_init();
    klog_info("ps2kbd.driver: keyboard controller initialized");
    return 0;
}

static void keyboard_stop(struct device_node *node)
{
    (void)node;
    klog_info("ps2kbd.driver: keyboard controller shutdown");
}

static int keyboard_read(struct device_node *node, void *buffer, size_t length, size_t *out_read)
{
    (void)node;
    if (!buffer || length == 0)
        return -1;

    char *out = (char *)buffer;
    size_t produced = 0;
    while (produced < length)
    {
        char c = kb_getchar();
        if (!c)
            break;
        out[produced++] = c;
    }

    if (out_read)
        *out_read = produced;
    return (produced > 0) ? 0 : -1;
}

static const struct device_ops ps2kbd_ops = {
    keyboard_start,
    keyboard_stop,
    keyboard_read,
    NULL,
    NULL
};

static const struct device_ops ps2ctrl_ops = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

int module_init(void)
{
    const struct device_node *controller = devmgr_find("ps2ctrl0");
    if (!controller)
    {
        struct device_descriptor ctrl_desc = {
            "ps2ctrl0",
            "bus.ps2",
            "platform0",
            &ps2ctrl_ops,
            DEVICE_FLAG_INTERNAL,
            NULL
        };
        if (devmgr_register_device(&ctrl_desc, NULL) < 0)
        {
            klog_error("ps2kbd.driver: failed to register controller");
            return -1;
        }
    }

    struct device_descriptor desc = {
        "ps2kbd0",
        "input.keyboard",
        "ps2ctrl0",
        &ps2kbd_ops,
        DEVICE_FLAG_PUBLISH,
        NULL
    };

    if (devmgr_register_device(&desc, NULL) < 0)
    {
        klog_error("ps2kbd.driver: failed to register device");
        return -1;
    }

    const char *status = "keyboard: ready\n";
    size_t status_len = local_strlen(status);
    ramfs_write_file("/dev/ps2kbd0.status", status, status_len);

    char layout[512];
    int written = kb_dump_layout(layout, sizeof(layout));
    if (written > 0)
        ramfs_write_file("/dev/ps2kbd0.map", layout, (size_t)written);

    return 0;
}

void module_exit(void)
{
    devmgr_unregister_device("ps2kbd0");
    ramfs_remove("/dev/ps2kbd0.status");
    ramfs_remove("/dev/ps2kbd0.map");
}
