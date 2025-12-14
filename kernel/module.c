#include "module.h"

#include "elf.h"
#include "klog.h"
#include "memory.h"
#include "string.h"

#include <stdint.h>

#define MODULE_MAX_COUNT    16
#define MODULE_MAX_SYMBOLS  256
#define MODULE_MAX_SECTIONS 128

static module_handle_t module_table[MODULE_MAX_COUNT];
static size_t module_count = 0;

static struct kernel_symbol symbol_table[MODULE_MAX_SYMBOLS];
static size_t symbol_count = 0;

static void emit_log(int level, const char *prefix, const char *name);
static int str_equals(const char *a, const char *b);
static void zero_handle(module_handle_t *handle);

static int module_name_in_use(const char *name)
{
    if (!name)
        return 0;
    for (size_t i = 0; i < module_count; ++i)
    {
        if (str_equals(module_table[i].meta.name, name))
            return 1;
    }
    return 0;
}

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    if (alignment <= 1)
        return value;
    uint32_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

static size_t str_length(const char *s)
{
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        ++len;
    return len;
}

static void str_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    size_t len = str_length(src);
    if (len >= dst_size)
        len = dst_size - 1;
    for (size_t i = 0; i < len; ++i)
        dst[i] = src[i];
    dst[len] = '\0';
}

static int str_equals(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b)
    {
        if (*a != *b)
            return 0;
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0');
}

static void zero_handle(module_handle_t *handle)
{
    if (!handle)
        return;
    handle->meta.name[0] = '\0';
    handle->meta.version[0] = '\0';
    handle->meta.flags = 0;
    handle->meta.base = 0;
    handle->meta.size = 0;
    handle->meta.active = 0;
    handle->meta.initialized = 0;
    handle->meta.autostart = 0;
    handle->meta.builtin = 0;
    handle->init = NULL;
    handle->exit = NULL;
}

void module_register_kernel_symbol(const char *name, const void *addr)
{
    if (!name || !addr)
        return;

    for (size_t i = 0; i < symbol_count; ++i)
    {
        if (str_equals(symbol_table[i].name, name))
        {
            symbol_table[i].address = (uintptr_t)addr;
            return;
        }
    }

    if (symbol_count >= MODULE_MAX_SYMBOLS)
        return;

    symbol_table[symbol_count].name = name;
    symbol_table[symbol_count].address = (uintptr_t)addr;
    ++symbol_count;
}

void module_register_kernel_symbols(const struct kernel_symbol *symbols, size_t count)
{
    if (!symbols || count == 0)
        return;
    for (size_t i = 0; i < count; ++i)
        module_register_kernel_symbol(symbols[i].name, (const void *)symbols[i].address);
}

void *module_lookup_kernel_symbol(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < symbol_count; ++i)
    {
        if (str_equals(symbol_table[i].name, name))
            return (void *)symbol_table[i].address;
    }
    return NULL;
}

static uintptr_t resolve_symbol_value(const uint8_t *module_mem,
                                      const Elf32_Shdr *sections,
                                      const uint32_t *section_offsets,
                                      const Elf32_Sym *sym)
{
    (void)sections;
    if (sym->st_shndx == SHN_UNDEF)
        return 0;
    uint16_t index = sym->st_shndx;
    uint32_t base_offset = section_offsets[index];
    return (uintptr_t)(module_mem + base_offset + sym->st_value);
}

