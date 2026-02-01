#include "syscall.h"
#include "interrupts.h"
#include "proc.h"
#include "vga.h"
#include "spinlock.h"
#include "ipc.h"
#include "service.h"
#include "sync.h"

#include "config.h"

#include <stddef.h>
#include <stdint.h>

struct syscall_entry
{
    syscall_handler_t handler;
    const char *name;
};

static struct syscall_entry syscall_table[SYSCALL_TABLE_SIZE];
static spinlock_t syscall_lock;

static void copy_to_user(char *dst, const char *src, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        dst[i] = src[i];
}

static void copy_from_user(char *dst, const char *src, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        dst[i] = src[i];
}

int syscall_validate_user_pointer(const void *ptr)
{
    uintptr_t addr = (uintptr_t)ptr;
    if (addr == 0)
        return 0;
    if (addr >= CONFIG_USER_SPACE_LIMIT)
        return 0;
    return 1;
}

int syscall_validate_user_buffer(const void *ptr, size_t length)
{
    if (length == 0)
        return 1;
    if (!syscall_validate_user_pointer(ptr))
        return 0;

    uintptr_t addr = (uintptr_t)ptr;
    if (addr + length < addr)
        return 0;
    if ((addr + length) >= CONFIG_USER_SPACE_LIMIT)
        return 0;
    return 1;
}

static int32_t sys_write_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 2)
        return -1;

    const char *buf = (const char *)msg->args[0];
    size_t len = (size_t)msg->args[1];
    if (len == 0)
        return 0;
    if (!syscall_validate_user_buffer(buf, len))
        return -1;

    for (size_t i = 0; i < len; ++i)
    {
        char ch;
        copy_from_user(&ch, buf + i, 1);
        vga_write_char(ch);
    }

    return (int32_t)len;
}

static int32_t sys_yield_handler(struct syscall_envelope *msg)
{
    (void)msg;
    process_yield();
    return 0;
}

static int32_t sys_spawn_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 2)
        return -1;

    void (*entry)(void) = (void (*)(void))msg->args[0];
    size_t stack_size = (size_t)msg->args[1];
    if (!syscall_validate_user_pointer((const void *)entry))
        return -1;
    return process_create(entry, stack_size);
}

static int32_t sys_sleep_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 1)
        return -1;

    uint32_t ticks = msg->args[0];
    process_sleep(ticks);
    return 0;
}

static int32_t sys_ipc_send_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 3)
        return -1;

    pid_t target = (pid_t)msg->args[0];
    const void *buffer = (const void *)(uintptr_t)msg->args[1];
    size_t size = (size_t)msg->args[2];

    if (size > CONFIG_MSG_DATA_MAX)
        return -1;
    if (size > 0 && !syscall_validate_user_buffer(buffer, size))
        return -1;

    uint8_t local[CONFIG_MSG_DATA_MAX];
    if (size > 0)
        copy_from_user((char *)local, (const char *)buffer, size);

    return ipc_send(target, (size > 0) ? local : NULL, size);
}

static int32_t sys_ipc_recv_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 3)
        return -1;

    pid_t source = (pid_t)msg->args[0];
    void *buffer = (void *)(uintptr_t)msg->args[1];
    size_t max = (size_t)msg->args[2];

    if (max > 0 && !syscall_validate_user_buffer(buffer, max))
        return -1;

    size_t scratch_cap = (max > CONFIG_MSG_DATA_MAX) ? CONFIG_MSG_DATA_MAX : max;
    uint8_t scratch[CONFIG_MSG_DATA_MAX];
    uint8_t *recv_buf = (scratch_cap > 0) ? scratch : NULL;

    int rc = ipc_recv(source, recv_buf, scratch_cap);
    if (rc <= 0)
        return rc;

    if (buffer && max > 0 && recv_buf)
    {
        size_t available = (size_t)rc;
        size_t copy_len = (available > max) ? max : available;
        if (copy_len > scratch_cap)
            copy_len = scratch_cap;
        if (copy_len > 0)
            copy_to_user((char *)buffer, (const char *)recv_buf, copy_len);
    }

    return rc;
}

