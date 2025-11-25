#include "gfx.h"
#include "vbe.h"
#include "memory.h"
#include "fat16.h"
#include "fb_font.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_WINDOWS 4
#define TITLE_BAR_PADDING 2
#define BORDER_COLOR 0x00222222
#define TITLE_BAR_COLOR 0x003a6ea5
#define TITLE_TEXT_COLOR 0x00FFFFFF
#define WINDOW_BG_COLOR 0x00F0F0F0
#define WINDOW_TEXT_COLOR 0x00202020

struct window
{
    int used;
    int z;
    int x;
    int y;
    int w;
    int h;
    const char *title;
    uint32_t *pixels;
};

static struct window windows[MAX_WINDOWS];
static int z_stack[MAX_WINDOWS];
static int window_count = 0;
static int demo_ready = 0;

static const uint8_t *gfx_font_base = &font8x8_basic[0][0];
static uint32_t gfx_font_stride = 8;
static uint32_t gfx_font_height = 8;
static uint32_t gfx_font_width = 8;
static uint32_t gfx_font_row_bytes = 1;
static uint32_t gfx_font_first_char = 32;
static uint32_t gfx_font_char_count = 96;
static int gfx_title_bar_height = (8 + TITLE_BAR_PADDING);
static int gfx_font_lsb_left = 1;

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void gfx_refresh_font(void)
{
    const uint8_t *base = vbe_font_table();
    uint32_t stride = vbe_font_stride();
    uint32_t height = vbe_font_height();
    uint32_t width = vbe_font_width();
    uint32_t row_bytes = vbe_font_row_bytes();
    uint32_t first = vbe_font_first_char();
    uint32_t count = vbe_font_char_count();

    if (base && stride != 0 && height >= 8 && width != 0 && row_bytes != 0)
    {
        gfx_font_base = base;
        gfx_font_stride = stride;
        gfx_font_height = height;
        gfx_font_width = width;
        gfx_font_row_bytes = row_bytes;
        gfx_font_first_char = first;
        gfx_font_char_count = (count != 0) ? count : 256;
        gfx_font_lsb_left = vbe_font_lsb_left();
    }
    else
    {
        gfx_font_base = &font8x8_basic[0][0];
        gfx_font_stride = 8;
        gfx_font_height = 8;
        gfx_font_width = 8;
        gfx_font_row_bytes = 1;
        gfx_font_first_char = 32;
        gfx_font_char_count = 96;
        gfx_font_lsb_left = 1;
    }

    gfx_title_bar_height = (int)gfx_font_height + TITLE_BAR_PADDING;
    if (gfx_title_bar_height < 12)
        gfx_title_bar_height = 12;
}

static void window_fill(struct window *w, uint32_t color)
{
    int area = w->w * w->h;
    for (int i = 0; i < area; ++i)
        w->pixels[i] = color;
}

static void window_fill_rect(struct window *w, int x, int y, int width, int height, uint32_t color)
{
    if (!w || !w->pixels)
        return;
    for (int row = 0; row < height; ++row)
    {
        int dst_y = y + row;
        if (dst_y < 0 || dst_y >= w->h)
            continue;
        uint32_t *dst = w->pixels + dst_y * w->w;
        for (int col = 0; col < width; ++col)
        {
            int dst_x = x + col;
            if (dst_x < 0 || dst_x >= w->w)
                continue;
            dst[dst_x] = color;
        }
    }
}

static const uint8_t *gfx_glyph_for_char(unsigned char uc)
{
    if (!gfx_font_base || gfx_font_stride == 0)
        return NULL;

    uint32_t count = gfx_font_char_count ? gfx_font_char_count : 96;

    if (gfx_font_first_char == 0)
    {
        if ((uint32_t)uc >= count)
        {
            if ((uint32_t)'?' < count)
                uc = '?';
            else
                return NULL;
        }
        return gfx_font_base + ((uint32_t)uc) * gfx_font_stride;
    }

    if (uc >= gfx_font_first_char && uc < gfx_font_first_char + count)
        return gfx_font_base + ((uint32_t)(uc - gfx_font_first_char)) * gfx_font_stride;

    if ('?' >= gfx_font_first_char && '?' < gfx_font_first_char + count)
        return gfx_font_base + ((uint32_t)('?' - gfx_font_first_char)) * gfx_font_stride;

    return NULL;
}

static void window_draw_char(struct window *w, int px, int py, char c, uint32_t fg, uint32_t bg)
{
    if (!w || !w->pixels)
        return;
    unsigned char uc = (unsigned char)c;
    const uint8_t *glyph = gfx_glyph_for_char(uc);
    if (!glyph)
        return;

    for (uint32_t y = 0; y < gfx_font_height; ++y)
    {
        int dst_y = py + (int)y;
        if (dst_y < 0 || dst_y >= w->h)
            continue;
        const uint8_t *row_ptr = glyph + y * gfx_font_row_bytes;
        uint32_t *dst = w->pixels + dst_y * w->w;
        for (uint32_t x = 0; x < gfx_font_width; ++x)
        {
            int dst_x = px + (int)x;
            if (dst_x < 0 || dst_x >= w->w)
                continue;
            uint8_t row_byte = row_ptr[x / 8];
            uint8_t mask = gfx_font_lsb_left ? (uint8_t)(1u << (x & 7)) : (uint8_t)(0x80u >> (x & 7));
            dst[dst_x] = (row_byte & mask) ? fg : bg;
        }
    }
}

