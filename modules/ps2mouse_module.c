#include <stddef.h>
#include <stdint.h>

#include "module_api.h"
#include "devmgr.h"
#include "interrupts.h"
#include "io.h"
#include "klog.h"
#include "syscall.h"
#include "vfs.h"

MODULE_METADATA("ps2mouse", "0.2.0", MODULE_FLAG_AUTOSTART);

#define MOUSE_IRQ 12
#define MOUSE_DATA 0x60
#define MOUSE_STATUS 0x64
#define MOUSE_COMMAND 0x64
#define MOUSE_WRITE 0xD4
#define MOUSE_ENABLE 0xF4
#define SYS_MOUSE_POLL (SYS_DYNAMIC_BASE + 1u)
#define MOUSE_FIFO_CAPACITY 32

typedef struct
{
    uint32_t timestamp;
    int32_t dx;
    int32_t dy;
    int32_t wheel;
    uint32_t buttons;
} mouse_user_event_t;

static struct irq_mailbox mouse_mailbox;
static mouse_user_event_t fifo[MOUSE_FIFO_CAPACITY];
static uint8_t fifo_head;
static uint8_t fifo_tail;
static uint8_t packet[3];
static uint8_t packet_count;
static int registered;
static int syscall_registered;

static int wait_write(void)
{
    for (uint32_t count = 0; count < 100000u; ++count)
        if ((inb(MOUSE_STATUS) & 2u) == 0u)
            return 0;
    return -1;
}

static int wait_read(void)
{
    for (uint32_t count = 0; count < 100000u; ++count)
        if (inb(MOUSE_STATUS) & 1u)
            return 0;
    return -1;
}

static int mouse_write(uint8_t value)
{
    if (wait_write() < 0)
        return -1;
    outb(MOUSE_COMMAND, MOUSE_WRITE);
    if (wait_write() < 0)
        return -1;
    outb(MOUSE_DATA, value);
    return 0;
}

static int mouse_init_controller(void)
{
    uint8_t config;
    if (wait_write() < 0)
        return -1;
    outb(MOUSE_COMMAND, 0xA8);
    if (wait_write() < 0)
        return -1;
    outb(MOUSE_COMMAND, 0x20);
    if (wait_read() < 0)
        return -1;
    config = inb(MOUSE_DATA);
    config |= 0x02u;
    config &= (uint8_t)~0x20u;
    if (wait_write() < 0)
        return -1;
    outb(MOUSE_COMMAND, 0x60);
    if (wait_write() < 0)
        return -1;
    outb(MOUSE_DATA, config);
    if (mouse_write(MOUSE_ENABLE) < 0)
        return -1;
    if (wait_read() < 0 || inb(MOUSE_DATA) != 0xFAu)
        return -1;
    return 0;
}

static void fifo_push(const struct irq_event *event)
{
    uint8_t next = (uint8_t)((fifo_tail + 1u) % MOUSE_FIFO_CAPACITY);
    if (next == fifo_head)
        return;
    fifo[fifo_tail].timestamp = event->timestamp;
    fifo[fifo_tail].dx = (int8_t)packet[1];
    fifo[fifo_tail].dy = -(int8_t)packet[2];
    fifo[fifo_tail].wheel = 0;
    fifo[fifo_tail].buttons = packet[0] & 0x07u;
    fifo_tail = next;
}

static int fifo_pop(mouse_user_event_t *event)
{
    if (fifo_head == fifo_tail)
        return 0;
    if (event)
        *event = fifo[fifo_head];
    fifo_head = (uint8_t)((fifo_head + 1u) % MOUSE_FIFO_CAPACITY);
    return 1;
}

static void process_pending_events(void)
{
    struct irq_event event;
    while (irq_mailbox_receive(&mouse_mailbox, &event))
    {
        uint8_t value = (uint8_t)(event.data & 0xFFu);
        if (packet_count == 0 && (value & 0x08u) == 0u)
            continue;
        packet[packet_count++] = value;
        if (packet_count == 3)
        {
            fifo_push(&event);
            packet_count = 0;
        }
    }
}

static void mouse_irq_handler(struct regs *frame, void *context)
{
    (void)frame;
    (void)context;
    if (inb(MOUSE_STATUS) & 1u)
        irq_dispatch_event(MOUSE_IRQ, inb(MOUSE_DATA));
}

static int32_t sys_mouse_poll(struct syscall_envelope *message)
{
    if (!message || message->argc < 1)
        return -1;
    mouse_user_event_t *event = (mouse_user_event_t *)(uintptr_t)message->args[0];
    if (syscall_validate_user_buffer(event, sizeof(*event)) < 0)
        return -1;
    process_pending_events();
    return fifo_pop(event);
}

int module_init(void)
{
    struct device_descriptor controller = { "ps2ctrl0", "bus.ps2", "platform0", NULL, DEVICE_FLAG_INTERNAL, NULL };
    struct device_descriptor device = { "ps2mouse0", "input.mouse", "ps2ctrl0", NULL, DEVICE_FLAG_PUBLISH, NULL };
    fifo_head = 0;
    fifo_tail = 0;
    packet_count = 0;
    if (!devmgr_find("ps2ctrl0") && devmgr_register_device(&controller, NULL) < 0)
        return -1;
    if (mouse_init_controller() < 0)
    {
        klog_warn("ps2mouse.driver: controller unavailable");
        return -1;
    }
    if (devmgr_register_device(&device, NULL) < 0)
        return -1;
    irq_mailbox_init(&mouse_mailbox);
    if (irq_register_shared_handler(MOUSE_IRQ, mouse_irq_handler, NULL) < 0)
        return -1;
    if (irq_mailbox_subscribe(MOUSE_IRQ, &mouse_mailbox) < 0)
    {
        irq_unregister_shared_handler(MOUSE_IRQ, mouse_irq_handler, NULL);
        return -1;
    }
    registered = 1;
    if (syscall_register_handler(SYS_MOUSE_POLL, sys_mouse_poll, "ps2mouse.poll") < 0)
    {
        irq_mailbox_unsubscribe(MOUSE_IRQ, &mouse_mailbox);
        irq_unregister_shared_handler(MOUSE_IRQ, mouse_irq_handler, NULL);
        registered = 0;
        return -1;
    }
    syscall_registered = 1;
    vfs_write_file("/Devices/ps2mouse0.status", "mouse: ready\n", 13);
    klog_info("ps2mouse.driver: controller initialized");
    return 0;
}

void module_exit(void)
{
    if (syscall_registered)
        syscall_unregister_handler(SYS_MOUSE_POLL);
    if (registered)
    {
        irq_mailbox_unsubscribe(MOUSE_IRQ, &mouse_mailbox);
        irq_unregister_shared_handler(MOUSE_IRQ, mouse_irq_handler, NULL);
    }
    devmgr_unregister_device("ps2mouse0");
    vfs_remove("/Devices/ps2mouse0.status");
}
