#include "proc.h"
#include "vga.h"
#include "klog.h"

#include <stddef.h>
#include <stdint.h>

extern void context_switch(struct context *old_ctx, struct context *new_ctx);

static struct process processes[MAX_PROCS];
static struct context scheduler_ctx;
static struct process *current_process = NULL;
static int current_index = -1;
static int next_pid = 1;
static int scheduler_active = 0;

static int int_to_string(int value, char *out);

static void log_process_event(const char *prefix, int pid)
{
    if (!prefix)
        return;

    char buffer[64];
    int idx = 0;

    while (prefix[idx] && idx < (int)(sizeof(buffer) - 1))
    {
        buffer[idx] = prefix[idx];
        ++idx;
    }

    if (idx < (int)(sizeof(buffer) - 1))
    {
        char num[16];
        int num_len = int_to_string(pid, num);
        for (int i = 0; i < num_len && idx < (int)(sizeof(buffer) - 1); ++i)
            buffer[idx++] = num[i];
    }

    buffer[idx] = '\0';
    klog_debug(buffer);
}

static void zero_memory(void *ptr, size_t size)
{
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < size; ++i)
        p[i] = 0;
}

static int int_to_string(int value, char *out)
{
    if (value == 0)
    {
        out[0] = '0';
        return 1;
    }

    char temp[16];
    int idx = 0;
    while (value > 0 && idx < (int)sizeof(temp))
    {
        temp[idx++] = (char)('0' + (value % 10));
        value /= 10;
    }

    for (int i = 0; i < idx; ++i)
        out[i] = temp[idx - 1 - i];

    return idx;
}

extern void process_exit(int code);

static struct process *alloc_process_slot(void)
{
    for (int i = 0; i < MAX_PROCS; ++i)
    {
        if (processes[i].state == PROC_UNUSED || processes[i].state == PROC_ZOMBIE)
        {
            zero_memory(&processes[i], sizeof(struct process));
            processes[i].state = PROC_UNUSED;
            return &processes[i];
        }
    }
    return NULL;
}

static uint32_t *stack_align(uint32_t *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    value &= ~(uintptr_t)0xF;
    return (uint32_t *)value;
}

static void process_bootstrap(void) __attribute__((noreturn, naked));

static void process_bootstrap(void)
{
    __asm__ __volatile__(
        "pop %%eax\n"
        "call *%%eax\n"
        "push $0\n"
        "call process_exit\n"
        "1: hlt\n"
        "jmp 1b\n"
        :
        :
        : "eax"
    );
}

void process_system_init(void)
{
    for (int i = 0; i < MAX_PROCS; ++i)
    {
        processes[i].pid = -1;
        processes[i].state = PROC_UNUSED;
        processes[i].stack_size = PROC_STACK_SIZE;
        processes[i].queue.head = 0;
        processes[i].queue.tail = 0;
        processes[i].queue.count = 0;
    }

    scheduler_ctx.esp = 0;
    current_process = NULL;
    current_index = -1;
    next_pid = 1;
    scheduler_active = 0;
}

static int acquire_pid(void)
{
    if (next_pid <= 0)
        next_pid = 1;
    return next_pid++;
}

struct process *process_lookup(int pid)
{
    if (pid <= 0)
        return NULL;

    for (int i = 0; i < MAX_PROCS; ++i)
    {
        if (processes[i].pid == pid && processes[i].state != PROC_UNUSED)
            return &processes[i];
    }
    return NULL;
}

