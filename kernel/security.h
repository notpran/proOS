#ifndef SECURITY_H
#define SECURITY_H

#include <stddef.h>
#include <stdint.h>

#include "security_types.h"

struct security_session_info
{
    sid_t sid;
    uid_t uid;
    gid_t gid;
    uint32_t permissions;
    const user_t *user;
    uint32_t refcount;
};

void security_system_init(void);

/* User management */
size_t security_user_count(void);
const user_t *security_user_get(uid_t uid);
const user_t *security_user_find(const char *username);
size_t security_user_list(user_t *out, size_t max);

/* Session management */
sid_t security_session_kernel(void);
sid_t security_session_root(void);
int security_session_create(uid_t uid, uint32_t permissions, sid_t *out_sid);
int security_session_destroy(sid_t sid);
int security_session_info(sid_t sid, struct security_session_info *out);
int security_session_acquire(sid_t sid);
void security_session_release(sid_t sid);

#endif
