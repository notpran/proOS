<<<<<<< HEAD
# proOS Phase 4

proOS is a BIOS/MBR 32-bit hobby operating system. Phase 4 layers a read-only FAT16 loader and a simple framebuffer compositor on top of the Phase 1–3 base (boot chain, interrupts, cooperative scheduler, and IPC).
=======
# proOS

proOS is a BIOS/MBR 32-bit hobby operating system made by me!
>>>>>>> 80a19d1816f8ad0bad606b77190988b27f912bc5

## Repository Layout

- `boot/mbr.asm` – 512-byte MBR that loads the stage-2 loader
- `boot/stage2.asm` – real-mode loader that switches to 32-bit protected mode, loads the kernel and a FAT16 disk image, and passes VBE framebuffer info to the kernel
- `kernel/` – kernel sources (entry stub, linker script, VGA, shell, RAMFS, interrupts, drivers, scheduler)
- `kernel/vbe.c|h` – framebuffer driver and text console shim
- `kernel/gfx.c|h` – minimal compositor and demo windows
- `kernel/fat16.c|h` – in-memory FAT16 reader used for `lsfs`/`catfs`
- `kernel/memory.c|h` – bump allocator backing window buffers
- `kernel/fat16_image.c` – host helper that generates `build/fat16.img` for Stage 2 to load
- `iso/make_iso.sh` – helper to wrap the raw image in an El Torito ISO
- `Makefile` – builds the bootloader, kernel, and raw disk image
- `tests.md` – manual tests for this phase

## Prerequisites

Use MSYS2/WSL2 or another Unix-like environment on Windows. Install:

- `nasm`
- `i686-elf` cross toolchain (`i686-elf-gcc`, `i686-elf-ld`, `i686-elf-objcopy`)
- `xorriso` (for ISO creation)
- `qemu-system-i386` (optional, for quick testing)
- `gcc` (host compiler used to build the FAT16 image helper)

Ensure the cross toolchain binaries are on your `PATH`.

## Build

```bash
make
```

Outputs land in `build/`:

- `proos.img` – bootable raw disk image
- `kernel.elf` / `kernel.bin` – linked kernel artifacts
- `mbr.bin`, `stage2.bin` – boot stages
- `fat16.img` – 16 KB read-only FAT16 volume preloaded with `readme.txt`

Create an ISO (requires `xorriso`):

```bash
make iso
```

The script writes `build/proos.iso`.

Clean artifacts:

```bash
make clean
```

## Run in QEMU

```bash
make run-qemu
```
## Expected Boot Flow

1. `mbr.asm` loads `stage2.asm` into 0x7E00.
2. Stage 2 loads the kernel into 0x00010000, enables the A20 line, enters protected mode, copies the kernel to 0x00100000, and jumps to `_start`.
3. `_start` (from `crt0.s`) sets up the stack and calls `kmain`.
4. `kmain` initializes VGA text mode, prints a banner, initializes the RAMFS stub, and starts the shell.
5. The shell waits for keyboard input (IRQ-driven) and supports:
   - `help`
   - `clear`
   - `echo <text>` / `echo <text> > <file>`
   - `mem`
   - `reboot`
   - `ls`
   - `cat <file>`
   - `proc_list`
6. Cooperative scheduler boots a user `init` process that spawns a user `echo_service` via syscalls and demonstrates IPC (`ECHO: Hello` banner at boot).
7. FAT16 image is copied to 0x00200000 and exposed via `lsfs` / `catfs` shell commands.
8. When `gfx` is entered, the framebuffer compositor draws a demo desktop with windowed output; keyboard input remains routed to the shell.

## Known Limitations

- FAT16 support is read-only, limited to 8.3 filenames in the root directory.
- Framebuffer path assumes a 32-bpp VESA mode; systems without that support fall back to VGA text automatically.
- `mem` reports hard-coded values.
- RAMFS remains a stub for scratch files.
- `reboot` relies on controller reset and may fall through to an infinite `hlt` if unsupported.
