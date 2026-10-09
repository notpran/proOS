#include "syslib.h"
#include "../keyboard.h"
#include "../service_types.h"
#include "../ipc_types.h"
#include "../../gui/input.h"

static void send_input_event(pid_t gui_pid, const gui_input_event_t *event)
{
    if (gui_pid > 0 && event)
        (void)sys_ipc_send(gui_pid, event, sizeof(*event));
}

void user_inputd(void)
{
    struct user_keyboard_event keyboard;
    struct user_mouse_event mouse;
    uint32_t previous_buttons = 0;
    pid_t gui_pid = -1;
    for (;;)
    {
        if (gui_pid <= 0)
            gui_pid = sys_service_connect(SYSTEM_SERVICE_GUI, IPC_RIGHT_SEND);
        while (sys_keyboard_poll(&keyboard) > 0)
        {
            gui_input_event_t event;
            event.type = (keyboard.payload & KB_EVENT_FLAG_RELEASE) ? GUI_INPUT_KEY_UP : GUI_INPUT_KEY_DOWN;
            event.x = 0;
            event.y = 0;
            event.code = keyboard.payload & 0xFFu;
            event.buttons = 0;
            if (keyboard.ch >= (uint8_t)GUI_KEY_ARROW_UP && keyboard.ch <= (uint8_t)GUI_KEY_ARROW_RIGHT)
                event.code = keyboard.ch;
            send_input_event(gui_pid, &event);
        }
        while (sys_mouse_poll(&mouse) > 0)
        {
            gui_input_event_t event;
            event.type = GUI_INPUT_MOUSE_MOVE;
            event.x = mouse.dx;
            event.y = mouse.dy;
            event.code = 0;
            event.buttons = mouse.buttons;
            send_input_event(gui_pid, &event);
            uint32_t changed = previous_buttons ^ mouse.buttons;
            for (uint32_t button = 1u; button <= 4u; button <<= 1u)
            {
                if ((changed & button) == 0u)
                    continue;
                event.type = (mouse.buttons & button) ? GUI_INPUT_MOUSE_BUTTON_DOWN : GUI_INPUT_MOUSE_BUTTON_UP;
                event.code = button;
                send_input_event(gui_pid, &event);
            }
            previous_buttons = mouse.buttons;
        }
        sys_sleep(10);
    }
}
