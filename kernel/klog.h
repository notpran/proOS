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

#define KLOG_LEVEL_INHERIT (-1)

#ifdef KLOG_TAG
#define KLOG_DEFAULT_TAG KLOG_TAG
#else
#define KLOG_DEFAULT_TAG "kernel"
#endif

struct klog_entry
{
    uint32_t seq;
    uint8_t level;
    char module[CONFIG_KLOG_MODULE_NAME_LEN];
    char text[CONFIG_KLOG_ENTRY_LEN];
};

void klog_init(void);
void klog_set_level(int level);
int klog_get_level(void);
int klog_module_set_level(const char *module, int level);
int klog_module_get_level(const char *module);
void klog_emit(int level, const char *message);
void klog_emit_tagged(const char *module, int level, const char *message);
size_t klog_copy(struct klog_entry *out, size_t max_entries);
const char *klog_level_name(int level);
int klog_level_from_name(const char *name);
void klog_enable_proc_sink(void);
void klog_refresh_proc_sink(void);

#define klog_debug(msg) klog_emit_tagged(KLOG_DEFAULT_TAG, KLOG_DEBUG, msg)
#define klog_info(msg)  klog_emit_tagged(KLOG_DEFAULT_TAG, KLOG_INFO, msg)
#define klog_warn(msg)  klog_emit_tagged(KLOG_DEFAULT_TAG, KLOG_WARN, msg)
#define klog_error(msg) klog_emit_tagged(KLOG_DEFAULT_TAG, KLOG_ERROR, msg)

#endif
