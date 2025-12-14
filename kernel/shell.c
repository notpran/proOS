#include <stdint.h>
#include <stddef.h>
#include <limits.h>

#include "vga.h"
#include "keyboard.h"
#include "ramfs.h"
#include "pit.h"
#include "io.h"
#include "proc.h"
#include "fat16.h"
#include "gfx.h"
#include "klog.h"
#include "module.h"
#include "memory.h"

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

static void buffer_append(char *dst, size_t *pos, size_t max, const char *text)
{
    if (!dst || !pos || !text || max == 0)
        return;
    while (*text && *pos < max - 1)
    {
        dst[*pos] = *text;
        ++(*pos);
        ++text;
    }
}

static void strip_kmd_extension(char *name)
{
    if (!name)
        return;
    size_t len = str_len(name);
    if (len >= 4)
    {
        char *ext = name + len - 4;
        if (ext[0] == '.' && (ext[1] == 'k' || ext[1] == 'K') && (ext[2] == 'm' || ext[2] == 'M') && (ext[3] == 'd' || ext[3] == 'D'))
            ext[0] = '\0';
    }
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

static int parse_positive_int(const char *text, int *out_value)
{
    if (!text || !out_value)
        return 0;

    int value = 0;
    if (*text == '\0')
        return 0;

    for (size_t i = 0; text[i]; ++i)
    {
        char c = text[i];
        if (c < '0' || c > '9')
            return 0;
        int digit = c - '0';
        if (value > (INT_MAX - digit) / 10)
            return 0;
        value = value * 10 + digit;
    }

    if (value <= 0)
        return 0;

    *out_value = value;
    return 1;
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
    vga_write_line("  mod    - module control (list/load/unload .kmd)");
    vga_write_line("  gfx    - draw compositor demo");
    vga_write_line("  kdlg   - show kernel log");
    vga_write_line("  kdlvl [lvl] - adjust log verbosity");
    vga_write_line("  proc_count - show active process count");
    vga_write_line("  spawn <n> - stress process creation");
    vga_write_line("  shutdown - power off the system");
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

static void command_mod_list(void)
{
    const module_handle_t *handles[32];
    size_t count = module_enumerate(handles, sizeof(handles) / sizeof(handles[0]));
    if (count == 0)
    {
        vga_write_line("mod: no modules loaded");
        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        const module_handle_t *handle = handles[i];
        if (!handle)
            continue;

        char index_buf[16];
        write_u64((uint64_t)(i + 1u), index_buf);

        char line[128];
        size_t pos = 0;
        buffer_append(line, &pos, sizeof(line), index_buf);
        buffer_append(line, &pos, sizeof(line), ". ");
        buffer_append(line, &pos, sizeof(line), handle->meta.name);
        buffer_append(line, &pos, sizeof(line), " v");
        buffer_append(line, &pos, sizeof(line), handle->meta.version);
        buffer_append(line, &pos, sizeof(line), " (");
        buffer_append(line, &pos, sizeof(line), handle->meta.active ? "active" : "inactive");
        if (handle->meta.autostart)
            buffer_append(line, &pos, sizeof(line), ",autostart");
        if (handle->meta.builtin)
            buffer_append(line, &pos, sizeof(line), ",builtin");
        buffer_append(line, &pos, sizeof(line), ")");
        line[pos] = '\0';
        vga_write_line(line);
    }
}

static int build_module_filename(const char *name, char *out, size_t out_size)
{
    if (!name || !out || out_size == 0)
        return 0;

    size_t len = 0;
    while (name[len] && name[len] != ' ' && len + 1 < out_size)
    {
        out[len] = name[len];
        ++len;
    }
    out[len] = '\0';

    if (len == 0)
        return 0;

    int has_dot = 0;
    for (size_t i = 0; i < len; ++i)
    {
        if (out[i] == '.')
        {
            has_dot = 1;
            break;
        }
    }

    if (!has_dot)
    {
        if (len + 4 >= out_size)
            return 0;
        out[len++] = '.';
        out[len++] = 'k';
        out[len++] = 'm';
        out[len++] = 'd';
        out[len] = '\0';
    }

    return 1;
}

static void command_mod_load(const char *args)
{
    const char *name_ptr = skip_spaces(args);
    char module_name[MODULE_NAME_MAX];
    size_t idx = 0;
    while (name_ptr[idx] && name_ptr[idx] != ' ' && idx + 1 < sizeof(module_name))
    {
        module_name[idx] = name_ptr[idx];
        ++idx;
    }
    module_name[idx] = '\0';

    if (module_name[0] == '\0')
    {
        vga_write_line("Usage: mod load <module>");
        return;
    }

    strip_kmd_extension(module_name);

    if (module_find(module_name))
    {
        vga_write_line("mod: module already loaded");
        return;
    }

    if (!fat16_ready())
    {
        vga_write_line("mod: FAT16 image unavailable");
        return;
    }

    char filename[48];
    if (!build_module_filename(module_name, filename, sizeof(filename)))
    {
        vga_write_line("mod: invalid module name");
        return;
    }

    uint32_t file_size = 0;
    if (fat16_file_size(filename, &file_size) < 0 || file_size == 0)
    {
        vga_write_line("mod: file not found on FAT16");
        return;
    }

    uint8_t *buffer = (uint8_t *)kalloc(file_size);
    if (!buffer)
    {
        vga_write_line("mod: out of memory");
        return;
    }

    size_t read_size = 0;
    if (fat16_read_file(filename, buffer, file_size, &read_size) < 0 || read_size != file_size)
    {
        vga_write_line("mod: failed to read module image");
        return;
    }

    if (module_load_image(filename, buffer, file_size, 0) == 0)
    {
        vga_write("mod: loaded ");
        vga_write_line(module_name);
    }
    else
    {
        vga_write_line("mod: load failed");
    }
}

static void command_mod_unload(const char *args)
{
    const char *name_ptr = skip_spaces(args);
    char module_name[MODULE_NAME_MAX];
    size_t idx = 0;
    while (name_ptr[idx] && name_ptr[idx] != ' ' && idx + 1 < sizeof(module_name))
    {
        module_name[idx] = name_ptr[idx];
        ++idx;
    }
    module_name[idx] = '\0';

    if (module_name[0] == '\0')
    {
        vga_write_line("Usage: mod unload <module>");
        return;
    }

    strip_kmd_extension(module_name);

    const module_handle_t *handle = module_find(module_name);
    if (!handle)
    {
        vga_write_line("mod: module not loaded");
        return;
    }

    if (handle->meta.builtin)
    {
        vga_write_line("mod: cannot unload builtin module");
        return;
    }

    if (module_unload(module_name) == 0)
    {
        vga_write("mod: unloaded ");
        vga_write_line(module_name);
    }
    else
    {
        vga_write_line("mod: unload failed");
    }
}

static void command_mod(const char *args)
{
    const char *sub = skip_spaces(args);

    if (*sub == '\0')
    {
        command_mod_list();
        return;
    }

    char token[8];
    size_t idx = 0;
    while (sub[idx] && sub[idx] != ' ' && idx + 1 < sizeof(token))
    {
        token[idx] = sub[idx];
        ++idx;
    }
    token[idx] = '\0';

    const char *rest = skip_spaces(sub + idx);

    if (shell_str_equals(token, "list"))
    {
        command_mod_list();
    }
    else if (shell_str_equals(token, "load"))
    {
        command_mod_load(rest);
    }
    else if (shell_str_equals(token, "unload"))
    {
        command_mod_unload(rest);
    }
    else
    {
        vga_write_line("Usage: mod [list|load <name>|unload <name>]");
    }
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

/* Simple kernel worker that spins and yields to exercise the scheduler. */
static void stress_worker(void)
{
    for (;;)
    {
        for (volatile int i = 0; i < CONFIG_STRESS_SPIN_CYCLES; ++i)
            __asm__ __volatile__("nop");
        process_yield();
    }
}

static void command_kdlg(void)
{
    static struct klog_entry entries[CONFIG_KLOG_CAPACITY];
    size_t count = klog_copy(entries, CONFIG_KLOG_CAPACITY);
    if (count == 0)
    {
        vga_write_line("kdlg: no entries");
        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        char seq_buf[32];
        write_u64((uint64_t)entries[i].seq, seq_buf);

        const char *level = klog_level_name(entries[i].level);
        const char *text = entries[i].text;

        char line[CONFIG_KLOG_ENTRY_LEN + 32];
        size_t idx = 0;

        line[idx++] = '[';
        for (size_t j = 0; seq_buf[j] && idx < sizeof(line) - 1; ++j)
            line[idx++] = seq_buf[j];
        if (idx < sizeof(line) - 1)
            line[idx++] = ']';
        if (idx < sizeof(line) - 1)
            line[idx++] = ' ';

        for (size_t j = 0; level[j] && idx < sizeof(line) - 1; ++j)
            line[idx++] = level[j];
        if (idx < sizeof(line) - 1)
        {
            line[idx++] = ':';
            line[idx++] = ' ';
        }

        for (size_t j = 0; text[j] && idx < sizeof(line) - 1; ++j)
            line[idx++] = text[j];

        line[idx] = '\0';
        vga_write_line(line);
    }
}

static void command_kdlvl(const char *args)
{
    const char *token = skip_spaces(args);
    if (*token == '\0')
    {
        const char *name = klog_level_name(klog_get_level());
        vga_write("kdlvl: ");
        vga_write_line(name);
        return;
    }

    char buffer[12];
    size_t idx = 0;
    while (token[idx] && token[idx] != ' ' && idx + 1 < sizeof(buffer))
    {
        buffer[idx] = token[idx];
        ++idx;
    }
    buffer[idx] = '\0';

    int level = klog_level_from_name(buffer);
    if (level < 0)
    {
        vga_write_line("Usage: kdlvl [debug|info|warn|error|0-3]");
        return;
    }

    klog_set_level(level);

    const char *name = klog_level_name(level);

    char logbuf[48];
    const char *prefix = "kdlvl: level set to ";
    size_t pos = 0;
    while (prefix[pos] && pos < sizeof(logbuf) - 1)
    {
        logbuf[pos] = prefix[pos];
        ++pos;
    }
    for (size_t i = 0; name[i] && pos < sizeof(logbuf) - 1; ++i)
        logbuf[pos++] = name[i];
    logbuf[pos] = '\0';

    klog_emit(level, logbuf);

    vga_write("kdlvl set to ");
    vga_write_line(name);
}

static void command_proc_count(void)
{
    int total = process_count();
    char buffer[32];
    write_u64((uint64_t)total, buffer);
    vga_write("Processes active: ");
    vga_write_line(buffer);
}

static void command_spawn(const char *args)
{
    const char *token = skip_spaces(args);
    if (*token == '\0')
    {
        vga_write_line("Usage: spawn <count>");
        return;
    }

    char buffer[16];
    size_t idx = 0;
    while (token[idx] && token[idx] != ' ' && idx + 1 < sizeof(buffer))
    {
        buffer[idx] = token[idx];
        ++idx;
    }
    buffer[idx] = '\0';

    int requested = 0;
    if (!parse_positive_int(buffer, &requested))
    {
        vga_write_line("spawn: invalid count");
        return;
    }

    int available = MAX_PROCS - process_count();
    if (available <= 0)
    {
        vga_write_line("spawn: no slots available");
        return;
    }

    int to_create = (requested < available) ? requested : available;
    int spawned = 0;
    for (int i = 0; i < to_create; ++i)
    {
        if (process_create(stress_worker, PROC_STACK_SIZE) < 0)
            break;
        ++spawned;
    }

    char spawned_buf[32];
    char requested_buf[32];
    write_u64((uint64_t)spawned, spawned_buf);
    write_u64((uint64_t)requested, requested_buf);

    vga_write("spawn: created ");
    vga_write(spawned_buf);
    vga_write(" of ");
    vga_write(requested_buf);
    vga_write_line(" requested");

    if (spawned < requested)
        vga_write_line("spawn: limited by process capacity");

    char logbuf[80];
    const char *prefix = "spawn: requested ";
    size_t pos = 0;
    while (prefix[pos] && pos < sizeof(logbuf) - 1)
    {
        logbuf[pos] = prefix[pos];
        ++pos;
    }
    for (size_t i = 0; requested_buf[i] && pos < sizeof(logbuf) - 1; ++i)
        logbuf[pos++] = requested_buf[i];
    const char *mid = ", created ";
    for (size_t i = 0; mid[i] && pos < sizeof(logbuf) - 1; ++i)
        logbuf[pos++] = mid[i];
    for (size_t i = 0; spawned_buf[i] && pos < sizeof(logbuf) - 1; ++i)
        logbuf[pos++] = spawned_buf[i];
    logbuf[pos] = '\0';

    klog_info(logbuf);
}

static void command_shutdown(void)
{
    vga_write_line("Shutdown: powering off...");
    klog_info("shutdown: shell request");

    /* Try common ACPI power-off ports used by popular emulators. */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    outw(0x600, 0x2001);

    __asm__ __volatile__("cli");
    for (;;)
        __asm__ __volatile__("hlt");
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
    else if (shell_str_equals(line, "mod") || shell_str_starts_with(line, "mod "))
    {
        command_mod(line + 3);
    }
    else if (shell_str_equals(line, "gfx"))
    {
        command_gfx();
    }
    else if (shell_str_equals(line, "kdlg"))
    {
        command_kdlg();
    }
    else if (shell_str_equals(line, "kdlvl") || shell_str_starts_with(line, "kdlvl "))
    {
        command_kdlvl(line + 5);
    }
    else if (shell_str_equals(line, "proc_count"))
    {
        command_proc_count();
    }
    else if (shell_str_equals(line, "spawn") || shell_str_starts_with(line, "spawn "))
    {
        command_spawn(line + 5);
    }
    else if (shell_str_equals(line, "shutdown"))
    {
        command_shutdown();
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
