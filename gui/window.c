#include "window.h"

#include "memory.h"

static gui_window_t windows[GUI_MAX_WINDOWS];
static uint32_t next_window_id = 1;
static uint32_t next_z_order;

void gui_window_system_init(void)
{
    for (size_t i = 0; i < GUI_MAX_WINDOWS; ++i)
        windows[i].id = 0;
    next_window_id = 1;
    next_z_order = 0;
}

gui_window_t *gui_window_create(pid_t owner_pid, int x, int y, uint32_t width, uint32_t height, gui_event_handler_t handler)
{
    if (width == 0 || height == 0)
        return NULL;
    for (size_t i = 0; i < GUI_MAX_WINDOWS; ++i)
    {
        if (windows[i].id != 0)
            continue;
        gfx_surface_t *surface = gfx_surface_create(width, height);
        if (!surface)
            return NULL;
        windows[i].id = next_window_id++;
        windows[i].owner_pid = owner_pid;
        windows[i].x = x;
        windows[i].y = y;
        windows[i].width = width;
        windows[i].height = height;
        windows[i].min_width = 64;
        windows[i].min_height = 48;
        windows[i].z_order = ++next_z_order;
        windows[i].visible = 0;
        windows[i].focused = 0;
        windows[i].minimized = 0;
        windows[i].opacity = 255;
        windows[i].surface = surface;
        windows[i].handler = handler;
        return &windows[i];
    }
    return NULL;
}

void gui_window_destroy(gui_window_t *window)
{
    if (!window || window->id == 0)
        return;
    gfx_surface_destroy(window->surface);
    window->surface = NULL;
    window->id = 0;
}

void gui_window_show(gui_window_t *window)
{
    if (window && window->id != 0)
        window->visible = 1;
}

void gui_window_hide(gui_window_t *window)
{
    if (window && window->id != 0)
        window->visible = 0;
}

void gui_window_move(gui_window_t *window, int x, int y)
{
    if (window && window->id != 0)
    {
        window->x = x;
        window->y = y;
    }
}

void gui_window_resize(gui_window_t *window, uint32_t width, uint32_t height)
{
    if (!window || window->id == 0)
        return;
    if (width < window->min_width)
        width = window->min_width;
    if (height < window->min_height)
        height = window->min_height;
    if (width == window->width && height == window->height)
        return;
    gfx_surface_t *surface = gfx_surface_create(width, height);
    if (!surface)
        return;
    gfx_surface_destroy(window->surface);
    window->surface = surface;
    window->width = width;
    window->height = height;
}

void gui_window_focus(gui_window_t *window)
{
    if (!window || window->id == 0)
        return;
    for (size_t i = 0; i < GUI_MAX_WINDOWS; ++i)
        windows[i].focused = 0;
    window->focused = 1;
    window->z_order = ++next_z_order;
}

void gui_window_minimize(gui_window_t *window)
{
    if (window && window->id != 0)
        window->minimized = 1;
}

void gui_window_restore(gui_window_t *window)
{
    if (window && window->id != 0)
        window->minimized = 0;
}

void gui_window_dispatch(gui_window_t *window, const gui_event_t *event)
{
    if (window && window->id != 0 && window->handler)
        window->handler(window, event);
}

void gui_window_destroy_owner(pid_t owner_pid)
{
    for (size_t i = 0; i < GUI_MAX_WINDOWS; ++i)
    {
        if (windows[i].id != 0 && windows[i].owner_pid == owner_pid)
            gui_window_destroy(&windows[i]);
    }
}

size_t gui_window_snapshot(gui_window_t **out, size_t max_windows)
{
    size_t count = 0;
    if (!out)
        return 0;
    for (size_t i = 0; i < GUI_MAX_WINDOWS && count < max_windows; ++i)
    {
        if (windows[i].id != 0)
            out[count++] = &windows[i];
    }
    return count;
}

size_t gui_window_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < GUI_MAX_WINDOWS; ++i)
        if (windows[i].id != 0)
            ++count;
    return count;
}
