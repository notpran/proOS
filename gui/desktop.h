#ifndef GUI_DESKTOP_H
#define GUI_DESKTOP_H

#include "input.h"

int gui_desktop_init(void);
int gui_desktop_show(void);
int gui_desktop_ready(void);
int gui_desktop_run_input_event(const gui_input_event_t *input);
int gui_desktop_smoke_status(void);

#endif
