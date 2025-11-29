#include "vbe.h"
#include "fb_font.h"
#include "fat16.h"
#include "memory.h"

#include <stddef.h>
#include <stdint.h>
#include "string.h"

#define CONSOLE_COLUMNS 80
#define CONSOLE_ROWS 25
#define DEFAULT_FONT_WIDTH 8
#define FONT_FILE_NAME "font.psf"
#define FONT_BDF_FILE_NAME "font.bdf"

#ifdef HAVE_EMBEDDED_FONT
extern const uint8_t EMBEDDED_FONT_START[];
extern const uint8_t EMBEDDED_FONT_END[];
#endif

struct parsed_font
{
    const uint8_t *glyph_base;
    uint32_t stride;
    uint32_t height;
    uint32_t width;
    uint32_t first_char;
    uint32_t glyph_count;
    int lsb_left;
};

static const struct boot_info *bootinfo = (const struct boot_info *)BOOT_INFO_ADDR;
static uint32_t *fb_ptr = NULL;
static uint32_t fb_pitch_pixels = 0;
static uint32_t fb_w = 0;
static uint32_t fb_h = 0;
static int vbe_ready = 0;

static const uint8_t *font_base = &font8x8_basic[0][0];
static uint32_t font_stride = 8;
static uint32_t font_height_px = 8;
static uint32_t font_width_px = DEFAULT_FONT_WIDTH;
static uint32_t font_row_bytes = 1;
static uint32_t font_first_char = 32;
static uint32_t font_char_count = 96;
static int font_lsb_left = 1;
static uint8_t *font_external_blob = NULL;

static uint8_t console_fg = 0x0F;
static uint8_t console_bg = 0x00;
static size_t console_row = 0;
static size_t console_col = 0;
static uint32_t console_cols = CONSOLE_COLUMNS;
static uint32_t console_rows = CONSOLE_ROWS;
static char console_chars[CONSOLE_ROWS][CONSOLE_COLUMNS];
static uint8_t console_attr[CONSOLE_ROWS][CONSOLE_COLUMNS];

static uint32_t vga_palette[16] = {
    0x00000000, 0x000000AA, 0x0000AA00, 0x0000AAAA,
    0x00AA0000, 0x00AA00AA, 0x00AA5500, 0x00AAAAAA,
    0x00555555, 0x005555FF, 0x0055FF55, 0x0055FFFF,
    0x00FF5555, 0x00FF55FF, 0x00FFFF55, 0x00FFFFFF
};

static int parse_psf_font(const uint8_t *buffer, size_t size, struct parsed_font *out);
static int parse_bdf_font(const char *buffer, size_t size, struct parsed_font *out, uint8_t **blob_out);
static int try_use_embedded_font(void);
static int adopt_font_candidate(const struct parsed_font *candidate, uint8_t *owned_blob, int redraw_console);
static void update_console_geometry(void);
static void console_redraw(void);

static int configure_font_metrics(uint32_t height, uint32_t stride, uint32_t width_hint, uint32_t first_char, uint32_t count, int lsb_left)
{
    if (height == 0 || stride == 0)
        return 0;
    if (stride % height != 0)
        return 0;

    font_height_px = height;
    font_stride = stride;
    font_row_bytes = stride / height;
    if (font_row_bytes == 0)
        return 0;

    if (width_hint != 0)
        font_width_px = width_hint;
    else
        font_width_px = font_row_bytes * 8;

    uint32_t max_width = font_row_bytes * 8;
    if (font_width_px == 0 || font_width_px > max_width)
        font_width_px = max_width;
    if (font_width_px == 0)
        font_width_px = DEFAULT_FONT_WIDTH;

    font_first_char = first_char;
    font_char_count = (count != 0) ? count : 256;
    font_lsb_left = lsb_left;
    return 1;
}

static void update_console_geometry(void)
{
    uint32_t max_cols = CONSOLE_COLUMNS;
    uint32_t max_rows = CONSOLE_ROWS;

    uint32_t cols = max_cols;
    uint32_t rows = max_rows;

    if (fb_w != 0 && font_width_px != 0)
    {
        uint32_t possible = fb_w / font_width_px;
        if (possible == 0)
            possible = 1;
        cols = (possible < max_cols) ? possible : max_cols;
    }

    if (fb_h != 0 && font_height_px != 0)
    {
        uint32_t possible = fb_h / font_height_px;
        if (possible == 0)
            possible = 1;
        rows = (possible < max_rows) ? possible : max_rows;
    }

    if (cols == 0)
        cols = 1;
    if (rows == 0)
        rows = 1;

    console_cols = cols;
    console_rows = rows;

    if (console_row >= console_rows)
        console_row = console_rows - 1;
    if (console_col >= console_cols)
        console_col = (console_cols > 0) ? console_cols - 1 : 0;
}

