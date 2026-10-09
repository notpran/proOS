#include "memory.h"

#include <stdint.h>

#define HEAP_START_ADDR ((uint8_t *)0x00300000)
#define HEAP_SIZE_BYTES (0x01000000)

static uint8_t *heap_ptr = HEAP_START_ADDR;
static uint8_t *const heap_end = HEAP_START_ADDR + HEAP_SIZE_BYTES;

struct free_block
{
    size_t size;
    struct free_block *next;
};

static struct free_block *free_list;

static size_t align_up(size_t value, size_t alignment)
{
    size_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

void memory_init(void)
{
    heap_ptr = HEAP_START_ADDR;
    free_list = NULL;
}

void *kalloc(size_t size)
{
    struct free_block **link;
    struct free_block *block;
    if (size == 0)
        return NULL;

    size = align_up(size, 16);
    link = &free_list;
    while (*link)
    {
        if ((*link)->size >= size)
        {
            block = *link;
            *link = block->next;
            return (uint8_t *)block + sizeof(*block);
        }
        link = &(*link)->next;
    }
    size += sizeof(struct free_block);
    if (heap_ptr + size > heap_end)
        return NULL;

    void *result = heap_ptr + sizeof(struct free_block);
    ((struct free_block *)heap_ptr)->size = size - sizeof(struct free_block);
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

void kfree(void *ptr)
{
    struct free_block *block;
    if (!ptr)
        return;
    if ((uint8_t *)ptr < HEAP_START_ADDR + sizeof(struct free_block) || (uint8_t *)ptr >= heap_ptr)
        return;
    block = (struct free_block *)((uint8_t *)ptr - sizeof(*block));
    block->next = free_list;
    free_list = block;
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
