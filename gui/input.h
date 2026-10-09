#ifndef GUI_INPUT_H
#define GUI_INPUT_H

#include <stdint.h>

typedef enum
{
    GUI_INPUT_NONE = 0,
    GUI_INPUT_MOUSE_MOVE,
    GUI_INPUT_MOUSE_BUTTON_DOWN,
    GUI_INPUT_MOUSE_BUTTON_UP,
    GUI_INPUT_MOUSE_SCROLL,
    GUI_INPUT_KEY_DOWN,
    GUI_INPUT_KEY_UP
} gui_input_type_t;

typedef struct
{
    gui_input_type_t type;
    int32_t x;
    int32_t y;
    uint32_t code;
    uint32_t buttons;
} gui_input_event_t;

#define GUI_INPUT_IPC_TYPE 0x47554901u

#define GUI_KEY_ARROW_UP 0x80u
#define GUI_KEY_ARROW_DOWN 0x81u
#define GUI_KEY_ARROW_LEFT 0x82u
#define GUI_KEY_ARROW_RIGHT 0x83u
#define GUI_KEY_ESCAPE 0x01u

#endif
