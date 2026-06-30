#include "uapi_syscall.h"
#include "uapi_errno.h"
#include "elf.h"

typedef unsigned int u32;
typedef unsigned char u8;

#define LD_MAX_OBJECTS 32
#define LD_MAX_NEEDED 16
#define LD_MAX_DEPS LD_MAX_NEEDED
#define LD_MAX_OBJECT_PAGES 2048
#define LD_MAX_PATH 128
#define LD_ERROR_SIZE 160
#define LD_PAGE_SIZE 4096u
#define LD_MMAP_BASE 0x04000000u
#define LD_MMAP_LIMIT 0x08000000u
#define LD_PROT_READ 1
#define LD_PROT_WRITE 2
#define LD_PROT_EXEC 4
#define LD_MAP_PRIVATE 2
#define LD_MAP_FIXED 0x10
#define LD_MAP_ANON 0x20
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define SHN_UNDEF 0
#define RTLD_LAZY   0x001
#define RTLD_NOW    0x002
#define RTLD_GLOBAL 0x100

#define ELF32_ST_BIND(info) ((info) >> 4)

typedef struct ld_jmp_buf {
    unsigned int ebx;
    unsigned int esi;
    unsigned int edi;
    unsigned int ebp;
    unsigned int esp;
    unsigned int eip;
} ld_jmp_buf_t;

typedef struct ld_object {
    char name[LD_MAX_PATH];
    char path[LD_MAX_PATH];
    const char* soname;
    unsigned int load_bias;
    unsigned int map_start;
    unsigned int map_size;
    const Elf32_Phdr* phdr;
    unsigned int phnum;
    Elf32_Dyn* dynamic;
    Elf32_Sym* symtab;
    const char* strtab;
    unsigned int* hash;
    Elf32_Rel* rel;
    unsigned int relsz;
    Elf32_Rel* jmprel;
    unsigned int pltrelsz;
    void (*init)(void);
    void (**init_array)(void);
    unsigned int init_arraysz;
    void (*fini)(void);
    void (**fini_array)(void);
    unsigned int fini_arraysz;
    unsigned int soname_off;
    unsigned int rpath_off;
    unsigned int runpath_off;
    unsigned int needed[LD_MAX_NEEDED];
    unsigned int needed_count;
    unsigned int deps[LD_MAX_DEPS];
    unsigned int dep_count;
    unsigned int refcount;
    unsigned char loaded;
    unsigned char active;
    unsigned char pinned;
    unsigned char runtime_loaded;
    unsigned char failed;
    unsigned char relocated;
    unsigned char protected;
    unsigned char deps_loaded;
    unsigned char deps_loading;
    unsigned char initialized;
    unsigned char fini_done;
    unsigned char finalized;
    unsigned char init_visiting;
} ld_object_t;

typedef struct smallos_dlfcn_services {
    void* (*dlopen)(const char* filename, int flag);
    void* (*dlsym)(void* handle, const char* symbol);
    int (*dlclose)(void* handle);
    const char* (*dlerror)(void);
} smallos_dlfcn_services_t;

static ld_object_t g_objs[LD_MAX_OBJECTS];
static unsigned int g_obj_count;
static unsigned int g_next_map = LD_MMAP_BASE;
static unsigned int g_recover_obj_count;
static int g_last_mmap_error;
static char g_dl_error[LD_ERROR_SIZE];
static int g_dl_error_pending;
static int g_recover_active;
static ld_jmp_buf_t g_recover_env;

__attribute__((naked, noinline))
static int ld_setjmp(ld_jmp_buf_t* env) {
    __asm__ volatile(
        "movl 4(%esp), %edx\n"
        "movl %ebx, 0(%edx)\n"
        "movl %esi, 4(%edx)\n"
        "movl %edi, 8(%edx)\n"
        "movl %ebp, 12(%edx)\n"
        "leal 4(%esp), %eax\n"
        "movl %eax, 16(%edx)\n"
        "movl (%esp), %eax\n"
        "movl %eax, 20(%edx)\n"
        "xorl %eax, %eax\n"
        "ret\n");
}

__attribute__((noreturn, naked, noinline))
static void ld_longjmp(ld_jmp_buf_t* env, int value) {
    __asm__ volatile(
        "movl 8(%esp), %eax\n"
        "movl 4(%esp), %edx\n"
        "movl 0(%edx), %ebx\n"
        "movl 4(%edx), %esi\n"
        "movl 8(%edx), %edi\n"
        "movl 12(%edx), %ebp\n"
        "movl 16(%edx), %esp\n"
        "jmp *20(%edx)\n");
}

static int sc0(int num) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static int sc1(int num, u32 a) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a) : "memory");
    return ret;
}

static int sc2(int num, u32 a, u32 b) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a), "c"(b) : "memory");
    return ret;
}

static int sc3(int num, u32 a, u32 b, u32 c) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

static int sc4(int num, u32 a, u32 b, u32 c, u32 d) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return ret;
}

static int sc6(int num, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f) {
    int ret;
    __asm__ volatile(
        "pushl %[arg6]\n"
        "push %%ebp\n"
        "movl 4(%%esp), %%ebp\n"
        "int $0x80\n"
        "pop %%ebp\n"
        "addl $4, %%esp\n"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), [arg6] "g"(f)
        : "memory");
    return ret;
}

__attribute__((noreturn))
static void ld_exit(int code) {
    (void)sc1(SYS_EXIT, (u32)code);
    for (;;) {}
}

