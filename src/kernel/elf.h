#ifndef ELF_H
#define ELF_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define ELF_MAGIC 0x464C457F
#define ET_EXEC   2
#define ET_DYN    3

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_PHDR    6

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9

#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_PLTGOT       3
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_FINI         13
#define DT_SONAME       14
#define DT_RPATH        15
#define DT_SYMBOLIC     16
#define DT_REL          17
#define DT_RELSZ        18
#define DT_RELENT       19
#define DT_PLTREL       20
#define DT_DEBUG        21
#define DT_TEXTREL      22
#define DT_JMPREL       23
#define DT_BIND_NOW     24
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH      29

#define R_386_NONE     0
#define R_386_32       1
#define R_386_PC32     2
#define R_386_COPY     5
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8

#define ELF32_R_SYM(info)  ((info) >> 8)
#define ELF32_R_TYPE(info) ((unsigned char)(info))

typedef struct {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u32 e_entry;
    u32 e_phoff;
    u32 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    u32 p_type;
    u32 p_offset;
    u32 p_vaddr;
    u32 p_paddr;
    u32 p_filesz;
    u32 p_memsz;
    u32 p_flags;
    u32 p_align;
} Elf32_Phdr;

typedef struct {
    int d_tag;
    union {
        u32 d_val;
        u32 d_ptr;
    } d_un;
} Elf32_Dyn;

typedef struct {
    u32 st_name;
    u32 st_value;
    u32 st_size;
    unsigned char st_info;
    unsigned char st_other;
    u16 st_shndx;
} Elf32_Sym;

typedef struct {
    u32 r_offset;
    u32 r_info;
} Elf32_Rel;

#endif