static int32_t sys_ipc_share_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 3)
        return -1;

    pid_t target = (pid_t)msg->args[0];
    void *addr = (void *)(uintptr_t)msg->args[1];
    size_t pages = (size_t)msg->args[2];

    if (pages == 0)
        return -1;
    if (!syscall_validate_user_pointer(addr))
        return -1;

    uintptr_t end_addr = (uintptr_t)addr + (pages * (size_t)CONFIG_IPC_PAGE_SIZE);
    if (end_addr < (uintptr_t)addr)
        return -1;
    if (end_addr >= CONFIG_USER_SPACE_LIMIT)
        return -1;

    return ipc_share(target, addr, pages);
}

static int32_t sys_service_connect_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 2)
        return -1;

    enum system_service service = (enum system_service)msg->args[0];
    uint32_t rights = msg->args[1];

    struct process *proc = process_current();
    if (!proc || proc->pid <= 0)
        return -1;

    if (service_grant_capabilities(proc->pid, service, rights) < 0)
        return -1;

    return service_pid(service);
}

static int32_t sys_send_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 3)
        return -1;

    int channel_id = (int)msg->args[0];
    struct ipc_message *user_message = (struct ipc_message *)(uintptr_t)msg->args[1];
    uint32_t flags = msg->args[2];

    if (!syscall_validate_user_buffer(user_message, sizeof(struct ipc_message)))
        return -1;

    struct ipc_message local;
    copy_from_user((char *)&local, (const char *)user_message, sizeof(struct ipc_message));

    if (local.size > CONFIG_MSG_DATA_MAX)
        return -1;

    uint8_t data_buf[CONFIG_MSG_DATA_MAX];
    if (local.size > 0)
    {
        if (!syscall_validate_user_buffer(local.data, local.size))
            return -1;
        copy_from_user((char *)data_buf, (const char *)local.data, local.size);
    }

    struct process *sender = process_current();
    if (!sender)
        return -1;

    return ipc_channel_send(channel_id, sender->pid, local.header, local.type, (local.size > 0) ? data_buf : NULL, local.size, flags);
}

static int32_t sys_recv_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 3)
        return -1;

    int channel_id = (int)msg->args[0];
    struct ipc_message *user_message = (struct ipc_message *)(uintptr_t)msg->args[1];
    uint32_t flags = msg->args[2];

    if (!syscall_validate_user_buffer(user_message, sizeof(struct ipc_message)))
        return -1;

    struct ipc_message request;
    copy_from_user((char *)&request, (const char *)user_message, sizeof(struct ipc_message));

    void *user_buffer = request.data;
    size_t user_capacity = request.size;
    if (user_buffer && user_capacity > 0 && !syscall_validate_user_buffer(user_buffer, user_capacity))
        return -1;

    struct process *proc = process_current();
    if (!proc)
        return -1;

    uint8_t data_buf[CONFIG_MSG_DATA_MAX];
    struct ipc_message delivery;

    int rc = ipc_channel_receive(proc, channel_id, &delivery, (user_buffer && user_capacity > 0) ? data_buf : NULL, (user_buffer && user_capacity > 0) ? CONFIG_MSG_DATA_MAX : 0, flags);
    if (rc <= 0)
        return rc;

    size_t copy_len = delivery.size;
    if (copy_len > CONFIG_MSG_DATA_MAX)
        copy_len = CONFIG_MSG_DATA_MAX;

    if (user_buffer && user_capacity > 0)
    {
        size_t to_copy = (copy_len < user_capacity) ? copy_len : user_capacity;
        if (to_copy > 0)
            copy_to_user((char *)user_buffer, (const char *)data_buf, to_copy);
        if (copy_len > user_capacity)
            delivery.header |= IPC_MESSAGE_TRUNCATED;
        delivery.size = (uint32_t)to_copy;
        delivery.data = user_buffer;
    }
    else
    {
        delivery.size = 0;
        delivery.data = NULL;
    }

    copy_to_user((char *)user_message, (const char *)&delivery, sizeof(struct ipc_message));
    return 1;
}

