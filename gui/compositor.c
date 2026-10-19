#include "compositor.h"

#include "graphics.h"
#include "pit.h"

#define DESKTOP_COLOR 0x0018202Cu
#define WINDOW_COLOR 0x00E5E7EBu
#define WINDOW_BORDER 0x0038BDF8u
#define TITLE_COLOR 0x001F2937u
#define CURSOR_COLOR 0x00FFFFFFu

static int cursor_x;
static int cursor_y;
static uint32_t frame_count;
static uint32_t frames_since_tick;
static uint32_t frames_per_second;
static uint64_t fps_tick;
static gui_window_t *drag_window;
static gui_window_t *resize_window;
static int resize_width;
static int resize_height;

static int clamp_coordinate(int64_t value)
{
    if (value < (int64_t)-2147483648LL)
        return (int)-2147483648LL;
    if (value > (int64_t)2147483647LL)
        return (int)2147483647LL;
    return (int)value;
}

static int add_coordinate(int value, int delta)
{
    return clamp_coordinate((int64_t)value + delta);
}

static uint32_t divide_u32(uint32_t dividend, uint32_t divisor)
{
    uint32_t quotient = 0;
    uint32_t remainder = 0;

    if (divisor == 0)
        return 0;

    for (int bit = 31; bit >= 0; --bit)
    {
        remainder = (remainder << 1) | ((dividend >> bit) & 1u);
        if (remainder >= divisor)
        {
            remainder -= divisor;
            quotient |= 1u << bit;
        }
    }
    return quotient;
}

void gui_compositor_init(void)
{
    const framebuffer_t *framebuffer = gfx_framebuffer();
    cursor_x = framebuffer ? (int)framebuffer->width / 2 : 0;
    cursor_y = framebuffer ? (int)framebuffer->height / 2 : 0;
    frame_count = 0;
    frames_since_tick = 0;
    frames_per_second = 0;
    fps_tick = get_ticks();
    drag_window = NULL;
    resize_window = NULL;
}

void gui_compositor_set_cursor(int x, int y)
{
    const framebuffer_t *framebuffer = gfx_framebuffer();
    if (!framebuffer)
        return;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= (int)framebuffer->width)
        x = (int)framebuffer->width - 1;
    if (y >= (int)framebuffer->height)
        y = (int)framebuffer->height - 1;
    cursor_x = x;
    cursor_y = y;
}

void gui_compositor_get_cursor(int *x, int *y)
{
    if (x)
        *x = cursor_x;
    if (y)
        *y = cursor_y;
}

void gui_compositor_render(void)
{
    gfx_surface_t *backbuffer = gfx_backbuffer();
    gui_window_t *windows[GUI_MAX_WINDOWS];
    uint8_t rendered[GUI_MAX_WINDOWS] = { 0 };
    size_t count;
    if (!backbuffer)
        return;
    gfx_clear(DESKTOP_COLOR);
    count = gui_window_snapshot(windows, GUI_MAX_WINDOWS);
    for (size_t order = 0; order < count; ++order)
    {
        gui_window_t *selected = NULL;
        size_t selected_index = 0;
        for (size_t i = 0; i < count; ++i)
        {
            if (rendered[i] || !windows[i]->visible || windows[i]->minimized)
                continue;
            if (!selected || windows[i]->z_order < selected->z_order)
            {
                selected = windows[i];
                selected_index = i;
            }
        }
        if (!selected)
            break;
        rendered[selected_index] = 1;
        gfx_fill_rect(backbuffer, add_coordinate(selected->x, -1), add_coordinate(selected->y, -20),
                     (int)selected->width + 2, 20, TITLE_COLOR);
        gfx_fill_rect(backbuffer, add_coordinate(selected->x, (int)selected->width - 16),
                     add_coordinate(selected->y, -16), 12, 12, 0x00DC2626u);
        gfx_blit(backbuffer, selected->surface, selected->x, selected->y);
        gfx_draw_rect(backbuffer, add_coordinate(selected->x, -1), add_coordinate(selected->y, -20),
                      (int)selected->width + 2, (int)selected->height + 21, WINDOW_BORDER);
    }
    gfx_fill_rect(backbuffer, cursor_x, cursor_y, 2, 14, CURSOR_COLOR);
    gfx_fill_rect(backbuffer, cursor_x, cursor_y, 10, 2, CURSOR_COLOR);
    gfx_present();
    ++frame_count;
    ++frames_since_tick;
    uint64_t now = get_ticks();
    uint64_t elapsed_ticks = now - fps_tick;
    if (elapsed_ticks >= 250u)
    {
        uint32_t elapsed = (uint32_t)elapsed_ticks;
        uint32_t scaled_frames = frames_since_tick;
        if (scaled_frames > 0x00FFFFFFu)
            scaled_frames = 0x00FFFFFFu;
        scaled_frames *= 250u;
        frames_per_second = divide_u32(scaled_frames, elapsed);
        frames_since_tick = 0;
        fps_tick = now;
    }
}