static unsigned int ld_strlen(const char* s) {
    unsigned int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int ld_streq(const char* a, const char* b) {
    unsigned int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static void ld_strcpy(char* dst, unsigned int dst_size, const char* src) {
    unsigned int pos = 0;
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    while (src[pos] && pos + 1 < dst_size) {
        dst[pos] = src[pos];
        pos++;
    }
    dst[pos] = 0;
}

static void ld_strappend(char* dst, unsigned int dst_size, const char* src) {
    unsigned int pos = 0;
    if (!dst || dst_size == 0) return;
    while (dst[pos] && pos + 1 < dst_size) pos++;
    for (unsigned int i = 0; src && src[i] && pos + 1 < dst_size; i++) {
        dst[pos++] = src[i];
    }
    dst[pos] = 0;
}

static void ld_set_error3(const char* a, const char* b, const char* c) {
    ld_strcpy(g_dl_error, sizeof(g_dl_error), a ? a : "");
    ld_strappend(g_dl_error, sizeof(g_dl_error), b);
    ld_strappend(g_dl_error, sizeof(g_dl_error), c);
    g_dl_error_pending = 1;
}

static void ld_set_error(const char* s) {
    ld_set_error3(s, 0, 0);
}

static void ld_clear_error(void) {
    g_dl_error[0] = 0;
    g_dl_error_pending = 0;
}

static int ld_str_has_dollar(const char* s) {
    for (unsigned int i = 0; s && s[i]; i++) {
        if (s[i] == '$') return 1;
    }
    return 0;
}

static void ld_memcpy(void* dst, const void* src, unsigned int len) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned int i = 0; i < len; i++) d[i] = s[i];
}

static void ld_memset(void* dst, int value, unsigned int len) {
    unsigned char* d = (unsigned char*)dst;
    for (unsigned int i = 0; i < len; i++) d[i] = (unsigned char)value;
}

static void ld_puts(const char* s) {
    (void)sc3(SYS_WRITEFD, 2u, (u32)s, ld_strlen(s));
}

static void ld_put_uint(unsigned int v) {
    char buf[11];
    unsigned int pos = sizeof(buf);

    if (v == 0) {
        ld_puts("0");
        return;
    }
    while (v && pos > 0) {
        buf[--pos] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    (void)sc3(SYS_WRITEFD, 2u, (u32)(buf + pos), sizeof(buf) - pos);
}

static void ld_fail(const char* s) {
    ld_set_error(s);
    if (g_recover_active) {
        ld_longjmp(&g_recover_env, 1);
    }
    ld_puts("ld-smallos: ");
    ld_puts(s);
    ld_puts("\n");
    ld_exit(127);
}

static void ld_fail_errno(const char* s, int err) {
    ld_set_error(s);
    if (g_recover_active) {
        ld_longjmp(&g_recover_env, 1);
    }
    ld_puts("ld-smallos: ");
    ld_puts(s);
    if (err < 0) {
        ld_puts(" (errno ");
        ld_put_uint((unsigned int)(-err));
        ld_puts(")");
    }
    ld_puts("\n");
    ld_exit(127);
}

static void ld_fail2(const char* a, const char* b) {
    ld_set_error3(a, b, 0);
    if (g_recover_active) {
        ld_longjmp(&g_recover_env, 1);
    }
    ld_puts("ld-smallos: ");
    ld_puts(a);
    ld_puts(b);
    ld_puts("\n");
    ld_exit(127);
}

static unsigned int align_down(unsigned int v) {
    return v & ~(LD_PAGE_SIZE - 1u);
}

static unsigned int align_up(unsigned int v) {
    return (v + LD_PAGE_SIZE - 1u) & ~(LD_PAGE_SIZE - 1u);
}

static unsigned int ld_alloc_dso_area(unsigned int size) {
    unsigned int start = align_up(g_next_map);
    unsigned int len = align_up(size);
    if (len == 0 || start < LD_MMAP_BASE || start + len < start || start + len > LD_MMAP_LIMIT) {
        ld_fail("DSO address arena exhausted");
    }
    g_next_map = start + len;
    return start;
}

static void* ld_mmap(unsigned int addr, unsigned int len, unsigned int prot, unsigned int fixed) {
    int ret = sc6(SYS_MMAP,
                  addr,
                  len,
                  prot,
                  LD_MAP_PRIVATE | LD_MAP_ANON | (fixed ? LD_MAP_FIXED : 0u),
                  (u32)-1,
                  0);
    if (ret < 0) {
        g_last_mmap_error = ret;
        return (void*)0;
    }
    g_last_mmap_error = 0;
    return (void*)ret;
}

static void* ld_mmap_file(unsigned int addr,
                          unsigned int len,
                          unsigned int prot,
                          int fd,
                          unsigned int offset,
                          unsigned int fixed) {
    int ret = sc6(SYS_MMAP,
                  addr,
                  len,
                  prot,
                  LD_MAP_PRIVATE | (fixed ? LD_MAP_FIXED : 0u),
                  (u32)fd,
                  offset);
    if (ret < 0) {
        g_last_mmap_error = ret;
        return (void*)0;
    }
    g_last_mmap_error = 0;
    return (void*)ret;
}

static void ld_munmap(unsigned int addr, unsigned int len) {
    if (len) (void)sc2(SYS_MUNMAP, addr, len);
}

static void ld_mprotect(unsigned int addr, unsigned int len, unsigned int prot) {
    if (len) (void)sc3(SYS_MPROTECT, addr, len, prot);
}

static void ld_close(int fd) {
    if (fd >= 0) (void)sc1(SYS_CLOSE, (u32)fd);
}

static int ld_open_read(const char* path) {
    return sc2(SYS_OPEN_MODE, (u32)path, SYS_OPEN_MODE_READ);
}

static int ld_read_all(int fd, void* buf, unsigned int len) {
    unsigned int done = 0;
    while (done < len) {
        int n = sc3(SYS_FREAD, (u32)fd, (u32)((unsigned char*)buf + done), len - done);
        if (n < 0) return n;
        if (n == 0) break;
        done += (unsigned int)n;
    }
    return (int)done;
}

static unsigned int dyn_ptr(ld_object_t* obj, unsigned int value) {
    return obj->load_bias + value;
}

static void parse_dynamic(ld_object_t* obj) {
    if (!obj->dynamic) return;
    for (Elf32_Dyn* d = obj->dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_NEEDED:
                if (obj->needed_count < LD_MAX_NEEDED) {
                    obj->needed[obj->needed_count++] = d->d_un.d_val;
                }
                break;
            case DT_SONAME: obj->soname_off = d->d_un.d_val; break;
            case DT_RPATH: obj->rpath_off = d->d_un.d_val; break;
            case DT_RUNPATH: obj->runpath_off = d->d_un.d_val; break;
            case DT_HASH: obj->hash = (unsigned int*)dyn_ptr(obj, d->d_un.d_ptr); break;
            case DT_STRTAB: obj->strtab = (const char*)dyn_ptr(obj, d->d_un.d_ptr); break;
            case DT_SYMTAB: obj->symtab = (Elf32_Sym*)dyn_ptr(obj, d->d_un.d_ptr); break;
            case DT_REL: obj->rel = (Elf32_Rel*)dyn_ptr(obj, d->d_un.d_ptr); break;
            case DT_RELSZ: obj->relsz = d->d_un.d_val; break;
            case DT_JMPREL: obj->jmprel = (Elf32_Rel*)dyn_ptr(obj, d->d_un.d_ptr); break;
            case DT_PLTRELSZ: obj->pltrelsz = d->d_un.d_val; break;
            case DT_INIT: obj->init = (void (*)(void))dyn_ptr(obj, d->d_un.d_ptr); break;
            case DT_INIT_ARRAY: obj->init_array = (void (**)(void))dyn_ptr(obj, d->d_un.d_ptr); break;
            case DT_INIT_ARRAYSZ: obj->init_arraysz = d->d_un.d_val; break;
            case DT_FINI: obj->fini = (void (*)(void))dyn_ptr(obj, d->d_un.d_ptr); break;
            case DT_FINI_ARRAY: obj->fini_array = (void (**)(void))dyn_ptr(obj, d->d_un.d_ptr); break;
            case DT_FINI_ARRAYSZ: obj->fini_arraysz = d->d_un.d_val; break;
            case DT_RELA:
            case DT_RELASZ:
            case DT_RELAENT:
                ld_fail("unsupported RELA relocation table");
                break;
            default:
                break;
        }
    }
    if (obj->strtab && obj->soname_off) obj->soname = obj->strtab + obj->soname_off;
}

