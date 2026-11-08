#include "shell.h"

#include "pit.h"

#include <stddef.h>
#include <stdint.h>

#define SHELL_MENU_COLOR 0x00151B2Bu
#define SHELL_MENU_EDGE 0x00304763u
#define SHELL_MENU_TEXT 0x00E8F0F7u
#define SHELL_MENU_MUTED 0x0096A9B9u
#define SHELL_MENU_PANEL 0x00232F45u
#define SHELL_WALLPAPER_TOP 0x001B3148u
#define SHELL_WALLPAPER_BOTTOM 0x000B1728u
#define SHELL_WALLPAPER_LINE 0x00244D65u
#define SHELL_DOCK_COLOR 0x00202B3Au
#define SHELL_DOCK_EDGE 0x005B7890u
#define SHELL_DOCK_ICON 0x00D5E7F2u
#define SHELL_DOCK_HOVER 0x00F2B84Bu
#define SHELL_DOCK_SELECTED 0x0058C4B6u
#define SHELL_DOCK_ITEMS 5
#define SHELL_DOCK_ITEM_WIDTH 48
#define SHELL_DOCK_GAP 6

static int shell_ready;
static int menu_open;
static int menu_hover;
static int dock_hover = -1;
static int dock_selected = -1;

static const uint8_t *glyph_for(char value)
{
    static const uint8_t blank[5] = { 0, 0, 0, 0, 0 };
    static const uint8_t digits[10][5] = {
        { 0x1E, 0x21, 0x21, 0x21, 0x1E },
        { 0x08, 0x18, 0x08, 0x08, 0x1C },
        { 0x1E, 0x01, 0x1E, 0x20, 0x3F },
        { 0x1E, 0x01, 0x0E, 0x01, 0x1E },
        { 0x06, 0x0A, 0x12, 0x3F, 0x02 },
        { 0x3F, 0x20, 0x3E, 0x01, 0x3E },
        { 0x0E, 0x20, 0x3E, 0x21, 0x1E },
        { 0x3F, 0x01, 0x02, 0x04, 0x04 },
        { 0x1E, 0x21, 0x1E, 0x21, 0x1E },
        { 0x1E, 0x21, 0x1F, 0x01, 0x1E }
    };
    static const uint8_t letters[26][5] = {
        { 0x1E, 0x21, 0x3F, 0x21, 0x21 },
        { 0x3E, 0x21, 0x3E, 0x21, 0x3E },
        { 0x1E, 0x21, 0x20, 0x21, 0x1E },
        { 0x3E, 0x21, 0x21, 0x21, 0x3E },
        { 0x3F, 0x20, 0x3E, 0x20, 0x3F },
        { 0x3F, 0x20, 0x3E, 0x20, 0x20 },
        { 0x1E, 0x20, 0x27, 0x21, 0x1E },
        { 0x21, 0x21, 0x3F, 0x21, 0x21 },
        { 0x1C, 0x08, 0x08, 0x08, 0x1C },
        { 0x07, 0x02, 0x02, 0x22, 0x1C },
        { 0x21, 0x22, 0x3C, 0x22, 0x21 },
        { 0x20, 0x20, 0x20, 0x20, 0x3F },
        { 0x21, 0x33, 0x2D, 0x21, 0x21 },
        { 0x21, 0x31, 0x29, 0x25, 0x23 },
        { 0x1E, 0x21, 0x21, 0x21, 0x1E },
        { 0x3E, 0x21, 0x3E, 0x20, 0x20 },
        { 0x1E, 0x21, 0x21, 0x25, 0x1A },
        { 0x3E, 0x21, 0x3E, 0x22, 0x21 },
        { 0x1F, 0x20, 0x1E, 0x01, 0x3E },
        { 0x3F, 0x08, 0x08, 0x08, 0x08 },
        { 0x21, 0x21, 0x21, 0x21, 0x1E },
        { 0x21, 0x21, 0x21, 0x12, 0x0C },
        { 0x21, 0x21, 0x2D, 0x33, 0x21 },
        { 0x21, 0x12, 0x0C, 0x12, 0x21 },
        { 0x21, 0x12, 0x0C, 0x08, 0x08 },
        { 0x3F, 0x02, 0x0C, 0x10, 0x3F }
    };
    static const uint8_t colon[5] = { 0x00, 0x0C, 0x00, 0x0C, 0x00 };
    static const uint8_t dash[5] = { 0x00, 0x00, 0x1E, 0x00, 0x00 };
    static const uint8_t question[5] = { 0x1E, 0x01, 0x06, 0x00, 0x04 };

    if (value >= 'a' && value <= 'z')
        value = (char)(value - 'a' + 'A');
    if (value >= '0' && value <= '9')
        return digits[value - '0'];
    if (value >= 'A' && value <= 'Z')
        return letters[value - 'A'];
    if (value == ':')
        return colon;
    if (value == '-')
        return dash;
    if (value == '?')
        return question;
    return blank;
}

