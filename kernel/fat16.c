#include "fat16.h"
#include "vfs.h"
#include "klog.h"

struct fat16_bpb
{
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entries;
    uint32_t total_sectors;
    uint16_t sectors_per_fat;
    uint32_t fat_start;
    uint32_t root_start;
    uint32_t data_start;
};

static const uint8_t *volume_base = NULL;
static size_t volume_size = 0;
static struct fat16_bpb bpb;
static int fat_ready = 0;
static int fat_mounted = 0;

static char to_upper(char c)
{
    if (c >= 'a' && c <= 'z')
        return (char)(c - ('a' - 'A'));
    return c;
}

static uint16_t read_u16(const uint8_t *ptr)
{
    return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

static uint32_t read_u32(const uint8_t *ptr)
{
    return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
}

static uint32_t cluster_to_sector(uint16_t cluster)
{
    return bpb.data_start + (cluster - 2) * bpb.sectors_per_cluster;
}

static const uint8_t *sector_ptr(uint32_t sector)
{
    uint32_t offset = sector * bpb.bytes_per_sector;
    if (offset >= volume_size)
        return NULL;
    return volume_base + offset;
}

static int name_matches(const uint8_t *entry, const char *name);

static const uint8_t *find_root_entry(const char *path)
{
    if (!fat_ready || !path)
        return NULL;

    const uint8_t *root = sector_ptr(bpb.root_start);
    if (!root)
        return NULL;

    size_t entries = bpb.root_entries;
    for (size_t i = 0; i < entries; ++i)
    {
        const uint8_t *entry = root + i * 32;
        uint8_t first = entry[0];
        if (first == 0x00)
            break;
        if (first == 0xE5)
            continue;
        uint8_t attr = entry[11];
        if (attr == 0x0F || (attr & 0x08))
            continue;
        if (name_matches(entry, path))
            return entry;
    }

    return NULL;
}

int fat16_init(const void *base, size_t size)
{
    fat_ready = 0;
    fat_mounted = 0;
    if (!base || size < 512)
        return 0;

    volume_base = (const uint8_t *)base;
    volume_size = size;

    const uint8_t *boot = volume_base;
    bpb.bytes_per_sector = read_u16(boot + 11);
    bpb.sectors_per_cluster = boot[13];
    bpb.reserved_sectors = read_u16(boot + 14);
    bpb.fat_count = boot[16];
    bpb.root_entries = read_u16(boot + 17);

    uint16_t total16 = read_u16(boot + 19);
    uint32_t total32 = read_u32(boot + 32);
    bpb.total_sectors = total16 ? total16 : total32;

    bpb.sectors_per_fat = read_u16(boot + 22);
    if (bpb.sectors_per_fat == 0)
        bpb.sectors_per_fat = (uint16_t)read_u32(boot + 36);

    if (bpb.bytes_per_sector == 0 || bpb.sectors_per_cluster == 0)
        return 0;

    bpb.fat_start = bpb.reserved_sectors;
    uint32_t root_dir_sectors = ((uint32_t)bpb.root_entries * 32 + (bpb.bytes_per_sector - 1)) / bpb.bytes_per_sector;
    bpb.root_start = bpb.fat_start + bpb.fat_count * bpb.sectors_per_fat;
    bpb.data_start = bpb.root_start + root_dir_sectors;

    if ((uint32_t)bpb.bytes_per_sector * bpb.total_sectors > volume_size)
        return 0;

    fat_ready = 1;
    return 1;
}

int fat16_ready(void)
{
    return fat_ready;
}

static int build_mount_path(char *out, size_t cap, const char *name)
{
    if (!out || cap == 0)
        return 0;

    const char *prefix = "/Volumes/";
    size_t pos = 0;
    while (prefix[pos] && pos + 1 < cap)
    {
        out[pos] = prefix[pos];
        ++pos;
    }

    const char *src = name && name[0] ? name : "Disk0";
    size_t idx = 0;
    while (src[idx] && pos + 1 < cap)
        out[pos++] = src[idx++];

    if (src[idx])
        return 0;

    out[pos] = '\0';
    return 1;
}

static int fat16_vfs_list(void *ctx, const char *path, char *buffer, size_t buffer_size)
{
    (void)ctx;
    if (!fat_ready)
        return -1;

    if (!path || path[0] == '\0')
        return fat16_ls(buffer, buffer_size);

    return -1;
}

static int fat16_vfs_read(void *ctx, const char *path, char *buffer, size_t buffer_size)
{
    (void)ctx;
    if (!fat_ready || !path || path[0] == '\0' || !buffer || buffer_size == 0)
        return -1;

    return fat16_read(path, buffer, buffer_size);
}

static int fat16_vfs_write(void *ctx, const char *path, const char *data, size_t length, enum vfs_write_mode mode)
{
    (void)ctx;
    (void)path;
    (void)data;
    (void)length;
    (void)mode;
    return -1;
}

static int fat16_vfs_remove(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    return -1;
}

static const struct vfs_fs_ops fat16_vfs_ops = {
    .list = fat16_vfs_list,
    .read = fat16_vfs_read,
    .write = fat16_vfs_write,
    .remove = fat16_vfs_remove
};

static void trim_trailing_spaces(char *str)
{
    size_t len = 0;
    while (str[len])
        ++len;
    while (len > 0 && str[len - 1] == ' ')
    {
        str[len - 1] = '\0';
        --len;
    }
}

static void format_entry_name(const uint8_t *entry, char *out)
{
    for (int i = 0; i < 8; ++i)
        out[i] = (char)entry[i];
    out[8] = '\0';
    trim_trailing_spaces(out);

    char ext[4];
    for (int i = 0; i < 3; ++i)
        ext[i] = (char)entry[8 + i];
    ext[3] = '\0';
    trim_trailing_spaces(ext);

    if (ext[0] != '\0')
    {
        size_t len = 0;
        while (out[len])
            ++len;
        out[len++] = '.';
        out[len] = '\0';
        for (int i = 0; ext[i]; ++i)
        {
            out[len++] = ext[i];
            out[len] = '\0';
        }
    }
}

int fat16_ls(char *out, size_t max_len)
{
    if (!fat_ready || !out || max_len == 0)
        return -1;

    size_t written = 0;
    const uint8_t *root = sector_ptr(bpb.root_start);
    if (!root)
        return -1;

    size_t entries = bpb.root_entries;
    for (size_t i = 0; i < entries; ++i)
    {
        const uint8_t *entry = root + i * 32;
        uint8_t first = entry[0];
        if (first == 0x00)
            break;
        if (first == 0xE5)
            continue;
        uint8_t attr = entry[11];
        if (attr == 0x0F || (attr & 0x08))
            continue;

        char name[16];
        format_entry_name(entry, name);
        for (int j = 0; name[j]; ++j)
        {
            if (written + 1 >= max_len)
            {
                out[written] = '\0';
                return (int)written;
            }
            out[written++] = name[j];
        }
        if (written + 1 >= max_len)
        {
            out[written] = '\0';
            return (int)written;
        }
        out[written++] = '\n';
    }

    if (written < max_len)
        out[written] = '\0';
    return (int)written;
}

static void to_fat_name(const char *input, char out[11])
{
    for (int i = 0; i < 11; ++i)
        out[i] = ' ';

    int idx = 0;
    while (*input && *input != '.' && idx < 8)
    {
        out[idx++] = to_upper(*input++);
    }
    if (*input == '.')
    {
        ++input;
        for (int i = 0; i < 3 && *input; ++i)
            out[8 + i] = to_upper(*input++);
    }
}

static int name_matches(const uint8_t *entry, const char *name)
{
    char target[11];
    to_fat_name(name, target);

    for (int i = 0; i < 11; ++i)
    {
        char entry_char = (char)entry[i];
        if (entry_char == ' ' && target[i] == ' ')
            continue;
        if (to_upper(entry_char) != target[i])
            return 0;
    }
    return 1;
}

static uint16_t fat_next_cluster(uint16_t cluster)
{
    uint32_t offset = cluster * 2;
    const uint8_t *fat = sector_ptr(bpb.fat_start);
    if (!fat)
        return 0xFFFF;
    if (offset + 1 >= bpb.sectors_per_fat * bpb.bytes_per_sector)
        return 0xFFFF;
    return read_u16(fat + offset);
}

int fat16_read(const char *path, char *out, size_t max_len)
{
    if (!out || max_len == 0)
        return -1;

    size_t usable = max_len - 1;
    if (usable == 0)
    {
        out[0] = '\0';
        return 0;
    }

    size_t bytes_read = 0;
    int status = fat16_read_file(path, out, usable, &bytes_read);
    if (status < 0)
        return -1;

    if (bytes_read < usable)
        out[bytes_read] = '\0';
    else
        out[usable] = '\0';

    return (int)bytes_read;
}

int fat16_file_size(const char *path, uint32_t *out_size)
{
    const uint8_t *entry = find_root_entry(path);
    if (!entry)
        return -1;

    if (out_size)
        *out_size = read_u32(entry + 28);
    return 0;
}

int fat16_read_file(const char *path, void *out, size_t max_len, size_t *out_size)
{
    if (!fat_ready || !path || !out || max_len == 0)
        return -1;

    const uint8_t *entry = find_root_entry(path);
    if (!entry)
        return -1;

    uint32_t file_size = read_u32(entry + 28);
    uint16_t cluster = read_u16(entry + 26);
    uint8_t *dst = (uint8_t *)out;
    size_t copied = 0;

    while (cluster >= 2 && cluster < 0xFFF8 && copied < max_len && copied < file_size)
    {
        uint32_t sector = cluster_to_sector(cluster);
        for (uint8_t s = 0; s < bpb.sectors_per_cluster && copied < max_len && copied < file_size; ++s)
        {
            const uint8_t *data = sector_ptr(sector + s);
            if (!data)
                return -1;
            size_t to_copy = bpb.bytes_per_sector;
            if (copied + to_copy > max_len)
                to_copy = max_len - copied;
            if (copied + to_copy > file_size)
                to_copy = file_size - copied;
            for (size_t i = 0; i < to_copy; ++i)
                dst[copied + i] = data[i];
            copied += to_copy;
        }
        if (copied >= file_size || copied >= max_len)
            break;
        cluster = fat_next_cluster(cluster);
    }

    if (out_size)
        *out_size = copied;

    return (copied == file_size) ? 0 : ((copied < file_size) ? 1 : 0);
}

int fat16_mount_volume(const char *name)
{
    if (!fat_ready)
        return -1;
    if (fat_mounted)
        return 0;

    char mount_path[VFS_MAX_PATH];
    if (!build_mount_path(mount_path, sizeof(mount_path), name))
        return -1;

    if (vfs_write_file(mount_path, "", 0) < 0)
        klog_warn("fat16: failed to seed volume entry");

    if (vfs_mount(mount_path, &fat16_vfs_ops, NULL) < 0)
        return -1;

    fat_mounted = 1;
    return 0;
}
