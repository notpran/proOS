# proOS kernel entry stub

    .section .text
    .globl _start
    .extern kmain
    .extern __bss_start
    .extern __bss_end

_start:
    cli
    movl $0x002C0000, %esp
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    shrl $2, %ecx
    cld
    rep stosl
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
    .space 16384
stack_top:
