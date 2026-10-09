#include "syslib.h"
#include "../keyboard.h"
#include "../../gui/desktop.h"
#include "../../gui/input.h"
#include "../../gui/compositor.h"
#include "../klog.h"

static void gui_process_input(void)
{
    struct user_keyboard_event keyboard;
    struct user_mouse_event mouse;
    static uint32_t previous_buttons;
    while (sys_keyboard_poll(&keyboard) > 0)
    {
        gui_event_t event;
        event.type = (keyboard.payload & KB_EVENT_FLAG_RELEASE) ? GUI_EVENT_KEY_UP : GUI_EVENT_KEY_DOWN;
        event.x = 0;
        event.y = 0;
        event.code = keyboard.payload & 0xFFu;
        event.buttons = 0;
        if (keyboard.ch == (uint8_t)GUI_KEY_ARROW_LEFT)
            event.code = GUI_KEY_ARROW_LEFT;
        else if (keyboard.ch == (uint8_t)GUI_KEY_ARROW_RIGHT)
            event.code = GUI_KEY_ARROW_RIGHT;
        else if (keyboard.ch == (uint8_t)GUI_KEY_ARROW_UP)
            event.code = GUI_KEY_ARROW_UP;
        else if (keyboard.ch == (uint8_t)GUI_KEY_ARROW_DOWN)
            event.code = GUI_KEY_ARROW_DOWN;
        gui_compositor_dispatch(&event);
    }
    while (sys_mouse_poll(&mouse) > 0)
    {
        gui_event_t event;
        event.type = GUI_EVENT_MOUSE_MOVE;
        event.x = mouse.dx;
        event.y = mouse.dy;
        event.code = 0;
        event.buttons = mouse.buttons;
        gui_compositor_dispatch(&event);
        uint32_t changed = previous_buttons ^ mouse.buttons;
        for (uint32_t button = 1u; button <= 4u; button <<= 1u)
        {
            if ((changed & button) == 0u)
                continue;
            event.type = (mouse.buttons & button) ? GUI_EVENT_MOUSE_BUTTON_DOWN : GUI_EVENT_MOUSE_BUTTON_UP;
            event.code = button;
            gui_compositor_dispatch(&event);
        }
        previous_buttons = mouse.buttons;
    }
}

void user_gui(void)
{
    if (gui_desktop_init() < 0)
    {
        klog_error("gui: desktop initialization failed");
        sys_exit(1);
        return;
    }

    for (;;)
    {
        gui_process_input();
        gui_desktop_show();
        sys_sleep(4);
    }
}