static void draw_text(gfx_surface_t *surface, int x, int y, const char *text, uint32_t color)
{
    if (!surface || !text)
        return;
    while (*text)
    {
        const uint8_t *glyph = glyph_for(*text++);
        for (int row = 0; row < 5; ++row)
        {
            for (int col = 0; col < 5; ++col)
            {
                if (glyph[row] & (uint8_t)(1u << (4 - col)))
                    gfx_put_pixel(surface, x + col, y + row, color);
            }
        }
        x += 7;
    }
}

static void draw_icon(gfx_surface_t *surface, int x, int y, int item, uint32_t color)
{
    gfx_fill_rect(surface, x, y, 28, 28, color);
    gfx_draw_rect(surface, x, y, 28, 28, SHELL_DOCK_EDGE);
    if (item == 0)
    {
        gfx_fill_rect(surface, x + 7, y + 7, 14, 14, SHELL_MENU_COLOR);
        gfx_draw_rect(surface, x + 10, y + 10, 8, 8, color);
    }
    else if (item == 1)
    {
        gfx_draw_line(surface, x + 6, y + 21, x + 21, y + 6, SHELL_MENU_COLOR);
        gfx_draw_line(surface, x + 7, y + 7, x + 21, y + 21, SHELL_MENU_COLOR);
    }
    else if (item == 2)
    {
        gfx_fill_rect(surface, x + 6, y + 6, 16, 5, SHELL_MENU_COLOR);
        gfx_fill_rect(surface, x + 6, y + 15, 16, 7, SHELL_MENU_COLOR);
    }
    else if (item == 3)
    {
        gfx_draw_rect(surface, x + 6, y + 6, 16, 16, SHELL_MENU_COLOR);
        gfx_fill_rect(surface, x + 10, y + 10, 8, 8, SHELL_MENU_COLOR);
    }
    else
    {
        gfx_draw_line(surface, x + 7, y + 21, x + 14, y + 7, SHELL_MENU_COLOR);
        gfx_draw_line(surface, x + 14, y + 7, x + 21, y + 21, SHELL_MENU_COLOR);
    }
}

static void dock_geometry(const gfx_surface_t *surface, int *x, int *y, int *width)
{
    *width = SHELL_DOCK_ITEMS * SHELL_DOCK_ITEM_WIDTH + (SHELL_DOCK_ITEMS - 1) * SHELL_DOCK_GAP;
    *x = ((int)surface->width - *width) / 2;
    *y = (int)surface->height - GUI_SHELL_DOCK_HEIGHT;
}

void gui_shell_init(void)
{
    shell_ready = 1;
    menu_open = 0;
    menu_hover = 0;
    dock_hover = -1;
    dock_selected = -1;
}

void gui_shell_render_background(gfx_surface_t *surface)
{
    if (!surface || !surface->pixels)
        return;
    gfx_fill_rect(surface, 0, 0, (int)surface->width, (int)surface->height, SHELL_WALLPAPER_BOTTOM);
    for (int band = 0; band < 8; ++band)
    {
        int y = band * (int)surface->height / 8;
        int height = (int)surface->height / 8 + 1;
        uint32_t color = SHELL_WALLPAPER_TOP - (uint32_t)band * 0x00020408u;
        gfx_fill_rect(surface, 0, y, (int)surface->width, height, color);
    }
    for (int i = 0; i < 6; ++i)
    {
        int y = (int)surface->height / 3 + i * 18;
        gfx_draw_line(surface, 0, y, (int)surface->width, y - 34, SHELL_WALLPAPER_LINE);
    }
}

