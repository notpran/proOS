#include "fat16.h"

#include "fatfs.h"
#include "klog.h"
#include "vfs.h"

static int join_path(const char *base, const char *leaf, char *out, size_t capacity)
{
    if (!base || !leaf || !out || capacity == 0)
        return -1;

    size_t pos = 0;
    for (size_t i = 0; base[i] && pos + 1 < capacity; ++i)
        out[pos++] = base[i];

    if (pos == 0 || out[pos - 1] != '/')
    {
        if (pos + 1 >= capacity)
            return -1;
        out[pos++] = '/';
    }

    for (size_t i = 0; leaf[i] && pos + 1 < capacity; ++i)
        out[pos++] = leaf[i];

    if (pos >= capacity)
        return -1;
    out[pos] = '\0';
    return 0;
}

static void ensure_directory(const char *path)
{
    if (!path || path[0] == '\0')
        return;

    if (vfs_mkdir(path) == 0)
        return;

    char probe[8];
    if (vfs_list(path, probe, sizeof(probe)) >= 0)
        return;
}

static struct fatfs_volume g_fat_volume;
static int g_fat_ready = 0;
static int g_fat_type = FATFS_TYPE_NONE;

int fat16_init(const void *base, size_t size)
{
    g_fat_ready = 0;
    g_fat_type = fatfs_init(&g_fat_volume, (void *)base, size);
    if (g_fat_type == FATFS_TYPE_NONE)
    {
        klog_warn("fat: unsupported FAT volume");
        return 0;
    }

    g_fat_ready = 1;
    if (g_fat_type == FATFS_TYPE_FAT32)
        klog_info("fat: detected FAT32 volume");
    else
        klog_info("fat: detected FAT16 volume");
    return 1;
}

int fat16_ready(void)
{
    return g_fat_ready && fatfs_ready(&g_fat_volume);
}

int fat16_type(void)
{
    return g_fat_type;
}

int fat16_mount_volume(const char *name)
{
    if (!fat16_ready())
        return -1;
    if (fatfs_mount(&g_fat_volume, name) < 0)
        return -1;

    if (g_fat_volume.mount_path[0] != '\0')
    {
        char users_path[VFS_MAX_PATH];
        if (join_path(g_fat_volume.mount_path, "Users", users_path, sizeof(users_path)) == 0)
        {
            ensure_directory(users_path);

            char pran_path[VFS_MAX_PATH];
            if (join_path(users_path, "pran", pran_path, sizeof(pran_path)) == 0)
            {
                ensure_directory(pran_path);

                char docs_path[VFS_MAX_PATH];
                if (join_path(pran_path, "Documents", docs_path, sizeof(docs_path)) == 0)
                    ensure_directory(docs_path);
            }

            if (vfs_register_alias("/Users", users_path) == 0)
                klog_info("fat: /Users aliased to persistent volume");
            else
                klog_warn("fat: failed to alias /Users");
        }
    }

    return 0;
}

int fat16_ls(char *out, size_t max_len)
{
    if (!fat16_ready())
        return -1;
    return fatfs_list(&g_fat_volume, "", out, max_len);
}

int fat16_read(const char *path, char *out, size_t max_len)
{
    if (!fat16_ready())
        return -1;

    size_t copied = 0;
    if (fatfs_read(&g_fat_volume, path, out, max_len, &copied) < 0)
        return -1;
    if (copied < max_len)
        out[copied] = '\0';
    return (int)copied;
}

int fat16_read_file(const char *path, void *out, size_t max_len, size_t *out_size)
{
    if (!fat16_ready())
        return -1;
    return fatfs_read(&g_fat_volume, path, out, max_len, out_size);
}

int fat16_file_size(const char *path, uint32_t *out_size)
{
    if (!fat16_ready())
        return -1;
    return fatfs_file_size(&g_fat_volume, path, out_size);
}

int fat16_write_file(const char *path, const void *data, size_t length)
{
    if (!fat16_ready())
        return -1;
    return fatfs_write(&g_fat_volume, path, data, length, VFS_WRITE_REPLACE);
}

int fat16_append_file(const char *path, const void *data, size_t length)
{
    if (!fat16_ready())
        return -1;
    return fatfs_write(&g_fat_volume, path, data, length, VFS_WRITE_APPEND);
}

int fat16_remove(const char *path)
{
    if (!fat16_ready())
        return -1;
    return fatfs_remove(&g_fat_volume, path);
}

int fat16_mkdir(const char *path)
{
    if (!fat16_ready())
        return -1;
    return fatfs_mkdir(&g_fat_volume, path);
}

struct fatfs_volume *fat16_volume(void)
{
    return fat16_ready() ? &g_fat_volume : NULL;
}

void fat16_configure_backing(uint32_t lba_start, uint32_t sector_count)
{
    fatfs_bind_backing(&g_fat_volume, lba_start, sector_count);
}

