#ifndef GFX_H
#define GFX_H

#include <stdint.h>

void gfx_init(void);
void gfx_put_pixel(int x, int y, uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_clear(uint32_t color);
void gfx_swap_buffers(void);

int gfx_available(void);
int gfx_show_demo(void);

#endif
