#include "ipc.h"

#include "proc.h"
#include "spinlock.h"
#include "klog.h"

#include <stddef.h>
#include <stdint.h>

#define IPC_CHANNEL_FLAG_KERNEL 0x1u
#define IPC_INVALID_PID (-1)

struct ipc_share_record
{
    uint8_t used;
    pid_t owner;
    pid_t target;
    uintptr_t addr;
    size_t pages;
    uint32_t flags;
};

static spinlock_t capability_lock;
static spinlock_t share_lock;

static struct ipc_share_record share_table[CONFIG_IPC_MAX_SHARED_REGIONS];
static int ipc_initialized = 0;

static void buffer_copy(uint8_t *dst, const uint8_t *src, size_t length)
{
    if (!dst || !src || length == 0)
        return;

    for (size_t i = 0; i < length; ++i)
        dst[i] = src[i];
}

static void mailbox_init(struct ipc_mailbox_state *mailbox)
{
    if (!mailbox)
        return;

    spinlock_init(&mailbox->lock);
    mailbox->count = 0;
    mailbox->waiter_count = 0;

    for (size_t i = 0; i < CONFIG_MSG_QUEUE_LEN; ++i)
    {
        mailbox->slots[i].used = 0;
        mailbox->slots[i].sender = 0;
        mailbox->slots[i].flags = 0;
        mailbox->slots[i].size = 0;
    }

    for (size_t i = 0; i < CONFIG_IPC_ENDPOINT_WAITERS; ++i)
        mailbox->waiters[i] = IPC_INVALID_PID;
}

static void mailbox_clear(struct ipc_mailbox_state *mailbox)
{
    if (!mailbox)
        return;

    uint32_t flags;
    spinlock_lock_irqsave(&mailbox->lock, &flags);

    mailbox->count = 0;
    mailbox->waiter_count = 0;

    for (size_t i = 0; i < CONFIG_MSG_QUEUE_LEN; ++i)
    {
        mailbox->slots[i].used = 0;
        mailbox->slots[i].sender = 0;
        mailbox->slots[i].flags = 0;
        mailbox->slots[i].size = 0;
    }

    for (size_t i = 0; i < CONFIG_IPC_ENDPOINT_WAITERS; ++i)
        mailbox->waiters[i] = IPC_INVALID_PID;

    spinlock_unlock_irqrestore(&mailbox->lock, flags);
}

static int mailbox_push_waiter(struct ipc_mailbox_state *mailbox, pid_t pid)
{
    if (!mailbox || pid <= 0)
        return 0;

    for (uint8_t i = 0; i < mailbox->waiter_count; ++i)
    {
        if (mailbox->waiters[i] == pid)
            return 1;
    }

    if (mailbox->waiter_count >= CONFIG_IPC_ENDPOINT_WAITERS)
        return 0;

    mailbox->waiters[mailbox->waiter_count++] = pid;
    return 1;
}

static pid_t mailbox_pop_waiter(struct ipc_mailbox_state *mailbox)
{
    if (!mailbox || mailbox->waiter_count == 0)
        return IPC_INVALID_PID;

    pid_t value = mailbox->waiters[0];
    for (uint8_t i = 1; i < mailbox->waiter_count; ++i)
        mailbox->waiters[i - 1] = mailbox->waiters[i];
    mailbox->waiters[mailbox->waiter_count - 1] = IPC_INVALID_PID;
    --mailbox->waiter_count;
    return value;
}

static int mailbox_try_dequeue(struct process *proc, pid_t source, struct ipc_mailbox_slot *out)
{
    if (!proc || !out)
        return 0;

    struct ipc_mailbox_state *mailbox = &proc->ipc_mailbox;
    uint32_t flags;
    spinlock_lock_irqsave(&mailbox->lock, &flags);

    int match_index = -1;
    for (uint8_t i = 0; i < mailbox->count; ++i)
    {
        struct ipc_mailbox_slot *slot = &mailbox->slots[i];
        if (!slot->used)
            continue;
        if (source != IPC_ANY_PROCESS && slot->sender != source)
            continue;
        match_index = (int)i;
        break;
    }

    if (match_index >= 0)
    {
        *out = mailbox->slots[match_index];
        for (uint8_t j = (uint8_t)match_index + 1; j < mailbox->count; ++j)
            mailbox->slots[j - 1] = mailbox->slots[j];
        if (mailbox->count > 0)
            --mailbox->count;
        mailbox->slots[mailbox->count].used = 0;
        mailbox->slots[mailbox->count].sender = 0;
        mailbox->slots[mailbox->count].flags = 0;
        mailbox->slots[mailbox->count].size = 0;
    }

    spinlock_unlock_irqrestore(&mailbox->lock, flags);
    return (match_index >= 0);
}

