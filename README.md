# proOS Phase 1

proOS is a BIOS/MBR 32-bit hobby operating system. Phase 1 focuses on producing a custom boot chain and a minimal protected-mode kernel with a text shell.

## Repository Layout

- `boot/mbr.asm` – 512-byte MBR that loads the stage-2 loader
- `boot/stage2.asm` – real-mode loader that switches to 32-bit protected mode
- `kernel/` – kernel sources (entry stub, linker script, VGA, shell, RAMFS, interrupts, drivers)
- `iso/make_iso.sh` – helper to wrap the raw image in an El Torito ISO
- `Makefile` – builds the bootloader, kernel, and raw disk image
- `tests.md` – manual tests for this phase

## Prerequisites

Use WSL2 or another Unix-like environment on Windows. Install:

- `nasm`
- `i686-elf` cross toolchain (`i686-elf-gcc`, `i686-elf-ld`, `i686-elf-objcopy`)
- `xorriso` (for ISO creation)
- `qemu-system-i386` (optional, for quick testing)

Ensure the cross toolchain binaries are on your `PATH`.

## Build

```bash
make
```

Outputs land in `build/`:

- `proos.img` – bootable raw disk image
- `kernel.elf` / `kernel.bin` – linked kernel artifacts
- `mbr.bin`, `stage2.bin` – boot stages

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

## Run in VirtualBox

1. Convert the raw image to VDI:
   ```bash
   VBoxManage convertfromraw build/proos.img build/proos.vdi --format VDI
   ```
2. Create a new VirtualBox VM (Other/Unknown, 32-bit) with 32 MB RAM.
3. Attach `build/proos.vdi` as the primary disk.
4. Boot the VM.

Alternatively, attach `proos.img` as a virtual floppy/RAW disk if your setup supports it.

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

## Known Limitations

- Keyboard input uses simple polling; no interrupt-driven input yet.
- `mem` reports hard-coded values.
- RAMFS is a placeholder and stores no data.
- `reboot` relies on controller reset and may fall through to an infinite `hlt` if unsupported.

See `tests.md` for manual test guidance.
