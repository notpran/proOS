# proOS GUI Architecture

## Scope and status

This document describes the current GUI foundation through Phase 10.5. Runtime and emulator results must be recorded separately from source-level verification. The CLI remains a kernel thread and is not replaced by the GUI.

## Startup and framebuffer

`boot/stage2.asm` selects a 32-bpp linear VBE mode, records framebuffer address, pitch, dimensions, size, and RGB masks in `boot_info` at `BOOT_INFO_ADDR`, then enters protected mode. `kernel/vbe.c` validates the handoff, framebuffer footprint, pitch alignment, channel masks, and overlap with the boot-loaded FAT image and kernel heap. An unsafe low framebuffer mapping is rejected and the CLI fallback remains available. The framebuffer is physical/BIOS-owned and is never freed by GUI code.

`gui/graphics.c` allocates one kernel-heap backbuffer using `gfx_surface_create`-style ownership rules. GUI pixels use canonical `0x00RRGGBB` values; `gfx_present` either copies them directly for the validated native layout or packs them for the selected VBE channel layout.

During normal boot, `kmain` initializes the framebuffer first and starts the registered services. The existing `user_gui` service then calls `gui_desktop_init` and continuously calls `gui_desktop_show` in its scheduled process context; no second desktop implementation is created. `kmain` only checks that the GUI service was created successfully.

## Rendering surfaces

`gfx_surface_t` contains a pixel pointer, width, height, and pixel stride. Surfaces created by `gfx_surface_create` own their pixel allocation and must be released with `gfx_surface_destroy`. The physical framebuffer is not a `gfx_surface_t` and must not be destroyed.

Drawing APIs include `gfx_put_pixel`, `gfx_fill_rect`, `gfx_draw_line`, `gfx_draw_rect`, `gfx_blit`, `gfx_clear`, and `gfx_present`. Operations validate strides, clip rectangles, and use overflow-safe coordinate arithmetic. Blits are clipped copies; overlapping source and destination surfaces do not have memmove semantics.

## Window manager

`gui_window_t` separates window metadata from its owned pixel surface. Metadata includes:

- unique numeric ID;
- owner PID;
- position and dimensions;
- minimum dimensions;
- visibility, focus, minimized, and opacity state;
- z-order;
- event handler;
- owned `gfx_surface_t`.

The fixed registry is limited by `GUI_MAX_WINDOWS` (16). `gui_window_create` allocates a surface and initializes a hidden window. Creation rejects zero or oversized dimensions and constrains its position so part of the window remains visible when a framebuffer is available.

Lifecycle operations:

- `gui_window_show` and `gui_window_hide` control visibility;
- `gui_window_focus` clears other focus state and raises z-order;
- `gui_window_minimize` hides a window from rendering and clears focus;
- `gui_window_restore` unminimizes and focuses a visible window;
- `gui_window_move` applies screen-aware position constraints;
- `gui_window_resize` enforces minimum and framebuffer-aware maximum dimensions, then replaces the owned surface only after successful allocation;
- `gui_window_destroy` releases the surface, clears callbacks and state, invalidates the ID, and focuses the highest-z visible window when appropriate;
- `gui_window_destroy_owner` cleans up all windows belonging to a process.

Reinitializing the window system releases any existing registered surfaces before resetting the registry.

## Compositor and interaction policy

`gui_compositor.c` snapshots the window registry, renders visible non-minimized windows in deterministic back-to-front z-order, clips each surface through `gfx_blit`, draws simple title-bar chrome, draws the cursor last, and presents the backbuffer. It currently performs a full-frame redraw, which removes cursor trails without modifying underlying surfaces.

## Desktop shell

`gui/shell.c` provides the Phase 10.6 shell layer without replacing the compositor. The render order is:

1. procedural wallpaper background;
2. visible application/test windows and title chrome;
3. top menu bar and floating dock overlays;
4. software arrow cursor;
5. framebuffer presentation.