static int apply_relocations(const uint8_t *image,
                             size_t size,
                             uint8_t *module_mem,
                             const Elf32_Shdr *sections,
                             const uint32_t *section_offsets,
                             uint16_t section_count)
{
    (void)size;

    for (uint16_t i = 0; i < section_count; ++i)
    {
        const Elf32_Shdr *rel_sec = &sections[i];
        if (rel_sec->sh_type != SHT_REL)
            continue;

        if (rel_sec->sh_info >= section_count || rel_sec->sh_link >= section_count)
            return -1;

        const Elf32_Shdr *symtab_sec = &sections[rel_sec->sh_link];
        if (symtab_sec->sh_type != SHT_SYMTAB)
            return -1;

        const Elf32_Sym *symtab = (const Elf32_Sym *)(image + symtab_sec->sh_offset);
        size_t sym_count = symtab_sec->sh_size / sizeof(Elf32_Sym);

        if (symtab_sec->sh_link >= section_count)
            return -1;

        const Elf32_Shdr *strtab_sec = &sections[symtab_sec->sh_link];
        const char *strtab = (const char *)(image + strtab_sec->sh_offset);
        (void)strtab;

        size_t rel_count = rel_sec->sh_size / sizeof(Elf32_Rel);
        const Elf32_Rel *rel_entries = (const Elf32_Rel *)(image + rel_sec->sh_offset);

        uint32_t target_base = section_offsets[rel_sec->sh_info];
        if (target_base == 0 && !(sections[rel_sec->sh_info].sh_flags & SHF_ALLOC))
            return -1;

        for (size_t r = 0; r < rel_count; ++r)
        {
            const Elf32_Rel *rel = &rel_entries[r];
            uint32_t sym_index = ELF32_R_SYM(rel->r_info);
            uint32_t type = ELF32_R_TYPE(rel->r_info);
            if (sym_index >= sym_count)
                return -1;

            const Elf32_Sym *sym = &symtab[sym_index];
            uintptr_t sym_addr = 0;
            if (sym->st_shndx == SHN_UNDEF)
            {
                const char *sym_name = strtab + sym->st_name;
                void *resolved = module_lookup_kernel_symbol(sym_name);
                if (!resolved)
                {
                    const char *sym_name = strtab + sym->st_name;
                    emit_log(KLOG_ERROR, "module: unresolved symbol ", sym_name);
                    return -1;
                }
                sym_addr = (uintptr_t)resolved;
            }
            else
            {
                sym_addr = resolve_symbol_value(module_mem, sections, section_offsets, sym);
            }

            uint32_t target_off = section_offsets[rel_sec->sh_info] + rel->r_offset;
            uint32_t *target_word = (uint32_t *)(module_mem + target_off);
            uint32_t addend = *target_word;

            switch (type)
            {
                case R_386_32:
                    *target_word = (uint32_t)(sym_addr + addend);
                    break;
                case R_386_PC32:
                    *target_word = (uint32_t)(sym_addr + addend - (uintptr_t)target_word);
                    break;
                default:
                    klog_error("module: unsupported relocation type");
                    return -1;
            }
        }
    }

    return 0;
}

static const struct module_info *find_module_info(const uint8_t *image,
                                                  const Elf32_Shdr *sections,
                                                  const uint32_t *section_offsets,
                                                  const Elf32_Sym *symtab,
                                                  size_t sym_count,
                                                  const char *strtab,
                                                  uint8_t *module_base)
{
    (void)image;
    for (size_t i = 0; i < sym_count; ++i)
    {
        const Elf32_Sym *sym = &symtab[i];
        const char *name = strtab + sym->st_name;
        if (str_equals(name, "__module_info"))
        {
            uintptr_t addr = resolve_symbol_value(module_base, sections, section_offsets, sym);
            return (const struct module_info *)addr;
        }
    }
    return NULL;
}

static module_init_fn find_module_init(const uint8_t *image,
                                       const Elf32_Shdr *sections,
                                       const uint32_t *section_offsets,
                                       const Elf32_Sym *symtab,
                                       size_t sym_count,
                                       const char *strtab,
                                       uint8_t *module_base)
{
    (void)image;
    for (size_t i = 0; i < sym_count; ++i)
    {
        const Elf32_Sym *sym = &symtab[i];
        const char *name = strtab + sym->st_name;
        if (str_equals(name, "module_init"))
        {
            uintptr_t addr = resolve_symbol_value(module_base, sections, section_offsets, sym);
            return (module_init_fn)addr;
        }
    }
    return NULL;
}

static module_exit_fn find_module_exit(const uint8_t *image,
                                       const Elf32_Shdr *sections,
                                       const uint32_t *section_offsets,
                                       const Elf32_Sym *symtab,
                                       size_t sym_count,
                                       const char *strtab,
                                       uint8_t *module_base)
{
    (void)image;
    for (size_t i = 0; i < sym_count; ++i)
    {
        const Elf32_Sym *sym = &symtab[i];
        const char *name = strtab + sym->st_name;
        if (str_equals(name, "module_exit"))
        {
            uintptr_t addr = resolve_symbol_value(module_base, sections, section_offsets, sym);
            return (module_exit_fn)addr;
        }
    }
    return NULL;
}

static void emit_log(int level, const char *prefix, const char *name)
{
    char buffer[64];
    size_t idx = 0;
    if (prefix)
    {
        while (prefix[idx] && idx + 1 < sizeof(buffer))
        {
            buffer[idx] = prefix[idx];
            ++idx;
        }
    }

    if (name)
    {
        for (size_t j = 0; name[j] && idx + 1 < sizeof(buffer); ++j)
        {
            buffer[idx++] = name[j];
        }
    }

    buffer[idx] = '\0';
    klog_emit(level, buffer);
}

