# proOS kernel entry stub

    .section .text
    .globl _start
    .extern kmain

_start:
    cli
    movl $stack_top, %esp
    xorl %ebp, %ebp
    call kmain

halt:
    cli
    hlt
    jmp halt

    .section .bss
    .align 16
stack_storage:
    .space 4096
stack_top:
