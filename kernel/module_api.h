#ifndef MODULE_API_H
#define MODULE_API_H

#include <stdint.h>

#define MODULE_NAME_MAX     32
#define MODULE_VERSION_MAX  32

struct module_info
{
    char name[MODULE_NAME_MAX];
    char version[MODULE_VERSION_MAX];
    uint32_t flags;
    uint32_t reserved;
};

enum module_flags
{
    MODULE_FLAG_AUTOSTART = 1u << 0
};

#define MODULE_METADATA(name_literal, version_literal, flag_bits) \
    const struct module_info __module_info __attribute__((section(".modinfo"), used)) = { \
        name_literal, \
        version_literal, \
        flag_bits, \
        0u \
    }

#endif
