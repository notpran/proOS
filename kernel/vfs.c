#include "vfs.h"
#include "ramfs.h"
#include "devicefs.h"
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

static int mounts_initialized = 0;

#define VFS_MAX_ALIASES 8

struct vfs_alias
{
    int used;
    char from[VFS_MAX_PATH];
    size_t from_len;
    char to[VFS_MAX_PATH];
};

static struct vfs_alias alias_table[VFS_MAX_ALIASES];

/* Per-open descriptor record that keeps the resolved mount and remaining path. */
struct vfs_handle
{
    int used;
    struct vfs_mount *mount;
    char relative[VFS_MAX_PATH];
};

static struct vfs_handle open_table[VFS_MAX_OPEN_FILES];

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

static void reset_aliases(void)
{
    for (size_t i = 0; i < VFS_MAX_ALIASES; ++i)
    {
        alias_table[i].used = 0;
        alias_table[i].from[0] = '\0';
        alias_table[i].from_len = 0;
        alias_table[i].to[0] = '\0';
    }
}

static const char *apply_alias(const char *path, char *buffer, size_t buffer_size)
{
    if (!path || !buffer || buffer_size == 0)
        return path;

    for (size_t i = 0; i < VFS_MAX_ALIASES; ++i)
    {
        if (!alias_table[i].used)
            continue;
        size_t prefix_len = alias_table[i].from_len;
        if (prefix_len == 0)
            continue;
        if (local_strncmp(path, alias_table[i].from, prefix_len) != 0)
            continue;
        char next = path[prefix_len];
        if (!(next == '\0' || next == '/'))
            continue;

        const char *target = alias_table[i].to;
        size_t target_len = local_strlen(target);
        const char *suffix = path + prefix_len;
        if (*suffix == '/')
            ++suffix;
        size_t suffix_len = local_strlen(suffix);

        size_t extra_slash = 0;
        if (suffix_len > 0 && target_len > 0 && target[target_len - 1] != '/')
            extra_slash = 1;
        size_t total_len = target_len + suffix_len + extra_slash;
        if (total_len + 1 >= buffer_size)
            return path;

        size_t pos = 0;
        for (size_t j = 0; j < target_len; ++j)
            buffer[pos++] = target[j];
        if (extra_slash)
            buffer[pos++] = '/';
        for (size_t j = 0; j < suffix_len; ++j)
            buffer[pos++] = suffix[j];
        buffer[pos] = '\0';
        return buffer;
    }

    return path;
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

static int ramfs_list_adapter(void *ctx, const char *path, char *buffer, size_t buffer_size)
{
    struct ramfs_volume *volume = (struct ramfs_volume *)ctx;
    if (!volume)
        return -1;
    const char *dir = path ? path : "";
    return ramfs_volume_list(volume, dir, buffer, buffer_size);
}

static int ramfs_read_adapter(void *ctx, const char *path, char *buffer, size_t buffer_size)
{
    struct ramfs_volume *volume = (struct ramfs_volume *)ctx;
    if (!volume || !path)
        return -1;
    if (path[0] == '\0')
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
    if (mode == VFS_WRITE_REPLACE)
        return ramfs_volume_write(volume, path, data, length);
    return ramfs_volume_append(volume, path, data, length);
}

static int ramfs_remove_adapter(void *ctx, const char *path)
{
    struct ramfs_volume *volume = (struct ramfs_volume *)ctx;
    if (!volume || !path || path[0] == '\0')
        return -1;
    return ramfs_volume_remove(volume, path);
}

static int ramfs_mkdir_adapter(void *ctx, const char *path)
{
    struct ramfs_volume *volume = (struct ramfs_volume *)ctx;
    if (!volume || !path || path[0] == '\0')
        return -1;
    return ramfs_volume_mkdir(volume, path);
}

static const struct vfs_fs_ops ramfs_ops = {
    .list = ramfs_list_adapter,
    .read = ramfs_read_adapter,
    .write = ramfs_write_adapter,
    .remove = ramfs_remove_adapter,
    .mkdir = ramfs_mkdir_adapter
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

    char aliased[VFS_MAX_PATH];
    effective = apply_alias(effective, aliased, sizeof(aliased));

    char normalized[VFS_MAX_PATH];
    if (normalize_path(effective, normalized, sizeof(normalized)) < 0)
        return -1;

    const char *relative = NULL;
    struct vfs_mount *mount = resolve_mount(normalized, &relative);
    if (!mount || !mount->ops || !mount->ops->list)
        return -1;

    return mount->ops->list(mount->ctx, safe_relative(relative), buffer, buffer_size);
}

int vfs_read_path(const char *path, char *buffer, size_t buffer_size)
{
    if (!path || !buffer || buffer_size == 0)
        return -1;

    char aliased[VFS_MAX_PATH];
    const char *effective = apply_alias(path, aliased, sizeof(aliased));

    char normalized[VFS_MAX_PATH];
    if (normalize_path(effective, normalized, sizeof(normalized)) < 0)
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

    char aliased[VFS_MAX_PATH];
    const char *effective = apply_alias(path, aliased, sizeof(aliased));

    char normalized[VFS_MAX_PATH];
    if (normalize_path(effective, normalized, sizeof(normalized)) < 0)
        return -1;

    const char *relative = NULL;
    struct vfs_mount *mount = resolve_mount(normalized, &relative);
    if (!mount || !mount->ops || !mount->ops->write)
        return -1;

    return mount->ops->write(mount->ctx, safe_relative(relative), data, length, mode);
}

int vfs_append(const char *path, const char *data, size_t length)
{
    return vfs_write_internal(path, data, length, VFS_WRITE_APPEND);
}

int vfs_write_file(const char *path, const char *data, size_t length)
{
    return vfs_write_internal(path, data, length, VFS_WRITE_REPLACE);
}

static struct vfs_handle *get_handle(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES)
        return NULL;
    if (!open_table[fd].used)
        return NULL;
    return &open_table[fd];
}