static ld_object_t* object_from_phdr(const char* name,
                                     unsigned int phdr_addr,
                                     unsigned int phnum,
                                     unsigned int load_bias) {
    if (g_obj_count >= LD_MAX_OBJECTS) ld_fail("too many objects");
    ld_object_t* obj = &g_objs[g_obj_count++];
    ld_memset(obj, 0, sizeof(*obj));
    ld_strcpy(obj->name, sizeof(obj->name), name);
    obj->load_bias = load_bias;
    obj->phdr = (const Elf32_Phdr*)phdr_addr;
    obj->phnum = phnum;
    obj->loaded = 1;
    obj->active = 1;
    obj->pinned = 1;
    obj->refcount = 1;
    obj->protected = 1;
    for (unsigned int i = 0; i < phnum; i++) {
        if (obj->phdr[i].p_type == PT_DYNAMIC) {
            obj->dynamic = (Elf32_Dyn*)(load_bias + obj->phdr[i].p_vaddr);
        }
    }
    parse_dynamic(obj);
    return obj;
}

static unsigned int main_load_bias_from_phdr(unsigned int phdr_addr, unsigned int phnum) {
    const Elf32_Phdr* phdr = (const Elf32_Phdr*)phdr_addr;

    for (unsigned int i = 0; i < phnum; i++) {
        if (phdr[i].p_type == PT_PHDR) {
            return phdr_addr - phdr[i].p_vaddr;
        }
    }
    for (unsigned int i = 0; i < phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_offset == 0) {
            unsigned int page = phdr_addr & ~0xFFFu;
            unsigned int seg_page = phdr[i].p_vaddr & ~0xFFFu;
            return page - seg_page;
        }
    }
    return 0;
}

static void path_join(char* out, unsigned int out_size, const char* dir, const char* name) {
    unsigned int pos = 0;
    if (!out || out_size == 0) return;
    for (unsigned int i = 0; dir && dir[i] && pos + 1 < out_size; i++) out[pos++] = dir[i];
    if (pos > 0 && out[pos - 1] != '/' && pos + 1 < out_size) out[pos++] = '/';
    for (unsigned int i = 0; name && name[i] && pos + 1 < out_size; i++) out[pos++] = name[i];
    out[pos] = 0;
}

static int path_exists(const char* path) {
    sys_stat_info_t st;
    return sc2(SYS_STAT_FULL, (u32)path, (u32)&st) >= 0;
}

static int search_path_list(char* out,
                            unsigned int out_size,
                            const char* paths,
                            const char* name) {
    unsigned int pos = 0;
    if (!paths || !paths[0]) return 0;
    if (ld_str_has_dollar(paths)) ld_fail("unsupported dynamic library path token");
    while (paths[pos]) {
        char dir[LD_MAX_PATH];
        unsigned int dpos = 0;
        while (paths[pos] && paths[pos] != ':') {
            if (dpos + 1 < sizeof(dir)) dir[dpos++] = paths[pos];
            pos++;
        }
        dir[dpos] = 0;
        if (paths[pos] == ':') pos++;
        if (dpos == 0) continue;
        if (dir[0] != '/') ld_fail("unsupported relative dynamic library path");
        path_join(out, out_size, dir, name);
        if (path_exists(out)) return 1;
    }
    return 0;
}

static const char* object_search_path(ld_object_t* requester) {
    if (!requester || !requester->strtab) return 0;
    if (requester->runpath_off) return requester->strtab + requester->runpath_off;
    if (requester->rpath_off) return requester->strtab + requester->rpath_off;
    return 0;
}

static void make_lib_path(char* out,
                          unsigned int out_size,
                          ld_object_t* requester,
                          const char* name) {
    if (ld_str_has_dollar(name)) ld_fail("unsupported dynamic library path token");
    if (name[0] == '/') {
        ld_strcpy(out, out_size, name);
        return;
    }
    if (search_path_list(out, out_size, object_search_path(requester), name)) return;
    path_join(out, out_size, "/lib", name);
}

static ld_object_t* find_loaded(const char* name) {
    for (unsigned int i = 0; i < g_obj_count; i++) {
        if (!g_objs[i].loaded || g_objs[i].failed) continue;
        if (g_objs[i].name[0] && ld_streq(g_objs[i].name, name)) return &g_objs[i];
        if (g_objs[i].path[0] && ld_streq(g_objs[i].path, name)) return &g_objs[i];
        if (g_objs[i].soname && ld_streq(g_objs[i].soname, name)) return &g_objs[i];
    }
    return 0;
}