int process_create(void (*entry)(void), size_t stack_size)
{
    if (!entry)
        return -1;

    if (stack_size == 0 || stack_size > PROC_STACK_SIZE)
        stack_size = PROC_STACK_SIZE;

    struct process *proc = alloc_process_slot();
    if (!proc)
        return -1;

    uint32_t *stack_top = (uint32_t *)(proc->stack + PROC_STACK_SIZE);
    stack_top = stack_align(stack_top);

    uint32_t *sp = stack_top;
    *--sp = (uint32_t)entry;             /* parameter for bootstrap */
    *--sp = (uint32_t)process_bootstrap; /* return address for ret */
    *--sp = 0;                           /* saved EBP */
    *--sp = 0x202;                       /* eflags */
    *--sp = 0;                           /* eax */
    *--sp = 0;                           /* ecx */
    *--sp = 0;                           /* edx */
    *--sp = 0;                           /* ebx */
    *--sp = 0;                           /* esi */
    *--sp = 0;                           /* edi */

    proc->ctx.esp = (uint32_t)sp;
    proc->pid = acquire_pid();
    proc->state = PROC_READY;
    proc->stack_size = stack_size;
    proc->queue.head = 0;
    proc->queue.tail = 0;
    proc->queue.count = 0;
    proc->exit_code = 0;

    log_process_event("process: created pid ", proc->pid);

    return proc->pid;
}

struct process *process_current(void)
{
    return current_process;
}

void process_wake(struct process *proc)
{
    if (!proc)
        return;
    if (proc->state == PROC_WAITING)
        proc->state = PROC_READY;
}

void process_block_current(void)
{
    struct process *proc = current_process;
    if (!proc)
        return;

    proc->state = PROC_WAITING;
    context_switch(&proc->ctx, &scheduler_ctx);
    proc->state = PROC_RUNNING;
}

void process_yield(void)
{
    struct process *proc = current_process;
    if (!proc)
        return;

    proc->state = PROC_READY;
    context_switch(&proc->ctx, &scheduler_ctx);
    proc->state = PROC_RUNNING;
}

void process_exit(int code)
{
    struct process *proc = current_process;
    if (!proc)
        return;

    proc->exit_code = code;
    proc->state = PROC_ZOMBIE;
    log_process_event("process: exit pid ", proc->pid);
    context_switch(&proc->ctx, &scheduler_ctx);

    for (;;)
        __asm__ __volatile__("hlt");
}

static struct process *pick_next_process(void)
{
    int start = current_index;
    for (int i = 0; i < MAX_PROCS; ++i)
    {
        start = (start + 1) % MAX_PROCS;
        if (processes[start].state == PROC_READY)
        {
            current_index = start;
            return &processes[start];
        }
    }
    return NULL;
}

static void reclaim_zombie(struct process *proc)
{
    if (!proc || proc->state != PROC_ZOMBIE)
        return;

    proc->pid = -1;
    proc->state = PROC_UNUSED;
    proc->queue.head = proc->queue.tail = proc->queue.count = 0;
    proc->exit_code = 0;
    proc->ctx.esp = 0;
}

void process_schedule(void)
{
    if (scheduler_active)
        return;

    scheduler_active = 1;

    while (1)
    {
        struct process *next = pick_next_process();
        if (!next)
            break;

        current_process = next;
        next->state = PROC_RUNNING;

        context_switch(&scheduler_ctx, &next->ctx);

        struct process *finished = current_process;
        if (finished && finished->state == PROC_RUNNING)
            finished->state = PROC_READY;

        if (finished && finished->state == PROC_ZOMBIE)
        {
            int pid = finished->pid;
            reclaim_zombie(finished);
            log_process_event("process: reclaimed pid ", pid);
        }

        current_process = NULL;
    }

    scheduler_active = 0;
}

int process_count(void)
{
    int total = 0;
    for (int i = 0; i < MAX_PROCS; ++i)
    {
        if (processes[i].state != PROC_UNUSED)
            ++total;
    }
    return total;
}

void process_debug_list(void)
{
    static const char *state_names[] = {
        "UNUSED",
        "READY",
        "RUNNING",
        "WAITING",
        "ZOMBIE"
    };

    vga_write_line("PID  STATE");
    for (int i = 0; i < MAX_PROCS; ++i)
    {
        if (processes[i].state == PROC_UNUSED)
            continue;

        char buffer[32];
        int pid = processes[i].pid;
        const char *state = state_names[processes[i].state];

        int idx = int_to_string(pid, buffer);
        buffer[idx++] = ' ';
        buffer[idx++] = ' ';
        for (int j = 0; state[j] && idx < (int)(sizeof(buffer) - 1); ++j)
            buffer[idx++] = state[j];
        buffer[idx] = '\0';

        vga_write_line(buffer);
    }
}
