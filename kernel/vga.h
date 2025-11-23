#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include <stddef.h>

void vga_init(void);
void vga_clear(void);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_write_char(char c);
void vga_write(const char *str);
void vga_write_line(const char *str);
void vga_backspace(void);

#endif
