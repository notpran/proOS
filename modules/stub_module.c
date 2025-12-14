#include "module_api.h"

#include "klog.h"

MODULE_METADATA("stub", "0.0.1", 0);

int module_init(void)
{
    klog_info("stub.module: init");
    return 0;
}

void module_exit(void)
{
    klog_info("stub.module: exit");
}
