#ifndef GUI_COMPOSITOR_H
#define GUI_COMPOSITOR_H

#include "window.h"

void gui_compositor_init(void);
void gui_compositor_render(void);
void gui_compositor_set_cursor(int x, int y);
void gui_compositor_get_cursor(int *x, int *y);
void gui_compositor_dispatch(const gui_event_t *event);
uint32_t gui_compositor_frame_count(void);
uint32_t gui_compositor_fps(void);

#endif
