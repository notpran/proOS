#ifndef DEVMGR_H
#define DEVMGR_H

#include <stddef.h>
#include <stdint.h>

#define DEVMGR_MAX_DEVICES 32
#define DEVMGR_NAME_MAX 32
#define DEVMGR_TYPE_MAX 32

enum device_flags
{
    DEVICE_FLAG_PUBLISH = 1u << 0,
    DEVICE_FLAG_INTERNAL = 1u << 1
};

struct device_node;

struct device_ops
{
    int (*start)(struct device_node *node);
    void (*stop)(struct device_node *node);
    int (*read)(struct device_node *node, void *buffer, size_t length, size_t *out_read);
    int (*write)(struct device_node *node, const void *buffer, size_t length, size_t *out_written);
    int (*ioctl)(struct device_node *node, uint32_t request, void *arg);
};

struct device_descriptor
{
    const char *name;
    const char *type;
    const char *parent;
    const struct device_ops *ops;
    uint32_t flags;
    void *driver_data;
};

struct device_node
{
    uint32_t id;
    char name[DEVMGR_NAME_MAX];
    char type[DEVMGR_TYPE_MAX];
    uint32_t flags;
    struct device_node *parent;
    void *driver_data;
    const struct device_ops *ops;
};

void devmgr_init(void);
int devmgr_register_device(const struct device_descriptor *desc, struct device_node **out_node);
int devmgr_unregister_device(const char *name);
size_t devmgr_enumerate(const struct device_node **out, size_t max);
const struct device_node *devmgr_find(const char *name);
void devmgr_refresh_ramfs(void);

#endif
