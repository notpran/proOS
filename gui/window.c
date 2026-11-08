#include "window.h"

#include "memory.h"

static gui_window_t windows[GUI_MAX_WINDOWS];
static uint32_t next_window_id = 1;
static uint32_t next_z_order;

#define WINDOW_MIN_VISIBLE_WIDTH 32
#define WINDOW_MIN_VISIBLE_HEIGHT 24

static void focus_top_window(void)
{
    gui_window_t *top = NULL;
    for (size_t i = 0; i < GUI_MAX_WINDOWS; ++i)
    {
        windows[i].focused = 0;
        if (windows[i].id == 0 || !windows[i].visible || windows[i].minimized)
            continue;
        if (!top || windows[i].z_order > top->z_order)
            top = &windows[i];
    }
    if (top)
        top->focused = 1;
}

static void constrain_position(gui_window_t *window, int *x, int *y)
{
    const framebuffer_t *framebuffer = gfx_framebuffer();
    int64_t min_x;
    int64_t max_x;
    int64_t min_y;
    int64_t max_y;
    if (!window || !x || !y || !framebuffer || framebuffer->width == 0 || framebuffer->height == 0)
        return;

    min_x = -(int64_t)window->width + WINDOW_MIN_VISIBLE_WIDTH;
    max_x = (int64_t)framebuffer->width - WINDOW_MIN_VISIBLE_WIDTH;
    min_y = -(int64_t)window->height + WINDOW_MIN_VISIBLE_HEIGHT;
    max_y = (int64_t)framebuffer->height - WINDOW_MIN_VISIBLE_HEIGHT;
    if (*x < min_x)
        *x = (int)min_x;
    else if (*x > max_x)
        *x = (int)max_x;
    if (*y < min_y)
        *y = (int)min_y;
    else if (*y > max_y)
        *y = (int)max_y;
}

static void constrain_size(uint32_t *width, uint32_t *height, uint32_t min_width, uint32_t min_height)
{
    const framebuffer_t *framebuffer = gfx_framebuffer();
    if (!width || !height)
        return;
    if (*width < min_width)
        *width = min_width;
    if (*height < min_height)
        *height = min_height;
    if (!framebuffer)
        return;
    if (framebuffer->width >= min_width && *width > framebuffer->width)
        *width = framebuffer->width;
    if (framebuffer->height >= min_height && *height > framebuffer->height)
        *height = framebuffer->height;
}

void gui_window_system_init(void)
{
    for (size_t i = 0; i < GUI_MAX_WINDOWS; ++i)
    {
        if (windows[i].id != 0)
            gfx_surface_destroy(windows[i].surface);
        windows[i].id = 0;
        windows[i].surface = NULL;
        windows[i].focused = 0;
    }
    next_window_id = 1;
    next_z_order = 0;
}

gui_window_t *gui_window_create(pid_t owner_pid, int x, int y, uint32_t width, uint32_t height, gui_event_handler_t handler)
{
    if (width == 0 || height == 0 || width > 0x3FFFFFFFu || height > 0x3FFFFFFFu)
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
        constrain_position(&windows[i], &windows[i].x, &windows[i].y);
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
    if (window->focused)
        window->focused = 0;
    window->visible = 0;
    window->minimized = 0;
    window->handler = NULL;
    window->focused = 0;
    window->id = 0;
    focus_top_window();
}

void gui_window_show(gui_window_t *window)
{
    if (window && window->id != 0)
        window->visible = 1;
}

void gui_window_hide(gui_window_t *window)
{
    if (window && window->id != 0)
    {
        window->visible = 0;
        if (window->focused)
            focus_top_window();
    }
}

void gui_window_move(gui_window_t *window, int x, int y)
{
    if (window && window->id != 0)
    {
        constrain_position(window, &x, &y);
        window->x = x;
        window->y = y;
    }
}

void gui_window_resize(gui_window_t *window, uint32_t width, uint32_t height)
{
    if (!window || window->id == 0 || width > 0x3FFFFFFFu || height > 0x3FFFFFFFu)
        return;
    constrain_size(&width, &height, window->min_width, window->min_height);
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
    if (!window || window->id == 0 || !window->visible || window->minimized)
        return;
    for (size_t i = 0; i < GUI_MAX_WINDOWS; ++i)
        windows[i].focused = 0;
    window->focused = 1;
    window->z_order = ++next_z_order;
}

void gui_window_minimize(gui_window_t *window)
{
    if (window && window->id != 0)
    {
        window->minimized = 1;
        if (window->focused)
            focus_top_window();
    }
}

void gui_window_restore(gui_window_t *window)
{
    if (window && window->id != 0)
    {
        window->minimized = 0;
        if (window->visible)
            gui_window_focus(window);
    }
}

void gui_window_dispatch(gui_window_t *window, const gui_event_t *event)
{
    if (window && window->id != 0 && window->visible && !window->minimized && window->handler)
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
