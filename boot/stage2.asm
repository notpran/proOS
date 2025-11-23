; proOS Stage 2 Loader
; Loads the protected-mode kernel into memory, enables A20, switches to 32-bit mode,
; copies the kernel to its final location at 0x00100000, and jumps to it.

BITS 16
ORG 0x7E00

STAGE2_SECTORS      EQU 4
KERNEL_SECTORS      EQU 64
KERNEL_SIZE         EQU KERNEL_SECTORS * 512
KERNEL_START_SECTOR EQU 2 + STAGE2_SECTORS
KERNEL_TEMP_SEG     EQU 0x1000            ; 0x1000 << 4 = 0x00010000
KERNEL_TEMP_ADDR    EQU 0x00010000
KERNEL_BASE_ADDR    EQU 0x00100000
PM_STACK_TOP        EQU 0x00200000

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

    mov ah, 0x02
    mov al, KERNEL_SECTORS
    mov ch, 0x00
    mov cl, KERNEL_START_SECTOR
    mov dh, 0x00
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    call enable_a20
    call setup_gdt
    call enter_pm

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

    mov eax, KERNEL_BASE_ADDR
    jmp eax

; -----------------
; Data and tables
; -----------------

[BITS 16]
boot_drive: db 0

disk_error_msg: db "Stage2 disk error", 0

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
