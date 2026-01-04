#ifndef RAMFS_H
#define RAMFS_H

#include <stddef.h>
#include <stdint.h>

#define RAMFS_MAX_FILES      32
#define RAMFS_MAX_NAME       32
#define RAMFS_MAX_FILE_SIZE 1024

struct ramfs_entry
{
	int used;
	char name[RAMFS_MAX_NAME];
	uint8_t is_directory;
	size_t size;
	char data[RAMFS_MAX_FILE_SIZE];
};

struct ramfs_volume
{
	struct ramfs_entry files[RAMFS_MAX_FILES];
};

void ramfs_volume_init(struct ramfs_volume *volume);
int ramfs_volume_list(struct ramfs_volume *volume, const char *directory, char *buffer, size_t buffer_size);
int ramfs_volume_read(struct ramfs_volume *volume, const char *name, char *out, size_t out_size);
int ramfs_volume_append(struct ramfs_volume *volume, const char *name, const char *data, size_t length);
int ramfs_volume_write(struct ramfs_volume *volume, const char *name, const char *data, size_t length);
int ramfs_volume_remove(struct ramfs_volume *volume, const char *name);
int ramfs_volume_mkdir(struct ramfs_volume *volume, const char *name);

/* Legacy single-volume helpers (backed by the root RAMFS instance). */
void ramfs_init(void);
int ramfs_list(char *buffer, size_t buffer_size);
int ramfs_read(const char *name, char *out, size_t out_size);
int ramfs_write(const char *name, const char *data, size_t length);
int ramfs_write_file(const char *name, const char *data, size_t length);
int ramfs_remove(const char *name);
int ramfs_mkdir(const char *name);

struct ramfs_volume *ramfs_root_volume(void);

#endif
