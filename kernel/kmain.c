#include "vga.h"
#include "ramfs.h"
#include "interrupts.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"

extern void shell_run(void);

static void print_banner(void)
{
    vga_set_color(0xF, 0x0);
    vga_write_line("proOS — PSEK (Protected Mode)");
    vga_set_color(0xA, 0x0);
    vga_write_line("Welcome to Phase 2");
    vga_set_color(0x7, 0x0);
    vga_write_line("Type 'help' to list commands.");
    vga_write_char('\n');
}

void kmain(void)
{
    vga_init();
    vga_clear();
    ramfs_init();
    idt_init();
    pic_init();
    pit_init(100);
    kb_init();
    print_banner();
    __asm__ __volatile__("sti");
    shell_run();

    for (;;)
    {
        __asm__ __volatile__("hlt");
    }
}
