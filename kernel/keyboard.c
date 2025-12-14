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

static char translate_scancode(uint8_t scancode)
{
    if (scancode >= 128)
        return 0;

    if (shift_active)
        return keymap_shift[scancode];
    return keymap[scancode];
}

static void keyboard_irq_handler(struct regs *frame)
{
    (void)frame;

    uint8_t scancode = inb(KB_DATA_PORT);

    if (scancode == 0xE0)
        return;

    if (scancode & 0x80)
    {
        scancode &= 0x7F;
        if (scancode == 0x2A || scancode == 0x36)
            shift_active = 0;
        return;
    }

    if (scancode == 0x2A || scancode == 0x36)
    {
        shift_active = 1;
        return;
    }

    if (scancode == 0x0E)
    {
        buffer_push('\b');
        return;
    }

    if (scancode == 0x1C)
    {
        buffer_push('\n');
        return;
    }

    char c = translate_scancode(scancode);
    if (c)
        buffer_push(c);
}

void kb_init(void)
{
    head = 0;
    tail = 0;
    shift_active = 0;
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
