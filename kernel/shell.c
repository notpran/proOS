#include <stdint.h>
#include <stddef.h>
#include <limits.h>

#include "vga.h"
#include "keyboard.h"
#include "vfs.h"
#include "pit.h"
#include "io.h"
#include "proc.h"
#include "fat16.h"
#include "gfx.h"
#include "klog.h"
#include "module.h"
#include "memory.h"
#include "devmgr.h"
#include "debug.h"
#include "vbe.h"

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

static const char *resolve_absolute_path(const char *input, char *scratch, size_t scratch_size)
{
    if (!input || !scratch || scratch_size < 2)
        return NULL;
    if (input[0] == '/')
        return input;
    size_t len = str_len(input);
    if (len + 1 >= scratch_size)
        return NULL;
    scratch[0] = '/';
    for (size_t i = 0; i < len; ++i)
        scratch[i + 1] = input[i];
    scratch[len + 1] = '\0';
    return scratch;
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

static void write_hex32(uint32_t value, char *out)
{
    static const char digits[] = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 8; ++i)
    {
        unsigned nibble = (value >> ((7 - i) * 4)) & 0xFu;
        out[2 + i] = digits[nibble];
    }
    out[10] = '\0';
}

static void write_byte_hex(uint8_t value, char *out)
{
    static const char digits[] = "0123456789ABCDEF";
    out[0] = digits[(value >> 4) & 0xFu];
    out[1] = digits[value & 0xFu];
    out[2] = '\0';
}

static int parse_u32_token(const char *text, uint32_t *out_value)
{
    if (!text || !out_value || *text == '\0')
        return 0;

    uint32_t value = 0;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        text += 2;
        if (*text == '\0')
            return 0;
        while (*text)
        {
            char c = *text++;
            uint32_t digit;
            if (c >= '0' && c <= '9')
                digit = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = 10u + (uint32_t)(c - 'a');
            else if (c >= 'A' && c <= 'F')
                digit = 10u + (uint32_t)(c - 'A');
            else
                return 0;
            value = (value << 4) | digit;
        }
    }
    else
    {
        while (*text)
        {
            char c = *text++;
            if (c < '0' || c > '9')
                return 0;
            uint32_t digit = (uint32_t)(c - '0');
            uint32_t next = value * 10u + digit;
            if (next < value)
                return 0;
            value = next;
        }
    }

    *out_value = value;
    return 1;
}

static void write_hex_ptr(uintptr_t value, char *out)
{
    static const char digits[] = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    size_t nibble_count = sizeof(uintptr_t) * 2u;
    for (size_t i = 0; i < nibble_count; ++i)
    {
        size_t shift = (nibble_count - 1u - i) * 4u;
        unsigned int nibble = (unsigned int)((value >> shift) & 0xFu);
        out[2u + i] = digits[nibble];
    }
    out[2u + nibble_count] = '\0';
}

static void print_ptr_line(const char *label, uintptr_t value)
{
    char line[64];
    char hex_buf[2u + sizeof(uintptr_t) * 2u + 1u];
    write_hex_ptr(value, hex_buf);
    size_t pos = 0;
    buffer_append(line, &pos, sizeof(line), "  ");
    buffer_append(line, &pos, sizeof(line), label);
    buffer_append(line, &pos, sizeof(line), ": ");
    buffer_append(line, &pos, sizeof(line), hex_buf);
    line[pos] = '\0';
    vga_write_line(line);
}

static void print_size_line(const char *label, size_t bytes)
{
    char line[128];
    char value_buf[32];
    write_u64((uint64_t)bytes, value_buf);
    size_t pos = 0;
    buffer_append(line, &pos, sizeof(line), "  ");
    buffer_append(line, &pos, sizeof(line), label);
    buffer_append(line, &pos, sizeof(line), ": ");
    buffer_append(line, &pos, sizeof(line), value_buf);
    buffer_append(line, &pos, sizeof(line), " bytes");

    size_t kib = bytes / 1024u;
    if (kib > 0)
    {
        char kib_buf[32];
        write_u64((uint64_t)kib, kib_buf);
        buffer_append(line, &pos, sizeof(line), " (" );
        buffer_append(line, &pos, sizeof(line), kib_buf);
        buffer_append(line, &pos, sizeof(line), " KiB");

        size_t mib = bytes / (1024u * 1024u);
        if (mib > 0)
        {
            char mib_buf[32];
            write_u64((uint64_t)mib, mib_buf);
            buffer_append(line, &pos, sizeof(line), ", ");
            buffer_append(line, &pos, sizeof(line), mib_buf);
            buffer_append(line, &pos, sizeof(line), " MiB");
        }
        buffer_append(line, &pos, sizeof(line), ")");
    }

    line[pos] = '\0';
    vga_write_line(line);
}