static int mailbox_enqueue(struct process *target, pid_t sender, const uint8_t *data, size_t size, uint32_t flags_value)
{
    if (!target)
        return -1;

    struct ipc_mailbox_state *mailbox = &target->ipc_mailbox;

    uint32_t irq_flags;
    spinlock_lock_irqsave(&mailbox->lock, &irq_flags);

    if (mailbox->count >= CONFIG_MSG_QUEUE_LEN)
    {
        spinlock_unlock_irqrestore(&mailbox->lock, irq_flags);
        return -1;
    }

    struct ipc_mailbox_slot *slot = &mailbox->slots[mailbox->count];
    slot->used = 1;
    slot->sender = sender;
    slot->flags = flags_value;
    slot->size = (uint32_t)size;
    if (data && size > 0)
        buffer_copy(slot->data, data, size);

    mailbox->count++;

    pid_t wake_pid = mailbox_pop_waiter(mailbox);

    spinlock_unlock_irqrestore(&mailbox->lock, irq_flags);

    if (wake_pid > 0)
    {
        struct process *wake_proc = process_lookup(wake_pid);
        if (wake_proc)
            process_wake(wake_proc);
    }

    return (int)size;
}

static struct ipc_cap_entry *capability_find_slot(struct process *proc, pid_t peer)
{
    if (!proc)
        return NULL;

    for (size_t i = 0; i < CONFIG_IPC_CAPACITY_PER_PROC; ++i)
    {
        if (!proc->ipc_caps[i].used)
            continue;
        if (proc->ipc_caps[i].peer == peer)
            return &proc->ipc_caps[i];
    }
    return NULL;
}

static int capability_check(struct process *proc, pid_t peer, uint32_t rights)
{
    if (!proc)
        return 1;
    if (rights == 0)
        return 1;
    if (proc->kind == THREAD_KIND_KERNEL)
        return 1;
    if (peer <= 0)
        return 1;
    if (proc->pid == peer)
        return 1;

    uint32_t lock_flags;
    spinlock_lock_irqsave(&capability_lock, &lock_flags);
    struct ipc_cap_entry *entry = capability_find_slot(proc, peer);
    uint32_t granted = entry ? entry->rights : 0;
    spinlock_unlock_irqrestore(&capability_lock, lock_flags);

    return ((granted & rights) == rights);
}

static int share_attach_to_process(struct process *proc, int share_id)
{
    if (!proc)
        return 0;

    for (size_t i = 0; i < CONFIG_IPC_MAX_SHARED_PER_PROC; ++i)
    {
        if (!proc->ipc_shares[i].used)
        {
            proc->ipc_shares[i].used = 1;
            proc->ipc_shares[i].share_id = share_id;
            if (proc->ipc_share_count < CONFIG_IPC_MAX_SHARED_PER_PROC)
                ++proc->ipc_share_count;
            return 0;
        }
    }
    return -1;
}

static void share_detach_from_process(struct process *proc, int share_id)
{
    if (!proc)
        return;

    for (size_t i = 0; i < CONFIG_IPC_MAX_SHARED_PER_PROC; ++i)
    {
        if (!proc->ipc_shares[i].used)
            continue;
        if (proc->ipc_shares[i].share_id == share_id)
        {
            proc->ipc_shares[i].used = 0;
            proc->ipc_shares[i].share_id = -1;
            if (proc->ipc_share_count > 0)
                --proc->ipc_share_count;
            break;
        }
    }
}