static int32_t sys_chan_create_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 2)
        return -1;

    const char *name = (const char *)(uintptr_t)msg->args[0];
    size_t name_len = (size_t)msg->args[1];
    uint32_t flags = (msg->argc >= 3) ? msg->args[2] : 0;

    char buffer[CONFIG_IPC_CHANNEL_NAME_MAX];
    size_t copy_len = 0;

    if (name && name_len > 0)
    {
        if (!syscall_validate_user_buffer(name, name_len))
            return -1;
        copy_len = (name_len < (CONFIG_IPC_CHANNEL_NAME_MAX - 1)) ? name_len : (CONFIG_IPC_CHANNEL_NAME_MAX - 1);
        copy_from_user(buffer, name, copy_len);
        buffer[copy_len] = '\0';
    }
    else
    {
        buffer[0] = '\0';
    }

    return ipc_channel_create((copy_len > 0) ? buffer : NULL, copy_len, flags);
}

static int32_t sys_chan_join_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 1)
        return -1;

    int channel_id = (int)msg->args[0];
    struct process *proc = process_current();
    if (!proc)
        return -1;
    return ipc_channel_join(proc, channel_id);
}

static int32_t sys_chan_leave_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 1)
        return -1;

    int channel_id = (int)msg->args[0];
    struct process *proc = process_current();
    if (!proc)
        return -1;
    return ipc_channel_leave(proc, channel_id);
}

static int32_t sys_chan_peek_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 1)
        return -1;
    int channel_id = (int)msg->args[0];
    return ipc_channel_peek(channel_id);
}

static int32_t sys_service_channel_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 1)
        return -1;
    int service = (int)msg->args[0];
    return ipc_get_service_channel((enum ipc_service_channel)service);
}

static int32_t sys_exit_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 1)
        return -1;
    int code = (int)msg->args[0];
    process_exit(code);
    return 0;
}

static int32_t sys_sched_set_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 4)
        return -1;

    int pid = (int)msg->args[0];
    uint8_t policy = (uint8_t)msg->args[1];
    uint32_t weight = msg->args[2];
    uint64_t deadline = (uint64_t)msg->args[3];
    return process_set_scheduler(pid, policy, weight, deadline);
}

static int32_t sys_mutex_create_handler(struct syscall_envelope *msg)
{
    (void)msg;
    return sync_mutex_create();
}

static int32_t sys_mutex_lock_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 1)
        return -1;
    int id = (int)msg->args[0];
    return sync_mutex_lock(id);
}

static int32_t sys_mutex_unlock_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 1)
        return -1;
    int id = (int)msg->args[0];
    return sync_mutex_unlock(id);
}

static int32_t sys_sem_create_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 1)
        return -1;
    int initial = (int)msg->args[0];
    return sync_semaphore_create(initial);
}

static int32_t sys_sem_wait_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 1)
        return -1;
    int id = (int)msg->args[0];
    return sync_semaphore_wait(id);
}

static int32_t sys_sem_post_handler(struct syscall_envelope *msg)
{
    if (msg->argc < 1)
        return -1;
    int id = (int)msg->args[0];
    return sync_semaphore_post(id);
}

static int32_t syscall_invoke(struct syscall_envelope *msg)
{
    if (msg->number >= SYSCALL_TABLE_SIZE)
        return -1;

    syscall_handler_t handler = syscall_table[msg->number].handler;
    if (!handler)
        return -1;

    return handler(msg);
}

int syscall_register_handler(uint32_t number, syscall_handler_t handler, const char *name)
{
    if (number >= SYSCALL_TABLE_SIZE || !handler)
        return -1;

    uint32_t flags;
    spinlock_lock_irqsave(&syscall_lock, &flags);
    struct syscall_entry *entry = &syscall_table[number];
    if (entry->handler && entry->handler != handler)
    {
        spinlock_unlock_irqrestore(&syscall_lock, flags);
        return -1;
    }
    entry->handler = handler;
    entry->name = name;
    spinlock_unlock_irqrestore(&syscall_lock, flags);
    return 0;
}

