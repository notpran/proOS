#include <stddef.h>
#include <stdint.h>

#include "module_api.h"

#include "devmgr.h"
#include "interrupts.h"
#include "klog.h"
#include "keyboard.h"
#include "vfs.h"
#include "syscall.h"

MODULE_METADATA("ps2kbd", "0.1.0", MODULE_FLAG_AUTOSTART);

static struct irq_mailbox kbd_mailbox;

struct key_fifo_entry
{
    uint32_t payload;
    uint32_t timestamp;
    char ch;
};

#define KEY_FIFO_CAPACITY 64

static struct key_fifo_entry key_fifo[KEY_FIFO_CAPACITY];
static volatile uint8_t fifo_head = 0;
static volatile uint8_t fifo_tail = 0;
static int shift_state = 0;
static int controller_created = 0;
static int device_registered = 0;
static int syscall_registered = 0;

static const char keymap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char keymap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void fifo_push(uint32_t payload, char ch, uint32_t timestamp)
{
    uint8_t next = (uint8_t)((fifo_tail + 1) % KEY_FIFO_CAPACITY);
    if (next == fifo_head)
        return;
    key_fifo[fifo_tail].payload = payload;
    key_fifo[fifo_tail].timestamp = timestamp;
    key_fifo[fifo_tail].ch = ch;
    fifo_tail = next;
}

static int fifo_pop(struct key_fifo_entry *out)
{
    if (fifo_head == fifo_tail)
        return 0;
    if (out)
        *out = key_fifo[fifo_head];
    fifo_head = (uint8_t)((fifo_head + 1) % KEY_FIFO_CAPACITY);
    return 1;
}

// Pull IRQ mailbox events into our local FIFO before servicing reads or syscalls.
static void process_pending_events(void)
{
    struct irq_event event;
    while (irq_mailbox_receive(&kbd_mailbox, &event))
    {
        uint8_t scancode = (uint8_t)(event.data & 0xFF);
        int release = (event.data & KB_EVENT_FLAG_RELEASE) ? 1 : 0;
        int extended = (event.data & KB_EVENT_FLAG_EXTENDED) ? 1 : 0;

        if (extended)
        {
            if (release)
            {
                fifo_push(event.data, 0, event.timestamp);
                continue;
            }

            switch (scancode)
            {
                case 0x48:
                    fifo_push(event.data, KB_KEY_ARROW_UP, event.timestamp);
                    break;
                case 0x50:
                    fifo_push(event.data, KB_KEY_ARROW_DOWN, event.timestamp);
                    break;
                case 0x4B:
                    fifo_push(event.data, KB_KEY_ARROW_LEFT, event.timestamp);
                    break;
                case 0x4D:
                    fifo_push(event.data, KB_KEY_ARROW_RIGHT, event.timestamp);
                    break;
                default:
                    fifo_push(event.data, 0, event.timestamp);
                    break;
            }
            continue;
        }

        if (scancode == 0x2A || scancode == 0x36)
        {
            shift_state = release ? 0 : 1;
            fifo_push(event.data, 0, event.timestamp);
            continue;
        }

        if (release)
        {
            fifo_push(event.data, 0, event.timestamp);
            continue;
        }

        if (scancode == 0x0E)
        {
            fifo_push(event.data, '\b', event.timestamp);
            continue;
        }

        if (scancode == 0x1C)
        {
            fifo_push(event.data, '\n', event.timestamp);
            continue;
        }

        if (scancode >= 128)
            continue;

        char translated = shift_state ? keymap_shift[scancode] : keymap[scancode];
        if (translated)
            fifo_push(event.data, translated, event.timestamp);
    }
}

#define SYS_KBD_POLL (SYS_DYNAMIC_BASE + 0)

static int32_t sys_kbd_poll(struct syscall_envelope *message)
{
    if (!message)
        return -1;

    if (message->argc < 1)
    {
        message->result = -1;
        message->status = 1;
        return -1;
    }

    uintptr_t user_addr = (uintptr_t)message->args[0];
    if (user_addr == 0)
    {
        message->result = -1;
        message->status = 1;
        return -1;
    }

    struct ps2kbd_user_event
    {
        uint32_t timestamp;
        uint32_t payload;
        uint8_t ch;
        uint8_t reserved[3];
    };

    struct ps2kbd_user_event *user_event = (struct ps2kbd_user_event *)(uintptr_t)user_addr;
    if (syscall_validate_user_buffer(user_event, sizeof(*user_event)) < 0)
    {
        message->result = -1;
        message->status = 1;
        return -1;
    }

    process_pending_events();

    struct key_fifo_entry entry;
    if (!fifo_pop(&entry))
    {
        message->result = 0;
        message->status = 0;
        return 0;
    }

    user_event->timestamp = entry.timestamp;
    user_event->payload = entry.payload;
    user_event->ch = (uint8_t)entry.ch;
    user_event->reserved[0] = 0;
    user_event->reserved[1] = 0;
    user_event->reserved[2] = 0;

    message->result = 1;
    message->status = 0;
    return 0;
}