static int adopt_font_candidate(const struct parsed_font *candidate, uint8_t *owned_blob, int redraw_console)
{
    if (!candidate)
        return 0;

    if (!configure_font_metrics(candidate->height, candidate->stride, candidate->width, candidate->first_char, candidate->glyph_count, candidate->lsb_left))
        return 0;

    font_external_blob = owned_blob;
    font_base = candidate->glyph_base;
    update_console_geometry();

    if (redraw_console)
        console_redraw();

    return 1;
}

static inline uint32_t attr_to_color(uint8_t attr)
{
    return vga_palette[attr & 0x0F];
}

static const uint8_t *glyph_for_char(unsigned char uc)
{
    if (!font_base || font_stride == 0)
        return NULL;

    if (uc >= font_first_char && uc < font_first_char + font_char_count)
        return font_base + ((uint32_t)(uc - font_first_char)) * font_stride;

    if (font_first_char == 0 && uc < font_char_count)
        return font_base + ((uint32_t)uc) * font_stride;

    uc = '?';
    if (uc >= font_first_char && uc < font_first_char + font_char_count)
        return font_base + ((uint32_t)(uc - font_first_char)) * font_stride;

    return NULL;
}

static void draw_glyph(int px, int py, char c, uint32_t fg, uint32_t bg)
{
    if (!vbe_ready)
        return;

    const uint8_t *glyph = glyph_for_char((unsigned char)c);
    if (!glyph)
        return;

    for (uint32_t y = 0; y < font_height_px; ++y)
    {
        int dst_y = py + (int)y;
        if (dst_y < 0 || (uint32_t)dst_y >= fb_h)
            continue;

        const uint8_t *row_ptr = glyph + y * font_row_bytes;
        uint32_t *dst_row = fb_ptr + dst_y * fb_pitch_pixels;
        for (uint32_t x = 0; x < font_width_px; ++x)
        {
            int dst_x = px + (int)x;
            if (dst_x < 0 || (uint32_t)dst_x >= fb_w)
                continue;
            uint8_t row_byte = row_ptr[x / 8];
            uint8_t mask = font_lsb_left ? (uint8_t)(1u << (x & 7)) : (uint8_t)(0x80u >> (x & 7));
            uint32_t color = (row_byte & mask) ? fg : bg;
            dst_row[dst_x] = color;
        }
    }
}

static void console_redraw(void)
{
    if (!vbe_ready)
        return;

    for (uint32_t y = 0; y < console_rows; ++y)
    {
        for (uint32_t x = 0; x < console_cols; ++x)
        {
            uint8_t attr = console_attr[y][x];
            uint32_t fg = attr_to_color(attr & 0x0F);
            uint32_t bg = attr_to_color((attr >> 4) & 0x0F);
            draw_glyph(x * (int)font_width_px, (int)(y * font_height_px), console_chars[y][x], fg, bg);
        }
    }
}

static void console_clear_buffers(uint8_t fg, uint8_t bg)
{
    for (int y = 0; y < CONSOLE_ROWS; ++y)
    {
        for (int x = 0; x < CONSOLE_COLUMNS; ++x)
        {
            console_chars[y][x] = ' ';
            console_attr[y][x] = ((bg & 0x0F) << 4) | (fg & 0x0F);
        }
    }
    console_row = 0;
    console_col = 0;
}

static void console_newline(void)
{
    console_col = 0;
    ++console_row;
    if (console_row < console_rows)
        return;

    for (uint32_t y = 1; y < console_rows; ++y)
    {
        for (uint32_t x = 0; x < console_cols; ++x)
        {
            console_chars[y - 1][x] = console_chars[y][x];
            console_attr[y - 1][x] = console_attr[y][x];
        }
    }

    uint32_t target_row = (console_rows > 0) ? console_rows - 1 : 0;
    for (uint32_t x = 0; x < console_cols; ++x)
    {
        console_chars[target_row][x] = ' ';
        console_attr[target_row][x] = ((console_bg & 0x0F) << 4) | (console_fg & 0x0F);
    }

    console_row = target_row;
    console_col = 0;
    console_redraw();
}

