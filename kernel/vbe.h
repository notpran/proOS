#ifndef VBE_H
#define VBE_H

#include <stdint.h>
#include <stddef.h>

#define BOOT_INFO_MAGIC 0x534F5250u
#define BOOT_INFO_ADDR 0x0000FE00u

struct boot_info
{
    uint32_t magic;
    uint32_t version;
    uint32_t fb_ptr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_bpp;
    uint32_t fb_phys;
    uint32_t fb_size;
    uint32_t fat_ptr;
    uint32_t fat_size;
    uint32_t font_ptr;
    uint32_t font_height;
    uint32_t font_bytes_per_char;
    uint32_t font_char_count;
    uint32_t font_flags;
};

int vbe_init(void);
int vbe_available(void);
const struct boot_info *boot_info_get(void);

const uint8_t *vbe_font_table(void);
uint32_t vbe_font_stride(void);
uint32_t vbe_font_height(void);
uint32_t vbe_font_width(void);
uint32_t vbe_font_row_bytes(void);
uint32_t vbe_font_first_char(void);
uint32_t vbe_font_char_count(void);
int vbe_font_lsb_left(void);
int vbe_try_load_font_from_fat(void);

uint32_t *vbe_framebuffer(void);
uint32_t vbe_pitch(void);
uint32_t vbe_width(void);
uint32_t vbe_height(void);
void vbe_clear(uint32_t color);
void vbe_draw_pixel(int x, int y, uint32_t color);
void vbe_fill_rect(int x, int y, int w, int h, uint32_t color);
void vbe_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void vbe_draw_text(int x, int y, const char *text, uint32_t fg, uint32_t bg);

void vbe_console_set_colors(uint8_t fg_attr, uint8_t bg_attr);
void vbe_console_clear(uint8_t attr);
void vbe_console_putc(char c);

#endif