static void command_help(void)
{
    vga_write_line("Available commands:");
    vga_write_line("  help   - show this help");
    vga_write_line("  clear  - clear the screen");
    vga_write_line("  echo   - echo text or redirect");
    vga_write_line("  mem    - memory + uptime info");
    vga_write_line("  memdump <addr> [len] - hex dump memory");
    vga_write_line("  reboot - reset the machine");
    vga_write_line("  ls     - list RAMFS files");
    vga_write_line("  cat    - print file contents");
    vga_write_line("  lsfs   - list FAT16 files");
    vga_write_line("  catfs  - print FAT16 file");
    vga_write_line("  mod    - module control (list/load/unload .kmd)");
    vga_write_line("  gfx    - draw compositor demo");
    vga_write_line("  kdlg   - show kernel log");
    vga_write_line("  kdlvl [lvl] - adjust log verbosity");
    vga_write_line("  tasks  - list processes");
    vga_write_line("  proc_count - show active process count");
    vga_write_line("  spawn <n> - stress process creation");
    vga_write_line("  devs   - list devices");
    vga_write_line("  shutdown - power off the system");
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

    char path_buffer[VFS_MAX_PATH];
    const char *target = resolve_absolute_path(filename, path_buffer, sizeof(path_buffer));
    if (!target)
    {
        vga_write_line("Invalid path.");
        return;
    }

    if (vfs_write(target, data, len) < 0)
        vga_write_line("Write failed.");
    else
        vga_write_line("OK");
}

static void command_mem(void)
{
    debug_publish_memory_info();
    vga_write_line("Memory info:");
    print_ptr_line("heap base", memory_heap_base());
    print_ptr_line("heap limit", memory_heap_limit());
    print_size_line("heap total", memory_total_bytes());
    print_size_line("heap used", memory_used_bytes());
    print_size_line("heap free", memory_free_bytes());

    uint64_t ticks = get_ticks();
    uint32_t centis = 0;
    uint64_t seconds = u64_divmod(ticks, 100U, &centis);

    char sec_buf[32];
    char centi_buf[4];
    write_u64(seconds, sec_buf);
    centi_buf[0] = (char)('0' + (centis / 10U));
    centi_buf[1] = (char)('0' + (centis % 10U));
    centi_buf[2] = '\0';

    char uptime_line[64];
    size_t pos = 0;
    buffer_append(uptime_line, &pos, sizeof(uptime_line), "  uptime: ");
    buffer_append(uptime_line, &pos, sizeof(uptime_line), sec_buf);
    buffer_append(uptime_line, &pos, sizeof(uptime_line), ".");
    buffer_append(uptime_line, &pos, sizeof(uptime_line), centi_buf);
    buffer_append(uptime_line, &pos, sizeof(uptime_line), "s");
    uptime_line[pos] = '\0';
    vga_write_line(uptime_line);

    const struct boot_info *info = boot_info_get();
    if (info)
    {
        vga_write_line("Boot assets:");

        if (info->fat_ptr != 0u && info->fat_size != 0u)
        {
            print_ptr_line("fat16 ptr", (uintptr_t)info->fat_ptr);
            print_size_line("fat16 size", (size_t)info->fat_size);
        }
        else
        {
            vga_write_line("  fat16 image: unavailable");
        }

        if (info->magic == BOOT_INFO_MAGIC && info->fb_width && info->fb_height && info->fb_bpp)
        {
            char fb_line[96];
            size_t fb_pos = 0;
            char num_buf[32];
            buffer_append(fb_line, &fb_pos, sizeof(fb_line), "  framebuffer: ");
            write_u64((uint64_t)info->fb_width, num_buf);
            buffer_append(fb_line, &fb_pos, sizeof(fb_line), num_buf);
            buffer_append(fb_line, &fb_pos, sizeof(fb_line), "x");
            write_u64((uint64_t)info->fb_height, num_buf);
            buffer_append(fb_line, &fb_pos, sizeof(fb_line), num_buf);
            buffer_append(fb_line, &fb_pos, sizeof(fb_line), "x");
            write_u64((uint64_t)info->fb_bpp, num_buf);
            buffer_append(fb_line, &fb_pos, sizeof(fb_line), num_buf);
            buffer_append(fb_line, &fb_pos, sizeof(fb_line), "bpp");
            fb_line[fb_pos] = '\0';
            vga_write_line(fb_line);

            if (info->fb_size)
                print_size_line("framebuffer size", (size_t)info->fb_size);
        }
        else
        {
            vga_write_line("  framebuffer: text mode");
        }
    }
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
    int len = vfs_list("/", list, sizeof(list));
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

    char name[VFS_NODE_NAME_MAX];
    size_t idx = 0;
    while (name_ptr[idx] && name_ptr[idx] != ' ' && idx + 1 < sizeof(name))
    {
        name[idx] = name_ptr[idx];
        ++idx;
    }
    name[idx] = '\0';

    char path_buffer[VFS_MAX_PATH];
    const char *target = resolve_absolute_path(name, path_buffer, sizeof(path_buffer));
    if (!target)
    {
        vga_write_line("Invalid path.");
        return;
    }

    char data[VFS_INLINE_CAP];
    int read = vfs_read(target, data, sizeof(data));
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
    debug_publish_task_list();
    process_debug_list();
}

