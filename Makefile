BUILD_DIR := build
NASM := nasm
CC := i686-elf-gcc
LD := i686-elf-ld
OBJCOPY := i686-elf-objcopy

CFLAGS := -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -std=gnu99 -m32 -I kernel
LDFLAGS := -nostdlib -m elf_i386 -T kernel/link.ld

STAGE1 := $(BUILD_DIR)/mbr.bin
STAGE2 := $(BUILD_DIR)/stage2.bin
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
KERNEL_BIN := $(BUILD_DIR)/kernel.bin
DISK_IMG := $(BUILD_DIR)/proos.img
ISO_IMG := $(BUILD_DIR)/proos.iso

KERNEL_OBJS := $(BUILD_DIR)/crt0.o \
			   $(BUILD_DIR)/isr.o \
			   $(BUILD_DIR)/irq.o \
			   $(BUILD_DIR)/idt.o \
			   $(BUILD_DIR)/pic.o \
			   $(BUILD_DIR)/pit.o \
			   $(BUILD_DIR)/keyboard.o \
			   $(BUILD_DIR)/kmain.o \
			   $(BUILD_DIR)/vga.o \
			   $(BUILD_DIR)/shell.o \
			   $(BUILD_DIR)/ramfs.o

IMG_SECTORS := 2880
STAGE2_SECTORS := 4
KERNEL_SECTORS := 64
KERNEL_OFFSET := 5

.PHONY: all clean run-qemu iso

all: $(DISK_IMG)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(STAGE1): boot/mbr.asm | $(BUILD_DIR)
	$(NASM) -f bin $< -o $@

$(STAGE2): boot/stage2.asm | $(BUILD_DIR)
	$(NASM) -f bin $< -o $@

$(BUILD_DIR)/crt0.o: kernel/crt0.s | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: kernel/%.s | $(BUILD_DIR)
	$(CC) -m32 -c $< -o $@

$(BUILD_DIR)/%.o: kernel/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/link.ld | $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@

$(KERNEL_BIN): $(KERNEL_ELF) | $(BUILD_DIR)
	$(OBJCOPY) -O binary $< $@

$(DISK_IMG): $(STAGE1) $(STAGE2) $(KERNEL_BIN) | $(BUILD_DIR)
	rm -f $@
	dd if=/dev/zero of=$@ bs=512 count=$(IMG_SECTORS) status=none
	dd if=$(STAGE1) of=$@ conv=notrunc status=none
	dd if=$(STAGE2) of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_OFFSET) conv=notrunc status=none

iso: $(DISK_IMG)
	bash iso/make_iso.sh $(DISK_IMG) $(ISO_IMG)

run-qemu: $(DISK_IMG)
	qemu-system-i386 -drive format=raw,file=$(DISK_IMG)

clean:
	rm -rf $(BUILD_DIR)