static void share_drop_for_pid(pid_t pid)
{
    if (pid <= 0)
        return;

    uint32_t lock_flags;
    spinlock_lock_irqsave(&share_lock, &lock_flags);

    for (size_t i = 0; i < CONFIG_IPC_MAX_SHARED_REGIONS; ++i)
    {
        struct ipc_share_record *record = &share_table[i];
        if (!record->used)
            continue;
        if (record->owner != pid && record->target != pid)
            continue;

        pid_t owner_pid = record->owner;
        pid_t target_pid = record->target;

        if (owner_pid > 0)
        {
            struct process *owner_proc = process_lookup(owner_pid);
            if (owner_proc)
                share_detach_from_process(owner_proc, (int)i);
        }
        if (target_pid > 0)
        {
            struct process *target_proc = process_lookup(target_pid);
            if (target_proc)
                share_detach_from_process(target_proc, (int)i);
        }

        record->used = 0;
        record->owner = IPC_INVALID_PID;
        record->target = IPC_INVALID_PID;
        record->addr = 0;
        record->pages = 0;
        record->flags = 0;
    }

    spinlock_unlock_irqrestore(&share_lock, lock_flags);
}

void ipc_attach_process(struct process *proc)
{
    if (!proc)
        return;

    mailbox_init(&proc->ipc_mailbox);

    for (size_t i = 0; i < CONFIG_IPC_CAPACITY_PER_PROC; ++i)
    {
        proc->ipc_caps[i].used = 0;
        proc->ipc_caps[i].peer = IPC_INVALID_PID;
        proc->ipc_caps[i].rights = 0;
    }
    proc->ipc_cap_count = 0;

    for (size_t i = 0; i < CONFIG_IPC_MAX_SHARED_PER_PROC; ++i)
    {
        proc->ipc_shares[i].used = 0;
        proc->ipc_shares[i].share_id = -1;
    }
    proc->ipc_share_count = 0;
    proc->ipc_waiting = 0;
}

void ipc_detach_process(struct process *proc)
{
    if (!proc)
        return;

    mailbox_clear(&proc->ipc_mailbox);

    if (!ipc_initialized)
    {
        for (size_t i = 0; i < CONFIG_IPC_CAPACITY_PER_PROC; ++i)
        {
            proc->ipc_caps[i].used = 0;
            proc->ipc_caps[i].peer = IPC_INVALID_PID;
            proc->ipc_caps[i].rights = 0;
        }
        proc->ipc_cap_count = 0;

        for (size_t i = 0; i < CONFIG_IPC_MAX_SHARED_PER_PROC; ++i)
        {
            proc->ipc_shares[i].used = 0;
            proc->ipc_shares[i].share_id = -1;
        }
        proc->ipc_share_count = 0;
        proc->ipc_waiting = 0;
        return;
    }

    uint32_t cap_flags;
    spinlock_lock_irqsave(&capability_lock, &cap_flags);
    for (size_t i = 0; i < CONFIG_IPC_CAPACITY_PER_PROC; ++i)
    {
        proc->ipc_caps[i].used = 0;
        proc->ipc_caps[i].peer = IPC_INVALID_PID;
        proc->ipc_caps[i].rights = 0;
    }
    proc->ipc_cap_count = 0;
    spinlock_unlock_irqrestore(&capability_lock, cap_flags);

    share_drop_for_pid(proc->pid);

    for (size_t i = 0; i < CONFIG_IPC_MAX_SHARED_PER_PROC; ++i)
    {
        proc->ipc_shares[i].used = 0;
        proc->ipc_shares[i].share_id = -1;
    }
    proc->ipc_share_count = 0;
    proc->ipc_waiting = 0;
}

int ipc_send(pid_t target, const void *msg, size_t size)
{
    if (!ipc_initialized)
        return -1;

    if (target <= 0)
        return -1;
    if (size > CONFIG_MSG_DATA_MAX)
        return -1;
    if (size > 0 && !msg)
        return -1;

    struct process *target_proc = process_lookup(target);
    if (!target_proc)
        return -1;

    struct process *sender_proc = process_current();
    if (sender_proc && !capability_check(sender_proc, target, IPC_RIGHT_SEND))
        return -1;

    uint8_t buffer[CONFIG_MSG_DATA_MAX];
    if (size > 0 && msg)
    {
        const uint8_t *source = (const uint8_t *)msg;
        buffer_copy(buffer, source, size);
    }

    pid_t sender_pid = sender_proc ? sender_proc->pid : 0;
    return mailbox_enqueue(target_proc, sender_pid, (size > 0) ? buffer : NULL, size, 0);
}

