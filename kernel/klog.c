#define KLOG_TAG "klog"

#include "klog.h"
#include "string.h"
#include "ipc.h"
#include "service.h"
#include "service_types.h"
#include "vfs.h"

#ifndef CONFIG_KLOG_CAPACITY
#error "CONFIG_KLOG_CAPACITY must be defined in config.h"
#endif

struct klog_module_entry
{
    int used;
    int level;
    char name[CONFIG_KLOG_MODULE_NAME_LEN];
};

static struct klog_entry klog_buffer[CONFIG_KLOG_CAPACITY];
static size_t klog_count = 0;
static size_t klog_head = 0;
static uint32_t klog_sequence = 0;
static int klog_current_level = CONFIG_KLOG_DEFAULT_LEVEL;
static int klog_ready = 0;
static int logger_channel_id = -1;
static struct klog_module_entry module_table[CONFIG_KLOG_MAX_MODULES];
static int proc_sink_enabled = 0;
static int proc_sink_guard = 0;

enum log_sink_kind
{
    LOG_SINK_KERNEL = 0,
    LOG_SINK_NET = 1,
    LOG_SINK_IPC = 2,
    LOG_SINK_COUNT
};

static const char *log_sink_paths[LOG_SINK_COUNT] = {
    "/System/Logs/kernel.log",
    "/System/Logs/net.log",
    "/System/Logs/ipc.log"
};

static int logs_directory_ready = 0;

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