static unsigned int object_index(ld_object_t* obj) {
    return (unsigned int)(obj - g_objs);
}

static unsigned int ld_segment_prot(const Elf32_Phdr* ph) {
    unsigned int prot = LD_PROT_READ;
    if (ph->p_flags & PF_W) prot |= LD_PROT_WRITE;
    if (ph->p_flags & PF_X) prot |= LD_PROT_EXEC;
    return prot;
}

static int ld_page_shareable(const Elf32_Phdr* ph,
                             unsigned int page_vaddr,
                             const unsigned char* reloc_pages,
                             unsigned int min_vaddr,
                             unsigned int* out_file_offset) {
    unsigned int file_offset;
    unsigned int reloc_idx;

    if (!ph || !out_file_offset) return 0;
    if (ph->p_flags & PF_W) return 0;
    if (page_vaddr < ph->p_vaddr) return 0;
    if (page_vaddr + LD_PAGE_SIZE > ph->p_vaddr + ph->p_filesz) return 0;
    if (page_vaddr >= min_vaddr && reloc_pages) {
        reloc_idx = (page_vaddr - min_vaddr) / LD_PAGE_SIZE;
        if (reloc_pages[reloc_idx]) return 0;
    }

    file_offset = ph->p_offset + (page_vaddr - ph->p_vaddr);
    if ((file_offset & (LD_PAGE_SIZE - 1u)) != 0u) return 0;

    *out_file_offset = file_offset;
    return 1;
}

static int image_offset_for_vaddr(const Elf32_Phdr* ph,
                                  unsigned int phnum,
                                  unsigned int vaddr,
                                  unsigned int size,
                                  unsigned int* out_offset) {
    for (unsigned int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (vaddr < ph[i].p_vaddr) continue;
        if (size > ph[i].p_filesz) continue;
        if (vaddr - ph[i].p_vaddr > ph[i].p_filesz - size) continue;
        *out_offset = ph[i].p_offset + (vaddr - ph[i].p_vaddr);
        return 1;
    }
    return 0;
}

static const char* image_dynamic_string(const unsigned char* image,
                                        const Elf32_Phdr* ph,
                                        unsigned int phnum,
                                        unsigned int tag) {
    const Elf32_Dyn* dyn = 0;
    unsigned int dyn_count = 0;
    unsigned int strtab_vaddr = 0;
    unsigned int string_off = 0;
    unsigned int image_off = 0;

    for (unsigned int i = 0; i < phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (const Elf32_Dyn*)(image + ph[i].p_offset);
            dyn_count = ph[i].p_filesz / sizeof(Elf32_Dyn);
            break;
        }
    }
    if (!dyn) return 0;

    for (unsigned int i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
        if (dyn[i].d_tag == DT_STRTAB) strtab_vaddr = dyn[i].d_un.d_ptr;
        else if ((unsigned int)dyn[i].d_tag == tag) string_off = dyn[i].d_un.d_val;
    }
    if (!strtab_vaddr || !string_off) return 0;
    if (!image_offset_for_vaddr(ph, phnum, strtab_vaddr + string_off, 1, &image_off)) return 0;
    return (const char*)(image + image_off);
}

static void mark_relocation_pages(const unsigned char* image,
                                  const Elf32_Phdr* ph,
                                  unsigned int phnum,
                                  unsigned int min_vaddr,
                                  unsigned int max_vaddr,
                                  unsigned char* reloc_pages,
                                  unsigned int page_count) {
    const Elf32_Dyn* dyn = 0;
    unsigned int dyn_count = 0;
    unsigned int rel_vaddr = 0;
    unsigned int relsz = 0;
    unsigned int jmprel_vaddr = 0;
    unsigned int pltrelsz = 0;

    for (unsigned int i = 0; i < phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (const Elf32_Dyn*)(image + ph[i].p_offset);
            dyn_count = ph[i].p_filesz / sizeof(Elf32_Dyn);
            break;
        }
    }
    if (!dyn) return;

    for (unsigned int i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
        switch (dyn[i].d_tag) {
            case DT_REL: rel_vaddr = dyn[i].d_un.d_ptr; break;
            case DT_RELSZ: relsz = dyn[i].d_un.d_val; break;
            case DT_JMPREL: jmprel_vaddr = dyn[i].d_un.d_ptr; break;
            case DT_PLTRELSZ: pltrelsz = dyn[i].d_un.d_val; break;
            default: break;
        }
    }

    for (unsigned int table = 0; table < 2; table++) {
        unsigned int vaddr = table == 0 ? rel_vaddr : jmprel_vaddr;
        unsigned int bytes = table == 0 ? relsz : pltrelsz;
        unsigned int off = 0;
        unsigned int count;

        if (!vaddr || !bytes) continue;
        if (bytes % sizeof(Elf32_Rel) != 0) ld_fail("invalid relocation table size");
        if (!image_offset_for_vaddr(ph, phnum, vaddr, bytes, &off)) {
            ld_fail("invalid relocation table address");
        }
        count = bytes / sizeof(Elf32_Rel);
        for (unsigned int r = 0; r < count; r++) {
            const Elf32_Rel* rel = (const Elf32_Rel*)(image + off + r * sizeof(Elf32_Rel));
            unsigned int target = rel->r_offset;
            unsigned int idx;
            if (target < min_vaddr || target >= max_vaddr) continue;
            idx = (align_down(target) - min_vaddr) / LD_PAGE_SIZE;
            if (idx < page_count) reloc_pages[idx] = 1;
        }
    }
}

