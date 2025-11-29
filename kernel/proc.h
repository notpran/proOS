#ifndef PROC_H
#define PROC_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define MAX_PROCS       CONFIG_MAX_PROCS
#define PROC_STACK_SIZE CONFIG_PROC_STACK_SIZE

#define MSG_QUEUE_LEN CONFIG_MSG_QUEUE_LEN
#define MSG_DATA_MAX  CONFIG_MSG_DATA_MAX

typedef enum
{
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_WAITING,
    PROC_ZOMBIE
} proc_state_t;

struct context
{
    uint32_t esp;
};

struct message
{
    int from_pid;
    size_t length;
    char data[MSG_DATA_MAX];
};

struct message_queue
{
    struct message items[MSG_QUEUE_LEN];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
};

struct process
{
    int pid;
    proc_state_t state;
    struct context ctx;
    uint8_t stack[PROC_STACK_SIZE];
    size_t stack_size;
    struct message_queue queue;
    int exit_code;
};

void process_system_init(void);
int process_create(void (*entry)(void), size_t stack_size);
void process_yield(void);
void process_exit(int code);
void process_block_current(void);
void process_wake(struct process *proc);
void process_schedule(void);
struct process *process_current(void);
struct process *process_lookup(int pid);
void process_debug_list(void);
int process_count(void);

/* IPC helpers exposed to syscall layer */
int ipc_send(int target_pid, int from_pid, const char *data, size_t len);
int ipc_receive(struct process *proc, struct message *out);
int ipc_has_message(struct process *proc);

#endif
