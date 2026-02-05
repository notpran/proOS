#include "security.h"

#include "config.h"
#include "klog.h"
#include "spinlock.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

#ifndef CONFIG_SECURITY_MAX_USERS
#define CONFIG_SECURITY_MAX_USERS 16
#endif

#ifndef CONFIG_SECURITY_MAX_SESSIONS
#define CONFIG_SECURITY_MAX_SESSIONS 16
#endif

struct user_slot
{
    uint8_t used;
    user_t user;
    uint8_t reserved;
};

struct session_slot
{
    uint8_t used;
    sid_t sid;
    uid_t uid;
    gid_t gid;
    uint32_t permissions;
    uint32_t refcount;
};

static struct user_slot g_users[CONFIG_SECURITY_MAX_USERS];
static struct session_slot g_sessions[CONFIG_SECURITY_MAX_SESSIONS];
static spinlock_t g_user_lock;
static spinlock_t g_session_lock;
static sid_t g_next_sid = SECURITY_SID_KERNEL + 1u;
static sid_t g_root_session = SECURITY_SID_INVALID;

static void zero_memory(void *ptr, size_t size)
{
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < size; ++i)
        p[i] = 0;
}

static int strings_equal(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    size_t ia = 0;
    while (a[ia] && b[ia])
    {
        if (a[ia] != b[ia])
            return 0;
        ++ia;
    }
    return a[ia] == '\0' && b[ia] == '\0';
}

static struct user_slot *user_slot_by_uid(uid_t uid)
{
    for (size_t i = 0; i < CONFIG_SECURITY_MAX_USERS; ++i)
    {
        if (g_users[i].used && g_users[i].user.uid == uid)
            return &g_users[i];
    }
    return NULL;
}

static struct user_slot *user_slot_by_name(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < CONFIG_SECURITY_MAX_USERS; ++i)
    {
        if (!g_users[i].used)
            continue;
        if (strings_equal(g_users[i].user.username, name))
            return &g_users[i];
    }
    return NULL;
}

static struct session_slot *session_slot_by_sid(sid_t sid)
{
    for (size_t i = 0; i < CONFIG_SECURITY_MAX_SESSIONS; ++i)
    {
        if (g_sessions[i].used && g_sessions[i].sid == sid)
            return &g_sessions[i];
    }
    return NULL;
}

static struct user_slot *allocate_user_slot(void)
{
    for (size_t i = 0; i < CONFIG_SECURITY_MAX_USERS; ++i)
    {
        if (!g_users[i].used)
            return &g_users[i];
    }
    return NULL;
}

static struct session_slot *allocate_session_slot(void)
{
    for (size_t i = 0; i < CONFIG_SECURITY_MAX_SESSIONS; ++i)
    {
        if (!g_sessions[i].used)
            return &g_sessions[i];
    }
    return NULL;
}

static void register_builtin_user(uid_t uid, gid_t gid, const char *username, uint32_t permissions)
{
    uint32_t flags = 0;
    spinlock_lock_irqsave(&g_user_lock, &flags);

    struct user_slot *slot = user_slot_by_uid(uid);
    if (!slot)
        slot = allocate_user_slot();

    if (slot)
    {
        slot->used = 1;
        slot->user.uid = uid;
        slot->user.gid = gid;
        slot->user.permissions = permissions;
        for (size_t i = 0; i < sizeof(slot->user.username); ++i)
            slot->user.username[i] = '\0';
        if (username)
        {
            size_t idx = 0;
            while (username[idx] && idx < sizeof(slot->user.username) - 1u)
            {
                char ch = username[idx];
                if (ch < 32 || ch > 126)
                    ch = '?';
                slot->user.username[idx] = ch;
                ++idx;
            }
            slot->user.username[idx] = '\0';
        }
    }
    else
    {
        klog_warn("security: user table full");
    }

    spinlock_unlock_irqrestore(&g_user_lock, flags);
}

static sid_t register_builtin_session(uid_t uid, gid_t gid, uint32_t permissions, sid_t preferred)
{
    sid_t assigned = SECURITY_SID_INVALID;
    uint32_t flags = 0;
    spinlock_lock_irqsave(&g_session_lock, &flags);

    struct session_slot *slot = allocate_session_slot();
    if (slot)
    {
        slot->used = 1;
        slot->uid = uid;
        slot->gid = gid;
        slot->permissions = permissions;
        slot->refcount = 0;
        if (preferred != SECURITY_SID_INVALID)
        {
            slot->sid = preferred;
        }
        else
        {
            slot->sid = g_next_sid++;
            if (g_next_sid == SECURITY_SID_INVALID)
                g_next_sid = SECURITY_SID_KERNEL + 1u;
        }
        assigned = slot->sid;
    }

    spinlock_unlock_irqrestore(&g_session_lock, flags);
    return assigned;
}

void security_system_init(void)
{
    spinlock_init(&g_user_lock);
    spinlock_init(&g_session_lock);
    zero_memory(g_users, sizeof(g_users));
    zero_memory(g_sessions, sizeof(g_sessions));
    g_next_sid = SECURITY_SID_KERNEL + 1u;
    g_root_session = SECURITY_SID_INVALID;

    register_builtin_user(SECURITY_UID_KERNEL, SECURITY_GID_KERNEL, "kernel", SECURITY_PERMISSION_ALL);
    register_builtin_user(SECURITY_UID_ROOT, SECURITY_GID_ROOT, "kernel", SECURITY_PERMISSION_ALL);

    sid_t kernel_sid = register_builtin_session(SECURITY_UID_KERNEL, SECURITY_GID_KERNEL, SECURITY_PERMISSION_ALL, SECURITY_SID_KERNEL);
    if (kernel_sid != SECURITY_SID_KERNEL)
        klog_warn("security: failed to create kernel session");

    g_root_session = register_builtin_session(SECURITY_UID_ROOT, SECURITY_GID_ROOT, SECURITY_PERMISSION_ALL, SECURITY_SID_INVALID);
    if (g_root_session == SECURITY_SID_INVALID)
        klog_warn("security: failed to create root session");
}