int vfs_open(const char *path)
{
    if (!path)
        return -1;

    char aliased[VFS_MAX_PATH];
    const char *effective = apply_alias(path, aliased, sizeof(aliased));

    char normalized[VFS_MAX_PATH];
    if (normalize_path(effective, normalized, sizeof(normalized)) < 0)
        return -1;

    const char *relative = NULL;
    struct vfs_mount *mount = resolve_mount(normalized, &relative);
    if (!mount)
        return -1;
    if (!mount->ops)
        return -1;
    if (!mount->ops->read && !mount->ops->write)
        return -1;

    for (int fd = 0; fd < VFS_MAX_OPEN_FILES; ++fd)
    {
        if (open_table[fd].used)
            continue;

        open_table[fd].used = 1;
        open_table[fd].mount = mount;

        const char *rel = safe_relative(relative);
        size_t len = local_strlen(rel);
        if (len >= VFS_MAX_PATH)
        {
            open_table[fd].used = 0;
            open_table[fd].mount = NULL;
            open_table[fd].relative[0] = '\0';
            return -1;
        }
        for (size_t i = 0; i <= len; ++i)
            open_table[fd].relative[i] = rel[i];

        return fd;
    }

    return -1;
}

int vfs_read(int fd, void *buffer, size_t size)
{
    if (!buffer || size == 0)
        return -1;
    struct vfs_handle *handle = get_handle(fd);
    if (!handle || !handle->mount || !handle->mount->ops || !handle->mount->ops->read)
        return -1;
    return handle->mount->ops->read(handle->mount->ctx, safe_relative(handle->relative), buffer, size);
}

int vfs_write(int fd, const void *buffer, size_t size)
{
    if (!buffer)
        return -1;
    struct vfs_handle *handle = get_handle(fd);
    if (!handle || !handle->mount || !handle->mount->ops || !handle->mount->ops->write)
        return -1;
    return handle->mount->ops->write(handle->mount->ctx, safe_relative(handle->relative), (const char *)buffer, size, VFS_WRITE_REPLACE);
}

int vfs_close(int fd)
{
    struct vfs_handle *handle = get_handle(fd);
    if (!handle)
        return -1;
    handle->used = 0;
    handle->mount = NULL;
    handle->relative[0] = '\0';
    return 0;
}

