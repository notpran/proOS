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
#include "power.h"
#include "devmgr.h"
#include "debug.h"
#include "vbe.h"
#include "volmgr.h"
#include "net.h"
#include "arp.h"
#include "ipv4.h"
#include "icmp.h"
#include "pic.h"

#define SHELL_PROMPT "proOS >> "
#define INPUT_MAX 256
#define SHELL_HISTORY_CAPACITY 32
#define SHELL_TREE_MAX_DEPTH 8
#define SHELL_PING_DEFAULT_COUNT 4
#define SHELL_PING_MAX_COUNT 100
#define SHELL_PING_TIMEOUT_TICKS 250u
#define SHELL_PING_INTERVAL_TICKS 75u
#define SHELL_PING_ARP_WAIT_TICKS 250u

static char shell_history[SHELL_HISTORY_CAPACITY][INPUT_MAX];
static size_t shell_history_count = 0;
static size_t shell_history_next = 0;
static char shell_cwd[VFS_MAX_PATH] = "/";

static void shell_sleep_ticks(uint32_t ticks)
{
    __asm__ __volatile__("sti");
    process_sleep(ticks);
}

static void shell_yield(void)
{
    __asm__ __volatile__("sti");
    process_yield();
}

static void shell_recover_input(void)
{
    __asm__ __volatile__("sti");
    pic_clear_mask(0);
    pic_clear_mask(1);
}

static uint32_t shell_read_flags(void)
{
    uint32_t flags;
    __asm__ __volatile__("pushf\n\tpop %0" : "=r"(flags));
    return flags;
}

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

static const char *history_get_latest(size_t offset)
{
    if (offset >= shell_history_count)
        return NULL;
    size_t index = (shell_history_next + SHELL_HISTORY_CAPACITY - 1u - offset) % SHELL_HISTORY_CAPACITY;
    return shell_history[index];
}

