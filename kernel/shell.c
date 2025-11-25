#include <stdint.h>
#include <stddef.h>

#include "vga.h"
#include "keyboard.h"
#include "ramfs.h"
#include "pit.h"
#include "io.h"
#include "proc.h"
#include "fat16.h"
#include "gfx.h"

#define SHELL_PROMPT "proOS >> "
#define INPUT_MAX 256

static size_t str_len(const char *s)
{
    size_t len = 0;
    while (s[len])
        ++len;
    return len;
}

static uint64_t u64_divmod(uint64_t value, uint32_t divisor, uint32_t *remainder)
{
    uint64_t quotient = 0;
    uint64_t rem = 0;

    for (int bit = 63; bit >= 0; --bit)
    {
        rem = (rem << 1) | ((value >> bit) & 1ULL);
        if (rem >= divisor)
        {
            rem -= divisor;
            quotient |= (1ULL << bit);
        }
    }

    if (remainder)
        *remainder = (uint32_t)rem;

    return quotient;
}

static int shell_str_equals(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a++ != *b++)
            return 0;
    }
    return *a == *b;
}

static int shell_str_starts_with(const char *str, const char *prefix)
{
    while (*prefix)
    {
        if (*str++ != *prefix++)
            return 0;
    }
    return 1;
}

static const char *skip_spaces(const char *str)
{
    while (*str == ' ')
        ++str;
    return str;
}

static void trim_trailing_spaces(char *str)
{
    size_t len = str_len(str);
    while (len > 0 && str[len - 1] == ' ')
    {
        str[len - 1] = '\0';
        --len;
    }
}

static size_t shell_read_line(char *buffer, size_t max_len)
{
    size_t len = 0;

    while (1)
    {
        char c = kb_getchar();
        if (!c)
        {
            __asm__ __volatile__("hlt");
            continue;
        }

        if (c == '\b')
        {
            if (len > 0)
            {
                --len;
                vga_backspace();
            }
            continue;
        }

        if (c == '\n')
        {
            vga_write_char('\n');
            buffer[len] = '\0';
            return len;
        }

        if (c == '\t')
            c = ' ';

        if (len + 1 < max_len)
        {
            buffer[len++] = c;
            vga_write_char(c);
        }
    }
}

static void write_u64(uint64_t value, char *out)
{
    char temp[32];
    size_t index = 0;

    if (value == 0)
    {
        out[0] = '0';
        out[1] = '\0';
        return;
    }

    while (value > 0 && index < sizeof(temp))
    {
        uint32_t remainder = 0;
        value = u64_divmod(value, 10U, &remainder);
        temp[index++] = (char)('0' + remainder);
    }

    for (size_t i = 0; i < index; ++i)
        out[i] = temp[index - 1 - i];
    out[index] = '\0';
}

static void command_help(void)
{
    vga_write_line("Available commands:");
    vga_write_line("  help   - show this help");
    vga_write_line("  clear  - clear the screen");
    vga_write_line("  echo   - echo text or redirect");
    vga_write_line("  mem    - memory + uptime info");
    vga_write_line("  reboot - reset the machine");
    vga_write_line("  ls     - list RAMFS files");
    vga_write_line("  cat    - print file contents");
    vga_write_line("  lsfs   - list FAT16 files");
    vga_write_line("  catfs  - print FAT16 file");
    vga_write_line("  gfx    - draw compositor demo");
    vga_write_line("  proc_list - list processes");
}

static void command_clear(void)
{
    vga_clear();
}

static void command_echo(char *args)
{
    args = (char *)skip_spaces(args);
    if (*args == '\0')
    {
        vga_write_line("");
        return;
    }

    char *redirect = NULL;
    for (char *p = args; *p; ++p)
    {
        if (*p == '>')
        {
            redirect = p;
            break;
        }
    }

    if (!redirect)
    {
        vga_write_line(args);
        return;
    }

    *redirect = '\0';
    ++redirect;
    char *filename = (char *)skip_spaces(redirect);

    trim_trailing_spaces(args);
    trim_trailing_spaces(filename);

    if (*filename == '\0')
    {
        vga_write_line("No file specified.");
        return;
    }

    char data[INPUT_MAX];
    size_t len = str_len(args);
    if (len + 2 >= sizeof(data))
    {
        vga_write_line("Input too long.");
        return;
    }

    for (size_t i = 0; i < len; ++i)
        data[i] = args[i];
    data[len++] = '\n';

    if (ramfs_write(filename, data, len) < 0)
        vga_write_line("Write failed.");
    else
        vga_write_line("OK");
}

static void command_mem(void)
{
    vga_write_line("Memory info (stub):");
    vga_write_line("  Total: 32 MB");
    vga_write_line("  Used : 1 MB");
    vga_write_line("  Free : 31 MB");

    uint64_t ticks = get_ticks();
    uint32_t centis = 0;
    uint64_t seconds = u64_divmod(ticks, 100U, &centis);

    char sec_buf[32];
    char centi_buf[4];
    write_u64(seconds, sec_buf);
    centi_buf[0] = (char)('0' + (centis / 10U));
    centi_buf[1] = (char)('0' + (centis % 10U));
    centi_buf[2] = '\0';

    vga_write("  uptime: ");
    vga_write(sec_buf);
    vga_write(".");
    vga_write(centi_buf);
    vga_write_line("s");
}