int vfs_remove(const char *path)
{
    if (!path)
        return -1;

    char aliased[VFS_MAX_PATH];
    const char *effective = apply_alias(path, aliased, sizeof(aliased));

    char normalized[VFS_MAX_PATH];
    if (normalize_path(effective, normalized, sizeof(normalized)) < 0)
        return -1;

    const char *relative = NULL;
    struct vfs_mount *mount = resolve_mount(normalized, &relative);
    if (!mount || !mount->ops || !mount->ops->remove)
        return -1;

    return mount->ops->remove(mount->ctx, safe_relative(relative));
}

int vfs_mkdir(const char *path)
{
    if (!path)
        return -1;

    char aliased[VFS_MAX_PATH];
    const char *effective = apply_alias(path, aliased, sizeof(aliased));

    char normalized[VFS_MAX_PATH];
    if (normalize_path(effective, normalized, sizeof(normalized)) < 0)
        return -1;

    const char *relative = NULL;
    struct vfs_mount *mount = resolve_mount(normalized, &relative);
    if (!mount || !mount->ops || !mount->ops->mkdir)
        return -1;

    return mount->ops->mkdir(mount->ctx, safe_relative(relative));
}

static void add_root_directory(const char *name)
{
    if (!name)
        return;
    struct ramfs_volume *root = ramfs_root_volume();
    if (ramfs_volume_mkdir(root, name) < 0)
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

    vfs_mount("/System", &ramfs_ops, &system_volume);
    vfs_mount("/Volumes", &ramfs_ops, &volumes_volume);
    vfs_mount("/Users", &ramfs_ops, &users_volume);
    vfs_mount("/Apps", &ramfs_ops, &apps_volume);
    vfs_mount("/Temp", &ramfs_ops, &temp_volume);
    if (devicefs_mount() < 0)
        klog_warn("vfs: devicefs mount failed");

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

    reset_aliases();

    for (size_t i = 0; i < VFS_MAX_OPEN_FILES; ++i)
    {
        open_table[i].used = 0;
        open_table[i].mount = NULL;
        open_table[i].relative[0] = '\0';
    }

    vfs_prepare_virtual_fs();

    mounts_initialized = 1;
    return 0;
}

size_t vfs_mount_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (mount_table[i].used)
            ++count;
    }
    return count;
}

int vfs_mount_path_at(size_t index, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0)
        return -1;

    size_t seen = 0;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!mount_table[i].used)
            continue;
        if (seen == index)
        {
            size_t len = local_strlen(mount_table[i].mount_point);
            if (len >= buffer_size)
                len = buffer_size - 1;
            for (size_t j = 0; j < len; ++j)
                buffer[j] = mount_table[i].mount_point[j];
            buffer[len] = '\0';
            return 0;
        }
        ++seen;
    }

    return -1;
}

int vfs_register_alias(const char *from, const char *to)
{
    if (!from || !to)
        return -1;

    char norm_from[VFS_MAX_PATH];
    char norm_to[VFS_MAX_PATH];

    if (normalize_path(from, norm_from, sizeof(norm_from)) < 0)
        return -1;
    if (normalize_path(to, norm_to, sizeof(norm_to)) < 0)
        return -1;

    size_t from_len = local_strlen(norm_from);
    if (from_len <= 1)
        return -1;

    size_t to_len = local_strlen(norm_to);
    if (to_len == 0)
        return -1;

    if (from_len == to_len && local_strncmp(norm_from, norm_to, from_len) == 0)
        return -1;

    struct vfs_alias *slot = NULL;
    for (size_t i = 0; i < VFS_MAX_ALIASES; ++i)
    {
        if (!alias_table[i].used)
        {
            if (!slot)
                slot = &alias_table[i];
            continue;
        }
        if (alias_table[i].from_len == from_len && local_strncmp(alias_table[i].from, norm_from, from_len) == 0)
        {
            slot = &alias_table[i];
            break;
        }
    }

    if (!slot)
        return -1;

    slot->used = 1;
    slot->from_len = from_len;
    for (size_t i = 0; i < from_len && i + 1 < sizeof(slot->from); ++i)
        slot->from[i] = norm_from[i];
    slot->from[(from_len < sizeof(slot->from)) ? from_len : sizeof(slot->from) - 1] = '\0';

    if (to_len >= sizeof(slot->to))
        to_len = sizeof(slot->to) - 1;
    for (size_t i = 0; i < to_len; ++i)
        slot->to[i] = norm_to[i];
    slot->to[to_len] = '\0';

    return 0;
}