static size_t string_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return 0;
    size_t i = 0;
    if (src)
    {
        while (src[i] && i + 1 < cap)
        {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
    return i;
}

static int string_equals(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    size_t idx = 0;
    while (a[idx] && b[idx])
    {
        if (a[idx] != b[idx])
            return 0;
        ++idx;
    }
    return a[idx] == b[idx];
}

static char to_lower_ascii(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return (char)(ch + ('a' - 'A'));
    return ch;
}

static int string_equals_ignore_case(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    size_t idx = 0;
    while (a[idx] && b[idx])
    {
        if (to_lower_ascii(a[idx]) != to_lower_ascii(b[idx]))
            return 0;
        ++idx;
    }
    return a[idx] == b[idx];
}

static int string_has_prefix_ignore_case(const char *text, const char *prefix)
{
    if (!text || !prefix)
        return 0;
    size_t idx = 0;
    while (prefix[idx])
    {
        if (!text[idx])
            return 0;
        if (to_lower_ascii(text[idx]) != to_lower_ascii(prefix[idx]))
            return 0;
        ++idx;
    }
    return 1;
}

static void klog_reset_internal(void)
{
    klog_count = 0;
    klog_head = 0;
    klog_sequence = 0;
    for (size_t i = 0; i < CONFIG_KLOG_CAPACITY; ++i)
    {
        klog_buffer[i].seq = 0;
        klog_buffer[i].level = KLOG_INFO;
        klog_buffer[i].module[0] = '\0';
        klog_buffer[i].text[0] = '\0';
    }
    for (size_t i = 0; i < CONFIG_KLOG_MAX_MODULES; ++i)
    {
        module_table[i].used = 0;
        module_table[i].level = KLOG_LEVEL_INHERIT;
        module_table[i].name[0] = '\0';
    }
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

static void ensure_logs_directory(void)
{
    if (logs_directory_ready)
        return;
    vfs_mkdir("/System");
    vfs_mkdir("/System/Logs");
    logs_directory_ready = 1;
}

struct klog_ipc_event
{
    uint32_t seq;
    uint8_t level;
    uint8_t reserved[3];
    char module[CONFIG_KLOG_MODULE_NAME_LEN];
    char text[CONFIG_KLOG_ENTRY_LEN];
};

static void klog_publish_channel(uint32_t seq, uint8_t level, const char *module, const char *text)
{
    if (!ipc_is_initialized() || !text)
        return;
    struct klog_ipc_event payload;
    payload.seq = seq;
    payload.level = level;
    payload.reserved[0] = payload.reserved[1] = payload.reserved[2] = 0;
    string_copy(payload.module, sizeof(payload.module), module);

    size_t i = 0;
    while (text[i] && i + 1 < sizeof(payload.text))
    {
        payload.text[i] = text[i];
        ++i;
    }
    payload.text[i] = '\0';

    pid_t logd_pid = service_pid(SYSTEM_SERVICE_LOGD);
    if (logd_pid > 0)
    {
        if (ipc_send(logd_pid, &payload, sizeof(payload)) >= 0)
            return;
    }

    if (logger_channel_id < 0)
        logger_channel_id = ipc_get_service_channel(IPC_SERVICE_LOGGER);
    if (logger_channel_id >= 0)
        ipc_channel_send(logger_channel_id, 0, level, 0, &payload, sizeof(payload), 0);
}

static const char *sanitize_module(const char *module)
{
    if (!module || module[0] == '\0')
        return "kernel";
    return module;
}

static struct klog_module_entry *find_module_entry(const char *module, int allocate)
{
    const char *tag = sanitize_module(module);
    for (size_t i = 0; i < CONFIG_KLOG_MAX_MODULES; ++i)
    {
        if (!module_table[i].used)
            continue;
        if (string_equals(module_table[i].name, tag))
            return &module_table[i];
    }

    if (!allocate)
        return NULL;

    for (size_t i = 0; i < CONFIG_KLOG_MAX_MODULES; ++i)
    {
        if (module_table[i].used)
            continue;
        module_table[i].used = 1;
        module_table[i].level = KLOG_LEVEL_INHERIT;
        string_copy(module_table[i].name, sizeof(module_table[i].name), tag);
        return &module_table[i];
    }

    return NULL;
}

static int effective_threshold_for(const char *module)
{
    int threshold = klog_current_level;
    uint32_t flags = save_and_cli();
    struct klog_module_entry *entry = find_module_entry(module, 0);
    if (entry && entry->level != KLOG_LEVEL_INHERIT)
        threshold = entry->level;
    restore_flags(flags);
    return threshold;
}

static enum log_sink_kind classify_log_sink(const char *module, const char *text)
{
    if (module && module[0])
    {
        if (string_has_prefix_ignore_case(module, "net"))
            return LOG_SINK_NET;
        if (string_equals_ignore_case(module, "netd"))
            return LOG_SINK_NET;
        if (string_has_prefix_ignore_case(module, "ipc"))
            return LOG_SINK_IPC;
        if (string_equals_ignore_case(module, "logd"))
            return LOG_SINK_IPC;
    }

    if (text && text[0])
    {
        if (string_has_prefix_ignore_case(text, "net:"))
            return LOG_SINK_NET;
        if (string_has_prefix_ignore_case(text, "ipc:"))
            return LOG_SINK_IPC;
    }

    return LOG_SINK_KERNEL;
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

int klog_module_set_level(const char *module, int level)
{
    ensure_ready();
    const char *tag = sanitize_module(module);
    if (level != KLOG_LEVEL_INHERIT)
    {
        if (level < KLOG_DEBUG)
            level = KLOG_DEBUG;
        if (level > KLOG_ERROR)
            level = KLOG_ERROR;
    }

    uint32_t flags = save_and_cli();
    struct klog_module_entry *entry = find_module_entry(tag, 1);
    if (!entry)
    {
        restore_flags(flags);
        return -1;
    }
    entry->level = level;
    restore_flags(flags);
    if (proc_sink_enabled)
        klog_refresh_proc_sink();
    return 0;
}

int klog_module_get_level(const char *module)
{
    ensure_ready();
    uint32_t flags = save_and_cli();
    struct klog_module_entry *entry = find_module_entry(module, 0);
    int level = entry ? entry->level : KLOG_LEVEL_INHERIT;
    restore_flags(flags);
    return level;
}

static void klog_store_entry(const char *module, int level, const char *message, uint32_t seq_value, char *scratch_text, char *scratch_module)
{
    struct klog_entry *slot = &klog_buffer[klog_head];
    slot->seq = seq_value;
    slot->level = (uint8_t)level;
    string_copy(slot->module, sizeof(slot->module), module);

    size_t i = 0;
    while (message[i] && i + 1 < CONFIG_KLOG_ENTRY_LEN)
    {
        slot->text[i] = message[i];
        ++i;
    }
    slot->text[i] = '\0';

    if (scratch_text)
        string_copy(scratch_text, CONFIG_KLOG_ENTRY_LEN, slot->text);
    if (scratch_module)
        string_copy(scratch_module, CONFIG_KLOG_MODULE_NAME_LEN, slot->module);

    klog_head = (klog_head + 1U) % CONFIG_KLOG_CAPACITY;
    if (klog_count < CONFIG_KLOG_CAPACITY)
        ++klog_count;
}

static void append_char(char *dst, size_t *pos, size_t cap, char ch)
{
    if (*pos + 1 >= cap)
        return;
    dst[*pos] = ch;
    ++(*pos);
}

static void append_text(char *dst, size_t *pos, size_t cap, const char *text)
{
    if (!text)
        return;
    size_t i = 0;
    while (text[i] && *pos + 1 < cap)
    {
        dst[*pos] = text[i];
        ++(*pos);
        ++i;
    }
}

static void append_u32(char *dst, size_t *pos, size_t cap, uint32_t value)
{
    char tmp[12];
    size_t idx = 0;
    if (value == 0)
    {
        tmp[idx++] = '0';
    }
    else
    {
        while (value > 0 && idx < sizeof(tmp))
        {
            tmp[idx++] = (char)('0' + (value % 10U));
            value /= 10U;
        }
    }
    while (idx > 0 && *pos + 1 < cap)
    {
        dst[*pos] = tmp[--idx];
        ++(*pos);
    }
}

static void append_level_name(char *dst, size_t *pos, size_t cap, uint8_t level)
{
    const char *name = klog_level_name(level);
    append_text(dst, pos, cap, name);
}

void klog_refresh_proc_sink(void)
{
    if (!proc_sink_enabled)
        return;
    if (proc_sink_guard)
        return;

    proc_sink_guard = 1;

    struct klog_entry entries[CONFIG_KLOG_CAPACITY];
    size_t count = klog_copy(entries, CONFIG_KLOG_CAPACITY);

    ensure_logs_directory();

    for (size_t i = 0; i < LOG_SINK_COUNT; ++i)
        vfs_write_file(log_sink_paths[i], NULL, 0);

    for (size_t i = 0; i < count; ++i)
    {
        char line[CONFIG_KLOG_ENTRY_LEN + CONFIG_KLOG_MODULE_NAME_LEN + 48];
        size_t pos = 0;
        append_char(line, &pos, sizeof(line), '[');
        append_u32(line, &pos, sizeof(line), entries[i].seq);
        append_char(line, &pos, sizeof(line), ']');
        append_char(line, &pos, sizeof(line), ' ');
        append_level_name(line, &pos, sizeof(line), entries[i].level);
        append_char(line, &pos, sizeof(line), ' ');
        append_char(line, &pos, sizeof(line), '(');
        append_text(line, &pos, sizeof(line), entries[i].module);
        append_char(line, &pos, sizeof(line), ')');
        append_char(line, &pos, sizeof(line), ':');
        append_char(line, &pos, sizeof(line), ' ');
        append_text(line, &pos, sizeof(line), entries[i].text);
        line[pos] = '\0';

        enum log_sink_kind target = classify_log_sink(entries[i].module, entries[i].text);
        const char *path = log_sink_paths[(size_t)target];
        vfs_append(path, line, pos);
        vfs_append(path, "\n", 1);
    }

    proc_sink_guard = 0;
}

void klog_enable_proc_sink(void)
{
    proc_sink_enabled = 1;
    klog_refresh_proc_sink();
}

void klog_emit(int level, const char *message)
{
    klog_emit_tagged(KLOG_DEFAULT_TAG, level, message);
}

void klog_emit_tagged(const char *module, int level, const char *message)
{
    ensure_ready();
    if (!message)
        return;

    const char *tag = sanitize_module(module);
    int threshold = effective_threshold_for(tag);
    if (level < threshold)
        return;
    if (level < KLOG_DEBUG)
        level = KLOG_DEBUG;
    if (level > KLOG_ERROR)
        level = KLOG_ERROR;

    uint32_t flags = save_and_cli();
    uint32_t seq_value = klog_sequence++;
    char text_copy[CONFIG_KLOG_ENTRY_LEN];
    char module_copy[CONFIG_KLOG_MODULE_NAME_LEN];
    klog_store_entry(tag, level, message, seq_value, text_copy, module_copy);
    restore_flags(flags);

    klog_publish_channel(seq_value, (uint8_t)level, module_copy, text_copy);
    if (proc_sink_enabled)
        klog_refresh_proc_sink();
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
