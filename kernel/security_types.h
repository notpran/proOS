#ifndef SECURITY_TYPES_H
#define SECURITY_TYPES_H

#include <stdint.h>

typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint32_t sid_t;

typedef struct
{
    uid_t uid;
    gid_t gid;
    char username[32];
    uint32_t permissions;
} user_t;

#define SECURITY_UID_KERNEL       0xFFFFFFFEu
#define SECURITY_GID_KERNEL       0xFFFFFFFEu
#define SECURITY_UID_ROOT         0u
#define SECURITY_GID_ROOT         0u
#define SECURITY_SID_INVALID      0u
#define SECURITY_SID_KERNEL       1u
#define SECURITY_PERMISSION_ALL   0xFFFFFFFFu

#endif
