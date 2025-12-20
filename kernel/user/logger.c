#include "syslib.h"
#include "../config.h"

struct logger_event
{
    uint32_t seq;
    uint8_t level;
    uint8_t reserved0;
    uint8_t reserved1;
    uint8_t reserved2;
    char module[CONFIG_KLOG_MODULE_NAME_LEN];
    char text[CONFIG_KLOG_ENTRY_LEN];
};

static size_t bounded_strlen(const char *s, size_t max)
{
    size_t len = 0;
    while (len < max && s[len])
        ++len;
    return len;
}

static void append_char(char *dst, size_t *pos, size_t cap, char ch)
{
    if (*pos + 1 >= cap)
        return;
    dst[*pos] = ch;
    ++(*pos);
}

static void append_text(char *dst, size_t *pos, size_t cap, const char *text)
{
    if (!text)
        return;
    while (*text && *pos + 1 < cap)
    {
        dst[*pos] = *text;
        ++(*pos);
        ++text;
    }
}

static void append_u32(char *dst, size_t *pos, size_t cap, uint32_t value)
{
    char temp[12];
    size_t idx = 0;
    if (value == 0)
    {
        temp[idx++] = '0';
    }
    else
    {
        while (value > 0 && idx < sizeof(temp))
        {
            temp[idx++] = (char)('0' + (value % 10U));
            value /= 10U;
        }
    }
    while (idx > 0 && *pos + 1 < cap)
    {
        dst[*pos] = temp[--idx];
        ++(*pos);
    }
}

void user_logger(void)
{
    static const char *level_names[] = { "DEBUG", "INFO", "WARN", "ERROR" };

    int channel = sys_get_service_channel(IPC_SERVICE_LOGGER);
    if (channel < 0)
        sys_exit(1);

    if (sys_chan_join(channel) < 0)
        sys_exit(1);

    struct logger_event event;
    struct ipc_message message;

    for (;;)
    {
        message.header = 0;
        message.sender_pid = 0;
        message.type = 0;
        message.size = sizeof(event);
        message.data = &event;

        int rc = sys_chan_recv(channel, &message, 0);
        if (rc <= 0)
            continue;

        size_t text_len = bounded_strlen(event.text, CONFIG_KLOG_ENTRY_LEN);
        char line[CONFIG_KLOG_ENTRY_LEN + 32];
        size_t pos = 0;

        append_char(line, &pos, sizeof(line), '[');
        append_u32(line, &pos, sizeof(line), event.seq);
        append_char(line, &pos, sizeof(line), ']');
        append_char(line, &pos, sizeof(line), ' ');

        const char *level = (event.level < 4) ? level_names[event.level] : "LOG";
        append_text(line, &pos, sizeof(line), level);
        append_text(line, &pos, sizeof(line), " ");
        append_char(line, &pos, sizeof(line), '(');
        append_text(line, &pos, sizeof(line), event.module);
        append_char(line, &pos, sizeof(line), ')');
        append_text(line, &pos, sizeof(line), ": ");

        for (size_t i = 0; i < text_len && pos + 1 < sizeof(line); ++i)
            line[pos++] = event.text[i];

        line[pos] = '\0';

        sys_write(line, pos);
        sys_write("\n", 1);
    }
}