The shell uses actual framebuffer dimensions and existing drawing primitives. It provides a dark top bar with `PROOS`, `FILE`, `VIEW`, and `HELP` labels, a system indicator, and an `UP:` uptime value derived from PIT ticks. It does not claim wall-clock or RTC support. The dock has five placeholder icons with hover and selection feedback. Dock selection does not launch applications because no application-launch service exists yet. The left menu opens a small functional menu panel; its current entries are visual-only and do not perform unsupported actions.

Shell input is routed before window hit testing for left-button menu and dock clicks. Mouse movement updates dock hover state. The shell remains independent of window ownership and does not add a second compositor.

Pointer policy:

1. Cursor deltas are applied and clamped to framebuffer bounds.
2. A primary-button press hit-tests visible, non-minimized windows from highest z-order down.
3. A hit window is focused and raised.
4. The title bar begins a drag; the yellow title-bar control minimizes; the lower-right corner begins a resize; the close control dispatches `GUI_EVENT_CLOSE`.
5. Mouse release commits a bounded resize and clears drag/resize state.
6. Destroyed windows are detected and removed from compositor interaction pointers before later events.

Keyboard events are routed to the focused visible, non-minimized window. Hidden, minimized, and destroyed windows cannot receive dispatch. Existing arrow-key movement and title-bar drag/resize behavior are retained as test interactions; no polished window styling or desktop shell is present.

## Input path

The active path is:

`PS/2 IRQ -> driver IRQ mailbox -> bounded driver FIFO -> polling syscall -> user_inputd -> IPC -> user_gui -> gui_desktop_run_input_event -> gui_compositor_dispatch`.

`gui_input_event_t` and `gui_event_t` carry event type, coordinates, code, button state, and repeat flags. GUI ingress rejects unknown event types and invalid mouse button codes. See [gui-input.md](gui-input.md) for the detailed input audit.

## CLI diagnostics

The kernel shell remains available. The `gui` command supports framebuffer information, window listing, process listing, FPS/frame counts, smoke status, redraw, and lifecycle checks:

```text
gui info
gui windows
gui fps
gui smoke
gui redraw
gui minimize
gui restore
```

`gui minimize` minimizes the focused window. `gui restore` restores the first minimized window. These are debug/recovery operations, not desktop UI.

## Memory ownership and limitations

The kernel allocator is a fixed heap with a simple free list and no isolated GUI address spaces. GUI surfaces and window metadata are kernel-backed. IPC does not yet share application-owned rendering buffers with the compositor. Allocation failures leave the existing window intact during resize.

Current limitations:

- no application-owned or shared rendering surfaces;
- no transparency, alpha blending, damage tracking, or partial presentation;
- fixed maximum window count;
- no persistent titles or application metadata beyond owner PID and ID;
- existing test drag/resize interactions are basic;
- no automated graphics or input tests;
- runtime VBE, input, focus, clipping, cleanup, and shell behavior remains to be verified in the emulator;
- PS/2 mouse is the only implemented pointing-device interface; USB HID and USB host-controller support were not found;
- the system indicator shows uptime ticks, not real wall-clock time;
- dock icons are placeholders and do not launch applications.

## Startup and CLI recovery

`kmain` always creates the existing kernel shell task, but it remains dormant while GUI startup succeeds. If the GUI service cannot be created, or `user_gui` reports initialization/render failure, the recovery flag is set and the shell starts automatically. After a successful GUI startup, pressing Escape requests CLI recovery; `inputd` pauses so the shell can consume keyboard input without competing with GUI delivery. The CLI remains the recovery/debug path, including `gui info`, `gui windows`, `gui fps`, `gui smoke`, `gui redraw`, `gui minimize`, and `gui restore`.

## Phase 10.6 verification plan

Build the existing project and boot QEMU with its default PS/2 mouse, for example `qemu-system-i386 -drive format=raw,file=build/proos.img`. Verify shell initialization, wallpaper/menu/dock layout at the actual framebuffer dimensions, real PS/2 mouse movement and button transitions, cursor edge behavior, menu/dock hit testing, window drag/resize/focus, keyboard continuity, and Escape CLI recovery. Distinguish compiler success from observed emulator behavior. USB tablet and physical-hardware compatibility are not claimed.