static ld_object_t* load_object(ld_object_t* requester, const char* soname) {
    char path[128];
    sys_stat_info_t st;
    int fd;
    unsigned char* image;
    const Elf32_Ehdr* eh;
    const Elf32_Phdr* ph;
    unsigned int min_v = 0xFFFFFFFFu;
    unsigned int max_v = 0;
    unsigned int map_start;
    unsigned int map_size;
    unsigned int load_bias;
    unsigned int page_count;
    unsigned char mapped[LD_MAX_OBJECT_PAGES];
    unsigned char shared[LD_MAX_OBJECT_PAGES];
    unsigned char reloc_pages[LD_MAX_OBJECT_PAGES];
    ld_object_t* obj;

    obj = find_loaded(soname);
    if (obj) return obj;
    if (g_obj_count >= LD_MAX_OBJECTS) ld_fail("too many objects");

    make_lib_path(path, sizeof(path), requester, soname);
    obj = find_loaded(path);
    if (obj) return obj;
    if (sc2(SYS_STAT_FULL, (u32)path, (u32)&st) < 0) {
        ld_fail2("library not found: ", soname);
    }
    image = (unsigned char*)ld_mmap(0, align_up(st.size), LD_PROT_READ | LD_PROT_WRITE, 0);
    if (!image) ld_fail("cannot allocate file image");
    if ((unsigned int)image + align_up(st.size) > g_next_map) {
        g_next_map = (unsigned int)image + align_up(st.size);
    }
    fd = ld_open_read(path);
    if (fd < 0) ld_fail("cannot open library");
    if (ld_read_all(fd, image, st.size) != (int)st.size) ld_fail("cannot read library");

    eh = (const Elf32_Ehdr*)image;
    if (*(const unsigned int*)eh->e_ident != ELF_MAGIC || eh->e_type != ET_DYN) {
        ld_fail("library is not ET_DYN");
    }
    ph = (const Elf32_Phdr*)(image + eh->e_phoff);
    {
        const char* image_soname = image_dynamic_string(image, ph, eh->e_phnum, DT_SONAME);
        if (image_soname) {
            obj = find_loaded(image_soname);
            if (obj) {
                ld_close(fd);
                ld_munmap((unsigned int)image, align_up(st.size));
                return obj;
            }
        }
    }
    for (unsigned int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        unsigned int s = align_down(ph[i].p_vaddr);
        unsigned int e = align_up(ph[i].p_vaddr + ph[i].p_memsz);
        if (s < min_v) min_v = s;
        if (e > max_v) max_v = e;
    }
    if (min_v == 0xFFFFFFFFu) ld_fail("library has no load segments");
    map_size = max_v - min_v;
    page_count = align_up(map_size) / LD_PAGE_SIZE;
    if (page_count > LD_MAX_OBJECT_PAGES) ld_fail("library mapping too large");
    map_start = ld_alloc_dso_area(map_size);
    load_bias = map_start - min_v;
    ld_memset(mapped, 0, sizeof(mapped));
    ld_memset(shared, 0, sizeof(shared));
    ld_memset(reloc_pages, 0, sizeof(reloc_pages));
    mark_relocation_pages(image, ph, eh->e_phnum, min_v, max_v, reloc_pages, page_count);

    obj = &g_objs[g_obj_count++];
    ld_memset(obj, 0, sizeof(*obj));
    ld_strcpy(obj->name, sizeof(obj->name), soname);
    ld_strcpy(obj->path, sizeof(obj->path), path);
    obj->load_bias = load_bias;
    obj->map_start = map_start;
    obj->map_size = map_size;
    obj->phdr = (const Elf32_Phdr*)(load_bias + eh->e_phoff);
    obj->phnum = eh->e_phnum;
    obj->loaded = 1;
    obj->active = 1;
    obj->pinned = g_recover_active ? 0 : 1;
    obj->runtime_loaded = g_recover_active ? 1 : 0;
    obj->finalized = 0;
    obj->refcount = obj->pinned ? 1u : 0u;

    for (unsigned int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        {
            unsigned int seg_page_start = align_down(ph[i].p_vaddr);
            unsigned int seg_page_end = align_up(ph[i].p_vaddr + ph[i].p_memsz);
            for (unsigned int page_v = seg_page_start;
                 page_v < seg_page_end;
                 page_v += LD_PAGE_SIZE) {
                unsigned int idx = (page_v - min_v) / LD_PAGE_SIZE;
                unsigned int dst_page = load_bias + page_v;
                unsigned int file_offset = 0;
                int want_shared = ld_page_shareable(&ph[i],
                                                    page_v,
                                                    reloc_pages,
                                                    min_v,
                                                    &file_offset);

                if (idx >= page_count) ld_fail("library page index invalid");
                if (!mapped[idx]) {
                    if (want_shared) {
                        if (!ld_mmap_file(dst_page,
                                          LD_PAGE_SIZE,
                                          ld_segment_prot(&ph[i]),
                                          fd,
                                          file_offset,
                                          1)) {
                            ld_fail_errno("cannot map shared library page",
                                          g_last_mmap_error);
                        }
                        shared[idx] = 1;
                    } else {
                        if (!ld_mmap(dst_page,
                                     LD_PAGE_SIZE,
                                     LD_PROT_READ | LD_PROT_WRITE | LD_PROT_EXEC,
                                     1)) {
                            ld_fail_errno("cannot map private library page",
                                          g_last_mmap_error);
                        }
                    }
                    mapped[idx] = 1;
                } else if (shared[idx] && !want_shared) {
                    ld_fail("unsupported overlapping DSO page");
                }

                if (!shared[idx]) {
                    unsigned int copy_start = page_v;
                    unsigned int copy_end = page_v + LD_PAGE_SIZE;
                    unsigned int file_end = ph[i].p_vaddr + ph[i].p_filesz;
                    if (copy_start < ph[i].p_vaddr) copy_start = ph[i].p_vaddr;
                    if (copy_end > file_end) copy_end = file_end;
                    if (copy_start < copy_end) {
                        unsigned int src_off = ph[i].p_offset + (copy_start - ph[i].p_vaddr);
                        ld_memcpy((void*)(load_bias + copy_start),
                                  image + src_off,
                                  copy_end - copy_start);
                    }
                }
            }
        }
    }
    for (unsigned int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            obj->dynamic = (Elf32_Dyn*)(load_bias + ph[i].p_vaddr);
        }
    }
    parse_dynamic(obj);
    ld_close(fd);
    ld_munmap((unsigned int)image, align_up(st.size));
    return obj;
}

