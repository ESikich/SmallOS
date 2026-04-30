#include "gdt.h"
#include "memory.h"

/* ------------------------------------------------------------------ */
/* GDT entry / pointer structures                                       */
/* ------------------------------------------------------------------ */

struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char  base_middle;
    unsigned char  access;
    unsigned char  granularity;
    unsigned char  base_high;
} __attribute__((packed));

struct gdt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* TSS                                                                  */
/*                                                                      */
/* We only need the fields the CPU reads on a privilege-level change.   */
/* When a ring-3 program executes `int 0x80`, the CPU looks up TSS.SS0 */
/* and TSS.ESP0 to know where to put the ring-0 stack.                  */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned int  prev_tss;   /* unused — no hardware task switching */
    unsigned int  esp0;       /* kernel stack pointer on ring-3→0 transition */
    unsigned int  ss0;        /* kernel stack segment on ring-3→0 transition */
    /* remaining TSS fields — zeroed, not used */
    unsigned int  esp1, ss1;
    unsigned int  esp2, ss2;
    unsigned int  cr3, eip, eflags;
    unsigned int  eax, ecx, edx, ebx, esp, ebp, esi, edi;
    unsigned int  es, cs, ss, ds, fs, gs;
    unsigned int  ldt;
    unsigned short trap;
    unsigned short iomap_base;
} __attribute__((packed)) tss_t;

/* ------------------------------------------------------------------ */
/* Static data                                                          */
/* ------------------------------------------------------------------ */

static struct gdt_entry gdt[6];   /* null, k-code, k-data, u-code, u-data, TSS */
static struct gdt_ptr   gp;
static tss_t            tss;

extern void gdt_flush(unsigned int);
extern void tss_flush(unsigned int);   /* defined in interrupts.asm — executes ltr */

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void gdt_set_gate(int num,
                         unsigned int   base,
                         unsigned int   limit,
                         unsigned char  access,
                         unsigned char  gran)
{
    gdt[num].base_low    = (base  & 0xFFFF);
    gdt[num].base_middle = (base  >> 16) & 0xFF;
    gdt[num].base_high   = (base  >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access      = access;
}

/*
 * Write a TSS descriptor into a GDT slot.
 *
 * A TSS descriptor differs from a normal segment descriptor:
 *   - base  = address of the tss_t struct
 *   - limit = sizeof(tss_t) - 1
 *   - access byte = 0x89  (Present | DPL=0 | Type=9 = 32-bit available TSS)
 *   - granularity  = 0x00  (byte granularity, limit in bytes)
 */
static void gdt_set_tss(int num, unsigned int base, unsigned int limit) {
    gdt_set_gate(num, base, limit, 0x89, 0x00);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void gdt_init(void) {
    /* Size covers all 6 entries. */
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (unsigned int)&gdt;

    /* index 0 — null */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* index 1 — kernel code  DPL=0  access=0x9A  gran=0xCF (4KB, 32-bit) */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* index 2 — kernel data  DPL=0  access=0x92 */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /*
     * index 3 — user code  DPL=3
     * access = 0xFA  (Present | DPL=3 | S=1 | Type=0xA = code, readable)
     */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    /*
     * index 4 — user data  DPL=3
     * access = 0xF2  (Present | DPL=3 | S=1 | Type=0x2 = data, writable)
     */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* index 5 — TSS */
    __builtin_memset(&tss, 0, sizeof(tss));
    tss.ss0  = SEG_KERNEL_DATA;   /* 0x10 */
    /* boot stack top from loader2's generated constants; overwritten per-process
     * by tss_set_kernel_stack() before iret */
    tss.esp0 = KERNEL_BOOT_STACK_TOP;
    tss.iomap_base = sizeof(tss); /* no I/O permission bitmap */

    gdt_set_tss(5, (unsigned int)&tss, sizeof(tss) - 1);

    gdt_flush((unsigned int)&gp);

    /* Load the TSS selector into the task register. */
    tss_flush(SEG_TSS);           /* ltr 0x28 */
}

void tss_set_kernel_stack(unsigned int esp0) {
    tss.esp0 = esp0;
}

unsigned int tss_get_kernel_stack(void) {
    return tss.esp0;
}