void gui_compositor_dispatch(const gui_event_t *event)
{
    gui_window_t *windows[GUI_MAX_WINDOWS];
    size_t count = gui_window_snapshot(windows, GUI_MAX_WINDOWS);
    if (event && event->type == GUI_EVENT_MOUSE_MOVE)
    {
        gui_compositor_set_cursor(add_coordinate(cursor_x, event->x),
                                  add_coordinate(cursor_y, event->y));
        if (drag_window && (event->buttons & 1u))
            gui_window_move(drag_window, add_coordinate(drag_window->x, event->x),
                            add_coordinate(drag_window->y, event->y));
        if (resize_window && (event->buttons & 1u))
        {
            resize_width = add_coordinate(resize_width, event->x);
            resize_height = add_coordinate(resize_height, event->y);
        }
    }
    if (event && event->type == GUI_EVENT_MOUSE_BUTTON_DOWN && event->code == 1u)
    {
        gui_window_t *hit = NULL;
        for (size_t i = 0; i < count; ++i)
        {
            gui_window_t *candidate = windows[i];
            if (!candidate->visible || candidate->minimized)
                continue;
            if ((int64_t)cursor_x < (int64_t)candidate->x - 1 ||
                (int64_t)cursor_x > (int64_t)candidate->x + candidate->width + 1)
                continue;
            if ((int64_t)cursor_y < (int64_t)candidate->y - 20 ||
                (int64_t)cursor_y > (int64_t)candidate->y + candidate->height)
                continue;
            if (!hit || candidate->z_order > hit->z_order)
                hit = candidate;
        }
        drag_window = NULL;
        resize_window = NULL;
        if (hit)
        {
            gui_window_focus(hit);
            if (cursor_y < hit->y &&
                (int64_t)cursor_x >= (int64_t)hit->x + hit->width - 20)
            {
                gui_event_t close_event = { GUI_EVENT_CLOSE, 0, 0, 0, 0 };
                gui_window_dispatch(hit, &close_event);
                return;
            }
            if (cursor_y < hit->y)
                drag_window = hit;
            else if ((int64_t)cursor_x >= (int64_t)hit->x + hit->width - 18 &&
                     (int64_t)cursor_y >= (int64_t)hit->y + hit->height - 18)
            {
                resize_window = hit;
                resize_width = (int)hit->width;
                resize_height = (int)hit->height;
            }
        }
    }
    if (event && event->type == GUI_EVENT_MOUSE_BUTTON_UP && event->code == 1u)
    {
        if (resize_window)
            gui_window_resize(resize_window, (uint32_t)resize_width, (uint32_t)resize_height);
        drag_window = NULL;
        resize_window = NULL;
    }
    for (size_t i = 0; i < count; ++i)
    {
        if (windows[i]->focused)
        {
            gui_window_dispatch(windows[i], event);
            return;
        }
    }
}

uint32_t gui_compositor_frame_count(void)
{
    return frame_count;
}

uint32_t gui_compositor_fps(void)
{
    return frames_per_second;
}