int ipc_recv(pid_t source, void *buffer, size_t max)
{
    if (!ipc_initialized)
        return -1;

    struct process *proc = process_current();
    if (!proc)
        return -1;

    if (max > 0 && !buffer)
        return -1;

    for (;;)
    {
        struct ipc_mailbox_slot message;
        if (mailbox_try_dequeue(proc, source, &message))
        {
            if (!capability_check(proc, message.sender, IPC_RIGHT_RECV))
            {
                klog_warn("ipc: dropping message without recv capability");
                continue;
            }

            size_t to_copy = (message.size < max) ? message.size : max;
            if (buffer && to_copy > 0)
                buffer_copy((uint8_t *)buffer, message.data, to_copy);

            proc->ipc_waiting = 0;
            return (int)message.size;
        }

        if (proc->kind == THREAD_KIND_KERNEL)
            return 0;

        struct ipc_mailbox_state *mailbox = &proc->ipc_mailbox;
        uint32_t flags;
        spinlock_lock_irqsave(&mailbox->lock, &flags);
        int enqueued = mailbox_push_waiter(mailbox, proc->pid);
        spinlock_unlock_irqrestore(&mailbox->lock, flags);

        if (!enqueued)
            return -1;

        proc->ipc_waiting = 1;
        process_block_current();
        proc->ipc_waiting = 0;
    }
}

int ipc_share(pid_t target, void *addr, size_t pages)
{
    if (!ipc_initialized)
        return -1;

    if (target <= 0 || !addr || pages == 0)
        return -1;

    uintptr_t base = (uintptr_t)addr;
    if ((base & (CONFIG_IPC_PAGE_SIZE - 1u)) != 0u)
        return -1;

    uintptr_t limit = base + (pages * (size_t)CONFIG_IPC_PAGE_SIZE);
    if (limit < base || limit > CONFIG_USER_SPACE_LIMIT)
        return -1;

    struct process *owner = process_current();
    if (!owner)
        return -1;

    if (!capability_check(owner, target, IPC_RIGHT_SHARE))
        return -1;

    struct process *target_proc = process_lookup(target);
    if (!target_proc)
        return -1;

    uint32_t flags;
    spinlock_lock_irqsave(&share_lock, &flags);

    int slot = -1;
    for (size_t i = 0; i < CONFIG_IPC_MAX_SHARED_REGIONS; ++i)
    {
        if (!share_table[i].used)
        {
            slot = (int)i;
            break;
        }
    }

    if (slot < 0)
    {
        spinlock_unlock_irqrestore(&share_lock, flags);
        return -1;
    }

    share_table[slot].used = 1;
    share_table[slot].owner = owner->pid;
    share_table[slot].target = target;
    share_table[slot].addr = base;
    share_table[slot].pages = pages;
    share_table[slot].flags = 0;

    if (share_attach_to_process(owner, slot) < 0 || share_attach_to_process(target_proc, slot) < 0)
    {
        share_detach_from_process(owner, slot);
        share_detach_from_process(target_proc, slot);
        share_table[slot].used = 0;
        share_table[slot].owner = IPC_INVALID_PID;
        share_table[slot].target = IPC_INVALID_PID;
        share_table[slot].addr = 0;
        share_table[slot].pages = 0;
        share_table[slot].flags = 0;
        spinlock_unlock_irqrestore(&share_lock, flags);
        return -1;
    }

    spinlock_unlock_irqrestore(&share_lock, flags);
    return 0;
}

int ipc_cap_grant(pid_t owner, pid_t target, uint32_t rights)
{
    if (!ipc_initialized)
        return -1;

    if (owner <= 0 || target <= 0 || rights == 0)
        return -1;

    struct process *proc = process_lookup(owner);
    if (!proc)
        return -1;

    uint32_t flags;
    spinlock_lock_irqsave(&capability_lock, &flags);

    struct ipc_cap_entry *entry = capability_find_slot(proc, target);
    if (!entry)
    {
        for (size_t i = 0; i < CONFIG_IPC_CAPACITY_PER_PROC; ++i)
        {
            if (!proc->ipc_caps[i].used)
            {
                entry = &proc->ipc_caps[i];
                entry->used = 1;
                entry->peer = target;
                entry->rights = 0;
                if (proc->ipc_cap_count < CONFIG_IPC_CAPACITY_PER_PROC)
                    ++proc->ipc_cap_count;
                break;
            }
        }
    }

    if (!entry)
    {
        spinlock_unlock_irqrestore(&capability_lock, flags);
        return -1;
    }

    entry->rights |= rights;

    spinlock_unlock_irqrestore(&capability_lock, flags);
    return 0;
}