int syscall_unregister_handler(uint32_t number)
{
    if (number >= SYSCALL_TABLE_SIZE)
        return -1;

    uint32_t flags;
    spinlock_lock_irqsave(&syscall_lock, &flags);
    syscall_table[number].handler = NULL;
    syscall_table[number].name = NULL;
    spinlock_unlock_irqrestore(&syscall_lock, flags);
    return 0;
}

void syscall_init(void)
{
    spinlock_init(&syscall_lock);
    for (size_t i = 0; i < SYSCALL_TABLE_SIZE; ++i)
    {
        syscall_table[i].handler = NULL;
        syscall_table[i].name = NULL;
    }

    syscall_register_handler(SYS_WRITE, sys_write_handler, "sys_write");
    syscall_register_handler(SYS_YIELD, sys_yield_handler, "sys_yield");
    syscall_register_handler(SYS_SPAWN, sys_spawn_handler, "sys_spawn");
    syscall_register_handler(SYS_SEND, sys_send_handler, "sys_send");
    syscall_register_handler(SYS_RECV, sys_recv_handler, "sys_recv");
    syscall_register_handler(SYS_EXIT, sys_exit_handler, "sys_exit");
    syscall_register_handler(SYS_CHAN_CREATE, sys_chan_create_handler, "sys_chan_create");
    syscall_register_handler(SYS_CHAN_JOIN, sys_chan_join_handler, "sys_chan_join");
    syscall_register_handler(SYS_CHAN_LEAVE, sys_chan_leave_handler, "sys_chan_leave");
    syscall_register_handler(SYS_CHAN_PEEK, sys_chan_peek_handler, "sys_chan_peek");
    syscall_register_handler(SYS_GET_SERVICE_CHANNEL, sys_service_channel_handler, "sys_get_service_channel");
    syscall_register_handler(SYS_SLEEP, sys_sleep_handler, "sys_sleep");
    syscall_register_handler(SYS_IPC_SEND, sys_ipc_send_handler, "sys_ipc_send");
    syscall_register_handler(SYS_IPC_RECV, sys_ipc_recv_handler, "sys_ipc_recv");
    syscall_register_handler(SYS_IPC_SHARE, sys_ipc_share_handler, "sys_ipc_share");
    syscall_register_handler(SYS_SERVICE_CONNECT, sys_service_connect_handler, "sys_service_connect");
    syscall_register_handler(SYS_SCHED_SET, sys_sched_set_handler, "sys_sched_set");
    syscall_register_handler(SYS_MUTEX_CREATE, sys_mutex_create_handler, "sys_mutex_create");
    syscall_register_handler(SYS_MUTEX_LOCK, sys_mutex_lock_handler, "sys_mutex_lock");
    syscall_register_handler(SYS_MUTEX_UNLOCK, sys_mutex_unlock_handler, "sys_mutex_unlock");
    syscall_register_handler(SYS_SEM_CREATE, sys_sem_create_handler, "sys_sem_create");
    syscall_register_handler(SYS_SEM_WAIT, sys_sem_wait_handler, "sys_sem_wait");
    syscall_register_handler(SYS_SEM_POST, sys_sem_post_handler, "sys_sem_post");
}

void syscall_handler(struct regs *frame)
{
    struct syscall_envelope *message = (struct syscall_envelope *)frame->eax;
    if (!syscall_validate_user_buffer(message, sizeof(struct syscall_envelope)))
    {
        frame->eax = (uint32_t)-1;
        return;
    }

    if (message->argc > SYSCALL_MAX_ARGS)
    {
        message->status = 1;
        message->result = -1;
        frame->eax = (uint32_t)-1;
        return;
    }

    int32_t result = syscall_invoke(message);
    message->result = result;
    message->status = (result < 0) ? 1u : 0u;
    frame->eax = (uint32_t)result;
}
