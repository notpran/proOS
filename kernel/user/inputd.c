#include "syslib.h"
#include "../keyboard.h"
#include "../service_types.h"
#include "../ipc_types.h"
#include "../cli.h"
#include "../../gui/input.h"

#define INPUT_KEY_STATE_COUNT 256

static uint8_t key_state[INPUT_KEY_STATE_COUNT];

static void reset_input_state(uint32_t *previous_buttons)
{
    for (size_t i = 0; i < INPUT_KEY_STATE_COUNT; ++i)
        key_state[i] = 0;
    if (previous_buttons)
        *previous_buttons = 0;
}

static int send_input_event(pid_t *gui_pid, const gui_input_event_t *event)
{
    if (!gui_pid || *gui_pid <= 0 || !event)
        return -1;
    if (sys_ipc_send(*gui_pid, event, sizeof(*event)) < 0)
    {
        *gui_pid = -1;
        return -1;
    }
    return 0;
}

void user_inputd(void)
{
    struct user_keyboard_event keyboard;
    struct user_mouse_event mouse;
    uint32_t previous_buttons = 0;
    pid_t gui_pid = -1;
    reset_input_state(&previous_buttons);
    for (;;)
    {
        if (cli_recovery_active())
        {
            sys_sleep(10);
            continue;
        }
        if (gui_pid <= 0)
        {
            gui_pid = sys_service_connect(SYSTEM_SERVICE_GUI, IPC_RIGHT_SEND);
            if (gui_pid > 0)
                reset_input_state(&previous_buttons);
        }
        while (sys_keyboard_poll(&keyboard) > 0)
        {
            gui_input_event_t event;
            uint32_t raw_code = keyboard.payload & 0xFFu;
            uint32_t key_code = raw_code;
            int release = (keyboard.payload & KB_EVENT_FLAG_RELEASE) != 0;
            int extended = (keyboard.payload & KB_EVENT_FLAG_EXTENDED) != 0;
            if (extended)
            {
                if (raw_code == 0x48u)
                    key_code = GUI_KEY_ARROW_UP;
                else if (raw_code == 0x50u)
                    key_code = GUI_KEY_ARROW_DOWN;
                else if (raw_code == 0x4Bu)
                    key_code = GUI_KEY_ARROW_LEFT;
                else if (raw_code == 0x4Du)
                    key_code = GUI_KEY_ARROW_RIGHT;
            }
            event.type = release ? GUI_INPUT_KEY_UP : GUI_INPUT_KEY_DOWN;
            event.x = 0;
            event.y = 0;
            event.code = key_code;
            event.buttons = 0;
            event.flags = (!release && key_state[key_code] != 0) ? GUI_INPUT_FLAG_REPEAT : 0;
            key_state[key_code] = release ? 0 : 1;
            if (send_input_event(&gui_pid, &event) < 0)
                break;
        }
        while (sys_mouse_poll(&mouse) > 0)
        {
            gui_input_event_t event;
            event.type = GUI_INPUT_MOUSE_MOVE;
            event.x = mouse.dx;
            event.y = mouse.dy;
            event.code = 0;
            event.buttons = mouse.buttons;
            event.flags = 0;
            if (send_input_event(&gui_pid, &event) < 0)
                break;
            uint32_t changed = previous_buttons ^ mouse.buttons;
            for (uint32_t button = 1u; button <= 4u; button <<= 1u)
            {
                if ((changed & button) == 0u)
                    continue;
                event.type = (mouse.buttons & button) ? GUI_INPUT_MOUSE_BUTTON_DOWN : GUI_INPUT_MOUSE_BUTTON_UP;
                event.code = button;
                if (send_input_event(&gui_pid, &event) < 0)
                    break;
            }
            previous_buttons = mouse.buttons;
            if (gui_pid <= 0)
                break;
        }
        sys_sleep(10);
    }
}