int vbe_init(void)
{
    font_external_blob = NULL;
    font_base = &font8x8_basic[0][0];
    if (!configure_font_metrics(8, 8, DEFAULT_FONT_WIDTH, 32, 96, 1))
        return 0;

    if (bootinfo->magic != BOOT_INFO_MAGIC || bootinfo->fb_bpp != 32)
    {
        vbe_ready = 0;
        return 0;
    }

    fb_ptr = (uint32_t *)(uintptr_t)bootinfo->fb_ptr;
    fb_pitch_pixels = bootinfo->fb_pitch / 4;
    fb_w = bootinfo->fb_width;
    fb_h = bootinfo->fb_height;
    vbe_ready = 1;
    update_console_geometry();

    if (bootinfo->font_ptr && bootinfo->font_height >= 8 && bootinfo->font_bytes_per_char >= bootinfo->font_height)
    {
        const uint8_t *candidate = (const uint8_t *)(uintptr_t)bootinfo->font_ptr;
        uint32_t stride = bootinfo->font_bytes_per_char;
        uint32_t height = bootinfo->font_height;
        uint32_t count = bootinfo->font_char_count;
        if (count == 0)
            count = 256;
        int lsb = (bootinfo->font_flags & 1u) ? 1 : 0;
        if (configure_font_metrics(height, stride, DEFAULT_FONT_WIDTH, 0, count, lsb))
        {
            font_base = candidate;
            update_console_geometry();
        }
    }

    try_use_embedded_font();

    vbe_clear(0x00000000);
    console_clear_buffers(console_fg, console_bg);
    console_redraw();
    return 1;
}

int vbe_available(void)
{
    return vbe_ready;
}

const struct boot_info *boot_info_get(void)
{
    return bootinfo;
}

uint32_t *vbe_framebuffer(void)
{
    return fb_ptr;
}

uint32_t vbe_pitch(void)
{
    return bootinfo->fb_pitch;
}

uint32_t vbe_width(void)
{
    return fb_w;
}

uint32_t vbe_height(void)
{
    return fb_h;
}

void vbe_clear(uint32_t color)
{
    if (!vbe_ready)
        return;

    uint32_t total = fb_pitch_pixels * fb_h;
    for (uint32_t i = 0; i < total; ++i)
        fb_ptr[i] = color;
}

void vbe_draw_pixel(int x, int y, uint32_t color)
{
    if (!vbe_ready)
        return;
    if (x < 0 || y < 0)
        return;
    if ((uint32_t)x >= fb_w || (uint32_t)y >= fb_h)
        return;

    fb_ptr[y * fb_pitch_pixels + x] = color;
}

void vbe_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    if (!vbe_ready)
        return;

    for (int row = 0; row < h; ++row)
    {
        int dst_y = y + row;
        if (dst_y < 0 || (uint32_t)dst_y >= fb_h)
            continue;
        uint32_t *dst = fb_ptr + dst_y * fb_pitch_pixels;
        for (int col = 0; col < w; ++col)
        {
            int dst_x = x + col;
            if (dst_x < 0 || (uint32_t)dst_x >= fb_w)
                continue;
            dst[dst_x] = color;
        }
    }
}

void vbe_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg)
{
    draw_glyph(x, y, c, fg, bg);
}

void vbe_draw_text(int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    while (*text)
    {
        vbe_draw_char(x, y, *text++, fg, bg);
        x += (int)font_width_px;
    }
}

void vbe_console_set_colors(uint8_t fg_attr, uint8_t bg_attr)
{
    console_fg = fg_attr & 0x0F;
    console_bg = bg_attr & 0x0F;
}

void vbe_console_clear(uint8_t attr)
{
    console_fg = attr & 0x0F;
    console_bg = (attr >> 4) & 0x0F;
    if (!vbe_ready)
        return;

    vbe_clear(attr_to_color(console_bg));
    console_clear_buffers(console_fg, console_bg);
}

