#ifndef IPC_TYPES_H
#define IPC_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef int pid_t;

#define IPC_ANY_PROCESS   ((pid_t)-1)
#define IPC_RIGHT_SEND    0x1u
#define IPC_RIGHT_RECV    0x2u
#define IPC_RIGHT_SHARE   0x4u

enum ipc_service_channel
{
    IPC_SERVICE_DEVMGR = 0,
    IPC_SERVICE_MODULE_LOADER = 1,
    IPC_SERVICE_LOGGER = 2,
    IPC_SERVICE_SCHEDULER = 3,
    IPC_SERVICE_COUNT
};

struct ipc_message
{
    uint32_t header;
    int32_t sender_pid;
    uint32_t type;
    uint32_t size;
    void *data;
};

#define IPC_RECV_NONBLOCK 0x1u
#define IPC_MESSAGE_TRUNCATED 0x1u

struct ipc_raw_message
{
    pid_t sender;
    pid_t target;
    uint32_t flags;
    uint32_t size;
    uint8_t data[CONFIG_MSG_DATA_MAX];
};

#endif
