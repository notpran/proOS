BUILD_DIR := build
NASM := nasm
CC := i686-elf-gcc
LD := i686-elf-ld
OBJCOPY := i686-elf-objcopy
HOST_CC := gcc

CFLAGS := -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -std=gnu99 -m32 -I kernel
LDFLAGS := -nostdlib -m elf_i386 -T kernel/link.ld

.DEFAULT_GOAL := all

STAGE1 := $(BUILD_DIR)/mbr.bin
STAGE2 := $(BUILD_DIR)/stage2.bin
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
KERNEL_BIN := $(BUILD_DIR)/kernel.bin
DISK_IMG := $(BUILD_DIR)/proos.img
ISO_IMG := $(BUILD_DIR)/proos.iso
FAT16_IMG_TOOL := $(BUILD_DIR)/fat16_image.exe
FAT16_IMG := $(BUILD_DIR)/fat16.img
FAT16_SECTORS := 128
STAGE2_SECTORS := 4
KERNEL_SECTORS := 160
KERNEL_OFFSET := 5
FAT16_OFFSET := $(shell expr $(KERNEL_OFFSET) + $(KERNEL_SECTORS))

IMG_SECTORS := 2880
EMBEDDED_FONT := assets/font.bdf
EMBEDDED_FONT_OBJ := $(BUILD_DIR)/embedded_font.o
EMBEDDED_FONT_SYMBOL_BASE := $(subst /,_,$(subst .,_,$(EMBEDDED_FONT)))
EMBEDDED_FONT_START := _binary_$(EMBEDDED_FONT_SYMBOL_BASE)_start
EMBEDDED_FONT_END := _binary_$(EMBEDDED_FONT_SYMBOL_BASE)_end

KERNEL_OBJS := $(BUILD_DIR)/crt0.o \
			   $(BUILD_DIR)/context.o \
			   $(BUILD_DIR)/isr.o \
			   $(BUILD_DIR)/irq.o \
			   $(BUILD_DIR)/syscall_entry.o \
			   $(BUILD_DIR)/idt.o \
			   $(BUILD_DIR)/pic.o \
			   $(BUILD_DIR)/pit.o \
			   $(BUILD_DIR)/ipc.o \
			   $(BUILD_DIR)/process.o \
			   $(BUILD_DIR)/keyboard.o \
			   $(BUILD_DIR)/syscall.o \
			   $(BUILD_DIR)/kmain.o \
			   $(BUILD_DIR)/klog.o \
			   $(BUILD_DIR)/vga.o \
			   $(BUILD_DIR)/memory.o \
			   $(BUILD_DIR)/vbe.o \
			   $(BUILD_DIR)/gfx.o \
			   $(BUILD_DIR)/fat16.o \
			   $(BUILD_DIR)/shell.o \
			   $(BUILD_DIR)/ramfs.o \
			   $(BUILD_DIR)/user/init.o \
			   $(BUILD_DIR)/user/echo_service.o \
			   $(BUILD_DIR)/string.o

ifeq ($(wildcard $(EMBEDDED_FONT)),)
$(info [make] embedded font $(EMBEDDED_FONT) not found; framebuffer console will use BIOS/PSF-on-disk fonts only)
else
CFLAGS += -DHAVE_EMBEDDED_FONT -DEMBEDDED_FONT_START=$(EMBEDDED_FONT_START) -DEMBEDDED_FONT_END=$(EMBEDDED_FONT_END)
KERNEL_OBJS += $(EMBEDDED_FONT_OBJ)

$(EMBEDDED_FONT_OBJ): $(EMBEDDED_FONT) | $(BUILD_DIR)
	$(OBJCOPY) -I binary -O elf32-i386 -B i386 --rename-section .data=.rodata,alloc,load,readonly,data,contents $< $@
endif

.PHONY: all clean run-qemu iso

all: $(DISK_IMG)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(STAGE1): boot/mbr.asm | $(BUILD_DIR)
	$(NASM) -f bin $< -o $@

$(STAGE2): boot/stage2.asm | $(BUILD_DIR)
	$(NASM) -f bin -DSTAGE2_SECTORS=$(STAGE2_SECTORS) -DKERNEL_SECTORS=$(KERNEL_SECTORS) -DFAT16_SECTORS=$(FAT16_SECTORS) $< -o $@

$(BUILD_DIR)/crt0.o: kernel/crt0.s | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: kernel/%.s | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(BUILD_DIR)/%.o: kernel/%.S | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(BUILD_DIR)/%.o: kernel/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/link.ld | $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@

$(KERNEL_BIN): $(KERNEL_ELF) | $(BUILD_DIR)
	$(OBJCOPY) -O binary $< $@

$(FAT16_IMG_TOOL): kernel/fat16_image.c | $(BUILD_DIR)
	$(HOST_CC) -DFAT16_IMAGE_STANDALONE -o $@ $<

$(FAT16_IMG): $(FAT16_IMG_TOOL) | $(BUILD_DIR)
	$< $@

$(DISK_IMG): $(STAGE1) $(STAGE2) $(KERNEL_BIN) $(FAT16_IMG) | $(BUILD_DIR)
	rm -f $@
	dd if=/dev/zero of=$@ bs=512 count=$(IMG_SECTORS) status=none
	dd if=$(STAGE1) of=$@ conv=notrunc status=none
	dd if=$(STAGE2) of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_OFFSET) conv=notrunc status=none
	dd if=$(FAT16_IMG) of=$@ bs=512 seek=$(FAT16_OFFSET) conv=notrunc status=none


$(ISO_IMG): $(DISK_IMG)
	bash iso/make_iso.sh $(DISK_IMG) $(ISO_IMG)

iso: $(ISO_IMG)

run-qemu: $(DISK_IMG)
	qemu-system-i386 -drive format=raw,file=$(DISK_IMG)

clean:
	rm -rf $(BUILD_DIR)