static unsigned int find_symbol_in_object(ld_object_t* obj, const char* name) {
    unsigned int nchain;
    if (!obj || !obj->loaded || obj->failed || !obj->active ||
        !obj->symtab || !obj->strtab || !obj->hash) return 0;
    nchain = obj->hash[1];
    for (unsigned int s = 0; s < nchain; s++) {
        Elf32_Sym* sym = &obj->symtab[s];
        unsigned int bind = ELF32_ST_BIND(sym->st_info);
        if (sym->st_shndx == SHN_UNDEF) continue;
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        if (ld_streq(obj->strtab + sym->st_name, name)) {
            return obj->load_bias + sym->st_value;
        }
    }
    return 0;
}

static unsigned int lookup_symbol_from(const char* name, int allow_weak, unsigned int start_obj) {
    for (unsigned int i = start_obj; i < g_obj_count; i++) {
        unsigned int value = find_symbol_in_object(&g_objs[i], name);
        if (value) return value;
    }
    if (allow_weak) return 0;
    ld_fail2("unresolved symbol: ", name);
    return 0;
}

static unsigned int lookup_symbol_before(const char* name, unsigned int end_obj) {
    for (unsigned int i = 0; i < end_obj && i < g_obj_count; i++) {
        unsigned int value = find_symbol_in_object(&g_objs[i], name);
        if (value) return value;
    }
    return 0;
}

static unsigned int lookup_symbol(const char* name, int allow_weak) {
    return lookup_symbol_from(name, allow_weak, 0);
}

static void relocate_one(ld_object_t* obj, Elf32_Rel* rel, int apply_copy) {
    unsigned int type = ELF32_R_TYPE(rel->r_info);
    unsigned int sym_index = ELF32_R_SYM(rel->r_info);
    unsigned int* where = (unsigned int*)(obj->load_bias + rel->r_offset);
    unsigned int addend = *where;
    unsigned int value = 0;

    if (type == R_386_NONE) return;
    if (type == R_386_RELATIVE) {
        *where = obj->load_bias + addend;
        return;
    }

    if (!obj->symtab || !obj->strtab) ld_fail("missing symbol table");
    {
        Elf32_Sym* sym = &obj->symtab[sym_index];
        const char* name = obj->strtab + sym->st_name;
        int weak = ELF32_ST_BIND(sym->st_info) == STB_WEAK;
        unsigned int obj_index = (unsigned int)(obj - g_objs);
        if (type == R_386_COPY) {
            if (!apply_copy) return;
            unsigned int src = lookup_symbol_from(name, weak, 1);
            if (src && sym->st_size) ld_memcpy(where, (const void*)src, sym->st_size);
            return;
        }
        if (sym->st_shndx != SHN_UNDEF) {
            value = lookup_symbol_before(name, obj_index);
            if (!value) value = obj->load_bias + sym->st_value;
        } else {
            value = lookup_symbol(name, weak);
        }
    }

    switch (type) {
        case R_386_32:
            *where = value + addend;
            break;
        case R_386_PC32:
            *where = value + addend - (unsigned int)where;
            break;
        case R_386_GLOB_DAT:
        case R_386_JMP_SLOT:
            *where = value;
            break;
        default:
            ld_set_error("unsupported relocation type");
            if (g_recover_active) {
                ld_longjmp(&g_recover_env, 1);
            }
            ld_puts("ld-smallos: unsupported relocation type ");
            ld_put_uint(type);
            ld_puts("\n");
            ld_exit(127);
    }
}

static void relocate_object(ld_object_t* obj, int apply_copy) {
    unsigned int count;
    if (!obj || obj->relocated) return;
    if (obj->rel && obj->relsz) {
        count = obj->relsz / sizeof(Elf32_Rel);
        for (unsigned int i = 0; i < count; i++) relocate_one(obj, &obj->rel[i], apply_copy);
    }
    if (obj->jmprel && obj->pltrelsz) {
        count = obj->pltrelsz / sizeof(Elf32_Rel);
        for (unsigned int i = 0; i < count; i++) relocate_one(obj, &obj->jmprel[i], apply_copy);
    }
    obj->relocated = 1;
}

static void protect_object(ld_object_t* obj) {
    if (!obj || !obj->phdr) return;
    if (obj->protected) return;
    for (unsigned int i = 0; i < obj->phnum; i++) {
        const Elf32_Phdr* ph = &obj->phdr[i];
        unsigned int start;
        unsigned int len;
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        start = align_down(obj->load_bias + ph->p_vaddr);
        len = align_up(((obj->load_bias + ph->p_vaddr) & (LD_PAGE_SIZE - 1u)) +
                       ph->p_memsz);
        ld_mprotect(start, len, ld_segment_prot(ph));
    }
    obj->protected = 1;
}

static void load_object_dependencies(ld_object_t* obj) {
    if (!obj || obj->deps_loaded) return;
    if (obj->deps_loading) return;
    obj->deps_loading = 1;
    obj->dep_count = 0;
    if (obj->strtab) {
        for (unsigned int n = 0; n < obj->needed_count && n < LD_MAX_DEPS; n++) {
            ld_object_t* dep = load_object(obj, obj->strtab + obj->needed[n]);
            dep->active = 1;
            obj->deps[obj->dep_count++] = object_index(dep);
            load_object_dependencies(dep);
        }
    }
    obj->deps_loading = 0;
    obj->deps_loaded = 1;
}

static void load_dependencies(void) {
    for (unsigned int i = 0; i < g_obj_count; i++) {
        load_object_dependencies(&g_objs[i]);
    }
}

static void relocate_objects(void) {
    for (unsigned int i = 1; i < g_obj_count; i++) relocate_object(&g_objs[i], 0);
    relocate_object(&g_objs[0], 1);
}

static void protect_objects(void) {
    for (unsigned int i = 1; i < g_obj_count; i++) protect_object(&g_objs[i]);
}

