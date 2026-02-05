#include "sync.h"
#include "spinlock.h"
#include "proc.h"
#include "config.h"

#include <stddef.h>

struct sync_mutex
{
    int used;
    int locked;
    pid_t owner;
    pid_t waiters[CONFIG_SYNC_MAX_WAITERS];
    uint8_t wait_count;
};

struct sync_semaphore
{
    int used;
    int count;
    pid_t waiters[CONFIG_SYNC_MAX_WAITERS];
    uint8_t wait_count;
};

static struct sync_mutex mutexes[CONFIG_SYNC_MAX_MUTEXES];
static struct sync_semaphore semaphores[CONFIG_SYNC_MAX_SEMAPHORES];
static spinlock_t sync_lock;

static int waiter_exists(pid_t *list, uint8_t count, pid_t pid)
{
    for (uint8_t i = 0; i < count; ++i)
    {
        if (list[i] == pid)
            return 1;
    }
    return 0;
}

static int enqueue_waiter(pid_t *list, uint8_t *count, pid_t pid)
{
    if (!list || !count)
        return -1;
    if (*count >= CONFIG_SYNC_MAX_WAITERS)
        return -1;
    list[*count] = pid;
    ++(*count);
    return 0;
}

static pid_t dequeue_waiter(pid_t *list, uint8_t *count)
{
    if (!list || !count || *count == 0)
        return -1;
    pid_t pid = list[0];
    for (uint8_t i = 1; i < *count; ++i)
        list[i - 1] = list[i];
    --(*count);
    return pid;
}

void sync_init(void)
{
    spinlock_init(&sync_lock);
    for (int i = 0; i < CONFIG_SYNC_MAX_MUTEXES; ++i)
    {
        mutexes[i].used = 0;
        mutexes[i].locked = 0;
        mutexes[i].owner = -1;
        mutexes[i].wait_count = 0;
    }
    for (int i = 0; i < CONFIG_SYNC_MAX_SEMAPHORES; ++i)
    {
        semaphores[i].used = 0;
        semaphores[i].count = 0;
        semaphores[i].wait_count = 0;
    }
}

int sync_mutex_create(void)
{
    uint32_t flags;
    spinlock_lock_irqsave(&sync_lock, &flags);

    for (int i = 0; i < CONFIG_SYNC_MAX_MUTEXES; ++i)
    {
        if (!mutexes[i].used)
        {
            mutexes[i].used = 1;
            mutexes[i].locked = 0;
            mutexes[i].owner = -1;
            mutexes[i].wait_count = 0;
            spinlock_unlock_irqrestore(&sync_lock, flags);
            return i;
        }
    }

    spinlock_unlock_irqrestore(&sync_lock, flags);
    return -1;
}

int sync_mutex_lock(int id)
{
    if (id < 0 || id >= CONFIG_SYNC_MAX_MUTEXES)
        return -1;

    struct process *current = process_current();
    if (!current)
        return -1;

    pid_t self = current->pid;

    while (1)
    {
        uint32_t flags;
        spinlock_lock_irqsave(&sync_lock, &flags);

        struct sync_mutex *mtx = &mutexes[id];
        if (!mtx->used)
        {
            spinlock_unlock_irqrestore(&sync_lock, flags);
            return -1;
        }

        if (!mtx->locked)
        {
            mtx->locked = 1;
            mtx->owner = self;
            spinlock_unlock_irqrestore(&sync_lock, flags);
            return 0;
        }

        if (mtx->owner == self)
        {
            spinlock_unlock_irqrestore(&sync_lock, flags);
            return 0;
        }

        if (!waiter_exists(mtx->waiters, mtx->wait_count, self))
            enqueue_waiter(mtx->waiters, &mtx->wait_count, self);

        spinlock_unlock_irqrestore(&sync_lock, flags);
        process_block_current();
    }
}

int sync_mutex_unlock(int id)
{
    if (id < 0 || id >= CONFIG_SYNC_MAX_MUTEXES)
        return -1;

    struct process *current = process_current();
    if (!current)
        return -1;

    pid_t self = current->pid;

    pid_t wake_pid = -1;

    uint32_t flags;
    spinlock_lock_irqsave(&sync_lock, &flags);

    struct sync_mutex *mtx = &mutexes[id];
    if (!mtx->used || !mtx->locked || mtx->owner != self)
    {
        spinlock_unlock_irqrestore(&sync_lock, flags);
        return -1;
    }

    if (mtx->wait_count > 0)
    {
        wake_pid = dequeue_waiter(mtx->waiters, &mtx->wait_count);
        mtx->owner = wake_pid;
        mtx->locked = 1;
    }
    else
    {
        mtx->locked = 0;
        mtx->owner = -1;
    }

    spinlock_unlock_irqrestore(&sync_lock, flags);

    if (wake_pid > 0)
    {
        struct process *target = process_lookup(wake_pid);
        if (target)
            process_wake(target);
    }

    return 0;
}

int sync_semaphore_create(int initial_count)
{
    if (initial_count < 0)
        return -1;

    uint32_t flags;
    spinlock_lock_irqsave(&sync_lock, &flags);

    for (int i = 0; i < CONFIG_SYNC_MAX_SEMAPHORES; ++i)
    {
        if (!semaphores[i].used)
        {
            semaphores[i].used = 1;
            semaphores[i].count = initial_count;
            semaphores[i].wait_count = 0;
            spinlock_unlock_irqrestore(&sync_lock, flags);
            return i;
        }
    }

    spinlock_unlock_irqrestore(&sync_lock, flags);
    return -1;
}

int sync_semaphore_wait(int id)
{
    if (id < 0 || id >= CONFIG_SYNC_MAX_SEMAPHORES)
        return -1;

    struct process *current = process_current();
    if (!current)
        return -1;

    pid_t self = current->pid;

    while (1)
    {
        uint32_t flags;
        spinlock_lock_irqsave(&sync_lock, &flags);

        struct sync_semaphore *sem = &semaphores[id];
        if (!sem->used)
        {
            spinlock_unlock_irqrestore(&sync_lock, flags);
            return -1;
        }

        if (sem->count > 0)
        {
            sem->count -= 1;
            spinlock_unlock_irqrestore(&sync_lock, flags);
            return 0;
        }

        if (!waiter_exists(sem->waiters, sem->wait_count, self))
            enqueue_waiter(sem->waiters, &sem->wait_count, self);

        spinlock_unlock_irqrestore(&sync_lock, flags);
        process_block_current();
    }
}

int sync_semaphore_post(int id)
{
    if (id < 0 || id >= CONFIG_SYNC_MAX_SEMAPHORES)
        return -1;

    pid_t wake_pid = -1;

    uint32_t flags;
    spinlock_lock_irqsave(&sync_lock, &flags);

    struct sync_semaphore *sem = &semaphores[id];
    if (!sem->used)
    {
        spinlock_unlock_irqrestore(&sync_lock, flags);
        return -1;
    }

    if (sem->wait_count > 0)
    {
        wake_pid = dequeue_waiter(sem->waiters, &sem->wait_count);
    }
    else
    {
        sem->count += 1;
    }

    spinlock_unlock_irqrestore(&sync_lock, flags);

    if (wake_pid > 0)
    {
        struct process *target = process_lookup(wake_pid);
        if (target)
            process_wake(target);
    }

    return 0;
}
