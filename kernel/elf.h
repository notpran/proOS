#ifndef ELF_H
#define ELF_H

#include <stdint.h>

#define EI_NIDENT 16

typedef struct
{
    uint8_t e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct
{
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct
{
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
} Elf32_Sym;

typedef struct
{
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;

#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xF)
#define ELF32_ST_INFO(b,t) (((b) << 4) + ((t) & 0xF))
#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((uint8_t)(i))
#define ELF32_R_INFO(sym, type) (((sym) << 8) + (uint8_t)(type))

enum
{
    ET_NONE = 0,
    ET_REL = 1,
    ET_EXEC = 2
};

enum
{
    EM_386 = 3
};

enum
{
    EV_NONE = 0,
    EV_CURRENT = 1
};

enum
{
    SHT_NULL = 0,
    SHT_PROGBITS = 1,
    SHT_SYMTAB = 2,
    SHT_STRTAB = 3,
    SHT_RELA = 4,
    SHT_HASH = 5,
    SHT_DYNAMIC = 6,
    SHT_NOTE = 7,
    SHT_NOBITS = 8,
    SHT_REL = 9
};

enum
{
    SHF_WRITE = 0x1,
    SHF_ALLOC = 0x2,
    SHF_EXECINSTR = 0x4
};

enum
{
    SHN_UNDEF = 0
};

enum
{
    R_386_NONE = 0,
    R_386_32 = 1,
    R_386_PC32 = 2
};

#endif