void gui_shell_render_overlay(gfx_surface_t *surface)
{
    int dock_x;
    int dock_y;
    int dock_width;
    uint64_t ticks;
    char clock_text[16];
    if (!surface || !surface->pixels)
        return;
    gfx_fill_rect(surface, 0, 0, (int)surface->width, GUI_SHELL_MENU_HEIGHT, SHELL_MENU_COLOR);
    gfx_fill_rect(surface, 0, GUI_SHELL_MENU_HEIGHT - 1, (int)surface->width, 1, SHELL_MENU_EDGE);
    draw_text(surface, 12, 9, "PROOS", SHELL_MENU_TEXT);
    draw_text(surface, 58, 9, "FILE", SHELL_MENU_MUTED);
    draw_text(surface, 91, 9, "VIEW", SHELL_MENU_MUTED);
    draw_text(surface, 124, 9, "HELP", SHELL_MENU_MUTED);
    ticks = get_ticks();
    clock_text[0] = 'U';
    clock_text[1] = 'P';
    clock_text[2] = ':';
    clock_text[3] = ' ';
    clock_text[4] = (char)('0' + (ticks / 1000u) % 10u);
    clock_text[5] = (char)('0' + (ticks / 100u) % 10u);
    clock_text[6] = (char)('0' + (ticks / 10u) % 10u);
    clock_text[7] = (char)('0' + ticks % 10u);
    clock_text[8] = '\0';
    draw_text(surface, (int)surface->width - 72, 9, "SYS", SHELL_MENU_MUTED);
    draw_text(surface, (int)surface->width - 38, 9, clock_text, SHELL_MENU_TEXT);

    dock_geometry(surface, &dock_x, &dock_y, &dock_width);
    gfx_fill_rect(surface, dock_x - 8, dock_y, dock_width + 16, 58, SHELL_DOCK_COLOR);
    gfx_draw_rect(surface, dock_x - 8, dock_y, dock_width + 16, 58, SHELL_DOCK_EDGE);
    for (int item = 0; item < SHELL_DOCK_ITEMS; ++item)
    {
        int item_x = dock_x + item * (SHELL_DOCK_ITEM_WIDTH + SHELL_DOCK_GAP) + 10;
        uint32_t color = item == dock_selected ? SHELL_DOCK_SELECTED : SHELL_DOCK_ICON;
        if (item == dock_hover)
            color = SHELL_DOCK_HOVER;
        draw_icon(surface, item_x, dock_y + 14, item, color);
    }

    if (menu_open)
    {
        gfx_fill_rect(surface, 8, GUI_SHELL_MENU_HEIGHT, 176, 76, SHELL_MENU_PANEL);
        gfx_draw_rect(surface, 8, GUI_SHELL_MENU_HEIGHT, 176, 76, SHELL_MENU_EDGE);
        draw_text(surface, 20, GUI_SHELL_MENU_HEIGHT + 12, "ABOUT", SHELL_MENU_TEXT);
        draw_text(surface, 20, GUI_SHELL_MENU_HEIGHT + 32, "LOGOUT", SHELL_MENU_MUTED);
        draw_text(surface, 20, GUI_SHELL_MENU_HEIGHT + 52, "SHUTDOWN", SHELL_MENU_MUTED);
    }
}

int gui_shell_dispatch(const gui_event_t *event, int cursor_x, int cursor_y)
{
    const framebuffer_t *framebuffer = gfx_framebuffer();
    int dock_height;
    int dock_x;
    int dock_y;
    int dock_width;
    if (!shell_ready || !event || !framebuffer)
        return 0;
    dock_width = SHELL_DOCK_ITEMS * SHELL_DOCK_ITEM_WIDTH +
                 (SHELL_DOCK_ITEMS - 1) * SHELL_DOCK_GAP;
    dock_x = ((int)framebuffer->width - dock_width) / 2;
    dock_height = GUI_SHELL_DOCK_HEIGHT;
    dock_y = (int)framebuffer->height - dock_height;
    if (event->type == GUI_EVENT_MOUSE_MOVE)
    {
        dock_hover = -1;
        if (cursor_y >= dock_y && cursor_y < dock_y + 58 &&
            cursor_x >= dock_x && cursor_x < dock_x + dock_width)
        {
            int item = (cursor_x - dock_x) / (SHELL_DOCK_ITEM_WIDTH + SHELL_DOCK_GAP);
            if (item >= 0 && item < SHELL_DOCK_ITEMS)
                dock_hover = item;
        }
        menu_hover = menu_open && cursor_x >= 8 && cursor_x < 184 &&
                     cursor_y >= GUI_SHELL_MENU_HEIGHT && cursor_y < GUI_SHELL_MENU_HEIGHT + 76;
        return 0;
    }
    if (event->type != GUI_EVENT_MOUSE_BUTTON_DOWN || event->code != 1u)
        return 0;
    if (menu_open)
    {
        if (!menu_hover)
        {
            menu_open = 0;
            return 0;
        }
        menu_open = 0;
        return 1;
    }
    if (cursor_y < GUI_SHELL_MENU_HEIGHT && cursor_x < 52)
    {
        menu_open = 1;
        return 1;
    }
    if (cursor_y >= dock_y && cursor_y < dock_y + 58 &&
        cursor_x >= dock_x && cursor_x < dock_x + dock_width)
    {
        int item = (cursor_x - dock_x) / (SHELL_DOCK_ITEM_WIDTH + SHELL_DOCK_GAP);
        if (item >= 0 && item < SHELL_DOCK_ITEMS)
            dock_selected = item;
        return 1;
    }
    return 0;
}
