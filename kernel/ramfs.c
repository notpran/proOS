#include "ramfs.h"

static struct ramfs_volume root_volume;
static int root_initialized = 0;

static size_t str_len(const char *str)
{
    size_t len = 0;
    while (str[len])
        ++len;
    return len;
}

static int str_cmp(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void str_copy(char *dst, const char *src, size_t max_len)
{
    size_t i = 0;
    for (; i + 1 < max_len && src[i]; ++i)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void mem_copy(char *dst, const char *src, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        dst[i] = src[i];
}

static struct ramfs_entry *find_file(struct ramfs_volume *volume, const char *name)
{
    if (!volume)
        return NULL;
    for (size_t i = 0; i < RAMFS_MAX_FILES; ++i)
    {
        if (volume->files[i].used && str_cmp(volume->files[i].name, name) == 0)
            return &volume->files[i];
    }
    return NULL;
}

static struct ramfs_entry *create_file(struct ramfs_volume *volume, const char *name)
{
    if (!volume)
        return NULL;

    struct ramfs_entry *existing = find_file(volume, name);
    if (existing)
        return existing;

    for (size_t i = 0; i < RAMFS_MAX_FILES; ++i)
    {
        if (!volume->files[i].used)
        {
            volume->files[i].used = 1;
            volume->files[i].size = 0;
            str_copy(volume->files[i].name, name, RAMFS_MAX_NAME);
            volume->files[i].data[0] = '\0';
            return &volume->files[i];
        }
    }
    return NULL;
}

void ramfs_volume_init(struct ramfs_volume *volume)
{
    if (!volume)
        return;

    for (size_t i = 0; i < RAMFS_MAX_FILES; ++i)
    {
        volume->files[i].used = 0;
        volume->files[i].name[0] = '\0';
        volume->files[i].size = 0;
        volume->files[i].data[0] = '\0';
    }
}

int ramfs_volume_list(struct ramfs_volume *volume, char *buffer, size_t buffer_size)
{
    if (!volume || !buffer || buffer_size == 0)
        return 0;

    size_t written = 0;

    for (size_t i = 0; i < RAMFS_MAX_FILES; ++i)
    {
        if (!volume->files[i].used)
            continue;

        size_t name_len = str_len(volume->files[i].name);
        if (written + name_len + 1 >= buffer_size)
            break;

        for (size_t j = 0; j < name_len; ++j)
            buffer[written++] = volume->files[i].name[j];

        buffer[written++] = '\n';
    }

    if (written > 0)
        --written;

    buffer[written] = '\0';
    return (int)written;
}

int ramfs_volume_read(struct ramfs_volume *volume, const char *name, char *out, size_t out_size)
{
    if (!volume || !name || !out || out_size == 0)
        return -1;

    struct ramfs_entry *file = find_file(volume, name);
    if (!file)
        return -1;

    size_t to_copy = file->size;
    if (to_copy >= out_size)
        to_copy = out_size - 1;

    mem_copy(out, file->data, to_copy);
    out[to_copy] = '\0';
    return (int)to_copy;
}

int ramfs_volume_append(struct ramfs_volume *volume, const char *name, const char *data, size_t length)
{
    if (!volume || !name || !data || length == 0)
        return -1;

    struct ramfs_entry *file = create_file(volume, name);
    if (!file)
        return -1;

    if (file->size + length >= RAMFS_MAX_FILE_SIZE)
        return -1;

    mem_copy(file->data + file->size, data, length);
    file->size += length;
    file->data[file->size] = '\0';
    return (int)length;
}

int ramfs_volume_write(struct ramfs_volume *volume, const char *name, const char *data, size_t length)
{
    if (!volume || !name)
        return -1;

    struct ramfs_entry *file = create_file(volume, name);
    if (!file)
        return -1;

    if (!data)
    {
        file->size = 0;
        file->data[0] = '\0';
        return 0;
    }

    if (length >= RAMFS_MAX_FILE_SIZE)
        length = RAMFS_MAX_FILE_SIZE - 1;

    mem_copy(file->data, data, length);
    file->size = length;
    file->data[file->size] = '\0';
    return (int)length;
}

int ramfs_volume_remove(struct ramfs_volume *volume, const char *name)
{
    if (!volume || !name)
        return -1;

    struct ramfs_entry *file = find_file(volume, name);
    if (!file)
        return -1;

    file->used = 0;
    file->size = 0;
    file->name[0] = '\0';
    file->data[0] = '\0';
    return 0;
}

struct ramfs_volume *ramfs_root_volume(void)
{
    if (!root_initialized)
    {
        ramfs_volume_init(&root_volume);
        root_initialized = 1;
    }
    return &root_volume;
}

void ramfs_init(void)
{
    struct ramfs_volume *volume = ramfs_root_volume();
    ramfs_volume_init(volume);
}

int ramfs_list(char *buffer, size_t buffer_size)
{
    return ramfs_volume_list(ramfs_root_volume(), buffer, buffer_size);
}

int ramfs_read(const char *name, char *out, size_t out_size)
{
    return ramfs_volume_read(ramfs_root_volume(), name, out, out_size);
}

int ramfs_write(const char *name, const char *data, size_t length)
{
    return ramfs_volume_append(ramfs_root_volume(), name, data, length);
}

int ramfs_write_file(const char *name, const char *data, size_t length)
{
    return ramfs_volume_write(ramfs_root_volume(), name, data, length);
}

int ramfs_remove(const char *name)
{
    return ramfs_volume_remove(ramfs_root_volume(), name);
}
