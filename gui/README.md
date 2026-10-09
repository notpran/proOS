# proOS GUI

The GUI is layered around the existing PSEK services:

`application -> window surface -> compositor -> backbuffer -> framebuffer`

This first slice is kernel-backed because the current process implementation does not yet provide isolated address spaces. The public interfaces are kept independent of the text console so the service can move to a user-space process when memory sharing is available.