static void format_device_flags(uint32_t flags, char *out)
{
    size_t idx = 0;
    out[idx++] = '[';
    if (flags & DEVICE_FLAG_PUBLISH)
        out[idx++] = 'P';
    if (flags & DEVICE_FLAG_INTERNAL)
        out[idx++] = 'I';
    out[idx++] = ']';
    out[idx] = '\0';
}

static void command_devlist(void)
{
    debug_publish_device_list();

    const struct device_node *nodes[DEVMGR_MAX_DEVICES];
    size_t count = devmgr_enumerate(nodes, DEVMGR_MAX_DEVICES);
    if (count == 0)
    {
        vga_write_line("(no devices)");
        return;
    }

    vga_write_line("ID  NAME         TYPE                FLAGS PARENT");

    for (size_t i = 0; i < count; ++i)
    {
        const struct device_node *node = nodes[i];
        char id_buf[16];
        char flags_buf[8];
        write_u64((uint64_t)node->id, id_buf);
        format_device_flags(node->flags, flags_buf);

        char line[128];
        size_t pos = 0;

        for (size_t j = 0; id_buf[j] && pos < sizeof(line) - 1; ++j)
            line[pos++] = id_buf[j];
        while (pos < 4)
            line[pos++] = ' ';
        line[pos++] = ' ';

        const char *name = node->name;
        for (size_t j = 0; name[j] && pos < sizeof(line) - 1; ++j)
            line[pos++] = name[j];
        while (pos < 18)
            line[pos++] = ' ';

        const char *type = node->type;
        for (size_t j = 0; type[j] && pos < sizeof(line) - 1; ++j)
            line[pos++] = type[j];
        while (pos < 36)
            line[pos++] = ' ';

        for (size_t j = 0; flags_buf[j] && pos < sizeof(line) - 1; ++j)
            line[pos++] = flags_buf[j];
        line[pos++] = ' ';

        const char *parent = (node->parent) ? node->parent->name : "-";
        for (size_t j = 0; parent[j] && pos < sizeof(line) - 1; ++j)
            line[pos++] = parent[j];

        line[pos] = '\0';
        vga_write_line(line);
    }
}

