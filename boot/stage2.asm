; proOS Stage 2 Loader
; Loads the protected-mode kernel into memory, enables A20, switches to 32-bit mode,
; copies the kernel to its final location at 0x00100000, and jumps to it.

BITS 16
ORG 0x7E00

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS      4
%endif

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS      64
%endif

%ifndef FAT16_SECTORS
%define FAT16_SECTORS       32
%endif

%define KERNEL_SIZE         (KERNEL_SECTORS * 512)
%define KERNEL_START_SECTOR (2 + STAGE2_SECTORS)
%define KERNEL_TEMP_SEG     0x1000            ; 0x1000 << 4 = 0x00010000
%define KERNEL_TEMP_ADDR    0x00010000
%define KERNEL_BASE_ADDR    0x00100000
%define PM_STACK_TOP        0x00180000

%define FAT16_TEMP_SEG      0x9000
%define FAT16_TEMP_ADDR     0x00090000
%define FAT16_DEST_ADDR     0x00200000
%define FAT16_SIZE_BYTES    (FAT16_SECTORS * 512)
%define FAT16_START_SECTOR  (KERNEL_START_SECTOR + KERNEL_SECTORS)

%define BOOT_INFO_ADDR      0x0000FE00
%define BOOT_INFO_MAGIC     0x534F5250       ; "PROS"

start_stage2:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7E00
    sti

    mov [boot_drive], dl

    ; Load kernel into temporary buffer at 0x00010000
    mov ax, KERNEL_TEMP_SEG
    mov es, ax
    xor bx, bx
    mov ax, KERNEL_START_SECTOR
    mov cx, KERNEL_SECTORS
    call read_sectors
    jc disk_error

%if FAT16_SECTORS > 0
    ; Load FAT16 image into temporary buffer at 0x00090000
    mov ax, FAT16_TEMP_SEG
    mov es, ax
    xor bx, bx
    mov ax, FAT16_START_SECTOR
    mov cx, FAT16_SECTORS
    call read_sectors
    jc .fat_load_failed
    mov byte [fat_loaded], 1
    jmp .fat_load_done

.fat_load_failed:
    mov byte [fat_loaded], 0

.fat_load_done:
%endif

    call get_bios_font
    call try_vbe_modes
    call enable_a20
    call setup_gdt
    call enter_pm

; Read CX sectors starting at sector AX (1-based) into ES:BX using INT 13h extensions
; Returns CF set on failure, clear on success.
read_sectors:
    push bp
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    ; EDI: running physical destination address
    xor edi, edi
    mov di, bx
    xor ebp, ebp
    mov bp, es
    shl ebp, 4
    add edi, ebp

    ; EAX: current LBA (0-based)
    mov dx, ax
    xor eax, eax
    mov ax, dx
    dec eax

    ; EBX: remaining sector count
    xor ebx, ebx
    mov bx, cx

.read_loop:
    test ebx, ebx
    jz .done

    mov ecx, ebx
    cmp ecx, 63
    jbe .chunk_ready
    mov ecx, 63

.chunk_ready:
    mov word [dap_sector_count], cx

    mov edx, eax
    mov word [dap_lba_low], dx
    shr edx, 16
    mov word [dap_lba_low + 2], dx
    mov word [dap_lba_high], 0
    mov word [dap_lba_high + 2], 0

    mov edx, edi
    mov bp, dx
    and bp, 0x000F
    mov word [dap_buffer_offset], bp
    shr edx, 4
    mov word [dap_buffer_segment], dx

    mov si, dap_packet
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc .error

    sub ebx, ecx
    add eax, ecx

    mov edx, ecx
    shl edx, 9
    add edi, edx

    jmp .read_loop

.done:
    clc
    jmp .restore

.error:
    stc

.restore:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop bp
    ret

get_bios_font:
    mov word [font_seg], 0
    mov word [font_off], 0
    mov word [font_bytes], 0
    mov word [font_height], 0
    mov word [font_char_count], 0
    mov word [font_flags], 0

    mov ax, 0x1130
    mov bh, 0x00
    int 0x10
    jc .done

    cmp cx, 0
    je .done

    mov [font_seg], es
    mov [font_off], bp
    mov [font_bytes], cx
    mov [font_char_count], dx
    movzx ax, al
    mov [font_height], ax
    mov word [font_flags], 0

.done:
    ret

; Should never return

halt:
    cli
    hlt
    jmp halt

disk_error:
    mov si, disk_error_msg
.print_char:
    lodsb
    or al, al
    jz halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .print_char

; Enable A20 using the fast A20 gate (port 0x92)
enable_a20:
    in al, 0x92
    or al, 0x02
    out 0x92, al
    ret

try_vbe_modes:
    mov byte [vbe_available], 0
    mov si, vbe_mode_list

.next_mode:
    lodsw
    mov bx, ax
    cmp bx, 0
    je .done
    mov [vbe_mode_candidate], bx

    push si
    mov ax, cs
    mov es, ax
    lea di, [vbe_mode_info]
    mov cx, 256 / 2
    xor ax, ax
    rep stosw
    lea di, [vbe_mode_info]
    mov cx, bx
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .restore

    test word [vbe_mode_info + 0x00], 0x0080
    jz .restore

    mov al, [vbe_mode_info + 0x19]
    cmp al, 32
    jne .restore

    mov ax, 0x4F02
    mov bx, [vbe_mode_candidate]
    or bx, 0x4000
    int 0x10
    cmp ax, 0x004F
    jne .restore

    mov byte [vbe_available], 1
    mov [vbe_selected_mode], bx

    mov ax, [vbe_mode_info + 0x12]
    mov [vbe_width], ax
    mov word [vbe_width + 2], 0

    mov ax, [vbe_mode_info + 0x14]
    mov [vbe_height], ax
    mov word [vbe_height + 2], 0

    xor eax, eax
    mov al, [vbe_mode_info + 0x19]
    mov [vbe_bpp], eax

    movzx eax, word [vbe_mode_info + 0x10]
    cmp eax, 0
    jne .store_pitch
    movzx eax, word [vbe_mode_info + 0x40]
    cmp eax, 0
    jne .store_pitch
    mov eax, 0
