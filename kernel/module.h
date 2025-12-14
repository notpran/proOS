#ifndef MODULE_H
#define MODULE_H

#include <stddef.h>
#include <stdint.h>

#include "module_api.h"

struct kernel_symbol
{
    const char *name;
    uintptr_t address;
};

struct loaded_module
{
    char name[MODULE_NAME_MAX];
    char version[MODULE_VERSION_MAX];
    uint32_t flags;
    uintptr_t base;
    size_t size;
    int active;
    int initialized;
    int autostart;
    int builtin;
};

typedef int (*module_init_fn)(void);
typedef void (*module_exit_fn)(void);

typedef struct module_handle
{
    struct loaded_module meta;
    module_init_fn init;
    module_exit_fn exit;
} module_handle_t;

void module_system_init(void);
int module_load_image(const char *label, const void *image, size_t size, int builtin);
int module_unload(const char *name);
const module_handle_t *module_find(const char *name);
size_t module_enumerate(const module_handle_t **out_array, size_t max_count);

void module_register_kernel_symbol(const char *name, const void *addr);
void module_register_kernel_symbols(const struct kernel_symbol *symbols, size_t count);
void *module_lookup_kernel_symbol(const char *name);

/* internal helpers */
void module_register_builtin_symbols(void);

#endif
