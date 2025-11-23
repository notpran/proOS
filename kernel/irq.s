.section .text
.code32

.global irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
.global irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
.global irq_common_stub

.extern irq_handler

.macro IRQ_ENTRY num
.global irq\num
irq\num:
    pushl $0
    pushl $(32 + \num)
    jmp irq_common_stub
.endm

IRQ_ENTRY 0
IRQ_ENTRY 1
IRQ_ENTRY 2
IRQ_ENTRY 3
IRQ_ENTRY 4
IRQ_ENTRY 5
IRQ_ENTRY 6
IRQ_ENTRY 7
IRQ_ENTRY 8
IRQ_ENTRY 9
IRQ_ENTRY 10
IRQ_ENTRY 11
IRQ_ENTRY 12
IRQ_ENTRY 13
IRQ_ENTRY 14
IRQ_ENTRY 15

irq_common_stub:
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    pusha

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    mov %esp, %eax
    pushl %eax
    call irq_handler
    add $4, %esp

    popa
    popl %gs
    popl %fs
    popl %es
    popl %ds
    add $8, %esp
    iret
