#include "syslib.h"

#define BUFFER_SIZE 256

extern void user_echo_service(void);

int g_echo_channel = -1;

static size_t str_len(const char *s)
{
    size_t len = 0;
    while (s[len])
        ++len;
    return len;
}

static void write_line(const char *text)
{
    size_t len = str_len(text);
    if (len > 0)
        sys_write(text, len);
    sys_write("\n", 1);
}

void user_init(void)
{
    write_line("init: starting echo service");

    int channel = sys_chan_create("echo", 4, 0);
    if (channel < 0)
    {
        write_line("init: channel create failed");
        sys_exit(1);
    }

    if (sys_chan_join(channel) < 0)
    {
        write_line("init: join failed");
        sys_exit(1);
    }

    g_echo_channel = channel;

    int echo_pid = sys_spawn(user_echo_service, 4096);
    if (echo_pid < 0)
    {
        write_line("init: spawn failed");
        sys_exit(1);
    }

    const char *text = "Hello";
    struct ipc_message message;
    message.header = 0;
    message.sender_pid = 0;
    message.type = 1;
    message.size = str_len(text);
    message.data = (void *)text;

    if (sys_chan_send(channel, &message, 0) < 0)
        write_line("init: send failed");

    char reply_buffer[BUFFER_SIZE];
    struct ipc_message reply;
    reply.header = 0;
    reply.sender_pid = 0;
    reply.type = 0;
    reply.size = sizeof(reply_buffer) - 1;
    reply.data = reply_buffer;

    if (sys_chan_recv(channel, &reply, 0) > 0)
    {
        size_t count = (reply.size < (sizeof(reply_buffer) - 1)) ? reply.size : (sizeof(reply_buffer) - 1);
        reply_buffer[count] = '\0';
        sys_write(reply_buffer, count);
        sys_write("\n", 1);
    }

    sys_exit(0);
}
