#ifndef GUI_SHELL_H
#define GUI_SHELL_H

#include "graphics.h"
#include "window.h"

#define GUI_SHELL_MENU_HEIGHT 26
#define GUI_SHELL_DOCK_HEIGHT 72

void gui_shell_init(void);
void gui_shell_render_background(gfx_surface_t *surface);
void gui_shell_render_overlay(gfx_surface_t *surface);
int gui_shell_dispatch(const gui_event_t *event, int cursor_x, int cursor_y);

#endif