static void command_memdump(const char *args)
{
    const char *cursor = skip_spaces(args);
    if (*cursor == '\0')
    {
        vga_write_line("Usage: memdump <addr> [len]");
        return;
    }

    char address_token[32];
    size_t idx = 0;
    while (cursor[idx] && cursor[idx] != ' ' && idx + 1 < sizeof(address_token))
    {
        address_token[idx] = cursor[idx];
        ++idx;
    }
    address_token[idx] = '\0';

    uint32_t address = 0;
    if (!parse_u32_token(address_token, &address))
    {
        vga_write_line("memdump: invalid address");
        return;
    }

    const char *len_ptr = skip_spaces(cursor + idx);
    uint32_t length = 0;
    if (*len_ptr)
    {
        char length_token[16];
        size_t len_idx = 0;
        while (len_ptr[len_idx] && len_ptr[len_idx] != ' ' && len_idx + 1 < sizeof(length_token))
        {
            length_token[len_idx] = len_ptr[len_idx];
            ++len_idx;
        }
        length_token[len_idx] = '\0';
        if (!parse_u32_token(length_token, &length) || length == 0)
        {
            vga_write_line("memdump: invalid length");
            return;
        }
    }
    else
    {
        length = 128;
    }

    if (length > 512)
        length = 512;

    const uint8_t *ptr = (const uint8_t *)(uintptr_t)address;

    char line[96];
    for (uint32_t offset = 0; offset < length; offset += 16)
    {
        size_t pos = 0;
        uint32_t addr = address + offset;
        char addr_hex[11];
        write_hex32(addr, addr_hex);
        for (size_t j = 0; addr_hex[j] && pos < sizeof(line) - 1; ++j)
            line[pos++] = addr_hex[j];
        line[pos++] = ':';
        line[pos++] = ' ';

        for (int i = 0; i < 16; ++i)
        {
            if (offset + (uint32_t)i < length)
            {
                char byte_hex[3];
                write_byte_hex(ptr[offset + (uint32_t)i], byte_hex);
                if (pos + 2 >= sizeof(line))
                    break;
                line[pos++] = byte_hex[0];
                line[pos++] = byte_hex[1];
            }
            else
            {
                if (pos + 2 >= sizeof(line))
                    break;
                line[pos++] = ' ';
                line[pos++] = ' ';
            }
            if (pos < sizeof(line) - 1)
                line[pos++] = ' ';
        }

        line[pos++] = ' ';
        line[pos++] = '|';
        for (int i = 0; i < 16 && pos < sizeof(line) - 1; ++i)
        {
            if (offset + (uint32_t)i >= length)
            {
                line[pos++] = ' ';
                continue;
            }
            uint8_t c = ptr[offset + (uint32_t)i];
            if (c < 32 || c > 126)
                c = '.';
            line[pos++] = (char)c;
        }
        if (pos < sizeof(line) - 1)
            line[pos++] = '|';
        line[pos] = '\0';
        vga_write_line(line);
    }
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
        const char *module = entries[i].module[0] ? entries[i].module : "kernel";

        char line[CONFIG_KLOG_ENTRY_LEN + CONFIG_KLOG_MODULE_NAME_LEN + 48];
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
            line[idx++] = ' ';

        line[idx++] = '(';
        for (size_t j = 0; module[j] && idx < sizeof(line) - 1; ++j)
            line[idx++] = module[j];
        if (idx < sizeof(line) - 1)
            line[idx++] = ')';
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

    char first_token[CONFIG_KLOG_MODULE_NAME_LEN];
    size_t idx = 0;
    while (token[idx] && token[idx] != ' ' && idx + 1 < sizeof(first_token))
    {
        first_token[idx] = token[idx];
        ++idx;
    }
    first_token[idx] = '\0';

    const char *rest = skip_spaces(token + idx);
    if (*rest == '\0')
    {
        int level = klog_level_from_name(first_token);
        if (level < 0)
        {
            vga_write_line("Usage: kdlvl [level|<module> <level|inherit>]");
            return;
        }

        klog_set_level(level);
        const char *name = klog_level_name(level);

        char logbuf[64];
        const char *prefix = "kdlvl: global level ";
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

        vga_write("Global klog level set to ");
        vga_write_line(name);
        return;
    }

    char module_name[CONFIG_KLOG_MODULE_NAME_LEN];
    size_t module_len = 0;
    while (first_token[module_len] && module_len + 1 < sizeof(module_name))
    {
        module_name[module_len] = first_token[module_len];
        ++module_len;
    }
    module_name[module_len] = '\0';

    char level_token[16];
    size_t level_idx = 0;
    while (rest[level_idx] && rest[level_idx] != ' ' && level_idx + 1 < sizeof(level_token))
    {
        level_token[level_idx] = rest[level_idx];
        ++level_idx;
    }
    level_token[level_idx] = '\0';

    int level = klog_level_from_name(level_token);
    if (level < 0)
    {
        if ((level_token[0] == 'i' || level_token[0] == 'I') &&
            (level_token[1] == 'n' || level_token[1] == 'N') &&
            (level_token[2] == 'h' || level_token[2] == 'H'))
        {
            level = KLOG_LEVEL_INHERIT;
        }
        else
        {
            vga_write_line("Usage: kdlvl [level|<module> <level|inherit>]");
            return;
        }
    }

    if (klog_module_set_level(module_name, level) < 0)
    {
        vga_write_line("kdlvl: failed to update module level");
        return;
    }

    if (level == KLOG_LEVEL_INHERIT)
    {
        vga_write("Module ");
        vga_write(module_name);
        vga_write_line(" level reset to inherit");
    }
    else
    {
        const char *name = klog_level_name(level);
        vga_write("Module ");
        vga_write(module_name);
        vga_write(" level set to ");
        vga_write_line(name);
    }
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
    debug_publish_memory_info();

    uint64_t total = (uint64_t)memory_total_bytes();
    uint64_t used = (uint64_t)memory_used_bytes();
    uint64_t free_space = (uint64_t)memory_free_bytes();
    uint32_t base = (uint32_t)memory_heap_base();
    uint32_t limit = (uint32_t)memory_heap_limit();
    uint32_t cursor = base + (uint32_t)used;

    char total_buf[32];
    char total_kb[32];
    char used_buf[32];
    char used_kb[32];
    char free_buf[32];
    char free_kb[32];
    char base_hex[11];
    char limit_hex[11];
    char cursor_hex[11];

    write_u64(total, total_buf);
    write_u64(total / 1024u, total_kb);
    write_u64(used, used_buf);
    write_u64(used / 1024u, used_kb);
    write_u64(free_space, free_buf);
    write_u64(free_space / 1024u, free_kb);
    write_hex32(base, base_hex);
    write_hex32(limit, limit_hex);
    write_hex32(cursor, cursor_hex);

    vga_write_line("Memory statistics:");

    vga_write("  Total: ");
    vga_write(total_buf);
    vga_write(" bytes (");
    vga_write(total_kb);
    vga_write_line(" KB)");

    vga_write("  Used : ");
    vga_write(used_buf);
    vga_write(" bytes (");
    vga_write(used_kb);
    vga_write_line(" KB)");

    vga_write("  Free : ");
    vga_write(free_buf);
    vga_write(" bytes (");
    vga_write(free_kb);
    vga_write_line(" KB)");

    vga_write("  Heap base   = ");
    vga_write_line(base_hex);
    vga_write("  Heap cursor = ");
    vga_write_line(cursor_hex);
    vga_write("  Heap limit  = ");
    vga_write_line(limit_hex);

    uint64_t ticks = get_ticks();
    uint32_t centis = 0;
    uint64_t seconds = u64_divmod(ticks, 100U, &centis);

    char sec_buf[32];
    char centi_buf[4];
    write_u64(seconds, sec_buf);
    centi_buf[0] = (char)('0' + (centis / 10U));
    centi_buf[1] = (char)('0' + (centis % 10U));
    centi_buf[2] = '\0';

    vga_write("  Uptime: ");
    vga_write(sec_buf);
    vga_write(".");
    vga_write(centi_buf);
    vga_write_line("s");

    vga_write_line("Shutdown requested (not implemented).");
    klog_warn("shell: shutdown requested");
}

