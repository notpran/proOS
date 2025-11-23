#include "vga.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static uint16_t *const VGA_MEMORY = (uint16_t *)0xB8000;
static size_t cursor_row = 0;
static size_t cursor_col = 0;
static uint8_t current_color = 0x0F; /* White on black */

static inline uint16_t vga_entry(char c, uint8_t color)
{
    return (uint16_t)c | ((uint16_t)color << 8);
}

static void vga_scroll(void)
{
    if (cursor_row < VGA_HEIGHT)
        return;

    for (size_t row = 1; row < VGA_HEIGHT; ++row)
    {
        for (size_t col = 0; col < VGA_WIDTH; ++col)
        {
            VGA_MEMORY[(row - 1) * VGA_WIDTH + col] = VGA_MEMORY[row * VGA_WIDTH + col];
        }
    }

    size_t last = (VGA_HEIGHT - 1) * VGA_WIDTH;
    for (size_t col = 0; col < VGA_WIDTH; ++col)
    {
        VGA_MEMORY[last + col] = vga_entry(' ', current_color);
    }

    cursor_row = VGA_HEIGHT - 1;
}

void vga_init(void)
{
    current_color = 0x07; /* Light grey on black */
    vga_clear();
}

void vga_clear(void)
{
    for (size_t row = 0; row < VGA_HEIGHT; ++row)
    {
        for (size_t col = 0; col < VGA_WIDTH; ++col)
        {
            VGA_MEMORY[row * VGA_WIDTH + col] = vga_entry(' ', current_color);
        }
    }

    cursor_row = 0;
    cursor_col = 0;
}

void vga_set_color(uint8_t fg, uint8_t bg)
{
    current_color = fg | (bg << 4);
}

void vga_write_char(char c)
{
    if (c == '\n')
    {
        cursor_col = 0;
        ++cursor_row;
        vga_scroll();
        return;
    }

    if (c == '\r')
    {
        cursor_col = 0;
        return;
    }

    VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(c, current_color);
    ++cursor_col;

    if (cursor_col >= VGA_WIDTH)
    {
        cursor_col = 0;
        ++cursor_row;
        vga_scroll();
    }
}

void vga_write(const char *str)
{
    while (*str)
    {
        vga_write_char(*str++);
    }
}

void vga_write_line(const char *str)
{
    vga_write(str);
    vga_write_char('\n');
}

void vga_backspace(void)
{
    if (cursor_col == 0 && cursor_row == 0)
        return;

    if (cursor_col == 0)
    {
        cursor_col = VGA_WIDTH - 1;
        if (cursor_row > 0)
            --cursor_row;
    }
    else
    {
        --cursor_col;
    }

    VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(' ', current_color);
}
