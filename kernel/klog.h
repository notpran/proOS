#ifndef KLOG_H
#define KLOG_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

enum klog_level
{
    KLOG_DEBUG = 0,
    KLOG_INFO  = 1,
    KLOG_WARN  = 2,
    KLOG_ERROR = 3
};

struct klog_entry
{
    uint32_t seq;
    uint8_t level;
    char text[CONFIG_KLOG_ENTRY_LEN];
};

void klog_init(void);
void klog_set_level(int level);
int klog_get_level(void);
void klog_emit(int level, const char *message);
size_t klog_copy(struct klog_entry *out, size_t max_entries);
const char *klog_level_name(int level);
int klog_level_from_name(const char *name);

#define klog_debug(msg) klog_emit(KLOG_DEBUG, msg)
#define klog_info(msg)  klog_emit(KLOG_INFO, msg)
#define klog_warn(msg)  klog_emit(KLOG_WARN, msg)
#define klog_error(msg) klog_emit(KLOG_ERROR, msg)

#endif