static void run_object_initializer(ld_object_t* obj) {
    if (!obj || !obj->loaded || obj->failed || !obj->active || obj->initialized) return;
    if (obj->init_visiting) return;
    obj->init_visiting = 1;
    for (unsigned int i = 0; i < obj->dep_count; i++) {
        if (obj->deps[i] < g_obj_count) run_object_initializer(&g_objs[obj->deps[i]]);
    }
    if (obj->init) obj->init();
    if (obj->init_array && obj->init_arraysz) {
        unsigned int count = obj->init_arraysz / sizeof(void (*)(void));
        for (unsigned int j = 0; j < count; j++) {
            if (obj->init_array[j]) obj->init_array[j]();
        }
    }
    obj->initialized = 1;
    obj->fini_done = 0;
    obj->finalized = 0;
    obj->init_visiting = 0;
}

static void run_initializers(void) {
    for (unsigned int i = 1; i < g_obj_count; i++) {
        run_object_initializer(&g_objs[i]);
    }
}

static void run_object_finalizer_tree(ld_object_t* obj, unsigned char* visited);

static int g_finalizers_started;
static int g_process_finalizers;

static void run_finalizers(void) {
    unsigned char visited[LD_MAX_OBJECTS];
    if (g_finalizers_started) return;
    g_finalizers_started = 1;

    ld_memset(visited, 0, sizeof(visited));
    g_process_finalizers = 1;
    for (unsigned int i = 1; i < g_obj_count; i++) {
        if (g_objs[i].loaded && !g_objs[i].failed) visited[i] = 1;
    }
    run_object_finalizer_tree(0, visited);
    g_process_finalizers = 0;
}

static int object_should_finalize(ld_object_t* obj) {
    if (!obj || !obj->loaded || obj->failed) return 0;
    if (g_process_finalizers) return 1;
    if (obj->pinned || obj->refcount != 0) return 0;
    return 1;
}

static void collect_object_tree(ld_object_t* obj, unsigned char* visited) {
    unsigned int idx;
    if (!obj) return;
    idx = object_index(obj);
    if (idx >= LD_MAX_OBJECTS || visited[idx]) return;
    visited[idx] = 1;
    for (unsigned int i = 0; i < obj->dep_count; i++) {
        if (obj->deps[i] < g_obj_count) collect_object_tree(&g_objs[obj->deps[i]], visited);
    }
}

static int object_has_unfinalized_dependent(unsigned int idx, unsigned char* closure) {
    for (unsigned int i = 1; i < g_obj_count && i < LD_MAX_OBJECTS; i++) {
        if (!closure[i] || i == idx || !object_should_finalize(&g_objs[i])) continue;
        if (!g_objs[i].active && !g_objs[i].initialized) continue;
        for (unsigned int d = 0; d < g_objs[i].dep_count; d++) {
            if (g_objs[i].deps[d] == idx) return 1;
        }
    }
    return 0;
}

static int finalize_one_object(ld_object_t* obj) {
    if (!object_should_finalize(obj)) return 0;
    if (obj->active && obj->initialized && !obj->fini_done) {
        if (obj->fini_array && obj->fini_arraysz) {
            unsigned int count = obj->fini_arraysz / sizeof(void (*)(void));
            for (unsigned int j = count; j > 0; j--) {
                if (obj->fini_array[j - 1]) obj->fini_array[j - 1]();
            }
        }
        if (obj->fini) obj->fini();
        obj->fini_done = 1;
        obj->initialized = 0;
        obj->finalized = 1;
    }
    obj->active = 0;
    return 1;
}

static void run_finalizer_set(unsigned char* closure) {
    int progress = 1;
    while (progress) {
        progress = 0;
        for (unsigned int i = 1; i < g_obj_count && i < LD_MAX_OBJECTS; i++) {
            if (!closure[i]) continue;
            if (object_has_unfinalized_dependent(i, closure)) continue;
            if (finalize_one_object(&g_objs[i])) {
                closure[i] = 0;
                progress = 1;
            }
        }
    }
}

static void run_object_finalizer_tree(ld_object_t* obj, unsigned char* visited) {
    if (obj) collect_object_tree(obj, visited);
    run_finalizer_set(visited);
}

static void retain_object_tree(ld_object_t* obj, unsigned char* visited) {
    unsigned int idx;
    if (!obj) return;
    idx = object_index(obj);
    if (idx >= LD_MAX_OBJECTS || visited[idx]) return;
    if (!obj->loaded || obj->failed) return;
    visited[idx] = 1;
    obj->active = 1;
    obj->finalized = 0;
    obj->refcount++;
    for (unsigned int i = 0; i < obj->dep_count; i++) {
        if (obj->deps[i] < g_obj_count) retain_object_tree(&g_objs[obj->deps[i]], visited);
    }
}

static void activate_object_tree(ld_object_t* obj, unsigned char* visited) {
    unsigned int idx;
    if (!obj) return;
    idx = object_index(obj);
    if (idx >= LD_MAX_OBJECTS || visited[idx]) return;
    if (!obj->loaded || obj->failed) return;
    visited[idx] = 1;
    obj->active = 1;
    obj->finalized = 0;
    for (unsigned int i = 0; i < obj->dep_count; i++) {
        if (obj->deps[i] < g_obj_count) activate_object_tree(&g_objs[obj->deps[i]], visited);
    }
}

static void release_object_tree(ld_object_t* obj, unsigned char* visited) {
    unsigned int idx;
    if (!obj) return;
    idx = object_index(obj);
    if (idx >= LD_MAX_OBJECTS || visited[idx]) return;
    if (!obj->loaded || obj->failed) return;
    visited[idx] = 1;
    if (obj->refcount > 0) obj->refcount--;
    for (unsigned int i = 0; i < obj->dep_count; i++) {
        if (obj->deps[i] < g_obj_count) release_object_tree(&g_objs[obj->deps[i]], visited);
    }
}

static void rollback_runtime_objects(unsigned int start_count) {
    for (unsigned int i = start_count; i < g_obj_count && i < LD_MAX_OBJECTS; i++) {
        g_objs[i].active = 0;
        g_objs[i].initialized = 0;
        g_objs[i].fini_done = 1;
        g_objs[i].finalized = 1;
        g_objs[i].failed = 1;
        g_objs[i].refcount = 0;
    }
    g_obj_count = start_count;
}

