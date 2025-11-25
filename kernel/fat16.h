#ifndef FAT16_H
#define FAT16_H

#include <stddef.h>
#include <stdint.h>

int fat16_init(const void *base, size_t size);
int fat16_ready(void);
int fat16_ls(char *out, size_t max_len);
int fat16_read(const char *path, char *out, size_t max_len);
int fat16_read_file(const char *path, void *out, size_t max_len, size_t *out_size);
int fat16_file_size(const char *path, uint32_t *out_size);

#endif
