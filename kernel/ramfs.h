#ifndef RAMFS_H
#define RAMFS_H

#include <stddef.h>
#include <stdint.h>

#define RAMFS_MAX_FILES 32
#define RAMFS_MAX_NAME 32
#define RAMFS_MAX_FILE_SIZE 1024

void ramfs_init(void);
int ramfs_list(char *buffer, size_t buffer_size);
int ramfs_read(const char *name, char *out, size_t out_size);
int ramfs_write(const char *name, const char *data, size_t length);
int ramfs_write_file(const char *name, const char *data, size_t length);
int ramfs_remove(const char *name);

#endif