size_t security_user_count(void)
{
    size_t count = 0;
    uint32_t flags = 0;
    spinlock_lock_irqsave(&g_user_lock, &flags);
    for (size_t i = 0; i < CONFIG_SECURITY_MAX_USERS; ++i)
    {
        if (g_users[i].used)
            ++count;
    }
    spinlock_unlock_irqrestore(&g_user_lock, flags);
    return count;
}

const user_t *security_user_get(uid_t uid)
{
    const user_t *result = NULL;
    uint32_t flags = 0;
    spinlock_lock_irqsave(&g_user_lock, &flags);
    struct user_slot *slot = user_slot_by_uid(uid);
    if (slot)
        result = &slot->user;
    spinlock_unlock_irqrestore(&g_user_lock, flags);
    return result;
}

const user_t *security_user_find(const char *username)
{
    const user_t *result = NULL;
    uint32_t flags = 0;
    spinlock_lock_irqsave(&g_user_lock, &flags);
    struct user_slot *slot = user_slot_by_name(username);
    if (slot)
        result = &slot->user;
    spinlock_unlock_irqrestore(&g_user_lock, flags);
    return result;
}

size_t security_user_list(user_t *out, size_t max)
{
    if (!out || max == 0)
        return 0;

    size_t total = 0;
    uint32_t flags = 0;
    spinlock_lock_irqsave(&g_user_lock, &flags);
    for (size_t i = 0; i < CONFIG_SECURITY_MAX_USERS && total < max; ++i)
    {
        if (!g_users[i].used)
            continue;
        out[total++] = g_users[i].user;
    }
    spinlock_unlock_irqrestore(&g_user_lock, flags);
    return total;
}

sid_t security_session_kernel(void)
{
    return SECURITY_SID_KERNEL;
}

sid_t security_session_root(void)
{
    return g_root_session;
}

int security_session_create(uid_t uid, uint32_t permissions, sid_t *out_sid)
{
    struct user_slot user_copy;
    int result = -1;

    uint32_t user_flags = 0;
    spinlock_lock_irqsave(&g_user_lock, &user_flags);
    struct user_slot *user_slot = user_slot_by_uid(uid);
    if (user_slot)
        user_copy = *user_slot;
    spinlock_unlock_irqrestore(&g_user_lock, user_flags);

    if (!user_slot)
        return -1;

    uint32_t session_flags = 0;
    spinlock_lock_irqsave(&g_session_lock, &session_flags);

    struct session_slot *slot = allocate_session_slot();
    if (slot)
    {
        slot->used = 1;
        slot->sid = g_next_sid++;
        if (g_next_sid == SECURITY_SID_INVALID)
            g_next_sid = SECURITY_SID_KERNEL + 1u;
        slot->uid = user_copy.user.uid;
        slot->gid = user_copy.user.gid;
        slot->permissions = (permissions == 0u) ? user_copy.user.permissions : permissions;
        slot->refcount = 0;
        if (out_sid)
            *out_sid = slot->sid;
        result = 0;
    }

    spinlock_unlock_irqrestore(&g_session_lock, session_flags);
    return result;
}

int security_session_destroy(sid_t sid)
{
    if (sid == SECURITY_SID_KERNEL)
        return -1;
    if (sid == g_root_session)
        return -1;

    int result = -1;
    uint32_t flags = 0;
    spinlock_lock_irqsave(&g_session_lock, &flags);
    struct session_slot *slot = session_slot_by_sid(sid);
    if (slot && slot->used && slot->refcount == 0)
    {
        slot->used = 0;
        slot->sid = SECURITY_SID_INVALID;
        slot->uid = SECURITY_UID_KERNEL;
        slot->gid = SECURITY_GID_KERNEL;
        slot->permissions = 0;
        slot->refcount = 0;
        result = 0;
    }
    spinlock_unlock_irqrestore(&g_session_lock, flags);
    return result;
}

int security_session_info(sid_t sid, struct security_session_info *out)
{
    if (!out)
        return -1;

    struct session_slot copy;
    int have_copy = 0;

    uint32_t flags = 0;
    spinlock_lock_irqsave(&g_session_lock, &flags);
    struct session_slot *slot = session_slot_by_sid(sid);
    if (slot && slot->used)
    {
        copy = *slot;
        have_copy = 1;
    }
    spinlock_unlock_irqrestore(&g_session_lock, flags);

    if (!have_copy)
        return -1;

    out->sid = copy.sid;
    out->uid = copy.uid;
    out->gid = copy.gid;
    out->permissions = copy.permissions;
    out->refcount = copy.refcount;
    out->user = security_user_get(copy.uid);
    return 0;
}

int security_session_acquire(sid_t sid)
{
    int result = -1;
    uint32_t flags = 0;
    spinlock_lock_irqsave(&g_session_lock, &flags);
    struct session_slot *slot = session_slot_by_sid(sid);
    if (slot && slot->used)
    {
        ++slot->refcount;
        result = 0;
    }
    spinlock_unlock_irqrestore(&g_session_lock, flags);
    return result;
}

void security_session_release(sid_t sid)
{
    uint32_t flags = 0;
    spinlock_lock_irqsave(&g_session_lock, &flags);
    struct session_slot *slot = session_slot_by_sid(sid);
    if (slot && slot->used && slot->refcount > 0)
        --slot->refcount;
    spinlock_unlock_irqrestore(&g_session_lock, flags);
}
