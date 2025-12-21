#define KLOG_TAG "debug"

#include "debug.h"

#include "memory.h"
#include "proc.h"
#include "devmgr.h"
#include "vfs.h"
#include "klog.h"

#include <stddef.h>
#include <stdint.h>

static void append_char(char *dst, size_t *pos, size_t cap, char ch)
{
    if (!dst || !pos || *pos + 1 >= cap)
        return;
    dst[*pos] = ch;
    ++(*pos);
}

static void append_text(char *dst, size_t *pos, size_t cap, const char *text)
{
    if (!dst || !pos || !text)
        return;
    while (*text && *pos + 1 < cap)
    {
        dst[*pos] = *text;
        ++(*pos);
        ++text;
    }
}

static void append_decimal(char *dst, size_t *pos, size_t cap, uint32_t value)
{
    char tmp[32];
    size_t idx = 0;
    if (value == 0)
    {
        tmp[idx++] = '0';
    }
    else
    {
        while (value > 0 && idx < sizeof(tmp))
        {
            tmp[idx++] = (char)('0' + (value % 10u));
            value /= 10u;
        }
    }
    while (idx > 0 && *pos + 1 < cap)
    {
        dst[*pos] = tmp[--idx];
        ++(*pos);
    }
}

static void append_hex32(char *dst, size_t *pos, size_t cap, uint32_t value)
{
    const char digits[] = "0123456789ABCDEF";
    append_text(dst, pos, cap, "0x");
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        unsigned nibble = (value >> shift) & 0xFu;
        append_char(dst, pos, cap, digits[nibble]);
    }
}

static void append_newline(char *dst, size_t *pos, size_t cap)
{
    append_char(dst, pos, cap, '\n');
}

void debug_publish_memory_info(void)
{
    char buffer[256];
    size_t pos = 0;

    append_text(buffer, &pos, sizeof(buffer), "Memory Statistics\n");

    uint32_t total = (uint32_t)memory_total_bytes();
    uint32_t used = (uint32_t)memory_used_bytes();
    uint32_t free_space = (uint32_t)memory_free_bytes();
    uint32_t base = (uint32_t)memory_heap_base();
    uint32_t limit = (uint32_t)memory_heap_limit();
    uint32_t cursor = base + used;

    append_text(buffer, &pos, sizeof(buffer), "total_bytes: ");
    append_decimal(buffer, &pos, sizeof(buffer), total);
    append_text(buffer, &pos, sizeof(buffer), " (");
    append_decimal(buffer, &pos, sizeof(buffer), total / 1024u);
    append_text(buffer, &pos, sizeof(buffer), " KB)\n");

    append_text(buffer, &pos, sizeof(buffer), "used_bytes:  ");
    append_decimal(buffer, &pos, sizeof(buffer), used);
    append_text(buffer, &pos, sizeof(buffer), " (");
    append_decimal(buffer, &pos, sizeof(buffer), used / 1024u);
    append_text(buffer, &pos, sizeof(buffer), " KB)\n");

    append_text(buffer, &pos, sizeof(buffer), "free_bytes:  ");
    append_decimal(buffer, &pos, sizeof(buffer), free_space);
    append_text(buffer, &pos, sizeof(buffer), " (");
    append_decimal(buffer, &pos, sizeof(buffer), free_space / 1024u);
    append_text(buffer, &pos, sizeof(buffer), " KB)\n");

    append_text(buffer, &pos, sizeof(buffer), "heap_base:   ");
    append_hex32(buffer, &pos, sizeof(buffer), base);
    append_newline(buffer, &pos, sizeof(buffer));

    append_text(buffer, &pos, sizeof(buffer), "heap_cursor: ");
    append_hex32(buffer, &pos, sizeof(buffer), cursor);
    append_newline(buffer, &pos, sizeof(buffer));

    append_text(buffer, &pos, sizeof(buffer), "heap_limit:  ");
    append_hex32(buffer, &pos, sizeof(buffer), limit);
    append_newline(buffer, &pos, sizeof(buffer));

    if (pos >= sizeof(buffer))
        pos = sizeof(buffer) - 1;
    buffer[pos] = '\0';

    vfs_write_file("/System/meminfo", buffer, pos);
}

