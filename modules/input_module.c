#include <stddef.h>

#include "module_api.h"

#include "klog.h"
#include "keyboard.h"
#include "ramfs.h"

MODULE_METADATA("legacy-input", "0.0.0", 0);

static size_t local_strlen(const char *s)
{
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        ++len;
    return len;
}

int module_init(void)
{
    return -1;
}

void module_exit(void)
{
}
}