.store_pitch:
    mov [vbe_pitch], eax

    mov eax, [vbe_mode_info + 0x28]
    mov [vbe_fb_ptr], eax

    mov eax, [vbe_pitch]
    mov ecx, [vbe_height]
    mul ecx
    mov [vbe_fb_size], eax

    pop si
    jmp .done

.restore:
    pop si
    jmp .next_mode

.done:
    ret

setup_gdt:
    lgdt [gdt_descriptor]
    ret

enter_pm:
    cli
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    jmp CODE_SELECTOR:pm_entry

; 32-bit protected mode section
[BITS 32]
pm_entry:
    mov ax, DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, PM_STACK_TOP

    cld
    mov esi, KERNEL_TEMP_ADDR
    mov edi, KERNEL_BASE_ADDR
    mov ecx, KERNEL_SIZE
    rep movsb

%if FAT16_SECTORS > 0
    cmp byte [fat_loaded], 1
    jne .skip_fat_copy
    mov esi, FAT16_TEMP_ADDR
    mov edi, FAT16_DEST_ADDR
    mov ecx, FAT16_SIZE_BYTES
    rep movsb
.skip_fat_copy:
%endif

    ; Populate boot info structure
    mov edi, BOOT_INFO_ADDR
    mov ecx, 16
    xor eax, eax
    rep stosd

    mov dword [BOOT_INFO_ADDR + 4], 1        ; version

    cmp byte [vbe_available], 1
    jne .skip_vbe_info

    mov eax, BOOT_INFO_MAGIC
    mov [BOOT_INFO_ADDR + 0], eax

    mov eax, [vbe_fb_ptr]
    mov [BOOT_INFO_ADDR + 8], eax
    mov [BOOT_INFO_ADDR + 28], eax

    mov eax, [vbe_pitch]
    mov [BOOT_INFO_ADDR + 12], eax

    mov eax, [vbe_width]
    mov [BOOT_INFO_ADDR + 16], eax

    mov eax, [vbe_height]
    mov [BOOT_INFO_ADDR + 20], eax

    mov eax, [vbe_bpp]
    mov [BOOT_INFO_ADDR + 24], eax

    mov eax, [vbe_fb_size]
    mov [BOOT_INFO_ADDR + 32], eax

.skip_vbe_info:

%if FAT16_SECTORS > 0
    cmp byte [fat_loaded], 1
    jne .skip_fat_info
    mov eax, FAT16_DEST_ADDR
    mov [BOOT_INFO_ADDR + 36], eax
    mov eax, FAT16_SIZE_BYTES
    mov [BOOT_INFO_ADDR + 40], eax
.skip_fat_info:
%endif

    movzx eax, word [font_seg]
    or eax, eax
    jz .skip_font_info

    movzx ecx, word [font_height]
    test ecx, ecx
    jz .skip_font_info

    movzx edx, word [font_bytes]
    test edx, edx
    jz .skip_font_info

    mov ebx, eax
    shl ebx, 4
    movzx eax, word [font_off]
    add ebx, eax
    mov [BOOT_INFO_ADDR + 44], ebx

    mov [BOOT_INFO_ADDR + 48], ecx
    mov [BOOT_INFO_ADDR + 52], edx

    movzx eax, word [font_char_count]
    mov [BOOT_INFO_ADDR + 56], eax

    movzx eax, word [font_flags]
    mov [BOOT_INFO_ADDR + 60], eax

.skip_font_info:
    mov eax, KERNEL_BASE_ADDR
    jmp eax

; -----------------
; Data and tables
; -----------------

[BITS 16]
boot_drive: db 0
fat_loaded: db 0

vbe_available: db 0
vbe_selected_mode: dw 0
vbe_mode_candidate: dw 0
vbe_width: dd 0
vbe_height: dd 0
vbe_pitch: dd 0
vbe_bpp: dd 0
vbe_fb_ptr: dd 0
vbe_fb_size: dd 0

vbe_mode_list:
    dw 0x143, 0x142, 0x141, 0x118, 0

align 4
vbe_mode_info: times 256 db 0

align 4
dap_packet:
    db 0x10
    db 0x00
dap_sector_count: dw 0
dap_buffer_offset: dw 0
dap_buffer_segment: dw 0
dap_lba_low: dd 0
dap_lba_high: dd 0

disk_error_msg: db "Stage2 disk error", 0

font_seg: dw 0
font_off: dw 0
font_bytes: dw 0
font_height: dw 0
font_char_count: dw 0
font_flags: dw 0

align 8
GDT_START:
    dq 0x0000000000000000          ; Null descriptor
    dq 0x00CF9A000000FFFF          ; Code segment: base=0, limit=0xFFFFF, 4K granularity
    dq 0x00CF92000000FFFF          ; Data segment: base=0, limit=0xFFFFF, 4K granularity
GDT_END:

gdt_descriptor:
    dw GDT_END - GDT_START - 1
    dd GDT_START

CODE_SELECTOR EQU 0x08
DATA_SELECTOR EQU 0x10

TIMES (STAGE2_SECTORS * 512) - ($-$$) db 0
