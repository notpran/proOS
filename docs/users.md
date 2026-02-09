# proOS Users Overview
- Kernel ships two built-in users: kernel (UID=0xFFFFFFFE) and root (UID=0).
- Both start with full permissions; kernel always runs in SECURITY_DOMAIN_KERNEL.
- Root gets SECURITY_DOMAIN_SYSTEM and inherits to sessions created for userland.
- User sessions are reference-counted; `process_set_session` acquires and releases them.
- Processes default to kernel session until they bind to a user session.
- Sandboxed domains were reverted; no sandbox enforcement remains in v0.9.
- Shell commands execute under the session of the owning process (usually root).
- Destroying a session is blocked while any process still holds a reference.