static int load_module_internal(const char *label, const uint8_t *image, size_t size, int builtin)
{
    if (!image || size < sizeof(Elf32_Ehdr))
        return -1;

    if (module_count >= MODULE_MAX_COUNT)
        return -1;

    const Elf32_Ehdr *hdr = (const Elf32_Ehdr *)image;
    if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' || hdr->e_ident[2] != 'L' || hdr->e_ident[3] != 'F')
        return -1;
    if (hdr->e_type != ET_REL || hdr->e_machine != EM_386 || hdr->e_version != EV_CURRENT)
        return -1;
    if (hdr->e_shnum == 0 || hdr->e_shnum > MODULE_MAX_SECTIONS)
        return -1;

    const Elf32_Shdr *sections = (const Elf32_Shdr *)(image + hdr->e_shoff);
    uint32_t section_offsets[MODULE_MAX_SECTIONS];
    for (uint16_t i = 0; i < hdr->e_shnum; ++i)
        section_offsets[i] = 0;

    uint32_t total_size = 0;
    for (uint16_t i = 0; i < hdr->e_shnum; ++i)
    {
        const Elf32_Shdr *sec = &sections[i];
        if (!(sec->sh_flags & SHF_ALLOC))
            continue;
        uint32_t align = sec->sh_addralign ? sec->sh_addralign : 4u;
        total_size = align_up(total_size, align);
        section_offsets[i] = total_size;
        total_size += sec->sh_size;
    }

    if (total_size == 0)
        return -1;

    uint8_t *module_mem = (uint8_t *)kalloc_zero(total_size);
    if (!module_mem)
        return -1;

    for (uint16_t i = 0; i < hdr->e_shnum; ++i)
    {
        const Elf32_Shdr *sec = &sections[i];
        if (!(sec->sh_flags & SHF_ALLOC))
            continue;
        uint8_t *dest = module_mem + section_offsets[i];
        if (sec->sh_type == SHT_NOBITS)
        {
            memset(dest, 0, sec->sh_size);
        }
        else
        {
            if (sec->sh_offset + sec->sh_size > size)
                return -1;
            memcpy(dest, image + sec->sh_offset, sec->sh_size);
        }
    }

    if (apply_relocations(image, size, module_mem, sections, section_offsets, hdr->e_shnum) != 0)
        return -1;

    const Elf32_Sym *symtab = NULL;
    size_t sym_count = 0;
    const char *strtab = NULL;
    for (uint16_t i = 0; i < hdr->e_shnum; ++i)
    {
        const Elf32_Shdr *sec = &sections[i];
        if (sec->sh_type == SHT_SYMTAB)
        {
            symtab = (const Elf32_Sym *)(image + sec->sh_offset);
            sym_count = sec->sh_size / sizeof(Elf32_Sym);
            if (sec->sh_link < hdr->e_shnum)
            {
                const Elf32_Shdr *strings = &sections[sec->sh_link];
                strtab = (const char *)(image + strings->sh_offset);
            }
            break;
        }
    }

    if (!symtab || !strtab)
        return -1;

    const struct module_info *info = find_module_info(image, sections, section_offsets, symtab, sym_count, strtab, module_mem);
    if (!info)
        return -1;

    module_handle_t *handle = &module_table[module_count];
    zero_handle(handle);
    str_copy(handle->meta.name, sizeof(handle->meta.name), info->name);

    if (module_name_in_use(handle->meta.name))
    {
        emit_log(KLOG_WARN, "module: already loaded ", handle->meta.name);
        zero_handle(handle);
        return -1;
    }

    str_copy(handle->meta.version, sizeof(handle->meta.version), info->version);
    handle->meta.flags = info->flags;
    handle->meta.base = (uintptr_t)module_mem;
    handle->meta.size = total_size;
    handle->meta.autostart = (info->flags & MODULE_FLAG_AUTOSTART) ? 1 : 0;
    handle->meta.builtin = builtin;

    handle->init = find_module_init(image, sections, section_offsets, symtab, sym_count, strtab, module_mem);
    handle->exit = find_module_exit(image, sections, section_offsets, symtab, sym_count, strtab, module_mem);

    ++module_count;

    (void)label;
    emit_log(KLOG_INFO, "module: loaded ", handle->meta.name);

    if (handle->meta.autostart && handle->init)
    {
        int rc = handle->init();
        if (rc == 0)
        {
            handle->meta.active = 1;
            handle->meta.initialized = 1;
        }
        else
        {
            emit_log(KLOG_ERROR, "module: init failed ", handle->meta.name);
        }
    }

    return 0;
}

