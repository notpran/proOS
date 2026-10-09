#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include "graphics.h"
#include "proc.h"

#define GUI_MAX_WINDOWS 16

typedef enum
{
    GUI_EVENT_NONE = 0,
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_MOUSE_BUTTON_DOWN,
    GUI_EVENT_MOUSE_BUTTON_UP,
    GUI_EVENT_MOUSE_SCROLL,
    GUI_EVENT_KEY_DOWN,
    GUI_EVENT_KEY_UP,
    GUI_EVENT_CLOSE
} gui_event_type_t;

typedef struct
{
    gui_event_type_t type;
    int32_t x;
    int32_t y;
    uint32_t code;
    uint32_t buttons;
} gui_event_t;

struct gui_window;
typedef void (*gui_event_handler_t)(struct gui_window *window, const gui_event_t *event);

typedef struct gui_window
{
    uint32_t id;
    pid_t owner_pid;
    int x;
    int y;
    uint32_t width;
    uint32_t height;
    uint32_t min_width;
    uint32_t min_height;
    uint32_t z_order;
    uint8_t visible;
    uint8_t focused;
    uint8_t minimized;
    uint8_t opacity;
    gfx_surface_t *surface;
    gui_event_handler_t handler;
} gui_window_t;

void gui_window_system_init(void);
gui_window_t *gui_window_create(pid_t owner_pid, int x, int y, uint32_t width, uint32_t height, gui_event_handler_t handler);
void gui_window_destroy(gui_window_t *window);
void gui_window_show(gui_window_t *window);
void gui_window_hide(gui_window_t *window);
void gui_window_move(gui_window_t *window, int x, int y);
void gui_window_resize(gui_window_t *window, uint32_t width, uint32_t height);
void gui_window_focus(gui_window_t *window);
void gui_window_minimize(gui_window_t *window);
void gui_window_restore(gui_window_t *window);
void gui_window_dispatch(gui_window_t *window, const gui_event_t *event);
void gui_window_destroy_owner(pid_t owner_pid);
size_t gui_window_snapshot(gui_window_t **out, size_t max_windows);
size_t gui_window_count(void);

#endif
