#include "graphics.h"

#include "memory.h"
#include "vbe.h"

static framebuffer_t framebuffer;
static gfx_surface_t backbuffer;
static int graphics_ready;

static int clip_rect(const gfx_surface_t *surface, int *x, int *y, int *width, int *height)
{
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (!surface || !surface->pixels || !x || !y || !width || !height)
        return 0;
    if (surface->stride < surface->width || *width <= 0 || *height <= 0)
        return 0;

    left = *x;
    top = *y;
    right = left + *width;
    bottom = top + *height;
    if (right <= 0 || bottom <= 0 || left >= (int64_t)surface->width ||
        top >= (int64_t)surface->height)
        return 0;

    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > (int64_t)surface->width)
        right = surface->width;
    if (bottom > (int64_t)surface->height)
        bottom = surface->height;

    *x = (int)left;
    *y = (int)top;
    *width = (int)(right - left);
    *height = (int)(bottom - top);
    return *width > 0 && *height > 0;
}

int gfx_init(void)
{
    size_t backbuffer_bytes;
    if (graphics_ready)
        return 0;
    if (!vbe_available())
        return -1;

    framebuffer.address = (uint32_t)(uintptr_t)vbe_framebuffer();
    framebuffer.width = vbe_width();
    framebuffer.height = vbe_height();
    framebuffer.pitch = vbe_pitch();
    framebuffer.bpp = 32;
    if (framebuffer.width == 0 || framebuffer.height == 0 ||
        framebuffer.pitch < framebuffer.width * sizeof(uint32_t) ||
        (size_t)(framebuffer.pitch / sizeof(uint32_t)) >
            (size_t)-1 / framebuffer.height / sizeof(uint32_t))
        return -1;
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
    if (!surface || !surface->pixels || surface->stride < surface->width || x < 0 || y < 0 ||
        (uint32_t)x >= surface->width || (uint32_t)y >= surface->height)
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
    int64_t right;
    int64_t bottom;
    if (width <= 0 || height <= 0)
        return;
    right = (int64_t)x + width - 1;
    bottom = (int64_t)y + height - 1;
    gfx_fill_rect(surface, x, y, width, 1, color);
    if (bottom >= (int64_t)-2147483648LL && bottom <= 2147483647LL)
        gfx_fill_rect(surface, x, (int)bottom, width, 1, color);
    gfx_fill_rect(surface, x, y, 1, height, color);
    if (right >= (int64_t)-2147483648LL && right <= 2147483647LL)
        gfx_fill_rect(surface, (int)right, y, 1, height, color);
}

void gfx_draw_line(gfx_surface_t *surface, int x0, int y0, int x1, int y1, uint32_t color)
{
    int64_t current_x = x0;
    int64_t current_y = y0;
    int64_t target_x = x1;
    int64_t target_y = y1;
    int64_t dx = target_x > current_x ? target_x - current_x : current_x - target_x;
    int64_t sx = current_x < target_x ? 1 : -1;
    int64_t dy = target_y > current_y ? current_y - target_y : target_y - current_y;
    int64_t sy = current_y < target_y ? 1 : -1;
    int64_t error = dx + dy;
    for (;;)
    {
        if (surface && surface->pixels && current_x >= 0 && current_y >= 0 &&
            (uint64_t)current_x < surface->width && (uint64_t)current_y < surface->height)
            surface->pixels[(size_t)current_y * surface->stride + (size_t)current_x] = color;
        if (current_x == target_x && current_y == target_y)
            break;
        int64_t twice = 2 * error;
        if (twice >= dy)
        {
            error += dy;
            current_x += sx;
        }
        if (twice <= dx)
        {
            error += dx;
            current_y += sy;
        }
    }
}

void gfx_blit(gfx_surface_t *dst, const gfx_surface_t *src, int dst_x, int dst_y)
{
    int64_t source_x = 0;
    int64_t source_y = 0;
    int64_t target_x = dst_x;
    int64_t target_y = dst_y;
    int64_t width;
    int64_t height;

    if (!dst || !src || !dst->pixels || !src->pixels ||
        dst->stride < dst->width || src->stride < src->width)
        return;
    width = src->width;
    height = src->height;
    if (target_x < 0)
    {
        source_x = -target_x;
        width -= source_x;
        target_x = 0;
    }
    if (target_y < 0)
    {
        source_y = -target_y;
        height -= source_y;
        target_y = 0;
    }
    if (target_x >= (int64_t)dst->width || target_y >= (int64_t)dst->height || width <= 0 || height <= 0)
        return;
    if (width > (int64_t)dst->width - target_x)
        width = (int64_t)dst->width - target_x;
    if (height > (int64_t)dst->height - target_y)
        height = (int64_t)dst->height - target_y;

    for (int64_t row = 0; row < height; ++row)
    {
        uint32_t *out = dst->pixels + (size_t)(target_y + row) * dst->stride + (size_t)target_x;
        const uint32_t *in = src->pixels + (size_t)(source_y + row) * src->stride + (size_t)source_x;
        for (int64_t col = 0; col < width; ++col)
            out[col] = in[col];
    }
}

void gfx_present(void)
{
    int native_color;
    if (!graphics_ready)
        return;
    uint32_t *front = vbe_framebuffer();
    if (!front)
        return;
    native_color = vbe_color_is_native();
    for (uint32_t row = 0; row < framebuffer.height; ++row)
    {
        uint32_t *dst = (uint32_t *)((uint8_t *)front + (size_t)row * framebuffer.pitch);
        const uint32_t *src = backbuffer.pixels + (size_t)row * backbuffer.stride;
        for (uint32_t col = 0; col < framebuffer.width; ++col)
            dst[col] = native_color ? src[col] : vbe_pack_color(src[col]);
    }
}

gfx_surface_t *gfx_surface_create(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return NULL;
    if ((size_t)width > (size_t)-1 / height / sizeof(uint32_t))
        return NULL;
    gfx_surface_t *surface = (gfx_surface_t *)kalloc_zero(sizeof(*surface));
    if (!surface)
        return NULL;
    surface->pixels = (uint32_t *)kalloc_zero((size_t)width * height * sizeof(uint32_t));
    if (!surface->pixels)
    {
        kfree(surface);
        return NULL;
    }
    surface->width = width;
    surface->height = height;
    surface->stride = width;
    return surface;
}

void gfx_surface_destroy(gfx_surface_t *surface)
{
    if (surface)
    {
        kfree(surface->pixels);
        kfree(surface);
    }
}
