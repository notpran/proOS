#ifndef PROC_H
#define PROC_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "spinlock.h"
#include "ipc_types.h"

#define MAX_PROCS       CONFIG_MAX_PROCS
#define PROC_STACK_SIZE CONFIG_PROC_STACK_SIZE

typedef enum
{
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_WAITING,
    PROC_ZOMBIE
} proc_state_t;

typedef enum
{
    THREAD_KIND_KERNEL = 0,
    THREAD_KIND_USER = 1
} thread_kind_t;

/**
 * @brief Scheduling policy selection for a process.
 */
typedef enum
{
    SCHED_POLICY_FAIR = 0,
    SCHED_POLICY_DEADLINE = 1
} sched_policy_t;

typedef void (*process_entry_t)(void);

struct context
{
    uint32_t esp;
};

struct ipc_mailbox_slot
{
    uint8_t used;
    pid_t sender;
    uint32_t flags;
    uint32_t size;
    uint8_t data[CONFIG_MSG_DATA_MAX];
};

struct ipc_mailbox_state
{
    spinlock_t lock;
    struct ipc_mailbox_slot slots[CONFIG_MSG_QUEUE_LEN];
    uint8_t count;
    pid_t waiters[CONFIG_IPC_ENDPOINT_WAITERS];
    uint8_t waiter_count;
};

struct ipc_cap_entry
{
    uint8_t used;
    pid_t peer;
    uint32_t rights;
};

struct ipc_share_link
{
    uint8_t used;
    int share_id;
};

struct process
{
    int pid;
    proc_state_t state;
    struct context ctx;
    uint8_t stack[PROC_STACK_SIZE];
    size_t stack_size;
    int channel_slots[CONFIG_PROCESS_CHANNEL_SLOTS];
    uint8_t channel_count;
    int wait_channel;
    int ipc_waiting;
    int exit_code;
    thread_kind_t kind;
    uint8_t base_priority;
    uint8_t dynamic_priority;
    /** Scheduler policy (fair or deadline). */
    uint8_t sched_policy;
    /** Fair-share weight (higher gets more CPU). */
    uint32_t sched_weight;
    /** Absolute deadline tick (0 = none). */
    uint64_t sched_deadline;
    /** Virtual runtime used by fair-share selection. */
    uint64_t vruntime;
    uint8_t on_run_queue;
    uint32_t time_slice_ticks;
    uint32_t time_slice_remaining;
    uint64_t wake_deadline;
    struct process *next_run;
    struct process *next_sleep;
    process_entry_t entry;
    struct ipc_mailbox_state ipc_mailbox;
    struct ipc_cap_entry ipc_caps[CONFIG_IPC_CAPACITY_PER_PROC];
    uint8_t ipc_cap_count;
    struct ipc_share_link ipc_shares[CONFIG_IPC_MAX_SHARED_PER_PROC];
    uint8_t ipc_share_count;
};

struct process_info
{
    int pid;
    proc_state_t state;
    thread_kind_t kind;
    uint8_t base_priority;
    uint8_t dynamic_priority;
    uint8_t sched_policy;
    uint32_t sched_weight;
    uint64_t sched_deadline;
  /*removed duplicate variables*/ 
    uint64_t sched_deadline;
    uint64_t vruntime;
    uint32_t time_slice_remaining;
    uint32_t time_slice_ticks;
    uint64_t wake_deadline;
    uintptr_t stack_pointer;
    size_t stack_size;
};

void process_system_init(void);
int process_create(void (*entry)(void), size_t stack_size);
int process_create_kernel(void (*entry)(void), size_t stack_size);
void process_yield(void);
void process_exit(int code);
void process_block_current(void);
void process_wake(struct process *proc);
void process_sleep(uint32_t ticks);
void process_schedule(void);
struct process *process_current(void);
struct process *process_lookup(int pid);
void process_debug_list(void);
int process_count(void);
size_t process_snapshot(struct process_info *out, size_t max_entries);
void process_scheduler_tick(void);
/**
 * @brief Configure scheduling policy for a process.
 *
 * @param pid Target PID (<= 0 uses current process).
 * @param policy SCHED_POLICY_FAIR or SCHED_POLICY_DEADLINE.
 * @param weight Fair-share weight (0 uses default).
 * @param deadline_ticks Absolute deadline tick (0 clears).
 * @return 0 on success, -1 on error.
 */
int process_set_scheduler(int pid, uint8_t policy, uint32_t weight, uint64_t deadline_ticks);

/* IPC helpers exposed to syscall layer */
/* Legacy IPC entry points removed in favour of channel system */

#endif