void vbe_console_putc(char c)
{
    if (!vbe_ready)
        return;

    if (c == '\n')
    {
        console_newline();
        return;
    }

    if (c == '\r')
    {
        console_col = 0;
        return;
    }

    if (c == '\b')
    {
        if (console_col > 0)
            --console_col;
        else if (console_row > 0)
        {
            --console_row;
            console_col = (console_cols > 0) ? console_cols - 1 : 0;
        }
        console_chars[console_row][console_col] = ' ';
        console_attr[console_row][console_col] = (console_bg << 4) | console_fg;
        draw_glyph((int)console_col * (int)font_width_px, (int)(console_row * font_height_px), ' ', attr_to_color(console_fg), attr_to_color(console_bg));
        return;
    }

    console_chars[console_row][console_col] = c;
    console_attr[console_row][console_col] = ((console_bg & 0x0F) << 4) | (console_fg & 0x0F);
    draw_glyph((int)console_col * (int)font_width_px, (int)(console_row * font_height_px), c, attr_to_color(console_fg), attr_to_color(console_bg));
    if (++console_col >= console_cols)
        console_newline();
}

#define PSF2_MAGIC 0x864AB572u

struct psf2_header
{
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t glyph_count;
    uint32_t glyph_size;
    uint32_t height;
    uint32_t width;
};

static int parse_psf_font(const uint8_t *buffer, size_t size, struct parsed_font *out)
{
    if (!buffer || !out || size < sizeof(struct psf2_header))
        return 0;

    const struct psf2_header *hdr = (const struct psf2_header *)buffer;
    if (hdr->magic != PSF2_MAGIC)
        return 0;
    if (hdr->header_size < sizeof(struct psf2_header) || hdr->header_size > size)
        return 0;

    size_t glyph_bytes = (size_t)hdr->glyph_count * (size_t)hdr->glyph_size;
    if (hdr->glyph_count == 0 || hdr->glyph_size == 0)
        return 0;
    if (hdr->height == 0 || hdr->width == 0)
        return 0;
    if ((size_t)hdr->header_size + glyph_bytes > size)
        return 0;
    if (hdr->glyph_size % hdr->height != 0)
        return 0;
    uint32_t row_bytes = hdr->glyph_size / hdr->height;
    if (row_bytes == 0)
        return 0;
    if ((uint32_t)(row_bytes * 8) < hdr->width)
        return 0;

    out->glyph_base = buffer + hdr->header_size;
    out->stride = hdr->glyph_size;
    out->height = hdr->height;
    out->width = hdr->width;
    out->first_char = 0;
    out->glyph_count = hdr->glyph_count;
    out->lsb_left = 0;
    return 1;
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    return -1;
}

static void trim_spaces(const char **start, const char **end)
{
    const char *s = *start;
    const char *e = *end;
    while (s < e && (*s == ' ' || *s == '\t'))
        ++s;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        --e;
    *start = s;
    *end = e;
}

static int parse_int_token(const char **cursor, const char *end, int *out)
{
    const char *s = *cursor;
    while (s < end && (*s == ' ' || *s == '\t'))
        ++s;
    if (s >= end)
        return 0;

    int sign = 1;
    if (*s == '-')
    {
        sign = -1;
        ++s;
    }
    else if (*s == '+')
    {
        ++s;
    }

    if (s >= end || *s < '0' || *s > '9')
        return 0;

    int value = 0;
    while (s < end && *s >= '0' && *s <= '9')
    {
        value = value * 10 + (*s - '0');
        ++s;
    }

    *out = value * sign;
    *cursor = s;
    return 1;
}

