# proOS GUI Input Audit

## Scope and status

This document records the Phase 10.4 input path from source inspection. Build, emulator, and hardware input tests were not run in this pass, so all runtime behavior remains unverified.

## Hardware-to-GUI path

Keyboard input follows:

`PS/2 IRQ -> kernel/keyboard.c legacy IRQ handler and queue -> legacy keyboard poll syscall -> user_inputd -> IPC -> user_gui -> gui_desktop_run_input_event -> gui_compositor_dispatch`.

Mouse input follows:

`PS/2 IRQ -> ps2mouse module IRQ mailbox -> three-byte packet parser -> mouse FIFO -> SYS_MOUSE_POLL -> user_inputd -> IPC -> user_gui -> gui_desktop_run_input_event -> gui_compositor_dispatch`.

The driver accepts a packet only when the first-byte synchronization bit is set and the PS/2 overflow bits are clear. It reports relative movement and the three standard PS/2 button bits. Wheel packets are not enabled. USB host-controller and HID mouse support were not found in the repository; PS/2 support must not be presented as USB support.

Interrupt-side work remains limited to reading device data and dispatching it into IRQ mailbox infrastructure. Packet decoding and FIFO conversion run when the polling syscalls are serviced. GUI dispatch and rendering run in the GUI process, not in the interrupt handlers.

## Event interface

`gui_input_event_t` is the IPC payload used by `user_inputd` and `user_gui`. It contains:

- event type;
- relative mouse coordinates or zero for keyboard events;
- normalized key or mouse-button code;
- current mouse button bitmask;
- `GUI_INPUT_FLAG_REPEAT` for repeated keyboard press events.

`gui_event_t` mirrors these fields for compositor/window routing. The GUI ingress rejects `GUI_INPUT_NONE`, unknown event values, and mouse button codes outside buttons 1 through 4. IPC messages must be exactly one complete event payload before they are decoded.

Keyboard arrow press and release events are normalized to the same GUI arrow constants. `inputd` tracks 256 key slots and marks a repeated key-down without suppressing the event. Reconnecting to the GUI service resets key and mouse transient state.

The legacy keyboard queue is bounded and drops new entries when full. The `ps2kbd` module remains in the build for compatibility, but GUI input uses the legacy keyboard syscall so CLI recovery and GUI input share one decoder and queue. The mouse module's fixed FIFO is also bounded and drops new entries when full. No interrupt handler allocates memory or blocks on the GUI.

## Cursor and focus

The compositor owns the cursor coordinates. Mouse deltas are applied to the current position and clamped to the framebuffer dimensions. The cursor is rendered after all visible surfaces, so it appears above them. The compositor currently performs a full backbuffer redraw each frame, which removes cursor trails without saving and restoring pixels.

Pointer-button hit testing selects the highest-z-order visible, non-minimized window under the cursor and focuses it before dispatching the event. Keyboard events go to the focused visible, non-minimized window. Hidden, minimized, and destroyed windows cannot retain focus or receive dispatched events.

The existing compositor contains older title-bar drag and resize behavior. Phase 10.4 does not add or expand that behavior; later policy work must decide whether it remains part of the test compositor.

## Ownership and scheduling

Input driver queues are static module storage. GUI IPC uses the existing PSEK mailbox and capability checks. `user_inputd` polls devices and sleeps between passes. `user_gui` drains IPC messages, routes events, renders, and sleeps. Surface allocation and compositor rendering remain separate from interrupt processing.

## Verification status

Implemented from source:

- deferred keyboard and mouse interrupt processing;
- bounded keyboard and mouse FIFOs;
- normalized key press/release and mouse button events;
- repeat flag and reconnect state reset;
- malformed GUI event rejection;
- cursor boundary clamping and redraw above surfaces;
- focus clearing on hide, minimize, and destroy.

Not verified in this pass:

- successful PS/2 controller initialization in QEMU or on hardware;
- actual keyboard press/release delivery;
- actual mouse packet delivery and button transitions;
- queue overflow behavior under runtime load;
- cursor edge behavior using real mouse deltas;
- focus routing observed through the emulator;
- QEMU mouse delivery, shell recovery after Escape, and CLI behavior while the GUI is active.

The supported pointing-device path is the existing PS/2 mouse driver. No USB host-controller or USB HID/tablet implementation exists in this repository, so QEMU USB tablet support is not claimed. The menu bar, dock, wallpaper, cursor, and startup/recovery shell integration are documented in `gui-architecture.md`.
