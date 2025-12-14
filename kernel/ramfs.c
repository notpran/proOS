#include "ramfs.h"

struct ramfs_entry
{
    int used;
    char name[RAMFS_MAX_NAME];
    size_t size;
    char data[RAMFS_MAX_FILE_SIZE];
};

static struct ramfs_entry ramfs_files[RAMFS_MAX_FILES];

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

static struct ramfs_entry *find_file(const char *name)
{
    for (size_t i = 0; i < RAMFS_MAX_FILES; ++i)
    {
        if (ramfs_files[i].used && str_cmp(ramfs_files[i].name, name) == 0)
            return &ramfs_files[i];
    }
    return NULL;
}

static struct ramfs_entry *create_file(const char *name)
{
    struct ramfs_entry *existing = find_file(name);
    if (existing)
        return existing;

    for (size_t i = 0; i < RAMFS_MAX_FILES; ++i)
    {
        if (!ramfs_files[i].used)
        {
            ramfs_files[i].used = 1;
            ramfs_files[i].size = 0;
            str_copy(ramfs_files[i].name, name, RAMFS_MAX_NAME);
            ramfs_files[i].data[0] = '\0';
            return &ramfs_files[i];
        }
    }
    return NULL;
}

void ramfs_init(void)
{
    for (size_t i = 0; i < RAMFS_MAX_FILES; ++i)
    {
        ramfs_files[i].used = 0;
        ramfs_files[i].name[0] = '\0';
        ramfs_files[i].size = 0;
        ramfs_files[i].data[0] = '\0';
    }

    const char *welcome = "Welcome to proOS!";
    ramfs_write("hello.txt", welcome, str_len(welcome));
    ramfs_write("hello.txt", "\n", 1);
}

int ramfs_list(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0)
        return 0;

    size_t written = 0;

    for (size_t i = 0; i < RAMFS_MAX_FILES; ++i)
    {
        if (!ramfs_files[i].used)
            continue;

        size_t name_len = str_len(ramfs_files[i].name);
        if (written + name_len + 1 >= buffer_size)
            break;

        for (size_t j = 0; j < name_len; ++j)
            buffer[written++] = ramfs_files[i].name[j];

        buffer[written++] = '\n';
    }

    if (written > 0)
        --written; /* remove trailing newline */

    buffer[written] = '\0';
    return (int)written;
}

int ramfs_read(const char *name, char *out, size_t out_size)
{
    if (!name || !out || out_size == 0)
        return -1;

    struct ramfs_entry *file = find_file(name);
    if (!file)
        return -1;

    size_t to_copy = file->size;
    if (to_copy >= out_size)
        to_copy = out_size - 1;

    mem_copy(out, file->data, to_copy);
    out[to_copy] = '\0';
    return (int)to_copy;
}

int ramfs_write(const char *name, const char *data, size_t length)
{
    if (!name || !data || length == 0)
        return -1;

    struct ramfs_entry *file = create_file(name);
    if (!file)
        return -1;

    if (file->size + length >= RAMFS_MAX_FILE_SIZE)
        return -1;

    mem_copy(file->data + file->size, data, length);
    file->size += length;
    file->data[file->size] = '\0';
    return (int)length;
}

int ramfs_write_file(const char *name, const char *data, size_t length)
{
    if (!name)
        return -1;

    struct ramfs_entry *file = create_file(name);
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

int ramfs_remove(const char *name)
{
    if (!name)
        return -1;

    struct ramfs_entry *file = find_file(name);
    if (!file)
        return -1;

    file->used = 0;
    file->size = 0;
    file->name[0] = '\0';
    file->data[0] = '\0';
    return 0;
}