static void command_reboot(void)
{
    uint8_t status;
    do
    {
        status = inb(0x64);
    } while (status & 0x02);

    outb(0x64, 0xFE);

    for (;;)
        __asm__ __volatile__("hlt");
}

static void command_ls(void)
{
    char list[512];
    int len = ramfs_list(list, sizeof(list));
    if (len <= 0)
    {
        vga_write_line("(empty)");
        return;
    }

    char *ptr = list;
    while (*ptr)
    {
        char *line_start = ptr;
        while (*ptr && *ptr != '\n')
            ++ptr;
        char saved = *ptr;
        *ptr = '\0';
        vga_write_line(line_start);
        if (saved == '\n')
            ++ptr;
    }
}

static void command_cat(const char *arg)
{
    const char *name_ptr = skip_spaces(arg);
    if (*name_ptr == '\0')
    {
        vga_write_line("Usage: cat <file>");
        return;
    }

    char name[RAMFS_MAX_NAME];
    size_t idx = 0;
    while (name_ptr[idx] && name_ptr[idx] != ' ' && idx + 1 < sizeof(name))
    {
        name[idx] = name_ptr[idx];
        ++idx;
    }
    name[idx] = '\0';

    char data[RAMFS_MAX_FILE_SIZE];
    int read = ramfs_read(name, data, sizeof(data));
    if (read < 0)
    {
        vga_write_line("File not found.");
        return;
    }

    vga_write_line(data);
}

static void command_proc_list(void)
{
    process_debug_list();
}

static void command_lsfs(void)
{
    if (!fat16_ready())
    {
        vga_write_line("FAT16 image not available.");
        return;
    }

    char buffer[512];
    int len = fat16_ls(buffer, sizeof(buffer));
    if (len <= 0)
    {
        vga_write_line("(empty)");
        return;
    }

    char *ptr = buffer;
    while (*ptr)
    {
        char *line_start = ptr;
        while (*ptr && *ptr != '\n')
            ++ptr;
        char saved = *ptr;
        *ptr = '\0';
        vga_write_line(line_start);
        if (saved == '\n')
            ++ptr;
    }
}

static void command_catfs(const char *arg)
{
    if (!fat16_ready())
    {
        vga_write_line("FAT16 image not available.");
        return;
    }

    const char *name_ptr = skip_spaces(arg);
    if (*name_ptr == '\0')
    {
        vga_write_line("Usage: catfs <file>");
        return;
    }

    char name[32];
    size_t idx = 0;
    while (name_ptr[idx] && name_ptr[idx] != ' ' && idx + 1 < sizeof(name))
    {
        name[idx] = name_ptr[idx];
        ++idx;
    }
    name[idx] = '\0';

    char data[768];
    int read = fat16_read(name, data, sizeof(data));
    if (read < 0)
    {
        vga_write_line("File not found.");
        return;
    }

    vga_write_line(data);
}

static void command_gfx(void)
{
    if (!gfx_available())
    {
        vga_write_line("Graphics mode unavailable.");
        return;
    }

    if (gfx_show_demo() == 0)
        vga_write_line("Graphics demo drawn.");
    else
        vga_write_line("Graphics demo failed.");
}

static void shell_execute(char *line)
{
    line = (char *)skip_spaces(line);

    if (*line == '\0')
        return;

    if (shell_str_equals(line, "help"))
    {
        command_help();
    }
    else if (shell_str_equals(line, "clear"))
    {
        command_clear();
    }
    else if (shell_str_equals(line, "mem"))
    {
        command_mem();
    }
    else if (shell_str_equals(line, "reboot"))
    {
        command_reboot();
    }
    else if (shell_str_equals(line, "ls"))
    {
        command_ls();
    }
    else if (shell_str_equals(line, "catfs") || shell_str_starts_with(line, "catfs "))
    {
        command_catfs(line + 5);
    }
    else if (shell_str_equals(line, "cat") || shell_str_starts_with(line, "cat "))
    {
        command_cat(line + 3);
    }
    else if (shell_str_equals(line, "proc_list"))
    {
        command_proc_list();
    }
    else if (shell_str_equals(line, "lsfs"))
    {
        command_lsfs();
    }
    else if (shell_str_equals(line, "gfx"))
    {
        command_gfx();
    }
    else if (shell_str_equals(line, "echo") || shell_str_starts_with(line, "echo "))
    {
        command_echo(line + 4);
    }
    else
    {
        vga_write_line("Unknown command. Type 'help'.");
    }
}

void shell_run(void)
{
    char buffer[INPUT_MAX];

    while (1)
    {
        vga_set_color(0xB, 0x0);
        vga_write(SHELL_PROMPT);
        vga_set_color(0x7, 0x0);
        shell_read_line(buffer, sizeof(buffer));
        shell_execute(buffer);
    }
}