int ipc_cap_revoke(pid_t owner, pid_t target, uint32_t rights)
{
    if (!ipc_initialized)
        return -1;

    if (owner <= 0 || target <= 0 || rights == 0)
        return -1;

    struct process *proc = process_lookup(owner);
    if (!proc)
        return -1;

    uint32_t flags;
    spinlock_lock_irqsave(&capability_lock, &flags);

    struct ipc_cap_entry *entry = capability_find_slot(proc, target);
    if (entry)
    {
        entry->rights &= ~rights;
        if (entry->rights == 0)
        {
            entry->used = 0;
            entry->peer = IPC_INVALID_PID;
            if (proc->ipc_cap_count > 0)
                --proc->ipc_cap_count;
        }
    }

    spinlock_unlock_irqrestore(&capability_lock, flags);
    return 0;
}

int ipc_cap_query(pid_t owner, pid_t target, uint32_t *rights_out)
{
    if (!ipc_initialized)
        return -1;

    if (owner <= 0 || target <= 0)
        return -1;

    struct process *proc = process_lookup(owner);
    if (!proc)
        return -1;

    uint32_t rights = 0;

    uint32_t flags;
    spinlock_lock_irqsave(&capability_lock, &flags);
    struct ipc_cap_entry *entry = capability_find_slot(proc, target);
    if (entry)
        rights = entry->rights;
    spinlock_unlock_irqrestore(&capability_lock, flags);

    if (rights_out)
        *rights_out = rights;
    return 0;
}

struct ipc_message_slot
{
    uint32_t header;
    uint32_t type;
    uint32_t size;
    uint32_t flags;
    int32_t sender_pid;
    uint8_t data[CONFIG_MSG_DATA_MAX];
};

struct ipc_channel
{
    int used;
    int id;
    uint32_t flags;
    char name[CONFIG_IPC_CHANNEL_NAME_MAX];
    struct ipc_message_slot queue[CONFIG_IPC_CHANNEL_QUEUE_LEN];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    struct process *waiters[CONFIG_IPC_CHANNEL_WAITERS];
    uint8_t waiter_count;
    struct process *subscribers[CONFIG_IPC_CHANNEL_SUBSCRIBERS];
    uint8_t subscriber_count;
    spinlock_t lock;
};

static struct ipc_channel channel_table[CONFIG_IPC_MAX_CHANNELS];
static int next_channel_id = 1;
static int service_channel_ids[IPC_SERVICE_COUNT];

static size_t local_min(size_t a, size_t b)
{
    return (a < b) ? a : b;
}

static void channel_name_copy(char *dst, size_t dst_cap, const char *src, size_t src_len)
{
    if (!dst || dst_cap == 0)
        return;

    size_t limit = local_min(dst_cap - 1, src_len);
    size_t i = 0;
    for (; i < limit; ++i)
        dst[i] = src ? src[i] : '\0';
    dst[i] = '\0';
}

static struct ipc_channel *find_channel(int channel_id)
{
    if (channel_id <= 0)
        return NULL;

    for (size_t i = 0; i < CONFIG_IPC_MAX_CHANNELS; ++i)
    {
        if (channel_table[i].used && channel_table[i].id == channel_id)
            return &channel_table[i];
    }
    return NULL;
}

static int process_has_channel(const struct process *proc, int channel_id)
{
    if (!proc)
        return 0;

    for (uint8_t i = 0; i < proc->channel_count; ++i)
    {
        if (proc->channel_slots[i] == channel_id)
            return 1;
    }
    return 0;
}

static int process_add_channel(struct process *proc, int channel_id)
{
    if (!proc)
        return -1;

    if (process_has_channel(proc, channel_id))
        return 0;

    if (proc->channel_count >= CONFIG_PROCESS_CHANNEL_SLOTS)
        return -1;

    proc->channel_slots[proc->channel_count++] = channel_id;
    return 0;
}

