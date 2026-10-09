#ifndef GUI_GRAPHICS_H
#define GUI_GRAPHICS_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} framebuffer_t;

typedef struct
{
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} gfx_surface_t;

int gfx_init(void);
const framebuffer_t *gfx_framebuffer(void);
gfx_surface_t *gfx_backbuffer(void);
void gfx_clear(uint32_t color);
void gfx_put_pixel(gfx_surface_t *surface, int x, int y, uint32_t color);
void gfx_fill_rect(gfx_surface_t *surface, int x, int y, int width, int height, uint32_t color);
void gfx_draw_rect(gfx_surface_t *surface, int x, int y, int width, int height, uint32_t color);
void gfx_draw_line(gfx_surface_t *surface, int x0, int y0, int x1, int y1, uint32_t color);
void gfx_blit(gfx_surface_t *dst, const gfx_surface_t *src, int dst_x, int dst_y);
void gfx_present(void);

gfx_surface_t *gfx_surface_create(uint32_t width, uint32_t height);
void gfx_surface_destroy(gfx_surface_t *surface);

#endif
