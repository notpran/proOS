#include "devmgr.h"

#include "klog.h"
#include "ramfs.h"

#include <stddef.h>

struct device_entry
{
    int used;
    struct device_node node;
};

static struct device_entry device_table[DEVMGR_MAX_DEVICES];
static size_t device_count = 0;
static uint32_t next_device_id = 1;
static struct device_node *root_device = NULL;

static size_t str_length(const char *s)
{
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        ++len;
    return len;
}

static void str_copy(char *dst, size_t dst_cap, const char *src)
{
    if (dst_cap == 0)
        return;
    size_t i = 0;
    if (src)
    {
        while (i + 1 < dst_cap && src[i])
        {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

static struct device_node *find_device_by_name(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < DEVMGR_MAX_DEVICES; ++i)
    {
        if (!device_table[i].used)
            continue;
        const struct device_node *node = &device_table[i].node;
        size_t idx = 0;
        while (node->name[idx] && name[idx] && node->name[idx] == name[idx])
            ++idx;
        if (node->name[idx] == '\0' && name[idx] == '\0')
            return &device_table[i].node;
    }
    return NULL;
}

static struct device_node *allocate_slot(size_t *out_index)
{
    for (size_t i = 0; i < DEVMGR_MAX_DEVICES; ++i)
    {
        if (!device_table[i].used)
        {
            device_table[i].used = 1;
            ++device_count;
            if (out_index)
                *out_index = i;
            return &device_table[i].node;
        }
    }
    return NULL;
}

static void release_slot(size_t index)
{
    if (index >= DEVMGR_MAX_DEVICES)
        return;
    if (!device_table[index].used)
        return;
    device_table[index].used = 0;
    device_table[index].node.id = 0;
    device_table[index].node.name[0] = '\0';
    device_table[index].node.type[0] = '\0';
    device_table[index].node.flags = 0;
    device_table[index].node.parent = NULL;
    device_table[index].node.driver_data = NULL;
    device_table[index].node.ops = NULL;
    if (device_count > 0)
        --device_count;
}

static void detach_children(struct device_node *old_parent, struct device_node *new_parent)
{
    if (!old_parent)
        return;
    for (size_t i = 0; i < DEVMGR_MAX_DEVICES; ++i)
    {
        if (!device_table[i].used)
            continue;
        if (device_table[i].node.parent == old_parent)
            device_table[i].node.parent = new_parent;
    }
}

static void publish_device(const struct device_node *node)
{
    if (!node)
        return;
    if (!(node->flags & DEVICE_FLAG_PUBLISH))
        return;
    if (node->flags & DEVICE_FLAG_INTERNAL)
        return;

    char path[RAMFS_MAX_NAME];
    const char prefix[] = "/dev/";
    size_t prefix_len = sizeof(prefix) - 1;
    size_t name_len = str_length(node->name);
    if (prefix_len + name_len + 1 > sizeof(path))
    {
        klog_warn("devmgr: device name too long for /dev");
        return;
    }

    size_t pos = 0;
    for (size_t i = 0; i < prefix_len; ++i)
        path[pos++] = prefix[i];
    for (size_t i = 0; i < name_len; ++i)
        path[pos++] = node->name[i];
    path[pos] = '\0';

    char payload[128];
    size_t w = 0;

    const char *name_key = "name: ";
    const char *type_key = "type: ";
    const char *id_key = "id: ";
    const char *parent_key = "parent: ";

    const char *keys[] = { name_key, node->name, "\n", type_key, node->type, "\n", id_key };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
    {
        const char *src = keys[i];
        size_t idx = 0;
        while (src && src[idx])
        {
            if (w + 1 >= sizeof(payload))
                break;
            payload[w++] = src[idx++];
        }
    }

    if (w + 1 < sizeof(payload))
    {
        uint32_t id = node->id;
        char digits[16];
        int digit_count = 0;
        if (id == 0)
        {
            digits[digit_count++] = '0';
        }
        else
        {
            while (id > 0 && digit_count < (int)sizeof(digits))
            {
                digits[digit_count++] = (char)('0' + (id % 10u));
                id /= 10u;
            }
        }
        for (int j = digit_count - 1; j >= 0 && w + 1 < sizeof(payload); --j)
            payload[w++] = digits[j];
        if (w + 1 < sizeof(payload))
            payload[w++] = '\n';
    }

    if (w + str_length(parent_key) + 2 < sizeof(payload))
    {
        const char *parent_name = (node->parent) ? node->parent->name : "root";
        for (size_t i = 0; parent_key[i] && w + 1 < sizeof(payload); ++i)
            payload[w++] = parent_key[i];
        for (size_t i = 0; parent_name && parent_name[i] && w + 1 < sizeof(payload); ++i)
            payload[w++] = parent_name[i];
        if (w + 1 < sizeof(payload))
            payload[w++] = '\n';
    }

    if (w >= sizeof(payload))
        w = sizeof(payload) - 1;
    payload[w] = '\0';

    if (ramfs_write_file(path, payload, w) < 0)
        klog_warn("devmgr: failed to publish device node");
}

static void unpublish_device(const struct device_node *node)
{
    if (!node)
        return;
    if (!(node->flags & DEVICE_FLAG_PUBLISH))
        return;
    if (node->flags & DEVICE_FLAG_INTERNAL)
        return;

    char path[RAMFS_MAX_NAME];
    const char prefix[] = "/dev/";
    size_t prefix_len = sizeof(prefix) - 1;
    size_t name_len = str_length(node->name);
    if (prefix_len + name_len + 1 > sizeof(path))
        return;

    size_t pos = 0;
    for (size_t i = 0; i < prefix_len; ++i)
        path[pos++] = prefix[i];
    for (size_t i = 0; i < name_len; ++i)
        path[pos++] = node->name[i];
    path[pos] = '\0';

    ramfs_remove(path);
}

static size_t device_depth(const struct device_node *node)
{
    size_t depth = 0;
    while (node && node != root_device)
    {
        node = node->parent;
        ++depth;
    }
    return depth;
}

void devmgr_refresh_ramfs(void)
{
    char listing[RAMFS_MAX_FILE_SIZE];
    size_t pos = 0;

    const char header[] = "Device Tree\n";
    for (size_t i = 0; header[i] && pos + 1 < sizeof(listing); ++i)
        listing[pos++] = header[i];

    for (size_t i = 0; i < DEVMGR_MAX_DEVICES; ++i)
    {
        if (!device_table[i].used)
            continue;
        const struct device_node *node = &device_table[i].node;
        if (node == root_device)
            continue;

        size_t depth = device_depth(node);
        for (size_t d = 0; d < depth && pos + 2 < sizeof(listing); ++d)
        {
            listing[pos++] = ' ';
            listing[pos++] = ' ';
        }

        if (pos + 2 < sizeof(listing))
        {
            listing[pos++] = '-';
            listing[pos++] = ' ';
        }

        size_t name_len = str_length(node->name);
        for (size_t n = 0; n < name_len && pos + 1 < sizeof(listing); ++n)
            listing[pos++] = node->name[n];

        const char open_paren[] = " (";
        for (size_t c = 0; open_paren[c] && pos + 1 < sizeof(listing); ++c)
            listing[pos++] = open_paren[c];

        size_t type_len = str_length(node->type);
        for (size_t t = 0; t < type_len && pos + 1 < sizeof(listing); ++t)
            listing[pos++] = node->type[t];

        if (pos + 1 < sizeof(listing))
            listing[pos++] = ')';
        if (pos + 1 < sizeof(listing))
            listing[pos++] = '\n';
    }

    if (pos >= sizeof(listing))
        pos = sizeof(listing) - 1;
    listing[pos] = '\0';

    if (ramfs_write_file("/dev/devices", listing, pos) < 0)
        klog_warn("devmgr: failed to publish device tree");
}

static struct device_node *create_internal_device(const char *name, const char *type, struct device_node *parent)
{
    size_t index = 0;
    struct device_node *slot = allocate_slot(&index);
    if (!slot)
        return NULL;
    slot->id = next_device_id++;
    str_copy(slot->name, sizeof(slot->name), name);
    str_copy(slot->type, sizeof(slot->type), type);
    slot->flags = DEVICE_FLAG_INTERNAL;
    slot->parent = parent ? parent : root_device;
    slot->driver_data = NULL;
    slot->ops = NULL;
    return slot;
}

void devmgr_init(void)
{
    for (size_t i = 0; i < DEVMGR_MAX_DEVICES; ++i)
    {
        device_table[i].used = 0;
        device_table[i].node.id = 0;
        device_table[i].node.name[0] = '\0';
        device_table[i].node.type[0] = '\0';
        device_table[i].node.flags = 0;
        device_table[i].node.parent = NULL;
        device_table[i].node.driver_data = NULL;
        device_table[i].node.ops = NULL;
    }
    device_count = 0;
    next_device_id = 1;
    size_t root_index = 0;
    root_device = allocate_slot(&root_index);
    if (!root_device)
        return;
    root_device->id = 0;
    str_copy(root_device->name, sizeof(root_device->name), "root");
    str_copy(root_device->type, sizeof(root_device->type), "root");
    root_device->flags = DEVICE_FLAG_INTERNAL;
    root_device->parent = NULL;
    root_device->driver_data = NULL;
    root_device->ops = NULL;

    create_internal_device("platform0", "bus.platform", root_device);
    create_internal_device("storage0", "bus.storage", root_device);

    devmgr_refresh_ramfs();
}

int devmgr_register_device(const struct device_descriptor *desc, struct device_node **out_node)
{
    if (!desc || !desc->name || !desc->type)
        return -1;

    if (find_device_by_name(desc->name))
        return -1;

    size_t slot_index = 0;
    struct device_node *slot = allocate_slot(&slot_index);
    if (!slot)
        return -1;

    slot->id = next_device_id++;
    str_copy(slot->name, sizeof(slot->name), desc->name);
    str_copy(slot->type, sizeof(slot->type), desc->type);
    slot->flags = desc->flags;
    slot->driver_data = desc->driver_data;
    slot->ops = desc->ops;

    if (desc->parent)
    {
        struct device_node *parent = find_device_by_name(desc->parent);
        slot->parent = parent ? parent : root_device;
    }
    else
    {
        slot->parent = root_device;
    }

    if (slot->ops && slot->ops->start)
    {
        int rc = slot->ops->start(slot);
        if (rc < 0)
        {
            release_slot(slot_index);
            return -1;
        }
    }

    publish_device(slot);
    devmgr_refresh_ramfs();

    if (out_node)
        *out_node = slot;

    klog_info("devmgr: device registered");
    return 0;
}

int devmgr_unregister_device(const char *name)
{
    if (!name)
        return -1;

    struct device_node *target = find_device_by_name(name);
    if (!target)
        return -1;

    for (size_t i = 0; i < DEVMGR_MAX_DEVICES; ++i)
    {
        if (!device_table[i].used)
            continue;
        if (&device_table[i].node != target)
            continue;

        struct device_node *node = &device_table[i].node;
        if (node == root_device)
            return -1;

        if (node->ops && node->ops->stop)
            node->ops->stop(node);

        unpublish_device(node);
        detach_children(node, node->parent ? node->parent : root_device);
        release_slot(i);
        devmgr_refresh_ramfs();
        klog_info("devmgr: device unregistered");
        return 0;
    }

    return -1;
}

size_t devmgr_enumerate(const struct device_node **out, size_t max)
{
    if (!out || max == 0)
        return 0;

    size_t count = 0;
    for (size_t i = 0; i < DEVMGR_MAX_DEVICES && count < max; ++i)
    {
        if (!device_table[i].used)
            continue;
        out[count++] = &device_table[i].node;
    }
    return count;
}

const struct device_node *devmgr_find(const char *name)
{
    return find_device_by_name(name);
}