static void process_remove_channel(struct process *proc, int channel_id)
{
    if (!proc)
        return;

    for (uint8_t i = 0; i < proc->channel_count; ++i)
    {
        if (proc->channel_slots[i] == channel_id)
        {
            for (uint8_t j = i + 1; j < proc->channel_count; ++j)
                proc->channel_slots[j - 1] = proc->channel_slots[j];
            proc->channel_slots[proc->channel_count - 1] = -1;
            --proc->channel_count;
            break;
        }
    }
}

static void channel_remove_waiter(struct ipc_channel *channel, struct process *proc)
{
    if (!channel || !proc)
        return;

    for (uint8_t i = 0; i < channel->waiter_count; ++i)
    {
        if (channel->waiters[i] == proc)
        {
            for (uint8_t j = i + 1; j < channel->waiter_count; ++j)
                channel->waiters[j - 1] = channel->waiters[j];
            channel->waiters[channel->waiter_count - 1] = NULL;
            --channel->waiter_count;
            break;
        }
    }
}

void ipc_system_init(void)
{
    spinlock_init(&capability_lock);
    spinlock_init(&share_lock);

    for (size_t i = 0; i < CONFIG_IPC_MAX_SHARED_REGIONS; ++i)
    {
        share_table[i].used = 0;
        share_table[i].owner = IPC_INVALID_PID;
        share_table[i].target = IPC_INVALID_PID;
        share_table[i].addr = 0;
        share_table[i].pages = 0;
        share_table[i].flags = 0;
    }

    for (int svc = 0; svc < IPC_SERVICE_COUNT; ++svc)
        service_channel_ids[svc] = -1;

    for (size_t i = 0; i < CONFIG_IPC_MAX_CHANNELS; ++i)
    {
        channel_table[i].used = 0;
        channel_table[i].id = 0;
        channel_table[i].flags = 0;
        channel_table[i].name[0] = '\0';
        channel_table[i].head = 0;
        channel_table[i].tail = 0;
        channel_table[i].count = 0;
        channel_table[i].waiter_count = 0;
        channel_table[i].subscriber_count = 0;
        spinlock_init(&channel_table[i].lock);
        for (size_t j = 0; j < CONFIG_IPC_CHANNEL_QUEUE_LEN; ++j)
        {
            channel_table[i].queue[j].header = 0;
            channel_table[i].queue[j].type = 0;
            channel_table[i].queue[j].size = 0;
            channel_table[i].queue[j].flags = 0;
            channel_table[i].queue[j].sender_pid = -1;
        }
        for (size_t w = 0; w < CONFIG_IPC_CHANNEL_WAITERS; ++w)
            channel_table[i].waiters[w] = NULL;
        for (size_t s = 0; s < CONFIG_IPC_CHANNEL_SUBSCRIBERS; ++s)
            channel_table[i].subscribers[s] = NULL;
    }

    next_channel_id = 1;

    const char *service_names[IPC_SERVICE_COUNT] = {
        "svc.devmgr",
        "svc.module",
        "svc.logger",
        "svc.scheduler"
    };

    for (int svc = 0; svc < IPC_SERVICE_COUNT; ++svc)
    {
        int id = ipc_channel_create(service_names[svc], 0, IPC_CHANNEL_FLAG_KERNEL);
        if (id < 0)
        {
            klog_error("ipc: failed to create service channel");
            service_channel_ids[svc] = -1;
        }
        else
        {
            service_channel_ids[svc] = id;
        }
    }

    ipc_initialized = 1;
}

int ipc_channel_create(const char *name, size_t name_len, uint32_t flags)
{
    for (size_t i = 0; i < CONFIG_IPC_MAX_CHANNELS; ++i)
    {
        if (channel_table[i].used)
            continue;

        channel_table[i].used = 1;
        channel_table[i].id = next_channel_id++;
        channel_table[i].flags = flags;
        channel_table[i].head = 0;
        channel_table[i].tail = 0;
        channel_table[i].count = 0;
        channel_table[i].waiter_count = 0;
        channel_table[i].subscriber_count = 0;

        size_t effective_len = name_len;
        if (name && effective_len == 0)
        {
            while (name[effective_len] && effective_len < (CONFIG_IPC_CHANNEL_NAME_MAX - 1))
                ++effective_len;
        }

        channel_name_copy(channel_table[i].name, CONFIG_IPC_CHANNEL_NAME_MAX, name, effective_len);
        return channel_table[i].id;
    }

    return -1;
}

