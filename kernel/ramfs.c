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

static int str_starts_with(const char *str, const char *prefix)
{
    size_t idx = 0;
    while (prefix[idx])
    {
        if (str[idx] != prefix[idx])
            return 0;
        ++idx;
    }
    return 1;
}

static int path_is_valid(const char *name)
{
    if (!name || name[0] == '\0')
        return 0;
    size_t len = str_len(name);
    if (len >= RAMFS_MAX_NAME)
        return 0;
    if (name[0] == '/' || name[len - 1] == '/')
        return 0;
    return 1;
}

static struct ramfs_entry *find_entry(struct ramfs_volume *volume, const char *name)
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

static int ensure_parent_directory(struct ramfs_volume *volume, const char *name)
{
    if (!volume || !name)
        return 0;

    size_t len = str_len(name);
    size_t last_slash = (size_t)(-1);
    for (size_t i = 0; i < len; ++i)
    {
        if (name[i] == '/')
            last_slash = i;
    }

    if (last_slash == (size_t)(-1))
        return 1;

    if (last_slash == 0 || last_slash >= RAMFS_MAX_NAME)
        return 0;

    char parent[RAMFS_MAX_NAME];
    size_t parent_len = last_slash;
    for (size_t i = 0; i < parent_len && i + 1 < sizeof(parent); ++i)
        parent[i] = name[i];
    parent[parent_len] = '\0';

    struct ramfs_entry *entry = find_entry(volume, parent);
    if (!entry || !entry->is_directory)
        return 0;
    return 1;
}

static struct ramfs_entry *create_entry(struct ramfs_volume *volume, const char *name, int directory)
{
    if (!volume || !name)
        return NULL;

    if (!path_is_valid(name))
        return NULL;

    struct ramfs_entry *existing = find_entry(volume, name);
    if (existing)
    {
        if ((existing->is_directory ? 1 : 0) == (directory ? 1 : 0))
            return existing;
        return NULL;
    }

    if (!ensure_parent_directory(volume, name))
        return NULL;

    for (size_t i = 0; i < RAMFS_MAX_FILES; ++i)
    {
        if (!volume->files[i].used)
        {
            volume->files[i].used = 1;
            volume->files[i].size = 0;
            str_copy(volume->files[i].name, name, RAMFS_MAX_NAME);
            volume->files[i].is_directory = directory ? 1 : 0;
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
        volume->files[i].is_directory = 0;
        volume->files[i].size = 0;
        volume->files[i].data[0] = '\0';
    }
}

int ramfs_volume_list(struct ramfs_volume *volume, const char *directory, char *buffer, size_t buffer_size)
{
    if (!volume || !buffer || buffer_size == 0)
        return -1;

    const char *dir = directory ? directory : "";
    size_t dir_len = str_len(dir);
    int is_root = dir_len == 0;

    if (!is_root)
    {
        if (!path_is_valid(dir))
            return -1;
        struct ramfs_entry *dir_entry = find_entry(volume, dir);
        if (!dir_entry || !dir_entry->is_directory)
            return -1;
    }

    size_t written = 0;

    for (size_t i = 0; i < RAMFS_MAX_FILES; ++i)
    {
        struct ramfs_entry *entry = &volume->files[i];
        if (!entry->used)
            continue;

        const char *name = entry->name;

        const char *child = NULL;
        if (is_root)
        {
            int nested = 0;
            for (size_t j = 0; name[j]; ++j)
            {
                if (name[j] == '/')
                {
                    nested = 1;
                    break;
                }
            }
            if (nested)
                continue;
            child = name;
        }
        else
        {
            if (str_len(name) <= dir_len)
                continue;
            if (!str_starts_with(name, dir))
                continue;
            if (name[dir_len] != '/')
                continue;
            child = name + dir_len + 1;
            if (*child == '\0')
                continue;
            int nested = 0;
            for (size_t j = 0; child[j]; ++j)
            {
                if (child[j] == '/')
                {
                    nested = 1;
                    break;
                }
            }
            if (nested)
                continue;
        }

        size_t child_len = str_len(child);
        size_t extra = entry->is_directory ? 1 : 0;
        if (written + child_len + extra + 1 >= buffer_size)
            break;

        for (size_t j = 0; j < child_len; ++j)
            buffer[written++] = child[j];

        if (entry->is_directory)
            buffer[written++] = '/';

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

    if (!path_is_valid(name))
        return -1;

    struct ramfs_entry *file = find_entry(volume, name);
    if (!file || file->is_directory)
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

    if (!path_is_valid(name))
        return -1;

    struct ramfs_entry *file = create_entry(volume, name, 0);
    if (!file)
        return -1;

    if (file->is_directory)
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

    if (!path_is_valid(name))
        return -1;

    struct ramfs_entry *file = create_entry(volume, name, 0);
    if (!file)
        return -1;

    if (file->is_directory)
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

    if (!path_is_valid(name))
        return -1;

    struct ramfs_entry *file = find_entry(volume, name);
    if (!file)
        return -1;

    int was_directory = file->is_directory ? 1 : 0;

    file->used = 0;
    file->size = 0;
    file->is_directory = 0;
    file->name[0] = '\0';
    file->data[0] = '\0';

    if (was_directory)
    {
        size_t prefix_len = str_len(name);
        for (size_t i = 0; i < RAMFS_MAX_FILES; ++i)
        {
            struct ramfs_entry *child = &volume->files[i];
            if (!child->used)
                continue;
            size_t name_len = str_len(child->name);
            if (name_len <= prefix_len)
                continue;
            if (!str_starts_with(child->name, name))
                continue;
            if (child->name[prefix_len] != '/')
                continue;
            child->used = 0;
            child->size = 0;
            child->is_directory = 0;
            child->name[0] = '\0';
            child->data[0] = '\0';
        }
    }

    return 0;
}

int ramfs_volume_mkdir(struct ramfs_volume *volume, const char *name)
{
    if (!volume || !name)
        return -1;

    if (!path_is_valid(name))
        return -1;

    struct ramfs_entry *entry = create_entry(volume, name, 1);
    if (!entry)
        return -1;

    entry->size = 0;
    entry->data[0] = '\0';
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
    return ramfs_volume_list(ramfs_root_volume(), "", buffer, buffer_size);
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

int ramfs_mkdir(const char *name)
{
    return ramfs_volume_mkdir(ramfs_root_volume(), name);
}