static void history_store(const char *line)
{
    if (!line || line[0] == '\0')
        return;

    size_t len = str_len(line);
    if (len == 0)
        return;
    if (len >= INPUT_MAX)
        len = INPUT_MAX - 1u;

    if (shell_history_count > 0)
    {
        size_t last_index = (shell_history_next + SHELL_HISTORY_CAPACITY - 1u) % SHELL_HISTORY_CAPACITY;
        if (shell_str_equals(shell_history[last_index], line))
            return;
    }

    for (size_t i = 0; i < len; ++i)
        shell_history[shell_history_next][i] = line[i];
    shell_history[shell_history_next][len] = '\0';

    shell_history_next = (shell_history_next + 1u) % SHELL_HISTORY_CAPACITY;
    if (shell_history_count < SHELL_HISTORY_CAPACITY)
        ++shell_history_count;
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

static int shell_normalize_absolute(const char *input, char *out, size_t capacity)
{
    if (!input || !out || capacity < 2)
        return -1;
    if (input[0] != '/')
        return -1;

    const char *segments[32];
    size_t seg_lengths[32];
    size_t seg_count = 0;
    size_t i = 0;

    while (input[i])
    {
        while (input[i] == '/')
            ++i;
        if (input[i] == '\0')
            break;
        size_t start = i;
        while (input[i] && input[i] != '/')
            ++i;
        size_t len = i - start;
        if (len == 0)
            continue;
        if (len == 1 && input[start] == '.')
            continue;
        if (len == 2 && input[start] == '.' && input[start + 1] == '.')
        {
            if (seg_count > 0)
                --seg_count;
            continue;
        }
        if (seg_count >= 32)
            return -1;
        segments[seg_count] = &input[start];
        seg_lengths[seg_count] = len;
        ++seg_count;
    }

    size_t out_pos = 0;
    if (seg_count == 0)
    {
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }

    for (size_t seg = 0; seg < seg_count; ++seg)
    {
        size_t len = seg_lengths[seg];
        if (out_pos + len + 1 >= capacity)
            return -1;
        out[out_pos++] = '/';
        for (size_t j = 0; j < len; ++j)
            out[out_pos++] = segments[seg][j];
    }
    out[out_pos] = '\0';
    return 0;
}

static void shell_replace_input(char *buffer, size_t *len, size_t max_len, const char *text)
{
    if (!buffer || !len || !text || max_len == 0)
        return;

    while (*len > 0)
    {
        --(*len);
        vga_backspace();
    }

    size_t copy = 0;
    while (text[copy] && copy + 1 < max_len)
    {
        buffer[copy] = text[copy];
        vga_write_char(text[copy]);
        ++copy;
    }

    *len = copy;
    if (*len < max_len)
        buffer[*len] = '\0';
}

static const char *resolve_absolute_path(const char *input, char *scratch, size_t scratch_size)
{
    if (!scratch || scratch_size < 2)
        return NULL;

    if (!input || *input == '\0')
    {
        if (shell_normalize_absolute(shell_cwd, scratch, scratch_size) < 0)
            return NULL;
        return scratch;
    }

    if (input[0] == '/')
    {
        if (shell_normalize_absolute(input, scratch, scratch_size) < 0)
            return NULL;
        return scratch;
    }

    char candidate[VFS_MAX_PATH];
    size_t pos = 0;
    size_t cwd_len = str_len(shell_cwd);
    if (cwd_len == 0)
        return NULL;

    if (cwd_len == 1 && shell_cwd[0] == '/')
    {
        candidate[pos++] = '/';
    }
    else
    {
        if (cwd_len >= sizeof(candidate))
            return NULL;
        for (size_t i = 0; i < cwd_len && pos + 1 < sizeof(candidate); ++i)
            candidate[pos++] = shell_cwd[i];
        if (candidate[pos - 1] != '/' && pos + 1 < sizeof(candidate))
            candidate[pos++] = '/';
    }

    size_t idx = 0;
    while (input[idx] && pos + 1 < sizeof(candidate))
        candidate[pos++] = input[idx++];

    if (input[idx])
        return NULL;

    candidate[pos] = '\0';

    if (shell_normalize_absolute(candidate, scratch, scratch_size) < 0)
        return NULL;

    return scratch;
}

static void shell_set_cwd(const char *path)
{
    if (!path)
        return;
    if (shell_normalize_absolute(path, shell_cwd, sizeof(shell_cwd)) < 0)
        return;
}

static int segment_equals(const char *segment, size_t len, const char *text)
{
    size_t idx = 0;
    while (idx < len && text[idx])
    {
        if (segment[idx] != text[idx])
            return 0;
        ++idx;
    }
    return idx == len && text[idx] == '\0';
}

static int shell_join_paths(const char *parent, const char *name, char *out, size_t capacity)
{
    if (!parent || !name || !out || capacity < 2)
        return -1;

    char temp[VFS_MAX_PATH];
    size_t pos = 0;
    size_t parent_len = str_len(parent);
    if (parent_len == 0)
        return -1;

    if (parent_len == 1 && parent[0] == '/')
    {
        if (pos + 1 >= sizeof(temp))
            return -1;
        temp[pos++] = '/';
    }
    else
    {
        if (parent_len >= sizeof(temp))
            return -1;
        for (size_t i = 0; i < parent_len && pos + 1 < sizeof(temp); ++i)
            temp[pos++] = parent[i];
        if (temp[pos - 1] != '/')
        {
            if (pos + 1 >= sizeof(temp))
                return -1;
            temp[pos++] = '/';
        }
    }

    size_t idx = 0;
    while (name[idx] && pos + 1 < sizeof(temp))
        temp[pos++] = name[idx++];

    if (name[idx])
        return -1;

    temp[pos] = '\0';
    return shell_normalize_absolute(temp, out, capacity);
}

static void shell_tree_print_line(int depth, const char *name, int is_dir)
{
    if (!name)
        return;

    char line[160];
    size_t pos = 0;
    for (int i = 0; i < depth; ++i)
    {
        if (pos + 2 >= sizeof(line))
            break;
        line[pos++] = '|';
        line[pos++] = ' ';
    }
    if (pos + 3 < sizeof(line))
    {
        line[pos++] = '+';
        line[pos++] = '-';
        line[pos++] = ' ';
    }
    size_t name_len = str_len(name);
    for (size_t i = 0; i < name_len && pos + 1 < sizeof(line); ++i)
        line[pos++] = name[i];
    if (is_dir && pos + 1 < sizeof(line))
        line[pos++] = '/';
    line[pos] = '\0';
    vga_write_line(line);
}

static void shell_tree_walk(const char *path, int depth)
{
    if (!path)
        return;

    char listing[VFS_INLINE_CAP];
    int listed = vfs_list(path, listing, sizeof(listing));
    if (listed <= 0)
        return;

    char *cursor = listing;
    while (*cursor)
    {
        char *entry = cursor;
        while (*cursor && *cursor != '\n')
            ++cursor;
        if (*cursor == '\n')
        {
            *cursor = '\0';
            ++cursor;
        }

        if (entry[0] == '\0')
            continue;

        int is_dir = 0;
        size_t entry_len = str_len(entry);
        if (entry_len > 0 && entry[entry_len - 1] == '/')
        {
            is_dir = 1;
            entry[entry_len - 1] = '\0';
            --entry_len;
        }

        if (entry_len == 0)
            continue;

        char absolute[VFS_MAX_PATH];
        if (shell_join_paths(path, entry, absolute, sizeof(absolute)) < 0)
            continue;

        shell_tree_print_line(depth, entry, is_dir);

        if (is_dir)
        {
            if (depth + 1 >= SHELL_TREE_MAX_DEPTH)
            {
                shell_tree_print_line(depth + 1, "...", 0);
                continue;
            }
            shell_tree_walk(absolute, depth + 1);
        }
    }
}

static void shell_describe_location(char *volume, size_t volume_cap, char *relative, size_t relative_cap)
{
    if (!volume || volume_cap == 0 || !relative || relative_cap == 0)
        return;

    volume[0] = '\0';
    relative[0] = '\0';

    const char *cwd = shell_cwd;
    if (!cwd || cwd[0] == '\0')
        return;

    if (shell_str_equals(cwd, "/"))
    {
        const char *root = "Root";
        size_t pos = 0;
        while (root[pos] && pos + 1 < volume_cap)
        {
            volume[pos] = root[pos];
            ++pos;
        }
        volume[pos] = '\0';
        if (relative_cap > 1)
        {
            relative[0] = '/';
            relative[1] = '\0';
        }
        return;
    }

    const char *cursor = cwd;
    if (*cursor == '/')
        ++cursor;
    const char *first = cursor;
    size_t first_len = 0;
    while (first[first_len] && first[first_len] != '/')
        ++first_len;

    if (first_len == 0)
        return;

    const char *after_first = first + first_len;
    if (segment_equals(first, first_len, "Volumes"))
    {
        if (*after_first == '/')
            ++after_first;
        if (*after_first == '\0')
        {
            const char *volumes = "Volumes";
            size_t pos = 0;
            while (volumes[pos] && pos + 1 < volume_cap)
            {
                volume[pos] = volumes[pos];
                ++pos;
            }
            volume[pos] = '\0';
            if (relative_cap > 1)
            {
                relative[0] = '/';
                relative[1] = '\0';
            }
            return;
        }

        const char *second = after_first;
        size_t second_len = 0;
        while (second[second_len] && second[second_len] != '/')
            ++second_len;

        size_t copy_len = (second_len < volume_cap - 1) ? second_len : (volume_cap - 1);
        for (size_t i = 0; i < copy_len; ++i)
            volume[i] = second[i];
        volume[copy_len] = '\0';

        const char *rest = second + second_len;
        if (*rest == '/')
            ++rest;
        if (*rest == '\0')
        {
            if (relative_cap > 1)
            {
                relative[0] = '/';
                relative[1] = '\0';
            }
        }
        else
        {
            size_t pos = 0;
            if (relative_cap > 1)
            {
                relative[pos++] = '/';
                size_t idx = 0;
                while (rest[idx] && pos < relative_cap - 1)
                    relative[pos++] = rest[idx++];
                relative[pos] = '\0';
            }
        }
        return;
    }

    size_t volume_copy = (first_len < volume_cap - 1) ? first_len : (volume_cap - 1);
    for (size_t i = 0; i < volume_copy; ++i)
        volume[i] = first[i];
    volume[volume_copy] = '\0';

    const char *rest = after_first;
    if (*rest == '/')
        ++rest;
    if (*rest == '\0')
    {
        if (relative_cap > 1)
        {
            relative[0] = '/';
            relative[1] = '\0';
        }
    }
    else
    {
        size_t pos = 0;
        if (relative_cap > 1)
        {
            relative[pos++] = '/';
            size_t idx = 0;
            while (rest[idx] && pos < relative_cap - 1)
                relative[pos++] = rest[idx++];
            relative[pos] = '\0';
        }
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
    int history_pos = -1;

    /* Force full interrupt recovery before reading input */
    __asm__ __volatile__("sti" ::: "memory");
    pic_clear_mask(0);
    pic_clear_mask(1);

    while (1)
    {
        char c = kb_getchar();
        if (!c)
        {
            if (kb_poll())
            {
                c = kb_getchar();
                if (!c)
                    continue;
            }
            else
            {
                uint32_t flags = shell_read_flags();
                if ((flags & (1u << 9)) == 0)
                {
                    shell_recover_input();
                    continue;
                }

                __asm__ __volatile__("sti\n\thlt");
                continue;
            }
        }

        unsigned char uc = (unsigned char)c;

        if (uc == (unsigned char)KB_KEY_ARROW_UP)
        {
            if (shell_history_count > 0)
            {
                if (history_pos + 1 < (int)shell_history_count)
                    ++history_pos;
                const char *entry = history_get_latest((size_t)history_pos);
                if (entry)
                    shell_replace_input(buffer, &len, max_len, entry);
            }
            continue;
        }

        if (uc == (unsigned char)KB_KEY_ARROW_DOWN)
        {
            if (shell_history_count > 0 && history_pos >= 0)
            {
                if (history_pos > 0)
                {
                    --history_pos;
                    const char *entry = history_get_latest((size_t)history_pos);
                    if (entry)
                        shell_replace_input(buffer, &len, max_len, entry);
                }
                else
                {
                    history_pos = -1;
                    shell_replace_input(buffer, &len, max_len, "");
                }
            }
            continue;
        }

        if (uc == (unsigned char)KB_KEY_ARROW_LEFT || uc == (unsigned char)KB_KEY_ARROW_RIGHT)
            continue;

        if (c == '\b')
        {
            if (len > 0)
            {
                --len;
                vga_backspace();
                buffer[len] = '\0';
            }
            history_pos = -1;
            continue;
        }

        if (c == '\n')
        {
            vga_write_char('\n');
            buffer[len] = '\0';
            history_pos = -1;
            return len;
        }

        if (c == '\t')
            c = ' ';

        if (len + 1 < max_len)
        {
            buffer[len++] = c;
            buffer[len] = '\0';
            vga_write_char(c);
            history_pos = -1;
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

static void format_mac_address(const uint8_t mac[6], char *out, size_t capacity)
{
    if (!mac || !out || capacity == 0)
        return;

    size_t pos = 0;
    for (size_t i = 0; i < 6 && pos + 2 < capacity; ++i)
    {
        char hex[3];
        write_byte_hex(mac[i], hex);
        if (pos + 2 >= capacity)
            break;
        out[pos++] = hex[0];
        out[pos++] = hex[1];
        if (i < 5 && pos < capacity - 1)
            out[pos++] = ':';
    }

    if (pos >= capacity)
        pos = capacity - 1;
    out[pos] = '\0';
}

static void format_ipv4_address(const uint8_t ipv4[4], char *out, size_t capacity)
{
    if (!ipv4 || !out || capacity == 0)
        return;

    size_t pos = 0;
    for (size_t i = 0; i < 4; ++i)
    {
        unsigned int value = ipv4[i];
        char digits[3];
        size_t digit_count = 0;

        if (value == 0)
        {
            if (pos < capacity - 1)
                out[pos++] = '0';
        }
        else
        {
            while (value > 0 && digit_count < sizeof(digits))
            {
                digits[digit_count++] = (char)('0' + (value % 10u));
                value /= 10u;
            }

            while (digit_count > 0 && pos < capacity - 1)
                out[pos++] = digits[--digit_count];
        }

        if (i < 3 && pos < capacity - 1)
            out[pos++] = '.';
    }
    out[pos] = '\0';
}

static int parse_ipv4_address(const char *text, uint8_t out[4])
{
    if (!text || !out)
        return 0;

    uint32_t value = 0;
    int segment = 0;
    int digits = 0;

    while (*text)
    {
        char c = *text++;
        if (c == '.')
        {
            if (segment >= 3 || digits == 0 || value > 255u)
                return 0;
            out[segment++] = (uint8_t)value;
            value = 0;
            digits = 0;
            continue;
        }

        if (c < '0' || c > '9')
            return 0;

        value = value * 10u + (uint32_t)(c - '0');
        if (value > 255u)
            return 0;
        ++digits;
    }

    if (segment != 3 || digits == 0 || value > 255u)
        return 0;

    out[segment] = (uint8_t)value;
    return 1;
}

static struct net_device *shell_default_net_device(void)
{
    if (net_device_count() == 0)
        return NULL;
    return net_get_device(0);
}

static void shell_flush_icmp_notifications(void)
{
    uint16_t identifier;
    uint16_t sequence;
    uint8_t src[4];

    while (icmp_take_any_echo_reply(&identifier, &sequence, src))
    {
        char ip_text[32];
        format_ipv4_address(src, ip_text, sizeof(ip_text));

        char seq_buf[32];
        write_u64((uint64_t)sequence, seq_buf);

        char id_buf[32];
        write_hex32((uint32_t)identifier, id_buf);

        vga_write("net: echo reply from ");
        vga_write(ip_text);
        vga_write(" seq=");
        vga_write(seq_buf);
        vga_write(" id=");
        vga_write_line(id_buf);
    }
}

static int shell_ping_wait_for_arp(struct net_device *dev, const uint8_t target[4], uint64_t deadline)
{
    if (!dev || !target)
        return -1;

    uint8_t mac_dummy[6];

    while (1)
    {
        if (arp_cache_lookup(dev, target, mac_dummy))
            return 1;

        uint64_t now = get_ticks();
        if (now >= deadline)
            break;

        int events = net_poll_devices();
        if (events < 0)
            return -1;

        if (arp_cache_lookup(dev, target, mac_dummy))
            return 1;

        if (events == 0)
            shell_sleep_ticks(1);
        else
            shell_yield();
    }

    return arp_cache_lookup(dev, target, mac_dummy) ? 1 : 0;
}

static int shell_ping_transmit(struct net_device *dev, const uint8_t target[4], uint16_t identifier, uint16_t sequence, uint64_t deadline, uint64_t *sent_tick)
{
    if (!dev || !target)
        return -1;

    while (1)
    {
        int rc = icmp_send_echo_request(dev, target, identifier, sequence);
        if (rc == 0)
        {
            if (sent_tick)
                *sent_tick = get_ticks();
            return 1;
        }

        if (rc < 0)
            return -1;

        int arp_status = shell_ping_wait_for_arp(dev, target, deadline);
        if (arp_status <= 0)
            return arp_status;
    }
}

static int shell_ping_wait_for_reply(uint16_t identifier, uint16_t sequence, uint64_t send_tick, uint64_t deadline, uint32_t *rtt_ms, uint8_t src_ipv4[4])
{
    while (1)
    {
        if (icmp_take_echo_reply(identifier, sequence, src_ipv4))
        {
            uint64_t now = get_ticks();
            if (now < send_tick)
                now = send_tick;
            if (rtt_ms)
            {
                uint64_t ticks_elapsed = now - send_tick;
                *rtt_ms = (uint32_t)(ticks_elapsed * 10u);
            }
            return 1;
        }

        uint64_t now = get_ticks();
        if (now >= deadline)
            break;

        int events = net_poll_devices();
        if (events < 0)
            return -1;

        if (icmp_take_echo_reply(identifier, sequence, src_ipv4))
        {
            uint64_t recv_tick = get_ticks();
            if (recv_tick < send_tick)
                recv_tick = send_tick;
            if (rtt_ms)
            {
                uint64_t ticks_elapsed = recv_tick - send_tick;
                *rtt_ms = (uint32_t)(ticks_elapsed * 10u);
            }
            return 1;
        }

        if (events == 0)
            shell_sleep_ticks(1);
        else
            shell_yield();
    }

    return 0;
}

static uint16_t shell_ping_identifier = 0x1234u;
static uint16_t shell_ping_sequence;

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
    vga_write_line("  ls [path] - list directory contents");
    vga_write_line("  tree [path] - display directory tree");
    vga_write_line("  cd [path] - change working directory");
    vga_write_line("  whereami - show current volume/path");
    vga_write_line("  cat <file> - print file contents");
    vga_write_line("  mkdir <path> - create directory");
    vga_write_line("  touch <path> - create empty file");
    vga_write_line("  rm <path> - remove file or directory");
    vga_write_line("  volumes - list detected volumes");
    vga_write_line("  mount - list active mounts");
    vga_write_line("  mod    - module control (list/load/unload .kmd)");
    vga_write_line("  net    - networking utilities");
    vga_write_line("  gfx    - draw compositor demo");
    vga_write_line("  kdlg   - show kernel log");
    vga_write_line("  logs <name> - view logs (kernel|net|ipc)");
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

    if (vfs_append(target, data, len) < 0)
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
        __asm__ __volatile__("sti\n\thlt");
}

static void command_ls(const char *args)
{
    char list[512];
    char path[VFS_MAX_PATH];
    char absolute[VFS_MAX_PATH];

    const char *trimmed = skip_spaces(args ? args : "");
    const char *target = shell_cwd;

    if (*trimmed)
    {
        size_t idx = 0;
        while (trimmed[idx] && trimmed[idx] != ' ' && idx + 1 < sizeof(path))
        {
            path[idx] = trimmed[idx];
            ++idx;
        }

        if (trimmed[idx] != '\0' && trimmed[idx] != ' ')
        {
            vga_write_line("ls: path too long");
            return;
        }

        path[idx] = '\0';

        const char *rest = skip_spaces(trimmed + idx);
        if (*rest)
        {
            vga_write_line("ls: too many arguments");
            return;
        }

        const char *resolved = resolve_absolute_path(path, absolute, sizeof(absolute));
        if (!resolved)
        {
            vga_write_line("ls: invalid path");
            return;
        }

        target = resolved;
    }

    int len = vfs_list(target, list, sizeof(list));
    if (len <= 0)
    {
        if (len < 0)
            vga_write_line("ls: path not found");
        else
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

static void command_tree(const char *args)
{
    char provided[VFS_MAX_PATH];
    char absolute[VFS_MAX_PATH];

    const char *trimmed = skip_spaces(args ? args : "");
    const char *target = resolve_absolute_path(NULL, absolute, sizeof(absolute));
    if (!target)
    {
        vga_write_line("tree: unable to resolve current directory");
        return;
    }

    if (*trimmed)
    {
        size_t idx = 0;
        while (trimmed[idx] && trimmed[idx] != ' ' && idx + 1 < sizeof(provided))
        {
            provided[idx] = trimmed[idx];
            ++idx;
        }
        if (trimmed[idx] != '\0' && trimmed[idx] != ' ')
        {
            vga_write_line("tree: path too long");
            return;
        }
        provided[idx] = '\0';

        const char *rest = skip_spaces(trimmed + idx);
        if (*rest)
        {
            vga_write_line("Usage: tree [path]");
            return;
        }

        target = resolve_absolute_path(provided, absolute, sizeof(absolute));
        if (!target)
        {
            vga_write_line("tree: invalid path");
            return;
        }
    }

    char probe[64];
    int listed = vfs_list(target, probe, sizeof(probe));
    if (listed < 0)
    {
        int fd = vfs_open(target);
        if (fd >= 0)
        {
            vfs_close(fd);
            vga_write_line("tree: not a directory");
        }
        else
        {
            vga_write_line("tree: path not found");
        }
        return;
    }

    vga_write_line(target);
    if (listed > 0)
        shell_tree_walk(target, 0);
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

    int fd = vfs_open(target);
    if (fd < 0)
    {
        vga_write_line("File not found.");
        return;
    }

    char data[VFS_INLINE_CAP];
    int read = vfs_read(fd, data, sizeof(data) - 1u);
    if (read < 0)
    {
        vfs_close(fd);
        vga_write_line("File not readable.");
        return;
    }

    if ((size_t)read >= sizeof(data))
        read = (int)(sizeof(data) - 1u);
    data[read] = '\0';
    vfs_close(fd);

    vga_write_line(data);
}

static void command_cd(const char *args)
{
    const char *token = skip_spaces(args ? args : "");
    if (*token == '\0')
    {
        vga_write_line(shell_cwd);
        return;
    }

    char target[VFS_MAX_PATH];
    size_t idx = 0;
    while (token[idx] && token[idx] != ' ' && idx + 1 < sizeof(target))
    {
        target[idx] = token[idx];
        ++idx;
    }

    if (token[idx] != '\0' && token[idx] != ' ')
    {
        vga_write_line("cd: path too long");
        return;
    }

    target[idx] = '\0';

    const char *rest = skip_spaces(token + idx);
    if (*rest)
    {
        vga_write_line("cd: too many arguments");
        return;
    }

    char absolute[VFS_MAX_PATH];
    const char *resolved = resolve_absolute_path(target, absolute, sizeof(absolute));
    if (!resolved)
    {
        vga_write_line("cd: invalid path");
        return;
    }

    char probe[64];
    int list = vfs_list(resolved, probe, sizeof(probe));
    if (list < 0)
    {
        int fd = vfs_open(resolved);
        if (fd >= 0)
        {
            vfs_close(fd);
            vga_write_line("cd: not a directory");
        }
        else
        {
            vga_write_line("cd: no such path");
        }
        return;
    }

    shell_set_cwd(resolved);
}

static void command_whereami(void)
{
    char volume[32];
    char rel[VFS_MAX_PATH];
    shell_describe_location(volume, sizeof(volume), rel, sizeof(rel));

    if (volume[0] == '\0')
    {
        const char *fallback = "Root";
        size_t pos = 0;
        while (fallback[pos] && pos + 1 < sizeof(volume))
        {
            volume[pos] = fallback[pos];
            ++pos;
        }
        volume[pos] = '\0';
    }

    const char *path_display = rel;
    if (path_display[0] == '\0')
    {
        path_display = shell_cwd;
        if (!path_display || path_display[0] == '\0')
            path_display = "/";
    }

    char line[160];
    size_t pos = 0;
    buffer_append(line, &pos, sizeof(line), "Volume: ");
    buffer_append(line, &pos, sizeof(line), volume);
    buffer_append(line, &pos, sizeof(line), " Path: ");
    buffer_append(line, &pos, sizeof(line), path_display);
    line[pos] = '\0';
    vga_write_line(line);
}

static int shell_copy_token(const char *input, char *buffer, size_t capacity)
{
    size_t idx = 0;
    while (input[idx] && input[idx] != ' ' && idx + 1 < capacity)
    {
        buffer[idx] = input[idx];
        ++idx;
    }
    if (input[idx] != '\0' && input[idx] != ' ')
        return 0;
    buffer[idx] = '\0';
    return 1;
}

static void command_mkdir(const char *args)
{
    const char *token = skip_spaces(args ? args : "");
    if (*token == '\0')
    {
        vga_write_line("Usage: mkdir <path>");
        return;
    }

    char target[VFS_MAX_PATH];
    if (!shell_copy_token(token, target, sizeof(target)))
    {
        vga_write_line("mkdir: path too long");
        return;
    }

    const char *rest = skip_spaces(token + str_len(target));
    if (*rest)
    {
        vga_write_line("mkdir: too many arguments");
        return;
    }

    char absolute[VFS_MAX_PATH];
    const char *resolved = resolve_absolute_path(target, absolute, sizeof(absolute));
    if (!resolved)
    {
        vga_write_line("mkdir: invalid path");
        return;
    }

    if (vfs_mkdir(resolved) == 0)
        vga_write_line("Directory created.");
    else
        vga_write_line("mkdir: failed");
}

static void command_touch(const char *args)
{
    const char *token = skip_spaces(args ? args : "");
    if (*token == '\0')
    {
        vga_write_line("Usage: touch <path>");
        return;
    }

    char target[VFS_MAX_PATH];
    if (!shell_copy_token(token, target, sizeof(target)))
    {
        vga_write_line("touch: path too long");
        return;
    }

    const char *rest = skip_spaces(token + str_len(target));
    if (*rest)
    {
        vga_write_line("touch: too many arguments");
        return;
    }

    char absolute[VFS_MAX_PATH];
    const char *resolved = resolve_absolute_path(target, absolute, sizeof(absolute));
    if (!resolved)
    {
        vga_write_line("touch: invalid path");
        return;
    }

    int fd = vfs_open(resolved);
    if (fd >= 0)
    {
        vfs_close(fd);
        vga_write_line("File updated.");
        return;
    }

    char probe[16];
    if (vfs_list(resolved, probe, sizeof(probe)) >= 0)
    {
        vga_write_line("touch: path is a directory");
        return;
    }

    if (vfs_write_file(resolved, NULL, 0) < 0)
        vga_write_line("touch: failed");
    else
        vga_write_line("File created.");
}

static void command_rm(const char *args)
{
    const char *token = skip_spaces(args ? args : "");
    if (*token == '\0')
    {
        vga_write_line("Usage: rm <path>");
        return;
    }

    char target[VFS_MAX_PATH];
    if (!shell_copy_token(token, target, sizeof(target)))
    {
        vga_write_line("rm: path too long");
        return;
    }

    const char *rest = skip_spaces(token + str_len(target));
    if (*rest)
    {
        vga_write_line("rm: too many arguments");
        return;
    }

    char absolute[VFS_MAX_PATH];
    const char *resolved = resolve_absolute_path(target, absolute, sizeof(absolute));
    if (!resolved)
    {
        vga_write_line("rm: invalid path");
        return;
    }

    if (vfs_remove(resolved) == 0)
        vga_write_line("Removed.");
    else
        vga_write_line("rm: failed");
}

static void command_volumes(void)
{
    size_t count = volmgr_volume_count();
    if (count == 0)
    {
        vga_write_line("No volumes detected.");
        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        const struct volume_info *info = volmgr_volume_at(i);
        if (!info)
            continue;

        const char *name = (info->name && info->name[0]) ? info->name : "(unnamed)";
        const char *mount_path = (info->mount_path && info->mount_path[0]) ? info->mount_path : "(unmounted)";

        char line[160];
        size_t pos = 0;
        buffer_append(line, &pos, sizeof(line), name);
        buffer_append(line, &pos, sizeof(line), " -> ");
        buffer_append(line, &pos, sizeof(line), mount_path);
        line[pos] = '\0';
        vga_write_line(line);
    }
}

static void command_mount(const char *args)
{
    const char *trimmed = skip_spaces(args ? args : "");
    if (*trimmed)
    {
        vga_write_line("Usage: mount");
        return;
    }

    size_t count = vfs_mount_count();
    if (count == 0)
    {
        vga_write_line("No mounts active.");
        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        char path[VFS_MAX_PATH];
        if (vfs_mount_path_at(i, path, sizeof(path)) < 0)
            continue;

        char line[192];
        size_t pos = 0;
        char index_buf[32];
        write_u64((uint64_t)(i + 1u), index_buf);
        buffer_append(line, &pos, sizeof(line), index_buf);
        buffer_append(line, &pos, sizeof(line), ": ");
        buffer_append(line, &pos, sizeof(line), path);
        line[pos] = '\0';
        vga_write_line(line);
    }
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

static void release_module_buffer(void *buffer)
{
    (void)buffer;
}

static int append_kmd_extension(char *path, size_t capacity)
{
    if (!path || capacity == 0)
        return 0;

    size_t len = 0;
    size_t last_sep = 0;
    while (path[len])
    {
        if (path[len] == '/')
            last_sep = len + 1u;
        ++len;
        if (len >= capacity)
            return 0;
    }

    int has_dot = 0;
    for (size_t i = last_sep; i < len; ++i)
    {
        if (path[i] == '.')
        {
            has_dot = 1;
            break;
        }
    }

    if (has_dot)
        return 1;

    if (len + 4 >= capacity)
        return 0;

    path[len++] = '.';
    path[len++] = 'k';
    path[len++] = 'm';
    path[len++] = 'd';
    path[len] = '\0';
    return 1;
}

static int module_filename_from_path(const char *path, char *out, size_t capacity)
{
    if (!path || !out || capacity == 0)
        return 0;

    size_t last = 0;
    for (size_t i = 0; path[i]; ++i)
    {
        if (path[i] == '/')
            last = i + 1u;
    }

    size_t idx = 0;
    while (path[last + idx] && idx + 1 < capacity)
    {
        out[idx] = path[last + idx];
        ++idx;
    }

    if (path[last + idx])
    {
        out[0] = '\0';
        return 0;
    }

    out[idx] = '\0';
    return idx > 0;
}

static int load_module_image_from_absolute(const char *absolute, uint8_t **out_buffer, size_t *out_size)
{
    if (!absolute || !out_buffer || !out_size)
        return -1;

    *out_buffer = NULL;
    *out_size = 0;

    const char volume_prefix[] = "/Volumes/Disk0/";
    if (shell_str_starts_with(absolute, volume_prefix))
    {
        if (!fat16_ready())
            return -2;

        const char *name = absolute + str_len(volume_prefix);
        if (!name[0])
            return -1;

        for (size_t i = 0; name[i]; ++i)
        {
            if (name[i] == '/')
                return -1;
        }

        if (str_len(name) >= 48)
            return -1;

        uint32_t file_size = 0;
        if (fat16_file_size(name, &file_size) < 0 || file_size == 0)
            return -1;

        uint8_t *buffer = (uint8_t *)kalloc(file_size);
        if (!buffer)
            return -3;

        size_t read_size = 0;
        if (fat16_read_file(name, buffer, file_size, &read_size) < 0 || read_size != file_size)
        {
            release_module_buffer(buffer);
            return -1;
        }

        *out_buffer = buffer;
        *out_size = file_size;
        return 0;
    }

    uint8_t *buffer = (uint8_t *)kalloc(VFS_INLINE_CAP);
    if (!buffer)
        return -3;

    int fd = vfs_open(absolute);
    if (fd < 0)
    {
        release_module_buffer(buffer);
        return -1;
    }

    int read = vfs_read(fd, (char *)buffer, VFS_INLINE_CAP);
    vfs_close(fd);
    if (read <= 0)
    {
        release_module_buffer(buffer);
        return -1;
    }

    *out_buffer = buffer;
    *out_size = (size_t)read;
    return 0;
}

static void command_mod_load(const char *args)
{
    const char *token = skip_spaces(args ? args : "");
    if (*token == '\0')
    {
        vga_write_line("Usage: mod load <module>");
        return;
    }

    char requested[VFS_MAX_PATH];
    size_t idx = 0;
    while (token[idx] && token[idx] != ' ' && idx + 1 < sizeof(requested))
    {
        requested[idx] = token[idx];
        ++idx;
    }

    if (token[idx] != '\0' && token[idx] != ' ')
    {
        vga_write_line("mod: path too long");
        return;
    }

    requested[idx] = '\0';

    const char *rest = skip_spaces(token + idx);
    if (*rest)
    {
        vga_write_line("mod: too many arguments");
        return;
    }

    int has_separator = 0;
    for (size_t i = 0; requested[i]; ++i)
    {
        if (requested[i] == '/')
        {
            has_separator = 1;
            break;
        }
    }

    if (!append_kmd_extension(requested, sizeof(requested)))
    {
        vga_write_line("mod: invalid module path");
        return;
    }

    char module_filename[MODULE_NAME_MAX];
    if (!module_filename_from_path(requested, module_filename, sizeof(module_filename)))
    {
        vga_write_line("mod: invalid module name");
        return;
    }

    char module_name[MODULE_NAME_MAX];
    size_t copy = 0;
    while (module_filename[copy] && copy + 1 < sizeof(module_name))
    {
        module_name[copy] = module_filename[copy];
        ++copy;
    }
    module_name[copy] = '\0';
    strip_kmd_extension(module_name);

    if (module_name[0] != '\0' && module_find(module_name))
    {
        vga_write_line("mod: module already loaded");
        return;
    }

    char absolute[VFS_MAX_PATH];
    const char *resolved = resolve_absolute_path(requested, absolute, sizeof(absolute));
    if (!resolved)
    {
        vga_write_line("mod: invalid path");
        return;
    }

    uint8_t *image = NULL;
    size_t image_size = 0;
    int status = load_module_image_from_absolute(resolved, &image, &image_size);

    char fallback_path[VFS_MAX_PATH];
    if (status < 0 && !has_separator)
    {
        const char volume_prefix[] = "/Volumes/Disk0/";
        size_t prefix_len = str_len(volume_prefix);
        size_t file_len = str_len(module_filename);

        if (status == -3)
        {
            /* already out of memory, skip fallback to avoid repeated allocation */
        }
        else if (status == -2 || !fat16_ready())
        {
            status = -2;
        }
        else if (prefix_len + file_len < sizeof(fallback_path))
        {
            for (size_t i = 0; i < prefix_len; ++i)
                fallback_path[i] = volume_prefix[i];
            for (size_t i = 0; i < file_len; ++i)
                fallback_path[prefix_len + i] = module_filename[i];
            fallback_path[prefix_len + file_len] = '\0';

            status = load_module_image_from_absolute(fallback_path, &image, &image_size);
            if (status == 0)
                resolved = fallback_path;
        }
    }

    if (status < 0)
    {
        if (image)
        {
            release_module_buffer(image);
            image = NULL;
        }

        if (status == -3)
            vga_write_line("mod: out of memory");
        else if (status == -2)
            vga_write_line("mod: FAT volume unavailable");
        else
            vga_write_line("mod: failed to read module image");
        return;
    }

    if (!module_filename_from_path(resolved, module_filename, sizeof(module_filename)))
    {
        if (image)
        {
            release_module_buffer(image);
            image = NULL;
        }
        vga_write_line("mod: invalid module name");
        return;
    }

    copy = 0;
    while (module_filename[copy] && copy + 1 < sizeof(module_name))
    {
        module_name[copy] = module_filename[copy];
        ++copy;
    }
    module_name[copy] = '\0';
    strip_kmd_extension(module_name);

    int rc = module_load_image(module_filename, image, image_size, 0);
    release_module_buffer(image);

    if (rc == 0)
    {
        vga_write("mod: loaded ");
        if (module_name[0])
            vga_write_line(module_name);
        else
            vga_write_line(module_filename);
    }
    else
    {
        vga_write_line("mod: load failed");
    }
}

static void command_mod_unload(const char *args)
{
    const char *token = skip_spaces(args ? args : "");
    if (*token == '\0')
    {
        vga_write_line("Usage: mod unload <module>");
        return;
    }

    char requested[VFS_MAX_PATH];
    size_t idx = 0;
    while (token[idx] && token[idx] != ' ' && idx + 1 < sizeof(requested))
    {
        requested[idx] = token[idx];
        ++idx;
    }

    if (token[idx] != '\0' && token[idx] != ' ')
    {
        vga_write_line("mod: path too long");
        return;
    }

    requested[idx] = '\0';

    const char *rest = skip_spaces(token + idx);
    if (*rest)
    {
        vga_write_line("mod: too many arguments");
        return;
    }

    int has_separator = 0;
    for (size_t i = 0; requested[i]; ++i)
    {
        if (requested[i] == '/')
        {
            has_separator = 1;
            break;
        }
    }

    char module_name[MODULE_NAME_MAX];
    if (has_separator)
    {
        if (!append_kmd_extension(requested, sizeof(requested)))
        {
            vga_write_line("mod: invalid module path");
            return;
        }

        char module_filename[MODULE_NAME_MAX];
        if (!module_filename_from_path(requested, module_filename, sizeof(module_filename)))
        {
            vga_write_line("mod: invalid module name");
            return;
        }

        size_t copy = 0;
        while (module_filename[copy] && copy + 1 < sizeof(module_name))
        {
            module_name[copy] = module_filename[copy];
            ++copy;
        }
        module_name[copy] = '\0';
    }
    else
    {
        size_t copy = 0;
        while (requested[copy] && copy + 1 < sizeof(module_name))
        {
            module_name[copy] = requested[copy];
            ++copy;
        }
        if (requested[copy])
        {
            vga_write_line("mod: module name too long");
            return;
        }
        module_name[copy] = '\0';
    }

    strip_kmd_extension(module_name);

    if (module_name[0] == '\0')
    {
        vga_write_line("mod: invalid module name");
        return;
    }

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

static void command_net_help(void)
{
    vga_write_line("Usage: net <command>");
    vga_write_line("  net list        - show network interfaces");
    vga_write_line("  net poll [n|watch] - service network devices");
    vga_write_line("  net arp <ipv4>  - resolve/display ARP entry");
    vga_write_line("  net ping <ipv4> - send ICMP echo request");
    vga_write_line("  net ip [ipv4]   - show or set local IPv4");
}

static void command_net_list(void)
{
    size_t count = net_device_count();
    if (count == 0)
    {
        vga_write_line("net: no interfaces registered");
        return;
    }

    vga_write_line("Name       MAC");
    for (size_t i = 0; i < count; ++i)
    {
        struct net_device *dev = net_get_device(i);
        if (!dev)
            continue;

        char line[64];
        size_t pos = 0;
        buffer_append(line, &pos, sizeof(line), "  ");
        buffer_append(line, &pos, sizeof(line), dev->name);
        while (pos < 12 && pos < sizeof(line) - 1)
            line[pos++] = ' ';

        if (pos < sizeof(line) - 1)
            line[pos++] = ' ';

        char macbuf[32];
        format_mac_address(dev->mac, macbuf, sizeof(macbuf));
        buffer_append(line, &pos, sizeof(line), macbuf);
        line[pos] = '\0';
        vga_write_line(line);
    }
}

static void command_net_poll(const char *args)
{
    if (net_device_count() == 0)
    {
        vga_write_line("net: no interfaces registered");
        return;
    }

    const char *token = skip_spaces(args ? args : "");
    int iterations = 1;
    int watch = 0;

    if (*token)
    {
        char word[16];
        size_t idx = 0;
        while (token[idx] && token[idx] != ' ' && idx + 1 < sizeof(word))
        {
            word[idx] = token[idx];
            ++idx;
        }

        if (token[idx] != '\0' && token[idx] != ' ')
        {
            vga_write_line("net: argument too long");
            return;
        }

        word[idx] = '\0';
        const char *extra = skip_spaces(token + idx);
        if (*extra)
        {
            vga_write_line("Usage: net poll [n|watch]");
            return;
        }

        if (shell_str_equals(word, "watch"))
        {
            watch = 1;
        }
        else if (!parse_positive_int(word, &iterations) || iterations <= 0)
        {
            vga_write_line("net: invalid iteration count");
            return;
        }
    }

    uint64_t total_events = 0;
    int performed = 0;

    if (watch)
    {
        const int max_loops = 64;
        while (performed < max_loops)
        {
            int rc = net_poll_devices();
            if (rc < 0)
            {
                vga_write_line("net: poll failed");
                return;
            }

            ++performed;
            total_events += (uint64_t)rc;
            if (rc == 0)
                break;

            shell_flush_icmp_notifications();
        }

        if (performed >= 64)
            vga_write_line("net: watch limit reached");
    }
    else
    {
        for (int i = 0; i < iterations; ++i)
        {
            int rc = net_poll_devices();
            if (rc < 0)
            {
                vga_write_line("net: poll failed");
                return;
            }

            ++performed;
            total_events += (uint64_t)rc;
            shell_flush_icmp_notifications();
            if (rc == 0)
                break;
        }
    }

    shell_flush_icmp_notifications();

    char events_buf[32];
    char iterations_buf[32];
    write_u64(total_events, events_buf);
    write_u64((uint64_t)performed, iterations_buf);

    char line[96];
    size_t pos = 0;
    buffer_append(line, &pos, sizeof(line), "net: polled ");
    buffer_append(line, &pos, sizeof(line), iterations_buf);
    buffer_append(line, &pos, sizeof(line), " time(s), events=");
    buffer_append(line, &pos, sizeof(line), events_buf);
    line[pos] = '\0';
    vga_write_line(line);
}

static void command_net_arp(const char *args)
{
    struct net_device *dev = shell_default_net_device();
    if (!dev)
    {
        vga_write_line("net: no interfaces registered");
        return;
    }

    const char *token = skip_spaces(args ? args : "");
    if (*token == '\0')
    {
        vga_write_line("Usage: net arp <ipv4>");
        return;
    }

    char ip_text[32];
    size_t idx = 0;
    while (token[idx] && token[idx] != ' ' && idx + 1 < sizeof(ip_text))
    {
        ip_text[idx] = token[idx];
        ++idx;
    }

    if (token[idx] != '\0' && token[idx] != ' ')
    {
        vga_write_line("net: address too long");
        return;
    }

    ip_text[idx] = '\0';
    const char *extra = skip_spaces(token + idx);
    if (*extra)
    {
        vga_write_line("Usage: net arp <ipv4>");
        return;
    }

    uint8_t target[4];
    if (!parse_ipv4_address(ip_text, target))
    {
        vga_write_line("net: invalid IPv4 address");
        return;
    }

    uint8_t mac[6];
    if (arp_cache_lookup(dev, target, mac))
    {
        char line[96];
        size_t pos = 0;
        buffer_append(line, &pos, sizeof(line), "net: ");
        buffer_append(line, &pos, sizeof(line), dev->name);
        buffer_append(line, &pos, sizeof(line), " -> ");

        char ip_formatted[32];
        format_ipv4_address(target, ip_formatted, sizeof(ip_formatted));
        buffer_append(line, &pos, sizeof(line), ip_formatted);
        buffer_append(line, &pos, sizeof(line), " is at ");

        char mac_text[32];
        format_mac_address(mac, mac_text, sizeof(mac_text));
        buffer_append(line, &pos, sizeof(line), mac_text);
        line[pos] = '\0';
        vga_write_line(line);
        return;
    }

    int rc = arp_resolve(dev, target, mac);
    if (rc < 0)
    {
        vga_write_line("net: failed to send ARP request");
        return;
    }

    if (rc == 0)
    {
        char line[96];
        size_t pos = 0;
        buffer_append(line, &pos, sizeof(line), "net: ");
        buffer_append(line, &pos, sizeof(line), dev->name);
        buffer_append(line, &pos, sizeof(line), " -> ");

        char ip_formatted[32];
        format_ipv4_address(target, ip_formatted, sizeof(ip_formatted));
        buffer_append(line, &pos, sizeof(line), ip_formatted);
        buffer_append(line, &pos, sizeof(line), " is at ");

        char mac_text[32];
        format_mac_address(mac, mac_text, sizeof(mac_text));
        buffer_append(line, &pos, sizeof(line), mac_text);
        line[pos] = '\0';
        vga_write_line(line);
        return;
    }

    char message[112];
    size_t pos = 0;
    buffer_append(message, &pos, sizeof(message), "net: ARP request sent for ");
    char ip_formatted[32];
    format_ipv4_address(target, ip_formatted, sizeof(ip_formatted));
    buffer_append(message, &pos, sizeof(message), ip_formatted);
    buffer_append(message, &pos, sizeof(message), " on ");
    buffer_append(message, &pos, sizeof(message), dev->name);
    message[pos] = '\0';
    vga_write_line(message);
    vga_write_line("net: run 'net poll' to service responses");
}

static void command_net_ping(const char *args)
{
    struct net_device *dev = shell_default_net_device();
    if (!dev)
    {
        vga_write_line("net: no interfaces registered");
        shell_recover_input();
        return;
    }

    const char *token = skip_spaces(args ? args : "");
    if (*token == '\0')
    {
        vga_write_line("Usage: net ping <ipv4> [count]");
        shell_recover_input();
        return;
    }

    char ip_text[32];
    size_t idx = 0;
    while (token[idx] && token[idx] != ' ' && idx + 1 < sizeof(ip_text))
    {
        ip_text[idx] = token[idx];
        ++idx;
    }

    if (token[idx] != '\0' && token[idx] != ' ')
    {
        vga_write_line("net: address too long");
        shell_recover_input();
        return;
    }

    ip_text[idx] = '\0';
    const char *rest = skip_spaces(token + idx);

    int count = SHELL_PING_DEFAULT_COUNT;
    if (*rest)
    {
        char count_text[16];
        size_t cidx = 0;
        while (rest[cidx] && rest[cidx] != ' ' && cidx + 1 < sizeof(count_text))
        {
            count_text[cidx] = rest[cidx];
            ++cidx;
        }

        if (rest[cidx] != '\0' && rest[cidx] != ' ')
        {
            vga_write_line("net: argument too long");
            return;
        }

        count_text[cidx] = '\0';
        const char *after = skip_spaces(rest + cidx);
        if (*after)
        {
            vga_write_line("Usage: net ping <ipv4> [count]");
            shell_recover_input();
            return;
        }

        if (!parse_positive_int(count_text, &count) || count <= 0)
        {
            vga_write_line("net: invalid count");
            shell_recover_input();
            return;
        }

        if (count > SHELL_PING_MAX_COUNT)
        {
            vga_write_line("net: count too large");
            shell_recover_input();
            return;
        }
    }

    uint8_t target[4];
    if (!parse_ipv4_address(ip_text, target))
    {
        vga_write_line("net: invalid IPv4 address");
        shell_recover_input();
        return;
    }

    icmp_clear_echo_replies();

    vga_write("net: pinging ");
    vga_write(ip_text);
    vga_write_line(" with 32 bytes of data");

    int sent = 0;
    int received = 0;
    uint32_t min_rtt = UINT32_MAX;
    uint32_t max_rtt = 0;
    uint32_t total_rtt_ms = 0;
    int abort_ping = 0;

    for (int i = 0; i < count; ++i)
    {
        ++sent;
        uint16_t sequence = ++shell_ping_sequence;
        uint64_t arp_deadline = get_ticks() + SHELL_PING_ARP_WAIT_TICKS;
        uint64_t send_tick = 0;

        int tx_status = shell_ping_transmit(dev, target, shell_ping_identifier, sequence, arp_deadline, &send_tick);
        if (tx_status < 0)
        {
            vga_write_line("net: ping transmit failed");
            abort_ping = 1;
            break;
        }

        if (tx_status == 0)
        {
            vga_write_line("net: ARP resolution timed out");
        }
        else
        {
            uint32_t rtt_ms = 0;
            uint8_t reply_src[4];
            uint64_t reply_deadline = send_tick + SHELL_PING_TIMEOUT_TICKS;
            int reply_status = shell_ping_wait_for_reply(shell_ping_identifier, sequence, send_tick, reply_deadline, &rtt_ms, reply_src);

            if (reply_status < 0)
            {
                vga_write_line("net: poll failed during ping");
                abort_ping = 1;
                break;
            }

            if (reply_status == 0)
            {
                vga_write_line("net: request timed out");
            }
            else
            {
                ++received;
                if (rtt_ms < min_rtt)
                    min_rtt = rtt_ms;
                if (rtt_ms > max_rtt)
                    max_rtt = rtt_ms;
                if (UINT32_MAX - total_rtt_ms < rtt_ms)
                    total_rtt_ms = UINT32_MAX;
                else
                    total_rtt_ms += rtt_ms;

                char reply_ip[32];
                format_ipv4_address(reply_src, reply_ip, sizeof(reply_ip));

                char seq_buf[32];
                write_u64((uint64_t)sequence, seq_buf);

                char time_buf[32];
                write_u64((uint64_t)rtt_ms, time_buf);

                vga_write("net: reply from ");
                vga_write(reply_ip);
                vga_write(" seq=");
                vga_write(seq_buf);
                vga_write(" time=");
                vga_write(time_buf);
                vga_write_line("ms");
            }
        }

        if (abort_ping)
            break;

        if (i + 1 < count)
        {
            uint64_t target_tick = get_ticks() + SHELL_PING_INTERVAL_TICKS;
            while (get_ticks() < target_tick)
            {
                int events = net_poll_devices();
                if (events < 0)
                {
                    vga_write_line("net: poll failed during interval");
                    abort_ping = 1;
                    break;
                }
                if (events == 0)
                    shell_sleep_ticks(1);
                else
                    shell_yield();
            }
            if (abort_ping)
                break;
        }
    }

    int lost = sent - received;
    if (lost < 0)
        lost = 0;

    vga_write("net: Ping statistics for ");
    vga_write(ip_text);
    vga_write_line("");

    char sent_buf[32];
    char recv_buf[32];
    char lost_buf[32];
    char loss_percent_buf[32];
    write_u64((uint64_t)sent, sent_buf);
    write_u64((uint64_t)received, recv_buf);
    write_u64((uint64_t)lost, lost_buf);

    uint32_t loss_percent = 0;
    if (sent > 0)
    {
        uint32_t lost_u32 = (lost < 0) ? 0u : (uint32_t)lost;
        uint32_t sent_u32 = (sent < 0) ? 0u : (uint32_t)sent;
        loss_percent = (lost_u32 * 100u) / sent_u32;
    }
    write_u64((uint64_t)loss_percent, loss_percent_buf);

    vga_write("net:     Packets: Sent = ");
    vga_write(sent_buf);
    vga_write(", Received = ");
    vga_write(recv_buf);
    vga_write(", Lost = ");
    vga_write(lost_buf);
    vga_write(" (");
    vga_write(loss_percent_buf);
    vga_write_line("% loss)");

    if (received > 0)
    {
        uint32_t avg_rtt = 0;
        if (received > 0)
            avg_rtt = total_rtt_ms / (uint32_t)received;

        char min_buf[32];
        char max_buf[32];
        char avg_buf[32];
        write_u64((uint64_t)min_rtt, min_buf);
        write_u64((uint64_t)max_rtt, max_buf);
        write_u64((uint64_t)avg_rtt, avg_buf);

        vga_write("net:     RTT min/avg/max = ");
        vga_write(min_buf);
        vga_write("/");
        vga_write(avg_buf);
        vga_write("/");
        vga_write(max_buf);
        vga_write_line(" ms");
    }

    shell_recover_input();
}

static void command_net_ip(const char *args)
{
    uint8_t current[4];
    ipv4_get_address(current);

    const char *token = skip_spaces(args ? args : "");
    if (*token == '\0')
    {
        char line[96];
        size_t pos = 0;
        buffer_append(line, &pos, sizeof(line), "net: local IPv4 ");

        char ip_text[32];
        format_ipv4_address(current, ip_text, sizeof(ip_text));
        buffer_append(line, &pos, sizeof(line), ip_text);

        int configured = 0;
        for (size_t i = 0; i < 4; ++i)
        {
            if (current[i] != 0)
            {
                configured = 1;
                break;
            }
        }

        if (!configured)
            buffer_append(line, &pos, sizeof(line), " (unset)");

        line[pos] = '\0';
        vga_write_line(line);
        return;
    }

    char ip_text[32];
    size_t idx = 0;
    while (token[idx] && token[idx] != ' ' && idx + 1 < sizeof(ip_text))
    {
        ip_text[idx] = token[idx];
        ++idx;
    }

    if (token[idx] != '\0' && token[idx] != ' ')
    {
        vga_write_line("net: address too long");
        return;
    }

    ip_text[idx] = '\0';
    const char *extra = skip_spaces(token + idx);
    if (*extra)
    {
        vga_write_line("Usage: net ip [ipv4]");
        return;
    }

    uint8_t addr[4];
    if (!parse_ipv4_address(ip_text, addr))
    {
        vga_write_line("net: invalid IPv4 address");
        return;
    }

    ipv4_set_address(addr);

    char line[96];
    size_t pos = 0;
    buffer_append(line, &pos, sizeof(line), "net: local IPv4 set to ");
    char formatted[32];
    format_ipv4_address(addr, formatted, sizeof(formatted));
    buffer_append(line, &pos, sizeof(line), formatted);
    line[pos] = '\0';
    vga_write_line(line);
}

static void command_net(const char *args)
{
    const char *sub = skip_spaces(args ? args : "");
    if (*sub == '\0')
    {
        command_net_help();
        return;
    }

    char token[8];
    size_t idx = 0;
    while (sub[idx] && sub[idx] != ' ' && idx + 1 < sizeof(token))
    {
        token[idx] = sub[idx];
        ++idx;
    }

    if (sub[idx] != '\0' && sub[idx] != ' ')
    {
        vga_write_line("net: command too long");
        return;
    }

    token[idx] = '\0';
    const char *rest = skip_spaces(sub + idx);

    if (shell_str_equals(token, "help"))
    {
        if (*rest)
        {
            vga_write_line("Usage: net help");
            return;
        }
        command_net_help();
    }
    else if (shell_str_equals(token, "list"))
    {
        if (*rest)
        {
            vga_write_line("Usage: net list");
            return;
        }
        command_net_list();
    }
    else if (shell_str_equals(token, "poll"))
    {
        command_net_poll(rest);
    }
    else if (shell_str_equals(token, "arp"))
    {
        command_net_arp(rest);
    }
    else if (shell_str_equals(token, "ping"))
    {
        command_net_ping(rest);
    }
    else if (shell_str_equals(token, "ip"))
    {
        command_net_ip(rest);
    }
    else
    {
        vga_write_line("net: unknown command");
        command_net_help();
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

static const char *logs_resolve_path(const char *name)
{
    if (shell_str_equals(name, "kernel"))
        return "/System/Logs/kernel.log";
    if (shell_str_equals(name, "net"))
        return "/System/Logs/net.log";
    if (shell_str_equals(name, "ipc"))
        return "/System/Logs/ipc.log";
    return NULL;
}

static void command_logs(const char *args)
{
    const char *trimmed = skip_spaces(args ? args : "");
    if (*trimmed == '\0')
    {
        vga_write_line("Usage: logs <kernel|net|ipc>");
        return;
    }

    char name[16];
    size_t idx = 0;
    while (trimmed[idx] && trimmed[idx] != ' ' && idx + 1 < sizeof(name))
    {
        name[idx] = trimmed[idx];
        ++idx;
    }

    if (trimmed[idx] != '\0' && trimmed[idx] != ' ')
    {
        vga_write_line("logs: name too long");
        return;
    }

    name[idx] = '\0';

    const char *rest = skip_spaces(trimmed + idx);
    if (*rest != '\0')
    {
        vga_write_line("Usage: logs <kernel|net|ipc>");
        return;
    }

    const char *path = logs_resolve_path(name);
    if (!path)
    {
        vga_write("logs: unknown log '");
        vga_write(name);
        vga_write_line("'");
        return;
    }

    klog_enable_proc_sink();
    klog_refresh_proc_sink();

    char buffer[VFS_INLINE_CAP];
    int length = vfs_read_path(path, buffer, sizeof(buffer));
    if (length < 0)
    {
        if (vfs_write_file(path, "", 0) == 0)
            length = vfs_read_path(path, buffer, sizeof(buffer));
        if (length < 0)
        {
            vga_write("logs: unable to read ");
            vga_write_line(name);
            return;
        }
    }

    size_t max_output = sizeof(buffer) - 1u;
    if ((size_t)length > max_output)
        length = (int)max_output;
    buffer[length] = '\0';

    if (length == 0)
    {
        vga_write(name);
        vga_write_line(" log empty.");
        return;
    }

    vga_write("logs[");
    vga_write(name);
    vga_write_line("]:");
    vga_write_line(buffer);

    if ((size_t)length == max_output)
        vga_write_line("logs: output truncated (buffer limit)");
}

/* Simple kernel worker that spins and yields to exercise the scheduler. */
static void stress_worker(void)
{
    for (;;)
    {
        for (volatile int i = 0; i < CONFIG_STRESS_SPIN_CYCLES; ++i)
            __asm__ __volatile__("nop");
        shell_yield();
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

    vga_write_line("Powering off...");
    klog_info("shell: invoking power shutdown");
    power_shutdown();
}

static void shell_render_prompt(void)
{
    const char *path_display = shell_cwd;
    if (!path_display || path_display[0] == '\0')
        path_display = "/";

    vga_set_color(0xB, 0x0);
    vga_write("proOS ");
    vga_write(path_display);
    vga_write(" >> ");
    vga_set_color(0x7, 0x0);
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
        command_ls("");
    }
    else if (shell_str_starts_with(cursor, "ls "))
    {
        command_ls(cursor + 2);
    }
    else if (shell_str_equals(cursor, "tree"))
    {
        command_tree("");
    }
    else if (shell_str_starts_with(cursor, "tree "))
    {
        command_tree(cursor + 4);
    }
    else if (shell_str_equals(cursor, "cd"))
    {
        command_cd("");
    }
    else if (shell_str_starts_with(cursor, "cd "))
    {
        command_cd(cursor + 2);
    }
    else if (shell_str_equals(cursor, "whereami"))
    {
        command_whereami();
    }
    else if (shell_str_equals(cursor, "cat") || shell_str_starts_with(cursor, "cat "))
    {
        command_cat(cursor + 3);
    }
    else if (shell_str_equals(cursor, "mkdir") || shell_str_starts_with(cursor, "mkdir "))
    {
        command_mkdir(cursor + 5);
    }
    else if (shell_str_equals(cursor, "touch") || shell_str_starts_with(cursor, "touch "))
    {
        command_touch(cursor + 5);
    }
    else if (shell_str_equals(cursor, "rm") || shell_str_starts_with(cursor, "rm "))
    {
        command_rm(cursor + 2);
    }
    else if (shell_str_equals(cursor, "volumes"))
    {
        command_volumes();
    }
    else if (shell_str_equals(cursor, "mount") || shell_str_starts_with(cursor, "mount "))
    {
        command_mount(cursor + 5);
    }
    else if (shell_str_equals(cursor, "tasks") || shell_str_equals(cursor, "proc_list"))
    {
        command_proc_list();
    }
    else if (shell_str_equals(cursor, "mod") || shell_str_starts_with(cursor, "mod "))
    {
        command_mod(cursor + 3);
    }
    else if (shell_str_equals(cursor, "net") || shell_str_starts_with(cursor, "net "))
    {
        command_net(cursor + 3);
    }
    else if (shell_str_equals(cursor, "gfx"))
    {
        command_gfx();
    }
    else if (shell_str_equals(cursor, "kdlg"))
    {
        command_kdlg();
    }
    else if (shell_str_equals(cursor, "logs") || shell_str_starts_with(cursor, "logs "))
    {
        command_logs(cursor + 4);
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
    /* Ensure cwd is clean */
    shell_cwd[0] = '/';
    shell_cwd[1] = '\0';

    char buffer[INPUT_MAX];

    while (1)
    {
        __asm__ __volatile__("sti");
        pic_clear_mask(0);
        pic_clear_mask(1);
        shell_render_prompt();
        size_t len = shell_read_line(buffer, sizeof(buffer));
        if (len > 0)
            history_store(buffer);
        shell_execute(buffer);
    }
}