int ipc_channel_join(struct process *proc, int channel_id)
{
    if (!proc)
        return -1;

    struct ipc_channel *channel = find_channel(channel_id);
    if (!channel)
        return -1;

    if (process_add_channel(proc, channel_id) < 0)
        return -1;

    uint32_t flags;
    spinlock_lock_irqsave(&channel->lock, &flags);

    for (uint8_t i = 0; i < channel->subscriber_count; ++i)
    {
        if (channel->subscribers[i] == proc)
        {
            spinlock_unlock_irqrestore(&channel->lock, flags);
            return 0;
        }
    }

    if (channel->subscriber_count >= CONFIG_IPC_CHANNEL_SUBSCRIBERS)
    {
        spinlock_unlock_irqrestore(&channel->lock, flags);
        process_remove_channel(proc, channel_id);
        return -1;
    }

    channel->subscribers[channel->subscriber_count++] = proc;
    spinlock_unlock_irqrestore(&channel->lock, flags);
    return 0;
}

int ipc_channel_leave(struct process *proc, int channel_id)
{
    if (!proc)
        return -1;

    struct ipc_channel *channel = find_channel(channel_id);
    if (!channel)
        return -1;

    process_remove_channel(proc, channel_id);

    uint32_t flags;
    spinlock_lock_irqsave(&channel->lock, &flags);

    for (uint8_t i = 0; i < channel->subscriber_count; ++i)
    {
        if (channel->subscribers[i] == proc)
        {
            for (uint8_t j = i + 1; j < channel->subscriber_count; ++j)
                channel->subscribers[j - 1] = channel->subscribers[j];
            channel->subscribers[channel->subscriber_count - 1] = NULL;
            --channel->subscriber_count;
            break;
        }
    }

    channel_remove_waiter(channel, proc);

    spinlock_unlock_irqrestore(&channel->lock, flags);
    return 0;
}

int ipc_channel_send(int channel_id, int sender_pid, uint32_t header, uint32_t type, const void *data, size_t size, uint32_t flags)
{
    (void)flags;

    if (size > CONFIG_MSG_DATA_MAX)
        return -1;

    struct ipc_channel *channel = find_channel(channel_id);
    if (!channel)
        return -1;

    struct process *sender_proc = NULL;
    if (sender_pid > 0)
    {
        sender_proc = process_lookup(sender_pid);
        if (!sender_proc)
            return -1;

        if (!process_has_channel(sender_proc, channel_id) && !(channel->flags & IPC_CHANNEL_FLAG_KERNEL))
            return -1;
    }

    uint32_t irq_flags;
    spinlock_lock_irqsave(&channel->lock, &irq_flags);

    if (channel->count >= CONFIG_IPC_CHANNEL_QUEUE_LEN)
    {
        spinlock_unlock_irqrestore(&channel->lock, irq_flags);
        return -1;
    }

    struct ipc_message_slot *slot = &channel->queue[channel->tail];
    slot->header = header;
    slot->type = type;
    slot->size = (uint32_t)size;
    slot->flags = flags;
    slot->sender_pid = sender_pid;
    if (size > 0 && data)
    {
        const uint8_t *src = (const uint8_t *)data;
        for (size_t i = 0; i < size; ++i)
            slot->data[i] = src[i];
    }

    channel->tail = (uint8_t)((channel->tail + 1) % CONFIG_IPC_CHANNEL_QUEUE_LEN);
    ++channel->count;

    struct process *wakeup_proc = NULL;
    if (channel->waiter_count > 0)
    {
        wakeup_proc = channel->waiters[0];
        for (uint8_t i = 1; i < channel->waiter_count; ++i)
            channel->waiters[i - 1] = channel->waiters[i];
        channel->waiters[channel->waiter_count - 1] = NULL;
        --channel->waiter_count;
        if (wakeup_proc)
            wakeup_proc->wait_channel = -1;
    }

    spinlock_unlock_irqrestore(&channel->lock, irq_flags);

    if (wakeup_proc)
        process_wake(wakeup_proc);

    return (int)size;
}

