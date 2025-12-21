#include "vfs.h"
#include "ramfs.h"
#include "klog.h"
#include "string.h"
#include "debug.h"

struct vfs_mount
{
    int used;
    char mount_point[VFS_MAX_PATH];
    size_t prefix_len;
    const struct vfs_fs_ops *ops;
    void *ctx;
};

static struct vfs_mount mount_table[VFS_MAX_MOUNTS];
static struct vfs_mount *root_mount = NULL;
static struct ramfs_volume system_volume;
static struct ramfs_volume volumes_volume;
static struct ramfs_volume users_volume;
static struct ramfs_volume apps_volume;
static struct ramfs_volume temp_volume;
static struct ramfs_volume devices_volume;

static int mounts_initialized = 0;

static size_t local_strlen(const char *s)
{
    return s ? strlen(s) : 0;
}

static int local_strncmp(const char *a, const char *b, size_t n)
{
    while (n > 0)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca != cb)
            return (int)ca - (int)cb;
        if (ca == 0)
            break;
        --n;
    }
    return 0;
}

static int normalize_path(const char *input, char *out, size_t capacity)
{
    if (!input || !out || capacity < 2)
        return -1;
    if (input[0] == '\0')
        return -1;
    if (input[0] != '/')
        return -1;

    const char *segments[32];
    size_t seg_lengths[32];
    size_t seg_count = 0;
    size_t i = 0;

    while (input[i])
    {
        while (input[i] == '/')
            ++i;
        if (input[i] == '\0')
            break;
        size_t start = i;
        while (input[i] && input[i] != '/')
            ++i;
        size_t len = i - start;
        if (len == 0)
            continue;
        if (len == 1 && input[start] == '.')
            continue;
        if (len == 2 && input[start] == '.' && input[start + 1] == '.')
        {
            if (seg_count > 0)
                --seg_count;
            continue;
        }
        if (seg_count >= 32)
            return -1;
        segments[seg_count] = &input[start];
        seg_lengths[seg_count] = len;
        ++seg_count;
    }

    size_t out_pos = 0;
    if (seg_count == 0)
    {
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }

    for (size_t seg = 0; seg < seg_count; ++seg)
    {
        size_t len = seg_lengths[seg];
        if (out_pos + len + 1 >= capacity)
            return -1;
        out[out_pos++] = '/';
        for (size_t j = 0; j < len; ++j)
            out[out_pos++] = segments[seg][j];
    }
    out[out_pos] = '\0';
    return 0;
}

static struct vfs_mount *acquire_mount_slot(void)
{
    for (size_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!mount_table[i].used)
            return &mount_table[i];
    }
    return NULL;
}

static struct vfs_mount *resolve_mount(const char *path, const char **relative_out)
{
    if (!path)
        return NULL;

    struct vfs_mount *best = NULL;
    size_t best_len = 0;

    for (size_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!mount_table[i].used || !mount_table[i].ops)
            continue;

        size_t len = mount_table[i].prefix_len;
        if (len == 0)
            continue;

        if (len == 1)
        {
            if (path[0] != '/')
                continue;
        }
        else
        {
            if (local_strncmp(path, mount_table[i].mount_point, len) != 0)
                continue;
            char next = path[len];
            if (!(next == '\0' || next == '/'))
                continue;
        }

        if (!best || len > best_len)
        {
            best = &mount_table[i];
            best_len = len;
        }
    }

    if (!best)
        return NULL;

    if (relative_out)
    {
        const char *relative = path + best->prefix_len;
        if (*relative == '/')
            ++relative;
        if (!relative || *relative == '\0')
            relative = "";
        *relative_out = relative;
    }

    return best;
}

static int path_has_separator(const char *path)
{
    if (!path)
        return 0;
    while (*path)
    {
        if (*path == '/')
            return 1;
        ++path;
    }
    return 0;
}

static int ramfs_list_adapter(void *ctx, const char *path, char *buffer, size_t buffer_size)
{
    struct ramfs_volume *volume = (struct ramfs_volume *)ctx;
    if (!volume)
        return -1;
    if (path && path[0] != '\0')
        return -1;
    return ramfs_volume_list(volume, buffer, buffer_size);
}

static int ramfs_read_adapter(void *ctx, const char *path, char *buffer, size_t buffer_size)
{
    struct ramfs_volume *volume = (struct ramfs_volume *)ctx;
    if (!volume || !path)
        return -1;
    if (path[0] == '\0')
        return -1;
    if (path_has_separator(path))
        return -1;
    return ramfs_volume_read(volume, path, buffer, buffer_size);
}

static int ramfs_write_adapter(void *ctx, const char *path, const char *data, size_t length, enum vfs_write_mode mode)
{
    struct ramfs_volume *volume = (struct ramfs_volume *)ctx;
    if (!volume || !path)
        return -1;
    if (path[0] == '\0')
        return -1;
    if (path_has_separator(path))
        return -1;
    if (mode == VFS_WRITE_REPLACE)
        return ramfs_volume_write(volume, path, data, length);
    return ramfs_volume_append(volume, path, data, length);
}

static int ramfs_remove_adapter(void *ctx, const char *path)
{
    struct ramfs_volume *volume = (struct ramfs_volume *)ctx;
    if (!volume || !path || path[0] == '\0')
        return -1;
    if (path_has_separator(path))
        return -1;
    return ramfs_volume_remove(volume, path);
}

static const struct vfs_fs_ops ramfs_ops = {
    .list = ramfs_list_adapter,
    .read = ramfs_read_adapter,
    .write = ramfs_write_adapter,
    .remove = ramfs_remove_adapter
};