static int parse_bdf_font(const char *buffer, size_t size, struct parsed_font *out, uint8_t **blob_out)
{
    if (!buffer || !out || !blob_out)
        return 0;

    const char *ptr = buffer;
    const char *end = buffer + size;

    uint32_t bbox_width = 0;
    uint32_t bbox_height = 0;
    int have_bbox = 0;

    uint32_t row_bytes = 0;
    uint32_t glyph_stride = 0;
    uint8_t *glyph_data = NULL;

    uint32_t glyph_capacity = 256;
    int max_encoding = -1;
    uint32_t max_dwidth = 0;

    int in_glyph = 0;
    int glyph_encoding = -1;
    uint32_t glyph_height = 0;
    int bitmap_active = 0;
    uint32_t bitmap_row = 0;
    uint8_t *glyph_target = NULL;

    while (ptr < end)
    {
        const char *line = ptr;
        const char *line_end = line;
        while (line_end < end && *line_end != '\n')
            ++line_end;

        const char *trim_start = line;
        const char *trim_end = line_end;
        trim_spaces(&trim_start, &trim_end);

        size_t trimmed_len = (trim_end > trim_start) ? (size_t)(trim_end - trim_start) : 0;

        if (trimmed_len > 0)
        {
            if (!have_bbox)
            {
                const char prefix[] = "FONTBOUNDINGBOX";
                size_t prefix_len = sizeof(prefix) - 1;
                if (trimmed_len >= prefix_len && memcmp(trim_start, prefix, prefix_len) == 0)
                {
                    const char *cursor = trim_start + prefix_len;
                    int values[4];
                    for (int i = 0; i < 4; ++i)
                    {
                        if (!parse_int_token(&cursor, trim_end, &values[i]))
                            return 0;
                    }
                    if (values[0] <= 0 || values[1] <= 0)
                        return 0;
                    bbox_width = (uint32_t)values[0];
                    bbox_height = (uint32_t)values[1];
                    (void)values[2];
                    (void)values[3];
                    row_bytes = (bbox_width + 7) / 8;
                    glyph_stride = row_bytes * bbox_height;
                    if (glyph_stride == 0)
                        return 0;
                    glyph_data = (uint8_t *)kalloc(glyph_stride * glyph_capacity);
                    if (!glyph_data)
                        return 0;
                    memset(glyph_data, 0, glyph_stride * glyph_capacity);
                    have_bbox = 1;
                }
            }
            else if (trimmed_len >= 8 && memcmp(trim_start, "STARTCHAR", 9) == 0)
            {
                in_glyph = 1;
                glyph_encoding = -1;
                glyph_height = bbox_height;
                bitmap_active = 0;
                bitmap_row = 0;
                glyph_target = NULL;
            }
            else if (in_glyph && trimmed_len >= 8 && memcmp(trim_start, "ENDCHAR", 7) == 0)
            {
                in_glyph = 0;
                bitmap_active = 0;
                glyph_target = NULL;
            }
            else if (in_glyph && trimmed_len >= 8 && memcmp(trim_start, "ENCODING", 8) == 0)
            {
                const char *cursor = trim_start + 8;
                int value = -1;
                if (parse_int_token(&cursor, trim_end, &value))
                {
                    glyph_encoding = value;
                    if (glyph_encoding >= 0 && glyph_encoding < (int)glyph_capacity)
                    {
                        glyph_target = glyph_data + (size_t)glyph_encoding * glyph_stride;
                        memset(glyph_target, 0, glyph_stride);
                        if (glyph_encoding > max_encoding)
                            max_encoding = glyph_encoding;
                    }
                    else
                    {
                        glyph_target = NULL;
                    }
                }
            }
            else if (in_glyph && trimmed_len >= 6 && memcmp(trim_start, "DWIDTH", 6) == 0)
            {
                const char *cursor = trim_start + 6;
                int value = 0;
                if (parse_int_token(&cursor, trim_end, &value) && value > 0)
                {
                    if ((uint32_t)value > max_dwidth)
                        max_dwidth = (uint32_t)value;
                }
            }
            else if (in_glyph && trimmed_len >= 3 && memcmp(trim_start, "BBX", 3) == 0)
            {
                const char *cursor = trim_start + 3;
                int values[4];
                for (int i = 0; i < 4; ++i)
                {
                    if (!parse_int_token(&cursor, trim_end, &values[i]))
                        return 0;
                }
                if (values[1] > 0)
                    glyph_height = (uint32_t)values[1];
                (void)values[2];
                (void)values[3];
            }
            else if (in_glyph && trimmed_len >= 6 && memcmp(trim_start, "BITMAP", 6) == 0)
            {
                bitmap_active = 1;
                bitmap_row = 0;
            }
            else if (bitmap_active && glyph_target && row_bytes > 0)
            {
                uint8_t row_buffer[128];
                if (row_bytes > sizeof(row_buffer))
                    return 0;
                memset(row_buffer, 0, row_bytes);

                size_t hex_pos = 0;
                size_t dst_index = 0;
                while (hex_pos + 1 < trimmed_len)
                {
                    int hi = hex_value(trim_start[hex_pos]);
                    int lo = hex_value(trim_start[hex_pos + 1]);
                    if (hi < 0 || lo < 0)
                        break;
                    if (dst_index < row_bytes)
                        row_buffer[dst_index] = (uint8_t)((hi << 4) | lo);
                    ++dst_index;
                    hex_pos += 2;
                }

                if (bitmap_row < glyph_height && bitmap_row < bbox_height)
                {
                    uint32_t target_row = bitmap_row;
                    if (target_row < bbox_height)
                    {
                        uint8_t *dst = glyph_target + target_row * row_bytes;
                        memcpy(dst, row_buffer, row_bytes);
                    }
                }

                ++bitmap_row;
                if (bitmap_row >= glyph_height)
                    bitmap_active = 0;
            }
        }

        if (line_end < end && *line_end == '\n')
            ++line_end;
        ptr = line_end;
    }

    if (!have_bbox || !glyph_data)
        return 0;

    out->glyph_base = glyph_data;
    out->stride = glyph_stride;
    out->height = bbox_height;
    uint32_t max_possible_width = row_bytes * 8;
    uint32_t resolved_width = bbox_width;
    if (max_dwidth > 0)
        resolved_width = (max_dwidth < max_possible_width) ? max_dwidth : max_possible_width;
    if (resolved_width == 0)
        resolved_width = bbox_width ? bbox_width : 1;
    if (resolved_width > max_possible_width)
        resolved_width = max_possible_width;
    out->width = resolved_width;
    out->first_char = 0;
    out->glyph_count = (max_encoding >= 0) ? (uint32_t)(max_encoding + 1) : glyph_capacity;
    if (out->glyph_count > glyph_capacity)
        out->glyph_count = glyph_capacity;
    out->lsb_left = 0;
    *blob_out = glyph_data;
    return 1;
}