static size_t local_strlen(const char *s)
{
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        ++len;
    return len;
}

static int keyboard_start(struct device_node *node)
{
    (void)node;
    fifo_head = 0;
    fifo_tail = 0;
    shift_state = 0;

    irq_mailbox_init(&kbd_mailbox);
    if (irq_mailbox_subscribe(KB_IRQ_LINE, &kbd_mailbox) < 0)
    {
        klog_error("ps2kbd.driver: failed to subscribe mailbox");
        return -1;
    }

    kb_init();
    klog_info("ps2kbd.driver: keyboard controller initialized");
    return 0;
}

static void keyboard_stop(struct device_node *node)
{
    (void)node;
    irq_mailbox_unsubscribe(KB_IRQ_LINE, &kbd_mailbox);
    irq_mailbox_flush(&kbd_mailbox);
    fifo_head = 0;
    fifo_tail = 0;
    shift_state = 0;
    klog_info("ps2kbd.driver: keyboard controller shutdown");
}

static int keyboard_read(struct device_node *node, void *buffer, size_t length, size_t *out_read)
{
    (void)node;
    if (!buffer || length == 0)
        return -1;

    char *out = (char *)buffer;
    size_t produced = 0;

    process_pending_events();

    struct key_fifo_entry entry;
    while (produced < length && fifo_pop(&entry))
    {
        if (!entry.ch)
            continue;
        out[produced++] = entry.ch;
    }

    if (out_read)
        *out_read = produced;
    return (produced > 0) ? 0 : -1;
}

static const struct device_ops ps2kbd_ops = {
    keyboard_start,
    keyboard_stop,
    keyboard_read,
    NULL,
    NULL
};

static const struct device_ops ps2ctrl_ops = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

int module_init(void)
{
    controller_created = 0;
    device_registered = 0;
    syscall_registered = 0;

    const struct device_node *controller = devmgr_find("ps2ctrl0");
    if (!controller)
    {
        struct device_descriptor ctrl_desc = {
            "ps2ctrl0",
            "bus.ps2",
            "platform0",
            &ps2ctrl_ops,
            DEVICE_FLAG_INTERNAL,
            NULL
        };
        if (devmgr_register_device(&ctrl_desc, NULL) < 0)
        {
            klog_error("ps2kbd.driver: failed to register controller");
            return -1;
        }
        controller_created = 1;
    }

    struct device_descriptor desc = {
        "ps2kbd0",
        "input.keyboard",
        "ps2ctrl0",
        &ps2kbd_ops,
        DEVICE_FLAG_PUBLISH,
        NULL
    };

    if (devmgr_register_device(&desc, NULL) < 0)
    {
        klog_error("ps2kbd.driver: failed to register device");
        if (controller_created)
        {
            devmgr_unregister_device("ps2ctrl0");
            controller_created = 0;
        }
        return -1;
    }
    device_registered = 1;

    if (syscall_register_handler(SYS_KBD_POLL, sys_kbd_poll, "ps2kbd.poll") < 0)
    {
        klog_error("ps2kbd.driver: failed to register syscall handler");
        devmgr_unregister_device("ps2kbd0");
        device_registered = 0;
        if (controller_created)
        {
            devmgr_unregister_device("ps2ctrl0");
            controller_created = 0;
        }
        return -1;
    }
    syscall_registered = 1;

    const char *status = "keyboard: ready\n";
    size_t status_len = local_strlen(status);
    vfs_write_file("/Devices/ps2kbd0.status", status, status_len);

    char layout[512];
    int written = kb_dump_layout(layout, sizeof(layout));
    if (written > 0)
        vfs_write_file("/Devices/ps2kbd0.map", layout, (size_t)written);

    return 0;
}

void module_exit(void)
{
    if (syscall_registered)
    {
        if (syscall_unregister_handler(SYS_KBD_POLL) < 0)
            klog_warn("ps2kbd.driver: failed to unregister syscall handler");
        syscall_registered = 0;
    }

    if (device_registered)
    {
        devmgr_unregister_device("ps2kbd0");
        device_registered = 0;
    }

    if (controller_created)
    {
        devmgr_unregister_device("ps2ctrl0");
        controller_created = 0;
    }

    vfs_remove("/Devices/ps2kbd0.status");
    vfs_remove("/Devices/ps2kbd0.map");
}
