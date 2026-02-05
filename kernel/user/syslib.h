#ifndef USER_SYSLIB_H
#define USER_SYSLIB_H

#include "../syscall.h"
#include "../ipc_types.h"
#include "../service_types.h"
#include <stddef.h>
#include <stdint.h>

#define USER_SCHED_POLICY_FAIR 0u
#define USER_SCHED_POLICY_DEADLINE 1u

static inline int32_t sys_call(uint32_t number, uint32_t argc, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    struct syscall_envelope request;
    request.number = number;
    request.argc = argc;
    request.args[0] = arg0;
    request.args[1] = arg1;
    request.args[2] = arg2;
    request.args[3] = arg3;
    request.result = 0;
    request.status = 0;

    int32_t ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(&request) : "memory");
    return request.result;
}

static inline int sys_write(const char *buf, size_t len)
{
    return (int)sys_call(SYS_WRITE, 2, (uint32_t)(uintptr_t)buf, (uint32_t)len, 0, 0);
}

static inline int sys_yield(void)
{
    return (int)sys_call(SYS_YIELD, 0, 0, 0, 0, 0);
}

static inline int sys_spawn(void (*entry)(void), size_t stack_size)
{
    return (int)sys_call(SYS_SPAWN, 2, (uint32_t)(uintptr_t)entry, (uint32_t)stack_size, 0, 0);
}

static inline int sys_thread_create(void (*entry)(void), size_t stack_size)
{
    return sys_spawn(entry, stack_size);
}

static inline int sys_sleep(uint32_t ticks)
{
    return (int)sys_call(SYS_SLEEP, 1, ticks, 0, 0, 0);
}

static inline int sys_ipc_send(pid_t target, const void *buffer, size_t size)
{
    return (int)sys_call(SYS_IPC_SEND, 3, (uint32_t)target, (uint32_t)(uintptr_t)buffer, (uint32_t)size, 0);
}

static inline int sys_ipc_recv(pid_t source, void *buffer, size_t max)
{
    return (int)sys_call(SYS_IPC_RECV, 3, (uint32_t)source, (uint32_t)(uintptr_t)buffer, (uint32_t)max, 0);
}

static inline int sys_ipc_share(pid_t target, void *addr, size_t pages)
{
    return (int)sys_call(SYS_IPC_SHARE, 3, (uint32_t)target, (uint32_t)(uintptr_t)addr, (uint32_t)pages, 0);
}

static inline int sys_service_connect(enum system_service service, uint32_t rights)
{
    return (int)sys_call(SYS_SERVICE_CONNECT, 2, (uint32_t)service, rights, 0, 0);
}

static inline int sys_sched_set(int pid, uint8_t policy, uint32_t weight, uint64_t deadline_ticks)
{
    uint32_t deadline_low = (uint32_t)(deadline_ticks & 0xFFFFFFFFu);
    return (int)sys_call(SYS_SCHED_SET, 4, (uint32_t)pid, (uint32_t)policy, weight, deadline_low);
}

static inline int sys_mutex_create(void)
{
    return (int)sys_call(SYS_MUTEX_CREATE, 0, 0, 0, 0, 0);
}

static inline int sys_mutex_lock(int id)
{
    return (int)sys_call(SYS_MUTEX_LOCK, 1, (uint32_t)id, 0, 0, 0);
}

static inline int sys_mutex_unlock(int id)
{
    return (int)sys_call(SYS_MUTEX_UNLOCK, 1, (uint32_t)id, 0, 0, 0);
}

static inline int sys_sem_create(int initial_count)
{
    return (int)sys_call(SYS_SEM_CREATE, 1, (uint32_t)initial_count, 0, 0, 0);
}

static inline int sys_sem_wait(int id)
{
    return (int)sys_call(SYS_SEM_WAIT, 1, (uint32_t)id, 0, 0, 0);
}

static inline int sys_sem_post(int id)
{
    return (int)sys_call(SYS_SEM_POST, 1, (uint32_t)id, 0, 0, 0);
}

static inline int sys_chan_create(const char *name, size_t name_len, uint32_t flags)
{
    return (int)sys_call(SYS_CHAN_CREATE, 3, (uint32_t)(uintptr_t)name, (uint32_t)name_len, flags, 0);
}

static inline int sys_chan_join(int channel_id)
{
    return (int)sys_call(SYS_CHAN_JOIN, 1, (uint32_t)channel_id, 0, 0, 0);
}

static inline int sys_chan_leave(int channel_id)
{
    return (int)sys_call(SYS_CHAN_LEAVE, 1, (uint32_t)channel_id, 0, 0, 0);
}

static inline int sys_chan_peek(int channel_id)
{
    return (int)sys_call(SYS_CHAN_PEEK, 1, (uint32_t)channel_id, 0, 0, 0);
}

static inline int sys_get_service_channel(enum ipc_service_channel service)
{
    return (int)sys_call(SYS_GET_SERVICE_CHANNEL, 1, (uint32_t)service, 0, 0, 0);
}

static inline int sys_chan_send(int channel_id, struct ipc_message *message, uint32_t flags)
{
    return (int)sys_call(SYS_SEND, 3, (uint32_t)channel_id, (uint32_t)(uintptr_t)message, flags, 0);
}

static inline int sys_chan_recv(int channel_id, struct ipc_message *message, uint32_t flags)
{
    return (int)sys_call(SYS_RECV, 3, (uint32_t)channel_id, (uint32_t)(uintptr_t)message, flags, 0);
}

static inline void sys_exit(int code)
{
    (void)sys_call(SYS_EXIT, 1, (uint32_t)code, 0, 0, 0);
}

#endif
