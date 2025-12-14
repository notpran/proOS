#include <stddef.h>
#include <stdint.h>

#include "module_api.h"

#include "klog.h"
#include "pit.h"
#include "ramfs.h"

MODULE_METADATA("time", "0.1.0", MODULE_FLAG_AUTOSTART);

static size_t local_strlen(const char *s)
{
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        ++len;
    return len;
}

static size_t format_u64(uint64_t value, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0)
        return 0;

    char tmp[32];
    size_t idx = 0;
    if (value == 0)
    {
        if (buffer_size > 1)
        {
            buffer[0] = '0';
            buffer[1] = '\0';
            return 1;
        }
        buffer[0] = '\0';
        return 0;
    }

    while (value > 0 && idx < sizeof(tmp))
    {
        tmp[idx++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    size_t out_len = (idx < (buffer_size - 1)) ? idx : (buffer_size - 1);
    for (size_t i = 0; i < out_len; ++i)
        buffer[i] = tmp[out_len - i - 1];
    buffer[out_len] = '\0';
    return out_len;
}

int module_init(void)
{
    klog_info("time.module: init");

    uint64_t ticks = get_ticks();
    char ticks_text[32];
    size_t tick_len = format_u64(ticks, ticks_text, sizeof(ticks_text));
    (void)tick_len;

    char message[64] = "time.module: ticks=";
    size_t prefix_len = local_strlen(message);
    size_t available = (prefix_len < sizeof(message)) ? (sizeof(message) - prefix_len) : 0;
    if (available > 1)
    {
        size_t copy_len = local_strlen(ticks_text);
        if (copy_len >= available)
            copy_len = available - 1;
        for (size_t i = 0; i < copy_len; ++i)
            message[prefix_len + i] = ticks_text[i];
        message[prefix_len + copy_len] = '\0';
    }

    klog_emit(KLOG_INFO, message);

    const char *file_name = "uptime.txt";
    if (ramfs_write(file_name, message, local_strlen(message)) < 0)
        klog_warn("time.module: ramfs_write failed");

    return 0;
}

void module_exit(void)
{
    klog_info("time.module: exit");
}
