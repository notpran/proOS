#include "proc.h"
#include "ipc.h"
#include "vga.h"
#include "klog.h"
#include "pit.h"
#include "debug.h"

#include <stddef.h>
#include <stdint.h>

extern void context_switch(struct context *old_ctx, struct context *new_ctx);

#if CONFIG_SCHED_PRIORITY_LEVELS < 1
#error "CONFIG_SCHED_PRIORITY_LEVELS must be at least 1"
#endif

#if CONFIG_SCHED_PRIORITY_LEVELS > 32
#error "CONFIG_SCHED_PRIORITY_LEVELS must not exceed 32"
#endif

#define SCHED_PRIORITY_LEVELS CONFIG_SCHED_PRIORITY_LEVELS
#define SCHED_PRIORITY_MIN    CONFIG_SCHED_MIN_PRIORITY
#define SCHED_PRIORITY_MAX    (CONFIG_SCHED_PRIORITY_LEVELS - 1)

struct run_queue
{
	struct process *head;
	struct process *tail;
};

static struct process processes[MAX_PROCS];
static struct run_queue ready_queues[SCHED_PRIORITY_LEVELS];
static uint32_t ready_bitmap = 0;
static struct context scheduler_ctx;
static struct process *current_process = NULL;
static struct process *idle_process = NULL;
static struct process *sleep_list = NULL;
static int next_pid = 1;
static int scheduler_active = 0;
static int scheduler_channel_id = -1;

enum scheduler_event_type
{
	SCHED_EVENT_CREATE = 1,
	SCHED_EVENT_EXIT = 2,
	SCHED_EVENT_RECLAIM = 3
};

struct scheduler_event
{
	uint8_t action;
	uint8_t state;
	uint16_t reserved;
	int32_t pid;
	int32_t value;
};

static int int_to_string(int value, char *out);
static void log_process_event(const char *prefix, int pid);
static void zero_memory(void *ptr, size_t size);
static void scheduler_send_event(uint8_t action, int pid, int value, proc_state_t state);
static struct process *alloc_process_slot(void);
static uint32_t *stack_align(uint32_t *ptr);
static void thread_bootstrap(void) __attribute__((noreturn, naked));
static void thread_entry_trampoline(void) __attribute__((used));
static int acquire_pid(void);
static uint8_t scheduler_clamp_priority(int value);
static uint32_t scheduler_timeslice_for(uint8_t priority);
static void scheduler_reset_priority(struct process *proc);
static void scheduler_demote_priority(struct process *proc);
static void scheduler_boost_priority(struct process *proc);
static void scheduler_arm_timeslice(struct process *proc);
static void scheduler_enqueue_ready(struct process *proc);
static struct process *scheduler_dequeue_next(void);
static void scheduler_insert_sleep(struct process *proc);
static void scheduler_remove_from_sleep(struct process *proc);
static void wake_sleepers(uint64_t now);
static void scheduler_preempt_running(int demote_priority);
static void reclaim_zombie(struct process *proc);
static struct process *scheduler_create_thread(process_entry_t entry, size_t stack_size, thread_kind_t kind, uint8_t base_priority, int emit_event, int is_idle);
static void idle_thread(void);
static uint8_t scheduler_default_user_priority(void);
static uint8_t scheduler_default_kernel_priority(void);

