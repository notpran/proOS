; proOS Stage 1 Bootloader (MBR)
; Loads the stage 2 loader from disk (sectors 2-5) into memory at 0x7E00.
; Assumes BIOS provides boot drive in DL.

BITS 16
ORG 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    mov bx, STAGE2_LOAD_OFFSET
    mov ax, STAGE2_LOAD_SEG
    mov es, ax

    mov ah, 0x02                ; BIOS disk read
    mov al, STAGE2_SECTORS
    mov ch, 0x00
    mov cl, 0x02                ; sector numbering starts at 1; sector 2 is first after MBR
    mov dh, 0x00
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    jmp STAGE2_LOAD_SEG:STAGE2_LOAD_OFFSET

disk_error:
    mov si, disk_error_msg
print_loop:
    lodsb
    or al, al
    jz halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print_loop

halt:
    cli
    hlt
    jmp halt

boot_drive: db 0

disk_error_msg: db "Disk read error", 0

STAGE2_LOAD_SEG    EQU 0x0000
STAGE2_LOAD_OFFSET EQU 0x7E00
STAGE2_SECTORS     EQU 4

TIMES 510-($-$$) db 0
dw 0xAA55