int vfs_mount(const char *mount_point, const struct vfs_fs_ops *ops, void *ctx)
{
    if (!mount_point || !ops)
        return -1;

    char normalized[VFS_MAX_PATH];
    if (normalize_path(mount_point, normalized, sizeof(normalized)) < 0)
        return -1;

    size_t path_len = local_strlen(normalized);
    if (path_len == 0)
        return -1;

    for (size_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!mount_table[i].used)
            continue;
        if (local_strlen(mount_table[i].mount_point) == path_len && local_strncmp(mount_table[i].mount_point, normalized, path_len) == 0)
            return -1;
    }

    struct vfs_mount *slot = acquire_mount_slot();
    if (!slot)
        return -1;

    slot->used = 1;
    for (size_t i = 0; i < path_len + 1 && i < sizeof(slot->mount_point); ++i)
        slot->mount_point[i] = normalized[i];
    slot->mount_point[path_len] = '\0';
    slot->prefix_len = path_len;
    slot->ops = ops;
    slot->ctx = ctx;

    if (path_len == 1 && normalized[0] == '/')
        root_mount = slot;

    return 0;
}

static const char *safe_relative(const char *relative)
{
    if (!relative)
        return "";
    return relative;
}

int vfs_list(const char *path, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0)
        return -1;

    const char *effective = path;
    if (!effective || effective[0] == '\0')
        effective = "/";

    char normalized[VFS_MAX_PATH];
    if (normalize_path(effective, normalized, sizeof(normalized)) < 0)
        return -1;

    const char *relative = NULL;
    struct vfs_mount *mount = resolve_mount(normalized, &relative);
    if (!mount || !mount->ops || !mount->ops->list)
        return -1;

    return mount->ops->list(mount->ctx, safe_relative(relative), buffer, buffer_size);
}

int vfs_read(const char *path, char *buffer, size_t buffer_size)
{
    if (!path || !buffer || buffer_size == 0)
        return -1;

    char normalized[VFS_MAX_PATH];
    if (normalize_path(path, normalized, sizeof(normalized)) < 0)
        return -1;

    const char *relative = NULL;
    struct vfs_mount *mount = resolve_mount(normalized, &relative);
    if (!mount || !mount->ops || !mount->ops->read)
        return -1;

    return mount->ops->read(mount->ctx, safe_relative(relative), buffer, buffer_size);
}

static int vfs_write_internal(const char *path, const char *data, size_t length, enum vfs_write_mode mode)
{
    if (!path)
        return -1;

    char normalized[VFS_MAX_PATH];
    if (normalize_path(path, normalized, sizeof(normalized)) < 0)
        return -1;

    const char *relative = NULL;
    struct vfs_mount *mount = resolve_mount(normalized, &relative);
    if (!mount || !mount->ops || !mount->ops->write)
        return -1;

    return mount->ops->write(mount->ctx, safe_relative(relative), data, length, mode);
}

int vfs_write(const char *path, const char *data, size_t length)
{
    return vfs_write_internal(path, data, length, VFS_WRITE_APPEND);
}

int vfs_write_file(const char *path, const char *data, size_t length)
{
    return vfs_write_internal(path, data, length, VFS_WRITE_REPLACE);
}

int vfs_remove(const char *path)
{
    if (!path)
        return -1;

    char normalized[VFS_MAX_PATH];
    if (normalize_path(path, normalized, sizeof(normalized)) < 0)
        return -1;

    const char *relative = NULL;
    struct vfs_mount *mount = resolve_mount(normalized, &relative);
    if (!mount || !mount->ops || !mount->ops->remove)
        return -1;

    return mount->ops->remove(mount->ctx, safe_relative(relative));
}

static void add_root_directory(const char *name)
{
    if (!name)
        return;
    struct ramfs_volume *root = ramfs_root_volume();
    ramfs_volume_write(root, name, NULL, 0);
}

static void vfs_prepare_virtual_fs(void)
{
    const char *top_level[] = {
        "System",
        "Volumes",
        "Users",
        "Apps",
        "Temp",
        "Devices"
    };

    for (size_t i = 0; i < sizeof(top_level) / sizeof(top_level[0]); ++i)
        add_root_directory(top_level[i]);

    ramfs_volume_init(&system_volume);
    ramfs_volume_init(&volumes_volume);
    ramfs_volume_init(&users_volume);
    ramfs_volume_init(&apps_volume);
    ramfs_volume_init(&temp_volume);
    ramfs_volume_init(&devices_volume);

    vfs_mount("/System", &ramfs_ops, &system_volume);
    vfs_mount("/Volumes", &ramfs_ops, &volumes_volume);
    vfs_mount("/Users", &ramfs_ops, &users_volume);
    vfs_mount("/Apps", &ramfs_ops, &apps_volume);
    vfs_mount("/Temp", &ramfs_ops, &temp_volume);
    vfs_mount("/Devices", &ramfs_ops, &devices_volume);

    const char *version = "proOS kernel/0.5\n";
    vfs_write_file("/System/version", version, local_strlen(version));

    const char *null_stub = "";
    vfs_write_file("/Devices/Null", null_stub, local_strlen(null_stub));

    klog_enable_proc_sink();
    debug_publish_memory_info();
}

int vfs_init(void)
{
    if (mounts_initialized)
        return 0;

    for (size_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        mount_table[i].used = 0;
        mount_table[i].mount_point[0] = '\0';
        mount_table[i].prefix_len = 0;
        mount_table[i].ops = NULL;
        mount_table[i].ctx = NULL;
    }

    ramfs_init();
    if (vfs_mount("/", &ramfs_ops, ramfs_root_volume()) < 0)
    {
        klog_error("vfs: failed to mount root filesystem");
        return -1;
    }

    vfs_prepare_virtual_fs();

    mounts_initialized = 1;
    return 0;
}
