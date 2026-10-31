#pragma once
#define EI_NIDENT   16

#include <stdint.h>

typedef uint64_t ELF64_ADDR;
typedef uint64_t ELF64_OFFS;
typedef uint16_t ELF64_HALF;
typedef uint32_t ELF64_WORD;
typedef uint64_t ELF64_LONG;
typedef int64_t  ELF64_SLONG;

#define PT_NULL     0
#define PT_LOAD     1
#define PT_INTERP   3

#define ET_REL      1
#define ET_EXEC     2

#define EM_X86_64   62

#define PF_N        0x0
#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4

#define EI_CLASS64  2

#define EI_LITTLE_ENDIAN    1
#define EI_BIG_ENDIAN       2

typedef struct {
    unsigned char   e_ident[EI_NIDENT];
    ELF64_HALF      e_type;
    ELF64_HALF      e_machine;
    ELF64_WORD      e_version;
    ELF64_ADDR      e_entry;
    ELF64_OFFS      e_phoff;
    ELF64_OFFS      e_shoff;
    ELF64_WORD      e_flags;
    ELF64_HALF      e_ehsize;
    ELF64_HALF      e_phentsize;
    ELF64_HALF      e_phnum;
    ELF64_HALF      e_shentsize;
    ELF64_HALF      e_shnum;
    ELF64_HALF      e_shstrndx;
} ELF64_EHDR;

typedef struct {
	ELF64_WORD	    p_type;
	ELF64_WORD	    p_flags;
	ELF64_OFFS	    p_offset;
	ELF64_ADDR	    p_vaddr;
	ELF64_ADDR	    p_paddr;
	ELF64_LONG	    p_filesz;
	ELF64_SLONG	    p_memsz;
	ELF64_SLONG	    p_align;
} ELF64_Phdr;

typedef struct {
    ELF64_WORD      sh_name;
    ELF64_WORD      sh_type;
    ELF64_LONG      sh_flags;
    ELF64_ADDR      sh_addr;
    ELF64_OFFS      sh_offset;
    ELF64_LONG      sh_size;
    ELF64_WORD      sh_link;
    ELF64_WORD      sh_info;
    ELF64_LONG      sh_addralign;
    ELF64_LONG      sh_entsize;
} ELF64_Shdr;

typedef struct {
    ELF64_WORD      st_name;
    unsigned char   st_info;
    unsigned char   st_other;
    ELF64_HALF      st_shndx;
    ELF64_ADDR      st_value;
    ELF64_LONG      st_size;
} ELF64_Sym;

typedef struct {
    ELF64_ADDR      r_offset;
    ELF64_LONG      r_info;
    ELF64_SLONG     r_addend;
} ELF64_Rela;

#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_NOBITS      8
#define SHT_REL         9

#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4

#define SHN_UNDEF       0
#define SHN_ABS         0xFFF1
#define SHN_COMMON      0xFFF2

#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STB_WEAK        2

#define STT_NOTYPE      0
#define STT_OBJECT      1
#define STT_FUNC        2
#define STT_SECTION     3
#define STT_FILE        4

#define ELF64_ST_BIND(i)    ((i) >> 4)
#define ELF64_ST_TYPE(i)    ((i) & 0xF)
#define ELF64_R_SYM(i)      ((i) >> 32)
#define ELF64_R_TYPE(i)     ((i) & 0xFFFFFFFF)

#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_PC32       2
#define R_X86_64_PLT32      4
#define R_X86_64_32         10
#define R_X86_64_32S        11
#define R_X86_64_PC64       24