static unsigned int dlsym_in_tree(ld_object_t* obj, const char* name, unsigned char* visited) {
    unsigned int idx;
    unsigned int value;
    if (!obj || !name) return 0;
    idx = object_index(obj);
    if (idx >= LD_MAX_OBJECTS || visited[idx]) return 0;
    if (!obj->loaded || obj->failed || !obj->active) return 0;
    visited[idx] = 1;
    value = find_symbol_in_object(obj, name);
    if (value) return value;
    for (unsigned int i = 0; i < obj->dep_count; i++) {
        if (obj->deps[i] < g_obj_count) {
            value = dlsym_in_tree(&g_objs[obj->deps[i]], name, visited);
            if (value) return value;
        }
    }
    return 0;
}

static void* ld_dlopen_service(const char* filename, int flag) {
    ld_object_t* obj;
    unsigned char visited[LD_MAX_OBJECTS];
    unsigned int valid_flags = RTLD_LAZY | RTLD_NOW | RTLD_GLOBAL;
    unsigned int start_count;

    ld_clear_error();
    if ((flag & ~valid_flags) != 0) {
        ld_set_error("unsupported dlopen flags");
        return 0;
    }
    if ((flag & (RTLD_LAZY | RTLD_NOW)) == 0) {
        ld_set_error("dlopen requires RTLD_NOW or RTLD_LAZY");
        return 0;
    }
    if (!filename) return &g_objs[0];

    start_count = g_obj_count;
    g_recover_obj_count = start_count;
    if (ld_setjmp(&g_recover_env) != 0) {
        g_recover_active = 0;
        rollback_runtime_objects(g_recover_obj_count);
        return 0;
    }
    g_recover_active = 1;
    obj = load_object(&g_objs[0], filename);
    obj->active = 1;
    load_object_dependencies(obj);
    ld_memset(visited, 0, sizeof(visited));
    activate_object_tree(obj, visited);
    for (unsigned int i = 1; i < g_obj_count; i++) relocate_object(&g_objs[i], 0);
    for (unsigned int i = 1; i < g_obj_count; i++) protect_object(&g_objs[i]);
    run_object_initializer(obj);
    ld_memset(visited, 0, sizeof(visited));
    retain_object_tree(obj, visited);
    g_recover_active = 0;
    return obj;
}

static void* ld_dlsym_service(void* handle, const char* symbol) {
    unsigned int value = 0;
    unsigned char visited[LD_MAX_OBJECTS];

    ld_clear_error();
    if (!symbol || !symbol[0]) {
        ld_set_error("dlsym missing symbol name");
        return 0;
    }
    if (!handle) {
        for (unsigned int i = 0; i < g_obj_count; i++) {
            value = find_symbol_in_object(&g_objs[i], symbol);
            if (value) return (void*)value;
        }
    } else {
        ld_object_t* obj = (ld_object_t*)handle;
        if (obj < g_objs || obj >= g_objs + g_obj_count ||
            !obj->loaded || obj->failed || !obj->active) {
            ld_set_error("invalid dlsym handle");
            return 0;
        }
        ld_memset(visited, 0, sizeof(visited));
        value = dlsym_in_tree(obj, symbol, visited);
        if (value) return (void*)value;
    }
    ld_set_error3("symbol not found: ", symbol, 0);
    return 0;
}

static int ld_dlclose_service(void* handle) {
    ld_object_t* obj = (ld_object_t*)handle;
    unsigned char visited[LD_MAX_OBJECTS];

    ld_clear_error();
    if (!handle) {
        ld_set_error("invalid dlclose handle");
        return -1;
    }
    if (obj < g_objs || obj >= g_objs + g_obj_count ||
        !obj->loaded || obj->failed || (!obj->pinned && !obj->active)) {
        ld_set_error("invalid dlclose handle");
        return -1;
    }
    if (obj->pinned) return 0;
    ld_memset(visited, 0, sizeof(visited));
    release_object_tree(obj, visited);
    if (obj->refcount == 0) {
        ld_memset(visited, 0, sizeof(visited));
        run_object_finalizer_tree(obj, visited);
    }
    return 0;
}

static const char* ld_dlerror_service(void) {
    if (!g_dl_error_pending) return 0;
    g_dl_error_pending = 0;
    return g_dl_error;
}

static smallos_dlfcn_services_t g_dlfcn_services = {
    ld_dlopen_service,
    ld_dlsym_service,
    ld_dlclose_service,
    ld_dlerror_service,
};

static void install_finalizer_hook(void) {
    unsigned int hook_addr = lookup_symbol("__smallos_fini_hook", 1);
    if (hook_addr) {
        void (**hook)(void) = (void (**)(void))hook_addr;
        *hook = run_finalizers;
    }
}

static void install_dlfcn_hook(void) {
    unsigned int hook_addr = lookup_symbol("__smallos_dlfcn_services", 1);
    if (hook_addr) {
        smallos_dlfcn_services_t** hook = (smallos_dlfcn_services_t**)hook_addr;
        *hook = &g_dlfcn_services;
    }
}

__attribute__((visibility("hidden"), noreturn))
void ld_smallos_main(int argc, char** argv, char** envp, unsigned int* auxv) {
    unsigned int at_phdr = 0;
    unsigned int at_phnum = 0;
    unsigned int at_entry = 0;
    unsigned int at_base = 0;
    unsigned int main_load_bias;

    (void)at_base;
    for (unsigned int* p = auxv; p && p[0] != AT_NULL; p += 2) {
        if (p[0] == AT_PHDR) at_phdr = p[1];
        else if (p[0] == AT_PHNUM) at_phnum = p[1];
        else if (p[0] == AT_ENTRY) at_entry = p[1];
        else if (p[0] == AT_BASE) at_base = p[1];
    }
    if (!at_phdr || !at_phnum || !at_entry) ld_fail("missing auxv");

    main_load_bias = main_load_bias_from_phdr(at_phdr, at_phnum);
    object_from_phdr("<main>", at_phdr, at_phnum, main_load_bias);
    load_dependencies();
    relocate_objects();
    protect_objects();
    install_finalizer_hook();
    install_dlfcn_hook();
    run_initializers();

    ((void (*)(int, char**, char**))at_entry)(argc, argv, envp);
    run_finalizers();
    ld_exit(0);
}
