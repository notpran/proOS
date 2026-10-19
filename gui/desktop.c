#include "desktop.h"

#include "compositor.h"
#include "graphics.h"
#include "window.h"
#include "proc.h"
#include "input.h"

static int desktop_ready;

static void paint_demo_surface(gui_window_t *window, uint32_t fill, uint32_t border, uint32_t line)
{
    if (!window || !window->surface)
        return;
    gfx_fill_rect(window->surface, 0, 0, (int)window->width, (int)window->height, fill);
    gfx_draw_rect(window->surface, 0, 0, (int)window->width, (int)window->height, border);
    gfx_draw_line(window->surface, 16, 24, (int)window->width - 24, 24, line);
}

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
    const framebuffer_t *framebuffer;
    gui_window_t *back_window;
    gui_window_t *front_window;
    gui_window_t *edge_window;
    int screen_width;
    int screen_height;
    if (desktop_ready)
        return 0;
    if (gfx_init() < 0)
        return -1;
    framebuffer = gfx_framebuffer();
    if (!framebuffer)
        return -1;
    screen_width = (int)framebuffer->width;
    screen_height = (int)framebuffer->height;
    gui_window_system_init();
    gui_compositor_init();
    back_window = gui_window_create(process_current() ? process_current()->pid : 0,
                                    screen_width / 4, screen_height / 5, 280, 160, test_app_event);
    front_window = gui_window_create(process_current() ? process_current()->pid : 0,
                                     screen_width / 3, screen_height / 4, 240, 140, test_app_event);
    edge_window = gui_window_create(process_current() ? process_current()->pid : 0,
                                    screen_width - 72, screen_height - 128, 176, 112, test_app_event);
    if (!back_window || !front_window || !edge_window)
    {
        gui_window_destroy(back_window);
        gui_window_destroy(front_window);
        gui_window_destroy(edge_window);
        return -1;
    }
    paint_demo_surface(back_window, 0x00F8FAFCu, 0x0038BDF8u, 0x002563EBu);
    paint_demo_surface(front_window, 0x00FDE68Au, 0x00D97706u, 0x00924C0Eu);
    paint_demo_surface(edge_window, 0x00A7F3D0u, 0x00047857u, 0x00065F46u);
    gui_window_show(back_window);
    gui_window_show(front_window);
    gui_window_show(edge_window);
    gui_window_focus(front_window);
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
