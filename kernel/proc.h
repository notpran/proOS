#ifndef PROC_H
#define PROC_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

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

typedef void (*process_entry_t)(void);

struct context
{
    uint32_t esp;
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
    int exit_code;
    thread_kind_t kind;
    uint8_t base_priority;
    uint8_t dynamic_priority;
    uint8_t on_run_queue;
    uint32_t time_slice_ticks;
    uint32_t time_slice_remaining;
    uint64_t wake_deadline;
    struct process *next_run;
    struct process *next_sleep;
    process_entry_t entry;
};

struct process_info
{
    int pid;
    proc_state_t state;
    thread_kind_t kind;
    uint8_t base_priority;
    uint8_t dynamic_priority;
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

/* IPC helpers exposed to syscall layer */
/* Legacy IPC entry points removed in favour of channel system */

#endif
