#include "syslib.h"
#include "../../gui/desktop.h"
#include "../../gui/input.h"
#include "../klog.h"

static void gui_process_input(void)
{
    gui_input_event_t input;
    uint8_t buffer[sizeof(gui_input_event_t)];
    while (sys_ipc_recv(IPC_ANY_PROCESS, buffer, sizeof(buffer)) > 0)
    {
        for (size_t i = 0; i < sizeof(input); ++i)
            ((uint8_t *)&input)[i] = buffer[i];
        gui_desktop_run_input_event(&input);
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
