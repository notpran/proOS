#include "keyboard.h"
#include "interrupts.h"
#include "io.h"

#include <stddef.h>
#include <stdint.h>

#define KB_DATA_PORT 0x60
#define KB_BUFFER_SIZE 256

static volatile char buffer[KB_BUFFER_SIZE];
static volatile uint32_t head = 0;
static volatile uint32_t tail = 0;
static volatile int shift_active = 0;
static volatile int extended_active = 0;

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

static char hex_digit(uint8_t value)
{
    value &= 0x0F;
    if (value < 10)
        return (char)('0' + value);
    return (char)('A' + (value - 10));
}

static char display_char(char c)
{
    if (c == 0)
        return '-';
    if (c == '\n')
        return 'N';
    if (c == '\t')
        return 'T';
    if (c == '\b')
        return 'B';
    if (c >= 32 && c < 127)
        return c;
    return '?';
}

static void buffer_push(char c)
{
    uint32_t next = (head + 1U) % KB_BUFFER_SIZE;
    if (next != tail)
    {
        buffer[head] = c;
        head = next;
    }
}

static void dispatch_scancode(uint8_t scancode, int release, int extended)
{
    uint32_t payload = (uint32_t)scancode;
    if (release)
        payload |= KB_EVENT_FLAG_RELEASE;
    if (extended)
        payload |= KB_EVENT_FLAG_EXTENDED;
    irq_dispatch_event(KB_IRQ_LINE, payload);
}

static char translate_scancode(uint8_t scancode)
{
    if (scancode >= 128)
        return 0;

    if (shift_active)
        return keymap_shift[scancode];
    return keymap[scancode];
}

static void keyboard_handle_scancode(uint8_t scancode)
{
    if (scancode == 0xE0)
    {
        extended_active = 1;
        return;
    }

    int release = (scancode & 0x80) ? 1 : 0;
    uint8_t code = (uint8_t)(scancode & 0x7F);

    if (extended_active)
    {
        if (release)
        {
            dispatch_scancode(code, 1, 1);
        }
        else
        {
            switch (code)
            {
                case 0x48:
                    buffer_push(KB_KEY_ARROW_UP);
                    break;
                case 0x50:
                    buffer_push(KB_KEY_ARROW_DOWN);
                    break;
                case 0x4B:
                    buffer_push(KB_KEY_ARROW_LEFT);
                    break;
                case 0x4D:
                    buffer_push(KB_KEY_ARROW_RIGHT);
                    break;
                default:
                    break;
            }
            dispatch_scancode(code, 0, 1);
        }
        extended_active = 0;
        return;
    }

    if (release)
    {
        dispatch_scancode(code, 1, 0);
        if (code == 0x2A || code == 0x36)
            shift_active = 0;
        return;
    }

    if (code == 0x2A || code == 0x36)
    {
        shift_active = 1;
        dispatch_scancode(code, 0, 0);
        return;
    }

    if (code == 0x0E)
    {
        buffer_push('\b');
        dispatch_scancode(code, 0, 0);
        return;
    }

    if (code == 0x1C)
    {
        buffer_push('\n');
        dispatch_scancode(code, 0, 0);
        return;
    }

    char c = translate_scancode(code);
    if (c)
    {
        buffer_push(c);
        dispatch_scancode(code, 0, 0);
    }
}

static void keyboard_irq_handler(struct regs *frame)
{
    (void)frame;

    uint8_t scancode = inb(KB_DATA_PORT);
    keyboard_handle_scancode(scancode);
}

void kb_init(void)
{
    head = 0;
    tail = 0;
    shift_active = 0;
    extended_active = 0;
    irq_install_handler(1, keyboard_irq_handler);
}

char kb_getchar(void)
{
    if (head == tail)
        return 0;

    char c = buffer[tail];
    tail = (tail + 1U) % KB_BUFFER_SIZE;
    return c;
}

int kb_poll(void)
{
    uint8_t status = inb(0x64);
    if ((status & 0x01u) == 0u)
        return 0;

    uint8_t scancode = inb(KB_DATA_PORT);
    keyboard_handle_scancode(scancode);
    return 1;
}

int kb_dump_layout(char *out, size_t max_len)
{
    if (!out || max_len == 0)
        return -1;

    size_t pos = 0;

    for (int scancode = 0; scancode < 128; ++scancode)
    {
        char lower = keymap[scancode];
        char upper = keymap_shift[scancode];
        if (!lower && !upper)
            continue;

        if (pos + 10 >= max_len)
            break;

        out[pos++] = '0';
        out[pos++] = 'x';
        out[pos++] = hex_digit((uint8_t)(scancode >> 4));
        out[pos++] = hex_digit((uint8_t)scancode);
        out[pos++] = ':';
        out[pos++] = ' ';
        out[pos++] = display_char(lower);
        out[pos++] = ' ';
        out[pos++] = display_char(upper);
        if (pos + 1 >= max_len)
            break;
        out[pos++] = '\n';
    }

    if (pos >= max_len)
        pos = max_len - 1;
    out[pos] = '\0';
    return (int)pos;
}