static void window_draw_text(struct window *w, int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    int cursor_x = x;
    int cursor_y = y;
    while (*text)
    {
        if (*text == '\n')
        {
            cursor_x = x;
            cursor_y += (int)gfx_font_height + TITLE_BAR_PADDING;
            ++text;
            continue;
        }
        window_draw_char(w, cursor_x, cursor_y, *text++, fg, bg);
        cursor_x += (int)gfx_font_width;
    }
}

static void window_draw_border(struct window *w)
{
    for (int x = 0; x < w->w; ++x)
    {
        w->pixels[x] = BORDER_COLOR;
        w->pixels[(w->h - 1) * w->w + x] = BORDER_COLOR;
    }
    for (int y = 0; y < w->h; ++y)
    {
        w->pixels[y * w->w] = BORDER_COLOR;
        w->pixels[y * w->w + (w->w - 1)] = BORDER_COLOR;
    }
}

static void window_draw(struct window *w)
{
    if (!w || !w->pixels)
        return;

    window_fill(w, WINDOW_BG_COLOR);
    window_draw_border(w);
    window_fill_rect(w, 1, 1, w->w - 2, gfx_title_bar_height, TITLE_BAR_COLOR);
    window_draw_text(w, 6, 4, w->title ? w->title : "", TITLE_TEXT_COLOR, TITLE_BAR_COLOR);

    int close_height = (int)gfx_font_height;
    if (close_height < 10)
        close_height = 10;
    window_fill_rect(w, w->w - 20, 4, 12, close_height, rgb(200, 80, 80));
    int close_text_y = 6;
    if (close_height > (int)gfx_font_height)
        close_text_y = 4 + (close_height - (int)gfx_font_height) / 2;
    window_draw_char(w, w->w - 18, close_text_y, 'x', TITLE_TEXT_COLOR, rgb(200, 80, 80));
}

static void window_draw_to_fb(const struct window *w)
{
    if (!w || !w->pixels || !vbe_available())
        return;

    uint32_t *fb = vbe_framebuffer();
    uint32_t pitch = vbe_pitch() / 4;
    uint32_t screen_w = vbe_width();
    uint32_t screen_h = vbe_height();

    for (int row = 0; row < w->h; ++row)
    {
        int dst_y = w->y + row;
        if (dst_y < 0 || (uint32_t)dst_y >= screen_h)
            continue;

        uint32_t *dst = fb + dst_y * pitch;
        const uint32_t *src = w->pixels + row * w->w;
        for (int col = 0; col < w->w; ++col)
        {
            int dst_x = w->x + col;
            if (dst_x < 0 || (uint32_t)dst_x >= screen_w)
                continue;
            dst[dst_x] = src[col];
        }
    }
}

static struct window *window_create(int x, int y, int w, int h, const char *title)
{
    if (window_count >= MAX_WINDOWS)
        return NULL;

    for (int i = 0; i < MAX_WINDOWS; ++i)
    {
        if (!windows[i].used)
        {
            uint32_t *buffer = (uint32_t *)kalloc_zero((size_t)w * (size_t)h * sizeof(uint32_t));
            if (!buffer)
                return NULL;

            windows[i].used = 1;
            windows[i].x = x;
            windows[i].y = y;
            windows[i].w = w;
            windows[i].h = h;
            windows[i].title = title;
            windows[i].pixels = buffer;
            windows[i].z = window_count;
            z_stack[window_count++] = i;
            return &windows[i];
        }
    }
    return NULL;
}

static void compositor_draw(void)
{
    if (!vbe_available())
        return;

    vbe_fill_rect(0, 0, (int)vbe_width(), (int)vbe_height(), rgb(24, 32, 48));

    for (int i = 0; i < window_count; ++i)
    {
        struct window *w = &windows[z_stack[i]];
        window_draw_to_fb(w);
    }
}

static void window_write_paragraph(struct window *w, int x, int y, const char *text)
{
    window_draw_text(w, x, y, text, WINDOW_TEXT_COLOR, WINDOW_BG_COLOR);
}

int gfx_available(void)
{
    return vbe_available();
}

static void ensure_demo_initialized(void)
{
    if (demo_ready || !vbe_available())
        return;

    gfx_refresh_font();

    for (int i = 0; i < MAX_WINDOWS; ++i)
    {
        windows[i].used = 0;
        windows[i].pixels = NULL;
    }
    window_count = 0;

    struct window *console = window_create(60, 60, 360, 200, "FAT16 readme");
    struct window *status = window_create(240, 140, 320, 160, "System status");
    if (!console || !status)
        return;

    window_draw(console);
    window_draw(status);

    char buffer[512];
    int len = fat16_read("readme.txt", buffer, sizeof(buffer));
    if (len < 0)
    {
        const char *msg = "readme.txt not found";
        window_write_paragraph(console, 10, 30, msg);
    }
    else
    {
        window_write_paragraph(console, 10, 30, buffer);
    }

    window_write_paragraph(status, 10, 30, "Graphics demo ready.\nUse keyboard as usual.");

    demo_ready = 1;
}

int gfx_show_demo(void)
{
    if (!vbe_available())
        return -1;

    gfx_refresh_font();
    ensure_demo_initialized();
    if (!demo_ready)
        return -1;

    compositor_draw();
    return 0;
}