int ipc_channel_receive(struct process *proc, int channel_id, struct ipc_message *out, void *buffer, size_t buffer_len, uint32_t flags)
{
    if (!proc)
        return -1;

    struct ipc_channel *channel = find_channel(channel_id);
    if (!channel)
        return -1;

    if (!process_has_channel(proc, channel_id) && !(channel->flags & IPC_CHANNEL_FLAG_KERNEL))
        return -1;

    for (;;)
    {
        uint32_t irq_flags;
        spinlock_lock_irqsave(&channel->lock, &irq_flags);

        if (channel->count > 0)
        {
            struct ipc_message_slot *slot = &channel->queue[channel->head];
            channel->head = (uint8_t)((channel->head + 1) % CONFIG_IPC_CHANNEL_QUEUE_LEN);
            --channel->count;

            if (out)
            {
                out->header = slot->header;
                out->type = slot->type;
                out->sender_pid = slot->sender_pid;
                out->size = slot->size;
                out->data = buffer;
            }

            size_t copy_len = local_min(slot->size, buffer_len);
            if (slot->size > buffer_len && out)
                out->header |= IPC_MESSAGE_TRUNCATED;

            if (buffer && copy_len > 0)
            {
                uint8_t *dst = (uint8_t *)buffer;
                for (size_t i = 0; i < copy_len; ++i)
                    dst[i] = slot->data[i];
            }

            spinlock_unlock_irqrestore(&channel->lock, irq_flags);
            proc->wait_channel = -1;
            return 1;
        }

        if (flags & IPC_RECV_NONBLOCK)
        {
            spinlock_unlock_irqrestore(&channel->lock, irq_flags);
            return 0;
        }

        int already_waiting = 0;
        for (uint8_t i = 0; i < channel->waiter_count; ++i)
        {
            if (channel->waiters[i] == proc)
            {
                already_waiting = 1;
                break;
            }
        }

        if (!already_waiting)
        {
            if (channel->waiter_count >= CONFIG_IPC_CHANNEL_WAITERS)
            {
                spinlock_unlock_irqrestore(&channel->lock, irq_flags);
                return -1;
            }
            channel->waiters[channel->waiter_count++] = proc;
            proc->wait_channel = channel_id;
        }

        spinlock_unlock_irqrestore(&channel->lock, irq_flags);
        process_block_current();
    }
}

int ipc_channel_peek(int channel_id)
{
    struct ipc_channel *channel = find_channel(channel_id);
    if (!channel)
        return -1;

    uint32_t flags;
    spinlock_lock_irqsave(&channel->lock, &flags);
    int has_message = (channel->count > 0) ? 1 : 0;
    spinlock_unlock_irqrestore(&channel->lock, flags);
    return has_message;
}

int ipc_get_service_channel(enum ipc_service_channel service)
{
    if (service < 0 || service >= IPC_SERVICE_COUNT)
        return -1;
    return service_channel_ids[service];
}

int ipc_is_initialized(void)
{
    return ipc_initialized;
}

void ipc_process_cleanup(struct process *proc)
{
    if (!proc)
        return;

    ipc_detach_process(proc);

    for (uint8_t slot = 0; slot < proc->channel_count; ++slot)
    {
        int channel_id = proc->channel_slots[slot];
        struct ipc_channel *channel = find_channel(channel_id);
        if (!channel)
            continue;

        uint32_t flags;
        spinlock_lock_irqsave(&channel->lock, &flags);

        for (uint8_t i = 0; i < channel->subscriber_count; ++i)
        {
            if (channel->subscribers[i] == proc)
            {
                for (uint8_t j = i + 1; j < channel->subscriber_count; ++j)
                    channel->subscribers[j - 1] = channel->subscribers[j];
                channel->subscribers[channel->subscriber_count - 1] = NULL;
                --channel->subscriber_count;
                break;
            }
        }

        channel_remove_waiter(channel, proc);

        spinlock_unlock_irqrestore(&channel->lock, flags);
    }

    for (uint8_t i = 0; i < CONFIG_PROCESS_CHANNEL_SLOTS; ++i)
        proc->channel_slots[i] = -1;
    proc->channel_count = 0;
    proc->wait_channel = -1;
}
