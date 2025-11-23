#include "pit.h"
#include "io.h"
#include "interrupts.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND 0x43
#define PIT_MODE 0x36
#define PIT_FREQUENCY 1193180U

static volatile uint64_t ticks = 0;

static void pit_irq_handler(struct regs *frame)
{
    (void)frame;
    ++ticks;
}

void pit_init(uint32_t frequency)
{
    if (frequency == 0)
        frequency = 100;

    uint32_t divisor = PIT_FREQUENCY / frequency;

    irq_install_handler(0, pit_irq_handler);

    outb(PIT_COMMAND, PIT_MODE);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

uint64_t get_ticks(void)
{
    return ticks;
}
