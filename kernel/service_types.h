#ifndef SERVICE_TYPES_H
#define SERVICE_TYPES_H

#include <stdint.h>

enum system_service
{
    SYSTEM_SERVICE_FSD = 0,
    SYSTEM_SERVICE_NETD = 1,
    SYSTEM_SERVICE_INPUTD = 2,
    SYSTEM_SERVICE_LOGD = 3,
    SYSTEM_SERVICE_GUI = 4,
    SYSTEM_SERVICE_COUNT
};

#endif
