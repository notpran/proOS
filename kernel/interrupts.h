#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

struct regs
{
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    uint32_t useresp;
    uint32_t ss;
};

typedef void (*isr_callback_t)(struct regs *frame);
typedef void (*irq_callback_t)(struct regs *frame);

void idt_init(void);
void isr_install_handler(int num, isr_callback_t handler);
void irq_install_handler(int irq, irq_callback_t handler);
void irq_uninstall_handler(int irq);

#endif
