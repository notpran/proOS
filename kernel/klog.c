#include "klog.h"
#include "string.h"

#ifndef CONFIG_KLOG_CAPACITY
#error "CONFIG_KLOG_CAPACITY must be defined in config.h"
#endif

static struct klog_entry klog_buffer[CONFIG_KLOG_CAPACITY];
static size_t klog_count = 0;
static size_t klog_head = 0;
static uint32_t klog_sequence = 0;
static int klog_current_level = CONFIG_KLOG_DEFAULT_LEVEL;
static int klog_ready = 0;

static uint32_t save_and_cli(void)
{
    uint32_t flags;
    __asm__ __volatile__("pushf\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}

static void restore_flags(uint32_t flags)
{
    __asm__ __volatile__("push %0\n\tpopf" :: "r"(flags) : "memory", "cc");
}

static void klog_reset_internal(void)
{
    klog_count = 0;
    klog_head = 0;
    klog_sequence = 0;
    for (size_t i = 0; i < CONFIG_KLOG_CAPACITY; ++i)
        klog_buffer[i].text[0] = '\0';
}

void klog_init(void)
{
    uint32_t flags = save_and_cli();
    klog_reset_internal();
    klog_current_level = CONFIG_KLOG_DEFAULT_LEVEL;
    if (klog_current_level < KLOG_DEBUG)
        klog_current_level = KLOG_DEBUG;
    if (klog_current_level > KLOG_ERROR)
        klog_current_level = KLOG_ERROR;
    klog_ready = 1;
    restore_flags(flags);
}

static void ensure_ready(void)
{
    if (!klog_ready)
        klog_init();
}

void klog_set_level(int level)
{
    ensure_ready();
    if (level < KLOG_DEBUG)
        level = KLOG_DEBUG;
    if (level > KLOG_ERROR)
        level = KLOG_ERROR;

    uint32_t flags = save_and_cli();
    klog_current_level = level;
    restore_flags(flags);
}

int klog_get_level(void)
{
    ensure_ready();
    return klog_current_level;
}

void klog_emit(int level, const char *message)
{
    ensure_ready();
    if (!message)
        return;
    if (level < klog_current_level)
        return;

    uint32_t flags = save_and_cli();

    struct klog_entry *slot = &klog_buffer[klog_head];
    slot->seq = klog_sequence++;
    slot->level = (uint8_t)((level >= KLOG_DEBUG && level <= KLOG_ERROR) ? level : KLOG_ERROR);

    size_t i = 0;
    while (message[i] && i + 1 < CONFIG_KLOG_ENTRY_LEN)
    {
        slot->text[i] = message[i];
        ++i;
    }
    slot->text[i] = '\0';

    klog_head = (klog_head + 1U) % CONFIG_KLOG_CAPACITY;
    if (klog_count < CONFIG_KLOG_CAPACITY)
        ++klog_count;

    restore_flags(flags);
}

size_t klog_copy(struct klog_entry *out, size_t max_entries)
{
    ensure_ready();
    if (!out || max_entries == 0)
        return 0;

    uint32_t flags = save_and_cli();

    size_t available = klog_count;
    if (available > max_entries)
        available = max_entries;

    size_t start = (klog_head + CONFIG_KLOG_CAPACITY - klog_count) % CONFIG_KLOG_CAPACITY;
    for (size_t i = 0; i < available; ++i)
    {
        size_t idx = (start + i) % CONFIG_KLOG_CAPACITY;
        out[i] = klog_buffer[idx];
    }

    restore_flags(flags);
    return available;
}

const char *klog_level_name(int level)
{
    switch (level)
    {
    case KLOG_DEBUG:
        return "DEBUG";
    case KLOG_INFO:
        return "INFO";
    case KLOG_WARN:
        return "WARN";
    case KLOG_ERROR:
    default:
        return "ERROR";
    }
}

static char to_upper(char c)
{
    if (c >= 'a' && c <= 'z')
        return (char)(c - ('a' - 'A'));
    return c;
}

int klog_level_from_name(const char *name)
{
    if (!name)
        return -1;

    size_t idx = 0;
    while (name[idx] == ' ')
        ++idx;
    if (name[idx] == '\0')
        return -1;

    char token[8];
    size_t token_len = 0;
    while (name[idx] && name[idx] != ' ' && token_len + 1 < sizeof(token))
    {
        token[token_len++] = to_upper(name[idx]);
        ++idx;
    }
    token[token_len] = '\0';

    if (token_len == 0)
        return -1;

    int numeric = 1;
    int value = 0;
    for (size_t i = 0; i < token_len; ++i)
    {
        char ch = token[i];
        if (ch >= '0' && ch <= '9')
        {
            value = (value * 10) + (ch - '0');
            if (value > KLOG_ERROR)
                return -1;
        }
        else
        {
            numeric = 0;
            break;
        }
    }

    if (numeric)
        return value;

    if (token[0] == 'D' && token_len == 5 && token[1] == 'E' && token[2] == 'B' && token[3] == 'U' && token[4] == 'G')
        return KLOG_DEBUG;
    if (token[0] == 'I' && token_len == 4 && token[1] == 'N' && token[2] == 'F' && token[3] == 'O')
        return KLOG_INFO;
    if (token[0] == 'W')
    {
        if (token_len == 4 && token[1] == 'A' && token[2] == 'R' && token[3] == 'N')
            return KLOG_WARN;
        if (token_len == 7 && token[1] == 'A' && token[2] == 'R' && token[3] == 'N' && token[4] == 'I' && token[5] == 'N' && token[6] == 'G')
            return KLOG_WARN;
    }
    if (token[0] == 'E')
    {
        if (token_len == 5 && token[1] == 'R' && token[2] == 'R' && token[3] == 'O' && token[4] == 'R')
            return KLOG_ERROR;
        if (token_len == 3 && token[1] == 'R' && token[2] == 'R')
            return KLOG_ERROR;
    }

    return -1;
}
