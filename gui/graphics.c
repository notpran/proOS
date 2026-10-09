#include "graphics.h"

#include "memory.h"
#include "vbe.h"

static framebuffer_t framebuffer;
static gfx_surface_t backbuffer;
static int graphics_ready;

static int clip_rect(const gfx_surface_t *surface, int *x, int *y, int *width, int *height)
{
    if (!surface || !surface->pixels || !x || !y || !width || !height)
        return 0;
    if (*width <= 0 || *height <= 0)
        return 0;
    if (*x < 0)
    {
        *width += *x;
        *x = 0;
    }
    if (*y < 0)
    {
        *height += *y;
        *y = 0;
    }
    if (*x >= (int)surface->width || *y >= (int)surface->height)
        return 0;
    if (*x + *width > (int)surface->width)
        *width = (int)surface->width - *x;
    if (*y + *height > (int)surface->height)
        *height = (int)surface->height - *y;
    return *width > 0 && *height > 0;
}

int gfx_init(void)
{
    size_t backbuffer_bytes;
    if (!vbe_available())
        return -1;

    framebuffer.address = (uint32_t)(uintptr_t)vbe_framebuffer();
    framebuffer.width = vbe_width();
    framebuffer.height = vbe_height();
    framebuffer.pitch = vbe_pitch();
    framebuffer.bpp = 32;
    backbuffer.width = framebuffer.width;
    backbuffer.height = framebuffer.height;
    backbuffer.stride = framebuffer.pitch / sizeof(uint32_t);
    backbuffer_bytes = (size_t)backbuffer.stride * backbuffer.height * sizeof(uint32_t);
    backbuffer.pixels = (uint32_t *)kalloc_zero(backbuffer_bytes);
    graphics_ready = backbuffer.pixels != NULL;
    return graphics_ready ? 0 : -1;
}

const framebuffer_t *gfx_framebuffer(void)
{
    return graphics_ready ? &framebuffer : NULL;
}

gfx_surface_t *gfx_backbuffer(void)
{
    return graphics_ready ? &backbuffer : NULL;
}

void gfx_clear(uint32_t color)
{
    gfx_fill_rect(&backbuffer, 0, 0, (int)backbuffer.width, (int)backbuffer.height, color);
}

void gfx_put_pixel(gfx_surface_t *surface, int x, int y, uint32_t color)
{
    if (!surface || !surface->pixels || x < 0 || y < 0 || x >= (int)surface->width || y >= (int)surface->height)
        return;
    surface->pixels[(size_t)y * surface->stride + (size_t)x] = color;
}

void gfx_fill_rect(gfx_surface_t *surface, int x, int y, int width, int height, uint32_t color)
{
    if (!clip_rect(surface, &x, &y, &width, &height))
        return;
    for (int row = 0; row < height; ++row)
    {
        uint32_t *dst = surface->pixels + (size_t)(y + row) * surface->stride + x;
        for (int col = 0; col < width; ++col)
            dst[col] = color;
    }
}

void gfx_draw_rect(gfx_surface_t *surface, int x, int y, int width, int height, uint32_t color)
{
    if (width <= 0 || height <= 0)
        return;
    gfx_fill_rect(surface, x, y, width, 1, color);
    gfx_fill_rect(surface, x, y + height - 1, width, 1, color);
    gfx_fill_rect(surface, x, y, 1, height, color);
    gfx_fill_rect(surface, x + width - 1, y, 1, height, color);
}

void gfx_draw_line(gfx_surface_t *surface, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;)
    {
        gfx_put_pixel(surface, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int twice = 2 * error;
        if (twice >= dy)
        {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx)
        {
            error += dx;
            y0 += sy;
        }
    }
}

void gfx_blit(gfx_surface_t *dst, const gfx_surface_t *src, int dst_x, int dst_y)
{
    if (!dst || !src || !dst->pixels || !src->pixels)
        return;
    for (uint32_t row = 0; row < src->height; ++row)
    {
        int y = dst_y + (int)row;
        if (y < 0 || y >= (int)dst->height)
            continue;
        int start = dst_x < 0 ? -dst_x : 0;
        int count = (int)src->width - start;
        if (dst_x + start + count > (int)dst->width)
            count = (int)dst->width - dst_x - start;
        if (count <= 0)
            continue;
        uint32_t *out = dst->pixels + (size_t)y * dst->stride + dst_x + start;
        const uint32_t *in = src->pixels + (size_t)row * src->stride + start;
        for (int col = 0; col < count; ++col)
            out[col] = in[col];
    }
}

void gfx_present(void)
{
    if (!graphics_ready)
        return;
    uint32_t *front = vbe_framebuffer();
    for (uint32_t row = 0; row < framebuffer.height; ++row)
    {
        uint32_t *dst = (uint32_t *)((uint8_t *)front + (size_t)row * framebuffer.pitch);
        const uint32_t *src = backbuffer.pixels + (size_t)row * backbuffer.stride;
        for (uint32_t col = 0; col < framebuffer.width; ++col)
            dst[col] = vbe_pack_color(src[col]);
    }
}

gfx_surface_t *gfx_surface_create(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return NULL;
    gfx_surface_t *surface = (gfx_surface_t *)kalloc_zero(sizeof(*surface));
    if (!surface)
        return NULL;
    surface->pixels = (uint32_t *)kalloc_zero((size_t)width * height * sizeof(uint32_t));
    if (!surface->pixels)
        return NULL;
    surface->width = width;
    surface->height = height;
    surface->stride = width;
    return surface;
}

void gfx_surface_destroy(gfx_surface_t *surface)
{
    if (surface)
        surface->pixels = NULL;
}
