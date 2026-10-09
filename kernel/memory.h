#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

void memory_init(void);
void *kalloc(size_t size);
void *kalloc_zero(size_t size);
void kfree(void *ptr);

size_t memory_total_bytes(void);
size_t memory_used_bytes(void);
size_t memory_free_bytes(void);
uintptr_t memory_heap_base(void);
uintptr_t memory_heap_limit(void);

#endif