static int load_builtin_modules(void)
{
    extern const uint8_t _binary_build_modules_fs_kmd_start[];
    extern const uint8_t _binary_build_modules_fs_kmd_end[];
    extern const uint8_t _binary_build_modules_ps2kbd_kmd_start[];
    extern const uint8_t _binary_build_modules_ps2kbd_kmd_end[];
    extern const uint8_t _binary_build_modules_ps2mouse_kmd_start[];
    extern const uint8_t _binary_build_modules_ps2mouse_kmd_end[];
    extern const uint8_t _binary_build_modules_pit_kmd_start[];
    extern const uint8_t _binary_build_modules_pit_kmd_end[];
    extern const uint8_t _binary_build_modules_rtc_kmd_start[];
    extern const uint8_t _binary_build_modules_rtc_kmd_end[];
    extern const uint8_t _binary_build_modules_biosdisk_kmd_start[];
    extern const uint8_t _binary_build_modules_biosdisk_kmd_end[];
    extern const uint8_t _binary_build_modules_ata_kmd_start[];
    extern const uint8_t _binary_build_modules_ata_kmd_end[];
    extern const uint8_t _binary_build_modules_time_kmd_start[];
    extern const uint8_t _binary_build_modules_time_kmd_end[];

    struct builtin_entry
    {
        const char *label;
        const uint8_t *begin;
        const uint8_t *end;
    };

    static const struct builtin_entry entries[] = {
        { "fs.kmd", _binary_build_modules_fs_kmd_start, _binary_build_modules_fs_kmd_end },
        { "ps2kbd.kmd", _binary_build_modules_ps2kbd_kmd_start, _binary_build_modules_ps2kbd_kmd_end },
        { "ps2mouse.kmd", _binary_build_modules_ps2mouse_kmd_start, _binary_build_modules_ps2mouse_kmd_end },
        { "pit.kmd", _binary_build_modules_pit_kmd_start, _binary_build_modules_pit_kmd_end },
        { "rtc.kmd", _binary_build_modules_rtc_kmd_start, _binary_build_modules_rtc_kmd_end },
        { "biosdisk.kmd", _binary_build_modules_biosdisk_kmd_start, _binary_build_modules_biosdisk_kmd_end },
        { "ata.kmd", _binary_build_modules_ata_kmd_start, _binary_build_modules_ata_kmd_end },
        { "time.kmd", _binary_build_modules_time_kmd_start, _binary_build_modules_time_kmd_end }
    };

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i)
    {
        size_t len = (size_t)(entries[i].end - entries[i].begin);
        if (len == 0)
            continue;
        module_load_image(entries[i].label, entries[i].begin, len, 1);
    }

    return 0;
}

void module_system_init(void)
{
    module_count = 0;
    symbol_count = 0;

    module_register_builtin_symbols();
    load_builtin_modules();
}

int module_load_image(const char *label, const void *image, size_t size, int builtin)
{
    return load_module_internal(label, (const uint8_t *)image, size, builtin);
}

int module_unload(const char *name)
{
    if (!name)
        return -1;

    for (size_t i = 0; i < module_count; ++i)
    {
        module_handle_t *handle = &module_table[i];
        if (!str_equals(handle->meta.name, name))
            continue;
        if (handle->meta.builtin)
            return -1;
        if (handle->exit && handle->meta.active)
            handle->exit();
        handle->meta.active = 0;
        handle->meta.initialized = 0;
        emit_log(KLOG_INFO, "module: unloaded ", handle->meta.name);

        size_t remaining = module_count - i - 1;
        if (remaining > 0)
            memmove(&module_table[i], &module_table[i + 1], remaining * sizeof(module_handle_t));
        --module_count;
        zero_handle(&module_table[module_count]);
        return 0;
    }

    return -1;
}

const module_handle_t *module_find(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < module_count; ++i)
    {
        if (str_equals(module_table[i].meta.name, name))
            return &module_table[i];
    }
    return NULL;
}

size_t module_enumerate(const module_handle_t **out_array, size_t max_count)
{
    if (!out_array || max_count == 0)
        return 0;

    size_t to_copy = (module_count < max_count) ? module_count : max_count;
    for (size_t i = 0; i < to_copy; ++i)
        out_array[i] = &module_table[i];
    return to_copy;
}

