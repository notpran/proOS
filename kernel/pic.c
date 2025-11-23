#include "pic.h"
#include "io.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI 0x20

static uint16_t irq_mask = 0xFFFF;

static void pic_remap(uint8_t offset1, uint8_t offset2)
{
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    outb(PIC1_DATA, offset1);
    io_wait();
    outb(PIC2_DATA, offset2);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

static void pic_apply_mask(void)
{
    outb(PIC1_DATA, (uint8_t)(irq_mask & 0xFF));
    outb(PIC2_DATA, (uint8_t)((irq_mask >> 8) & 0xFF));
}

void pic_init(void)
{
    pic_remap(0x20, 0x28);
    irq_mask = 0xFFFF;
    pic_apply_mask();
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_clear_mask(uint8_t irq)
{
    irq_mask &= (uint16_t)(~(1U << irq));
    pic_apply_mask();
}

void pic_set_mask(uint8_t irq)
{
    irq_mask |= (uint16_t)(1U << irq);
    pic_apply_mask();
}
