# Phase 2 Test Plan

## Build Checks

1. Run `make`.
   - Expect `build/proos.img` with no build errors.
2. Confirm boot stages sized correctly:
   ```bash
   ls -l build/mbr.bin build/stage2.bin build/kernel.bin
   ```
   - `mbr.bin` should be 512 bytes.
   - `stage2.bin` should remain 2048 bytes (4 sectors).
   - `kernel.bin` grows to include the new drivers.
3. Optional: `hexdump -C build/mbr.bin | head` to verify `55 aa` signature.

## QEMU Functional Test

1. Launch with `make run-qemu`.
2. Expect banner `proOS — PSEK (Protected Mode)` and prompt `proos>`.
3. Type `help`; confirm command list includes `help clear echo mem reboot ls cat`.
4. Type `ls`; expect to see `hello.txt`.
5. Type `cat hello.txt`; expect `Welcome to proOS!` on screen.
6. Type `echo testing > new.txt`; expect `OK` response.
7. Type `ls`; confirm `new.txt` appears.
8. Type `cat new.txt`; expect `testing`.
9. Type `mem`; confirm uptime shows a nonzero value that increases if reissued after a delay.
10. Type random text without pressing Enter; verify characters echo immediately (IRQ-driven input).
11. Type `reboot`; QEMU should reset to BIOS (or fall into `hlt` loop if host blocks the reset).

## VirtualBox Spot Check

1. Convert the raw image: `VBoxManage convertfromraw build/proos.img build/proos.vdi --format VDI`.
2. Attach the VDI to your VM and boot.
3. Repeat the shell checks above; ensure keyboard input feels responsive and uptime increments.

## ISO Boot (Optional)

1. Run `make iso`.
2. Boot with `qemu-system-i386 -cdrom build/proos.iso` and repeat the functional checks.

## Debug Tips

- If the shell stops responding, confirm interrupts are enabled (`sti` reached) or rebuild with logging in the handlers.
- Use `qemu-system-i386 -d int` to observe IRQ firing if the PIT/keyboard appear silent.
- Verify PIC masks with `outb` instrumentation when adding new drivers; `irq_install_handler` automatically unmasks.
- As always, `make clean && make` after toolchain or source changes to clear stale objects.
