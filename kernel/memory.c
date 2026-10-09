#include "memory.h"

#include <stdint.h>

#define HEAP_START_ADDR ((uint8_t *)0x00300000)
#define HEAP_SIZE_BYTES (0x01000000)

static uint8_t *heap_ptr = HEAP_START_ADDR;
static uint8_t *const heap_end = HEAP_START_ADDR + HEAP_SIZE_BYTES;

static size_t align_up(size_t value, size_t alignment)
{
    size_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

void memory_init(void)
{
    heap_ptr = HEAP_START_ADDR;
}

void *kalloc(size_t size)
{
    if (size == 0)
        return NULL;

    size = align_up(size, 16);
    if (heap_ptr + size > heap_end)
        return NULL;

    void *result = heap_ptr;
    heap_ptr += size;
    return result;
}

void *kalloc_zero(size_t size)
{
    uint8_t *ptr = (uint8_t *)kalloc(size);
    if (!ptr)
        return NULL;

    for (size_t i = 0; i < size; ++i)
        ptr[i] = 0;

    return ptr;
}

size_t memory_total_bytes(void)
{
    return HEAP_SIZE_BYTES;
}

size_t memory_used_bytes(void)
{
    return (size_t)(heap_ptr - HEAP_START_ADDR);
}

size_t memory_free_bytes(void)
{
    size_t used = memory_used_bytes();
    if (used >= HEAP_SIZE_BYTES)
        return 0;
    return HEAP_SIZE_BYTES - used;
}

uintptr_t memory_heap_base(void)
{
    return (uintptr_t)HEAP_START_ADDR;
}

uintptr_t memory_heap_limit(void)
{
    return (uintptr_t)heap_end;
}
