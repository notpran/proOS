#include "desktop.h"

#include "compositor.h"
#include "graphics.h"
#include "window.h"
#include "proc.h"
#include "input.h"

static int desktop_ready;

static void test_app_event(struct gui_window *window, const gui_event_t *event)
{
    if (!window || !event)
        return;
    if (event->type == GUI_EVENT_CLOSE)
        gui_window_destroy(window);
    else if (event->type == GUI_EVENT_KEY_DOWN && event->code == GUI_KEY_ARROW_LEFT)
        gui_window_move(window, window->x - 8, window->y);
    else if (event->type == GUI_EVENT_KEY_DOWN && event->code == GUI_KEY_ARROW_RIGHT)
        gui_window_move(window, window->x + 8, window->y);
    else if (event->type == GUI_EVENT_KEY_DOWN && event->code == GUI_KEY_ARROW_UP)
        gui_window_move(window, window->x, window->y - 8);
    else if (event->type == GUI_EVENT_KEY_DOWN && event->code == GUI_KEY_ARROW_DOWN)
        gui_window_move(window, window->x, window->y + 8);
}

int gui_desktop_init(void)
{
    gui_window_t *test_window;
    if (desktop_ready)
        return 0;
    if (gfx_init() < 0)
        return -1;
    gui_window_system_init();
    gui_compositor_init();
    test_window = gui_window_create(process_current() ? process_current()->pid : 0, 96, 72, 360, 220, test_app_event);
    if (!test_window)
        return -1;
    gfx_fill_rect(test_window->surface, 0, 0, (int)test_window->width, (int)test_window->height, 0x00F8FAFCu);
    gfx_draw_rect(test_window->surface, 0, 0, (int)test_window->width, (int)test_window->height, 0x0038BDF8u);
    gfx_draw_line(test_window->surface, 20, 30, 180, 30, 0x002563EBu);
    gui_window_show(test_window);
    gui_window_focus(test_window);
    desktop_ready = 1;
    return 0;
}

int gui_desktop_show(void)
{
    if (gui_desktop_init() < 0)
        return -1;
    gui_compositor_render();
    return 0;
}

int gui_desktop_ready(void)
{
    return desktop_ready;
}

int gui_desktop_run_input_event(const gui_input_event_t *input)
{
    gui_event_t event;
    if (!input)
        return -1;
    event.type = (gui_event_type_t)input->type;
    event.x = input->x;
    event.y = input->y;
    event.code = input->code;
    event.buttons = input->buttons;
    gui_compositor_dispatch(&event);
    return 0;
}

int gui_desktop_smoke_status(void)
{
    return desktop_ready && gui_window_count() > 0 && gui_compositor_frame_count() > 0;
}