static int try_use_embedded_font(void)
{
#ifndef HAVE_EMBEDDED_FONT
    return 0;
#else
    size_t size = (size_t)(EMBEDDED_FONT_END - EMBEDDED_FONT_START);
    if (size == 0)
        return 0;

    struct parsed_font candidate;
    if (parse_psf_font(EMBEDDED_FONT_START, size, &candidate))
        return adopt_font_candidate(&candidate, NULL, 0);

    uint8_t *glyph_blob = NULL;
    if (parse_bdf_font((const char *)EMBEDDED_FONT_START, size, &candidate, &glyph_blob))
    {
        if (glyph_blob && adopt_font_candidate(&candidate, glyph_blob, 0))
            return 1;
    }

    return 0;
#endif
}

int vbe_try_load_font_from_fat(void)
{
    if (!vbe_ready || font_external_blob)
        return 0;
    if (!fat16_ready())
        return 0;

    uint32_t font_size = 0;
    if (fat16_file_size(FONT_BDF_FILE_NAME, &font_size) >= 0 && font_size > 0)
    {
        uint8_t *bdf_buffer = (uint8_t *)kalloc(font_size);
        if (bdf_buffer)
        {
            size_t read_size = 0;
            int status = fat16_read_file(FONT_BDF_FILE_NAME, bdf_buffer, font_size, &read_size);
            if (status >= 0 && read_size == font_size)
            {
                struct parsed_font bdf_candidate;
                uint8_t *glyph_blob = NULL;
                if (parse_bdf_font((const char *)bdf_buffer, read_size, &bdf_candidate, &glyph_blob) && glyph_blob)
                {
                    if (adopt_font_candidate(&bdf_candidate, glyph_blob, 1))
                        return 1;
                }
            }
        }
    }

    font_size = 0;
    if (fat16_file_size(FONT_FILE_NAME, &font_size) < 0 || font_size == 0)
        return 0;

    uint8_t *psf_buffer = (uint8_t *)kalloc(font_size);
    if (!psf_buffer)
        return 0;

    size_t read_size = 0;
    int status = fat16_read_file(FONT_FILE_NAME, psf_buffer, font_size, &read_size);
    if (status < 0 || read_size != font_size)
        return 0;

    struct parsed_font psf_candidate;
    if (!parse_psf_font(psf_buffer, read_size, &psf_candidate))
        return 0;

    if (!adopt_font_candidate(&psf_candidate, psf_buffer, 1))
        return 0;

    return 1;
}

const uint8_t *vbe_font_table(void)
{
    return font_base;
}

uint32_t vbe_font_stride(void)
{
    return font_stride;
}

uint32_t vbe_font_height(void)
{
    return font_height_px;
}

uint32_t vbe_font_width(void)
{
    return font_width_px;
}

uint32_t vbe_font_row_bytes(void)
{
    return font_row_bytes;
}

uint32_t vbe_font_first_char(void)
{
    return font_first_char;
}

uint32_t vbe_font_char_count(void)
{
    return font_char_count ? font_char_count : 96;
}

int vbe_font_lsb_left(void)
{
    return font_lsb_left;
}