static void shell_execute(char *line)
{
    if (!line)
        return;

    const char *trimmed = skip_spaces(line);
    if (*trimmed == '\0')
        return;

    char *cursor = (char *)trimmed;
    trim_trailing_spaces(cursor);

    if (shell_str_equals(cursor, "help"))
    {
        command_help();
    }
    else if (shell_str_equals(cursor, "clear"))
    {
        command_clear();
    }
    else if (shell_str_equals(cursor, "mem"))
    {
        command_mem();
    }
    else if (shell_str_equals(cursor, "reboot"))
    {
        command_reboot();
    }
    else if (shell_str_equals(cursor, "ls"))
    {
        command_ls();
    }
    else if (shell_str_equals(cursor, "catfs") || shell_str_starts_with(cursor, "catfs "))
    {
        command_catfs(cursor + 5);
    }
    else if (shell_str_equals(cursor, "cat") || shell_str_starts_with(cursor, "cat "))
    {
        command_cat(cursor + 3);
    }
    else if (shell_str_equals(cursor, "tasks") || shell_str_equals(cursor, "proc_list"))
    {
        command_proc_list();
    }
    else if (shell_str_equals(cursor, "lsfs"))
    {
        command_lsfs();
    }
    else if (shell_str_equals(cursor, "mod") || shell_str_starts_with(cursor, "mod "))
    {
        command_mod(cursor + 3);
    }
    else if (shell_str_equals(cursor, "gfx"))
    {
        command_gfx();
    }
    else if (shell_str_equals(cursor, "kdlg"))
    {
        command_kdlg();
    }
    else if (shell_str_equals(cursor, "kdlvl") || shell_str_starts_with(cursor, "kdlvl "))
    {
        command_kdlvl(cursor + 5);
    }
    else if (shell_str_equals(cursor, "proc_count"))
    {
        command_proc_count();
    }
    else if (shell_str_equals(cursor, "spawn") || shell_str_starts_with(cursor, "spawn "))
    {
        command_spawn(cursor + 5);
    }
    else if (shell_str_equals(cursor, "devs"))
    {
        command_devlist();
    }
    else if (shell_str_equals(cursor, "shutdown"))
    {
        command_shutdown();
    }
    else if (shell_str_equals(cursor, "memdump") || shell_str_starts_with(cursor, "memdump "))
    {
        command_memdump(cursor + 7);
    }
    else if (shell_str_equals(cursor, "echo") || shell_str_starts_with(cursor, "echo "))
    {
        command_echo(cursor + 4);
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
