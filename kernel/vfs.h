#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <stdint.h>

#define VFS_MAX_MOUNTS 8
#define VFS_MAX_PATH    128
#define VFS_NODE_NAME_MAX 32
#define VFS_INLINE_CAP   8192

enum vfs_write_mode
{
    VFS_WRITE_APPEND = 0,
    VFS_WRITE_REPLACE = 1
};

struct vfs_fs_ops
{
    int (*list)(void *ctx, const char *path, char *buffer, size_t buffer_size);
    int (*read)(void *ctx, const char *path, char *buffer, size_t buffer_size);
    int (*write)(void *ctx, const char *path, const char *data, size_t length, enum vfs_write_mode mode);
    int (*remove)(void *ctx, const char *path);
};

int vfs_init(void);
int vfs_mount(const char *mount_point, const struct vfs_fs_ops *ops, void *ctx);
int vfs_list(const char *path, char *buffer, size_t buffer_size);
int vfs_read(const char *path, char *buffer, size_t buffer_size);
int vfs_write(const char *path, const char *data, size_t length);
int vfs_write_file(const char *path, const char *data, size_t length);
int vfs_remove(const char *path);

#endif