static const char *state_name(proc_state_t state)
{
    switch (state)
    {
    case PROC_READY:
        return "READY";
    case PROC_RUNNING:
        return "RUNNING";
    case PROC_WAITING:
        return "WAITING";
    case PROC_ZOMBIE:
        return "ZOMBIE";
    case PROC_UNUSED:
    default:
        return "UNUSED";
    }
}

void debug_publish_task_list(void)
{
    struct process_info info[MAX_PROCS];
    size_t count = process_snapshot(info, MAX_PROCS);

    vfs_write_file("/System/tasks", NULL, 0);

    char line[160];
    size_t pos = 0;
    append_text(line, &pos, sizeof(line), "PID STATE    KIND PRI(base/dyn) REM TICKS WAKE STACK ESP\n");
    vfs_write("/System/tasks", line, pos);

    for (size_t i = 0; i < count; ++i)
    {
        const struct process_info *entry = &info[i];
        pos = 0;
        append_decimal(line, &pos, sizeof(line), (uint32_t)entry->pid);
        append_char(line, &pos, sizeof(line), ' ');
        append_text(line, &pos, sizeof(line), state_name(entry->state));
        if (pos < sizeof(line) - 1)
            line[pos++] = ' ';
        append_char(line, &pos, sizeof(line), (entry->kind == THREAD_KIND_USER) ? 'U' : 'K');
        append_char(line, &pos, sizeof(line), ' ');
        append_decimal(line, &pos, sizeof(line), entry->base_priority);
        append_char(line, &pos, sizeof(line), '/');
        append_decimal(line, &pos, sizeof(line), entry->dynamic_priority);
        append_char(line, &pos, sizeof(line), ' ');
        append_decimal(line, &pos, sizeof(line), entry->time_slice_remaining);
        append_char(line, &pos, sizeof(line), ' ');
        append_decimal(line, &pos, sizeof(line), entry->time_slice_ticks);
        append_char(line, &pos, sizeof(line), ' ');
        append_decimal(line, &pos, sizeof(line), (uint32_t)entry->wake_deadline);
        append_char(line, &pos, sizeof(line), ' ');
        append_hex32(line, &pos, sizeof(line), (uint32_t)(entry->stack_pointer + entry->stack_size));
        append_char(line, &pos, sizeof(line), ' ');
        append_hex32(line, &pos, sizeof(line), (uint32_t)entry->stack_pointer);
        append_newline(line, &pos, sizeof(line));
        if (pos >= sizeof(line))
            pos = sizeof(line) - 1;
        line[pos] = '\0';
        vfs_write("/System/tasks", line, pos);
    }
}

static void append_flags(char *dst, size_t *pos, size_t cap, uint32_t flags)
{
    append_char(dst, pos, cap, '[');
    if (flags & DEVICE_FLAG_PUBLISH)
        append_char(dst, pos, cap, 'P');
    if (flags & DEVICE_FLAG_INTERNAL)
        append_char(dst, pos, cap, 'I');
    append_char(dst, pos, cap, ']');
}

void debug_publish_device_list(void)
{
    const struct device_node *nodes[DEVMGR_MAX_DEVICES];
    size_t count = devmgr_enumerate(nodes, DEVMGR_MAX_DEVICES);

    vfs_write_file("/System/devices", NULL, 0);

    char line[192];
    size_t pos = 0;
    append_text(line, &pos, sizeof(line), "ID NAME TYPE FLAGS PARENT\n");
    vfs_write("/System/devices", line, pos);

    for (size_t i = 0; i < count; ++i)
    {
        const struct device_node *node = nodes[i];
        pos = 0;
        append_decimal(line, &pos, sizeof(line), node->id);
        append_char(line, &pos, sizeof(line), ' ');
        append_text(line, &pos, sizeof(line), node->name);
        append_char(line, &pos, sizeof(line), ' ');
        append_text(line, &pos, sizeof(line), node->type);
        append_char(line, &pos, sizeof(line), ' ');
        append_flags(line, &pos, sizeof(line), node->flags);
        append_char(line, &pos, sizeof(line), ' ');
        const char *parent = (node->parent) ? node->parent->name : "-";
        append_text(line, &pos, sizeof(line), parent);
        append_newline(line, &pos, sizeof(line));
        if (pos >= sizeof(line))
            pos = sizeof(line) - 1;
        line[pos] = '\0';
        vfs_write("/System/devices", line, pos);
    }
}

void debug_publish_all(void)
{
    debug_publish_memory_info();
    debug_publish_task_list();
    debug_publish_device_list();
}