static void scheduler_send_event(uint8_t action, int pid, int value, proc_state_t state)
{
	if (pid <= 0)
		return;
	if (!ipc_is_initialized())
		return;
	if (scheduler_channel_id < 0)
		scheduler_channel_id = ipc_get_service_channel(IPC_SERVICE_SCHEDULER);
	if (scheduler_channel_id < 0)
		return;

	struct scheduler_event payload;
	payload.action = action;
	payload.state = (uint8_t)state;
	payload.reserved = 0;
	payload.pid = pid;
	payload.value = value;

	ipc_channel_send(scheduler_channel_id, 0, action, 0, &payload, sizeof(payload), 0);
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

static struct process *alloc_process_slot(void)
{
	for (int i = 0; i < MAX_PROCS; ++i)
	{
		if (processes[i].state == PROC_UNUSED || processes[i].state == PROC_ZOMBIE)
		{
			zero_memory(&processes[i], sizeof(struct process));
			processes[i].pid = -1;
			processes[i].state = PROC_UNUSED;
			processes[i].stack_size = PROC_STACK_SIZE;
			processes[i].wait_channel = -1;
			for (size_t slot = 0; slot < CONFIG_PROCESS_CHANNEL_SLOTS; ++slot)
				processes[i].channel_slots[slot] = -1;
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

static uint8_t scheduler_clamp_priority(int value)
{
	if (value < SCHED_PRIORITY_MIN)
		return (uint8_t)SCHED_PRIORITY_MIN;
	if (value > SCHED_PRIORITY_MAX)
		return (uint8_t)SCHED_PRIORITY_MAX;
	return (uint8_t)value;
}

static uint32_t scheduler_timeslice_for(uint8_t priority)
{
	uint32_t base = (CONFIG_SCHED_DEFAULT_TIMESLICE > 0u) ? CONFIG_SCHED_DEFAULT_TIMESLICE : 1u;
	int pr = (int)priority;
	if (pr < SCHED_PRIORITY_MIN)
		pr = SCHED_PRIORITY_MIN;
	if (pr > SCHED_PRIORITY_MAX)
		pr = SCHED_PRIORITY_MAX;

	uint32_t offset = (uint32_t)(pr - SCHED_PRIORITY_MIN);
	if (offset > 4u)
		offset = 4u;

	uint32_t slice = base << offset;
	if (slice == 0u)
		slice = 1u;
	return slice;
}

static void scheduler_reset_priority(struct process *proc)
{
	if (!proc)
		return;
	proc->dynamic_priority = proc->base_priority;
}

static void scheduler_demote_priority(struct process *proc)
{
	if (!proc)
		return;
	if (proc->dynamic_priority < (uint8_t)SCHED_PRIORITY_MAX)
		proc->dynamic_priority = (uint8_t)(proc->dynamic_priority + 1u);
}

static void scheduler_boost_priority(struct process *proc)
{
	if (!proc)
		return;

	uint8_t base = proc->base_priority;
	uint8_t target = base;

#if CONFIG_SCHED_MAX_DYNAMIC_BOOST > 0
	if (base > (uint8_t)SCHED_PRIORITY_MIN)
	{
		uint8_t max_boost = (uint8_t)CONFIG_SCHED_MAX_DYNAMIC_BOOST;
		uint8_t distance = (uint8_t)(base - (uint8_t)SCHED_PRIORITY_MIN);
		if (max_boost > distance)
			max_boost = distance;
		target = (uint8_t)(base - max_boost);
	}
#endif

	proc->dynamic_priority = scheduler_clamp_priority(target);
}

static void scheduler_arm_timeslice(struct process *proc)
{
	if (!proc)
		return;
	proc->time_slice_ticks = scheduler_timeslice_for(proc->dynamic_priority);
	proc->time_slice_remaining = proc->time_slice_ticks;
}

static void scheduler_enqueue_ready(struct process *proc)
{
	if (!proc || proc == idle_process)
		return;
	if (proc->on_run_queue)
		return;

	uint8_t priority = scheduler_clamp_priority(proc->dynamic_priority);
	struct run_queue *queue = &ready_queues[priority];

	proc->next_run = NULL;
	if (!queue->head)
	{
		queue->head = proc;
		queue->tail = proc;
	}
	else
	{
		queue->tail->next_run = proc;
		queue->tail = proc;
	}

	ready_bitmap |= (1u << priority);
	proc->on_run_queue = 1;
}

static struct process *scheduler_dequeue_next(void)
{
	for (int priority = SCHED_PRIORITY_MIN; priority <= SCHED_PRIORITY_MAX; ++priority)
	{
		if ((ready_bitmap & (1u << priority)) == 0u)
			continue;

		struct run_queue *queue = &ready_queues[priority];
		struct process *proc = queue->head;
		if (!proc)
		{
			ready_bitmap &= ~(1u << priority);
			queue->tail = NULL;
			continue;
		}

		queue->head = proc->next_run;
		if (!queue->head)
		{
			queue->tail = NULL;
			ready_bitmap &= ~(1u << priority);
		}

		proc->next_run = NULL;
		proc->on_run_queue = 0;
		return proc;
	}
	return NULL;
}

static void scheduler_insert_sleep(struct process *proc)
{
	if (!proc)
		return;

	scheduler_remove_from_sleep(proc);

	if (!sleep_list || proc->wake_deadline < sleep_list->wake_deadline)
	{
		proc->next_sleep = sleep_list;
		sleep_list = proc;
		return;
	}

	struct process *iter = sleep_list;
	while (iter->next_sleep && iter->next_sleep->wake_deadline <= proc->wake_deadline)
		iter = iter->next_sleep;

	proc->next_sleep = iter->next_sleep;
	iter->next_sleep = proc;
}

static void scheduler_remove_from_sleep(struct process *proc)
{
	if (!proc || !sleep_list)
		return;

	if (sleep_list == proc)
	{
		sleep_list = proc->next_sleep;
		proc->next_sleep = NULL;
		return;
	}

	struct process *prev = sleep_list;
	struct process *iter = sleep_list->next_sleep;
	while (iter)
	{
		if (iter == proc)
		{
			prev->next_sleep = iter->next_sleep;
			iter->next_sleep = NULL;
			return;
		}
		prev = iter;
		iter = iter->next_sleep;
	}
}

static void wake_sleepers(uint64_t now)
{
	while (sleep_list && sleep_list->wake_deadline <= now)
	{
		struct process *proc = sleep_list;
		sleep_list = proc->next_sleep;
		proc->next_sleep = NULL;
		proc->wake_deadline = 0;
		scheduler_boost_priority(proc);
		proc->state = PROC_READY;
		scheduler_enqueue_ready(proc);
	}
}

static void scheduler_preempt_running(int demote_priority)
{
	struct process *proc = current_process;
	if (!proc)
		return;

	if (proc == idle_process)
	{
		context_switch(&proc->ctx, &scheduler_ctx);
		proc->state = PROC_RUNNING;
		return;
	}

	if (demote_priority)
		scheduler_demote_priority(proc);

	proc->state = PROC_READY;
	scheduler_enqueue_ready(proc);
	context_switch(&proc->ctx, &scheduler_ctx);
	proc->state = PROC_RUNNING;
}

static void reclaim_zombie(struct process *proc)
{
	if (!proc || proc->state != PROC_ZOMBIE)
		return;

	int pid = proc->pid;
	int exit_code = proc->exit_code;

	scheduler_remove_from_sleep(proc);
	proc->on_run_queue = 0;
	proc->next_run = NULL;

	proc->pid = -1;
	proc->state = PROC_UNUSED;
	proc->channel_count = 0;
	proc->wait_channel = -1;
	proc->exit_code = 0;
	proc->ctx.esp = 0;
	proc->entry = NULL;
	proc->time_slice_ticks = 0;
	proc->time_slice_remaining = 0;
	proc->wake_deadline = 0;
	proc->kind = THREAD_KIND_KERNEL;
	proc->base_priority = scheduler_clamp_priority(SCHED_PRIORITY_MAX);
	proc->dynamic_priority = proc->base_priority;
	for (size_t slot = 0; slot < CONFIG_PROCESS_CHANNEL_SLOTS; ++slot)
		proc->channel_slots[slot] = -1;

	scheduler_send_event(SCHED_EVENT_RECLAIM, pid, exit_code, PROC_UNUSED);
}

static int acquire_pid(void)
{
	if (next_pid <= 0)
		next_pid = 1;
	return next_pid++;
}

static uint8_t scheduler_default_user_priority(void)
{
	int candidate = SCHED_PRIORITY_MIN + 1;
	if (candidate > SCHED_PRIORITY_MAX)
		candidate = SCHED_PRIORITY_MAX;
	return scheduler_clamp_priority(candidate);
}

static uint8_t scheduler_default_kernel_priority(void)
{
	return scheduler_clamp_priority(SCHED_PRIORITY_MIN);
}

static void thread_entry_trampoline(void)
{
	struct process *proc = current_process;
	if (proc && proc->entry)
		proc->entry();
}

extern void process_exit(int code);

static void thread_bootstrap(void)
{
	__asm__ __volatile__(
		"call thread_entry_trampoline\n"
		"push $0\n"
		"call process_exit\n"
		"1: hlt\n"
		"jmp 1b\n"
	);
}

static struct process *scheduler_create_thread(process_entry_t entry, size_t stack_size, thread_kind_t kind, uint8_t base_priority, int emit_event, int is_idle)
{
	if (!entry)
		return NULL;

	if (stack_size == 0 || stack_size > PROC_STACK_SIZE)
		stack_size = PROC_STACK_SIZE;

	struct process *proc = alloc_process_slot();
	if (!proc)
		return NULL;

	proc->kind = kind;
	proc->base_priority = scheduler_clamp_priority(base_priority);
	proc->dynamic_priority = proc->base_priority;
	proc->state = PROC_READY;
	proc->entry = entry;
	proc->stack_size = stack_size;
	proc->time_slice_ticks = 0;
	proc->time_slice_remaining = 0;
	proc->on_run_queue = 0;
	proc->wake_deadline = 0;
	proc->next_run = NULL;
	proc->next_sleep = NULL;
	proc->channel_count = 0;
	proc->wait_channel = -1;
	proc->exit_code = 0;

	proc->pid = is_idle ? 0 : acquire_pid();

	uint32_t *stack_top = stack_align((uint32_t *)(proc->stack + stack_size));
	uint32_t *sp = stack_top;
	*--sp = (uint32_t)thread_bootstrap;
	*--sp = 0u;
	*--sp = 0x202u;
	*--sp = 0u;
	*--sp = 0u;
	*--sp = 0u;
	*--sp = 0u;
	*--sp = 0u;
	*--sp = 0u;
	proc->ctx.esp = (uint32_t)sp;

	if (!is_idle)
	{
		scheduler_arm_timeslice(proc);
		scheduler_enqueue_ready(proc);
	}
	else
	{
		scheduler_arm_timeslice(proc);
		idle_process = proc;
	}

	if (emit_event && proc->pid > 0)
	{
		log_process_event("process: created pid ", proc->pid);
		scheduler_send_event(SCHED_EVENT_CREATE, proc->pid, 0, proc->state);
	}

	return proc;
}

static void idle_thread(void)
{
	for (;;)
		__asm__ __volatile__("hlt");
}

void process_system_init(void)
{
	for (int i = 0; i < MAX_PROCS; ++i)
	{
		processes[i].pid = -1;
		processes[i].state = PROC_UNUSED;
		processes[i].stack_size = PROC_STACK_SIZE;
		processes[i].channel_count = 0;
		processes[i].wait_channel = -1;
		processes[i].exit_code = 0;
		processes[i].on_run_queue = 0;
		processes[i].next_run = NULL;
		processes[i].next_sleep = NULL;
		processes[i].wake_deadline = 0;
		processes[i].time_slice_ticks = 0;
		processes[i].time_slice_remaining = 0;
		for (size_t slot = 0; slot < CONFIG_PROCESS_CHANNEL_SLOTS; ++slot)
			processes[i].channel_slots[slot] = -1;
	}

	for (int i = 0; i < SCHED_PRIORITY_LEVELS; ++i)
	{
		ready_queues[i].head = NULL;
		ready_queues[i].tail = NULL;
	}

	ready_bitmap = 0;
	scheduler_ctx.esp = 0;
	current_process = NULL;
	idle_process = NULL;
	sleep_list = NULL;
	next_pid = 1;
	scheduler_active = 0;
	scheduler_channel_id = ipc_is_initialized() ? ipc_get_service_channel(IPC_SERVICE_SCHEDULER) : -1;

	if (!scheduler_create_thread(idle_thread, PROC_STACK_SIZE, THREAD_KIND_KERNEL, SCHED_PRIORITY_MAX, 0, 1))
		klog_error("scheduler: failed to create idle thread");
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
	struct process *proc = scheduler_create_thread(entry, stack_size, THREAD_KIND_USER, scheduler_default_user_priority(), 1, 0);
	int pid = proc ? proc->pid : -1;
	if (pid > 0)
		debug_publish_task_list();
	return pid;
}

int process_create_kernel(void (*entry)(void), size_t stack_size)
{
	struct process *proc = scheduler_create_thread(entry, stack_size, THREAD_KIND_KERNEL, scheduler_default_kernel_priority(), 1, 0);
	int pid = proc ? proc->pid : -1;
	if (pid > 0)
		debug_publish_task_list();
	return pid;
}

struct process *process_current(void)
{
	return current_process;
}

void process_wake(struct process *proc)
{
	if (!proc || proc->state != PROC_WAITING)
		return;

	scheduler_remove_from_sleep(proc);
	scheduler_boost_priority(proc);
	proc->state = PROC_READY;
	scheduler_enqueue_ready(proc);
}

void process_block_current(void)
{
	struct process *proc = current_process;
	if (!proc || proc == idle_process)
		return;

	proc->state = PROC_WAITING;
	proc->time_slice_remaining = 0;
	context_switch(&proc->ctx, &scheduler_ctx);
	proc->state = PROC_RUNNING;
}

void process_sleep(uint32_t ticks)
{
	struct process *proc = current_process;
	if (!proc || proc == idle_process)
		return;

	if (ticks == 0u)
		ticks = 1u;

	uint64_t now = get_ticks();
	proc->wake_deadline = now + (uint64_t)ticks;
	proc->state = PROC_WAITING;
	proc->time_slice_remaining = 0;
	scheduler_insert_sleep(proc);
	context_switch(&proc->ctx, &scheduler_ctx);
	proc->state = PROC_RUNNING;
}

void process_yield(void)
{
	struct process *proc = current_process;
	if (!proc || proc == idle_process)
		return;

	scheduler_reset_priority(proc);
	proc->state = PROC_READY;
	proc->time_slice_remaining = 0;
	scheduler_enqueue_ready(proc);
	context_switch(&proc->ctx, &scheduler_ctx);
	proc->state = PROC_RUNNING;
}

void process_exit(int code)
{
	struct process *proc = current_process;
	if (!proc)
		return;

	ipc_process_cleanup(proc);

	scheduler_remove_from_sleep(proc);
	proc->on_run_queue = 0;
	proc->next_run = NULL;

	proc->exit_code = code;
	proc->state = PROC_ZOMBIE;
	log_process_event("process: exit pid ", proc->pid);
	scheduler_send_event(SCHED_EVENT_EXIT, proc->pid, code, proc->state);
	debug_publish_task_list();
	context_switch(&proc->ctx, &scheduler_ctx);

	for (;;)
		__asm__ __volatile__("hlt");
}

void process_schedule(void)
{
	if (scheduler_active)
		return;

	scheduler_active = 1;

	while (1)
	{
		wake_sleepers(get_ticks());

		struct process *next = scheduler_dequeue_next();
		if (!next)
			next = idle_process;

		current_process = next;
		next->state = PROC_RUNNING;
		scheduler_arm_timeslice(next);

		context_switch(&scheduler_ctx, &next->ctx);

		struct process *finished = current_process;
		if (finished && finished->state == PROC_ZOMBIE)
		{
			int pid = finished->pid;
			reclaim_zombie(finished);
			if (pid > 0)
				log_process_event("process: reclaimed pid ", pid);
			debug_publish_task_list();
		}

		if (finished && finished->state == PROC_READY && finished != idle_process && !finished->on_run_queue)
			scheduler_enqueue_ready(finished);

		current_process = NULL;
	}
}

int process_count(void)
{
	int total = 0;
	for (int i = 0; i < MAX_PROCS; ++i)
	{
		if (processes[i].state != PROC_UNUSED && processes[i].pid > 0)
			++total;
	}
	return total;
}

size_t process_snapshot(struct process_info *out, size_t max_entries)
{
	if (!out || max_entries == 0)
		return 0;

	size_t count = 0;
	for (int i = 0; i < MAX_PROCS && count < max_entries; ++i)
	{
		struct process *proc = &processes[i];
		if (proc->state == PROC_UNUSED || proc->pid <= 0)
			continue;

		struct process_info *slot = &out[count++];
		slot->pid = proc->pid;
		slot->state = proc->state;
		slot->kind = proc->kind;
		slot->base_priority = proc->base_priority;
		slot->dynamic_priority = proc->dynamic_priority;
		slot->time_slice_remaining = proc->time_slice_remaining;
		slot->time_slice_ticks = proc->time_slice_ticks;
		slot->wake_deadline = proc->wake_deadline;
		slot->stack_pointer = proc->ctx.esp;
		slot->stack_size = proc->stack_size;
	}

	return count;
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

	struct process_info snapshot[MAX_PROCS];
	size_t count = process_snapshot(snapshot, MAX_PROCS);
	vga_write_line("PID  STATE    KIND  PRI(base/dyn)  REM  TICKS");
	for (size_t i = 0; i < count; ++i)
	{
		const struct process_info *info = &snapshot[i];
		const char *state = state_names[info->state];
		char buffer[80];
		int idx = int_to_string(info->pid, buffer);
		while (idx < 4)
			buffer[idx++] = ' ';
		buffer[idx++] = ' ';
		for (int j = 0; state[j] && idx < (int)(sizeof(buffer) - 1); ++j)
			buffer[idx++] = state[j];
		while (idx < 12)
			buffer[idx++] = ' ';
		buffer[idx++] = ' ';
		buffer[idx++] = (info->kind == THREAD_KIND_USER) ? 'U' : 'K';
		buffer[idx++] = ' ';
		buffer[idx++] = ' ';
		idx += int_to_string((int)info->base_priority, buffer + idx);
		buffer[idx++] = '/';
		idx += int_to_string((int)info->dynamic_priority, buffer + idx);
		buffer[idx++] = ' ';
		buffer[idx++] = ' ';
		idx += int_to_string((int)info->time_slice_remaining, buffer + idx);
		buffer[idx++] = ' ';
		buffer[idx++] = ' ';
		idx += int_to_string((int)info->time_slice_ticks, buffer + idx);
		buffer[idx] = '\0';
		vga_write_line(buffer);
	}
}

void process_scheduler_tick(void)
{
	if (!scheduler_active)
		return;

	uint64_t now = get_ticks();
	wake_sleepers(now);

	struct process *proc = current_process;
	if (!proc)
		return;

	if (proc == idle_process)
	{
		if (ready_bitmap != 0u)
			scheduler_preempt_running(0);
		return;
	}

	if (proc->time_slice_remaining > 0)
		--proc->time_slice_remaining;

	if (proc->time_slice_remaining == 0)
		scheduler_preempt_running(1);
}
/* New scheduler implementation will be inserted here */
