#include "idt.h"
#include "ports.h"
#include "terminal.h"
#include "keyboard.h"
#include "mouse.h"
#include "sound.h"
#include "timer.h"
#include "paging.h"
#include "process.h"
#include "scheduler.h"
#include "klib.h"

extern void idt_flush(unsigned int);
extern void irq0_stub(void);
extern void irq1_stub(void);
extern void irq5_stub(void);
extern void irq12_stub(void);
extern void isr0_stub(void);
extern void isr1_stub(void);
extern void isr2_stub(void);
extern void isr3_stub(void);
extern void isr4_stub(void);
extern void isr5_stub(void);
extern void isr6_stub(void);
extern void isr7_stub(void);
extern void isr13_stub(void);
extern void isr14_stub(void);
extern void isr8_stub(void);
extern void isr10_stub(void);
extern void isr11_stub(void);
extern void isr12_stub(void);
extern void isr16_stub(void);
extern void isr17_stub(void);
extern void isr19_stub(void);
extern void isr20_stub(void);
extern void isr128_stub(void);

static struct idt_entry idt[256];
static struct idt_ptr idtp;

#define FAULT_SIGILL   4
#define FAULT_SIGTRAP  5
#define FAULT_SIGBUS   7
#define FAULT_SIGFPE   8
#define FAULT_SIGSEGV  11

typedef struct fault_meta {
    const char* tag;
    unsigned char has_err;
    unsigned char has_cr2;
    unsigned char signal;
} fault_meta_t;

typedef struct last_fault {
    unsigned int sequence;
    unsigned int vector;
    const char* tag;
    unsigned int pid;
    char process[PROCESS_NAME_MAX];
    unsigned int user_mode;
    unsigned int err_valid;
    unsigned int cr2_valid;
    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;
    unsigned int user_esp;
    unsigned int user_ss;
    unsigned int err;
    unsigned int cr2;
} last_fault_t;

static last_fault_t s_last_fault;

static const fault_meta_t s_fault_meta[21] = {
    { "de",  0, 0, FAULT_SIGFPE  }, /* 0  #DE divide error */
    { "db",  0, 0, FAULT_SIGTRAP }, /* 1  #DB debug */
    { "nmi", 0, 0, FAULT_SIGBUS  }, /* 2  NMI */
    { "bp",  0, 0, FAULT_SIGTRAP }, /* 3  #BP breakpoint */
    { "of",  0, 0, FAULT_SIGFPE  }, /* 4  #OF overflow */
    { "br",  0, 0, FAULT_SIGBUS  }, /* 5  #BR bound range */
    { "ud",  0, 0, FAULT_SIGILL  }, /* 6  #UD invalid opcode */
    { "nm",  0, 0, FAULT_SIGILL  }, /* 7  #NM device not available */
    { "df",  1, 0, FAULT_SIGBUS  }, /* 8  #DF double fault */
    { "09",  0, 0, FAULT_SIGBUS  }, /* 9  reserved/coprocessor segment */
    { "ts",  1, 0, FAULT_SIGSEGV }, /* 10 #TS invalid TSS */
    { "np",  1, 0, FAULT_SIGSEGV }, /* 11 #NP segment not present */
    { "ss",  1, 0, FAULT_SIGSEGV }, /* 12 #SS stack segment */
    { "gp",  1, 0, FAULT_SIGSEGV }, /* 13 #GP general protection */
    { "pf",  1, 1, FAULT_SIGSEGV }, /* 14 #PF page fault */
    { "15",  0, 0, FAULT_SIGBUS  }, /* 15 reserved */
    { "mf",  0, 0, FAULT_SIGFPE  }, /* 16 #MF x87 floating point */
    { "ac",  1, 0, FAULT_SIGBUS  }, /* 17 #AC alignment check */
    { "18",  0, 0, FAULT_SIGBUS  }, /* 18 machine check, not installed here */
    { "xm",  0, 0, FAULT_SIGFPE  }, /* 19 #XM SIMD floating point */
    { "ve",  1, 0, FAULT_SIGSEGV }, /* 20 #VE virtualization exception */
};

void idt_set_gate(unsigned char num, unsigned int base, unsigned short sel, unsigned char flags) {
    idt[num].base_low = base & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
    idt[num].base_high = (base >> 16) & 0xFFFF;
}

static void pic_remap(void) {
    unsigned char a1 = inb(0x21);
    unsigned char a2 = inb(0xA1);

    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();

    outb(0x21, 0x20);
    io_wait();
    outb(0xA1, 0x28);
    io_wait();

    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();

    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();

    outb(0x21, a1);
    outb(0xA1, a2);
}

void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    pic_remap();

    idt_set_gate(0,   (unsigned int)isr0_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(1,   (unsigned int)isr1_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(2,   (unsigned int)isr2_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(3,   (unsigned int)isr3_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_USER);
    idt_set_gate(4,   (unsigned int)isr4_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_USER);
    idt_set_gate(5,   (unsigned int)isr5_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(6,   (unsigned int)isr6_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(7,   (unsigned int)isr7_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(8,   (unsigned int)isr8_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(10,  (unsigned int)isr10_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(11,  (unsigned int)isr11_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(12,  (unsigned int)isr12_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(13,  (unsigned int)isr13_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(14,  (unsigned int)isr14_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(16,  (unsigned int)isr16_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(17,  (unsigned int)isr17_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(19,  (unsigned int)isr19_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(20,  (unsigned int)isr20_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(32,  (unsigned int)irq0_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(33,  (unsigned int)irq1_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(37,  (unsigned int)irq5_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(44,  (unsigned int)irq12_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);

    /*
     * int 0x80 syscall gate — DPL=3 so ring-3 code can invoke it.
     *
     * IDT_FLAG_INT_GATE_KERNEL (0x8E) has DPL=0: ring-3 hitting it
     * causes a #GP fault before the handler even runs.
     * IDT_FLAG_INT_GATE_USER  (0xEE) has DPL=3: the CPU allows the
     * software interrupt from any privilege level.
     *
     * The handler itself (isr128_stub → syscall_handler_main) always
     * runs in ring 0 because it is reached via an interrupt gate, which
     * clears IF and switches to the kernel code selector.
     */
    idt_set_gate(128, (unsigned int)isr128_stub, KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_USER);

    idtp.limit = sizeof(struct idt_entry) * 256 - 1;
    idtp.base = (unsigned int)&idt;

    idt_flush((unsigned int)&idtp);

    outb(0x21, 0xD8);
    outb(0xA1, 0xEF);
}

static unsigned int fault_frame_word(unsigned int esp, unsigned int index) {
    return ((unsigned int*)esp)[index];
}

static unsigned int pf_get_cr2(void) {
    unsigned int cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static const fault_meta_t* fault_meta_for_vector(unsigned int vector) {
    if (vector < sizeof(s_fault_meta) / sizeof(s_fault_meta[0]) &&
        s_fault_meta[vector].tag != 0) {
        return &s_fault_meta[vector];
    }
    return 0;
}

static void last_fault_record(unsigned int vector,
                              const fault_meta_t* meta,
                              unsigned int eip,
                              unsigned int cs,
                              unsigned int eflags,
                              unsigned int user_esp,
                              unsigned int user_ss,
                              unsigned int err,
                              unsigned int cr2) {
    process_t* proc = sched_current();
    unsigned int user_mode = (cs & 3u) == 3u;

    s_last_fault.sequence++;
    s_last_fault.vector = vector;
    s_last_fault.tag = meta ? meta->tag : "??";
    s_last_fault.pid = proc ? proc->pid : 0u;
    if (proc) {
        k_strncpy(s_last_fault.process, proc->name, sizeof(s_last_fault.process));
    } else {
        k_strncpy(s_last_fault.process, "kernel", sizeof(s_last_fault.process));
    }
    s_last_fault.user_mode = user_mode;
    s_last_fault.err_valid = meta && meta->has_err;
    s_last_fault.cr2_valid = meta && meta->has_cr2;
    s_last_fault.eip = eip;
    s_last_fault.cs = cs;
    s_last_fault.eflags = eflags;
    s_last_fault.user_esp = user_mode ? user_esp : 0u;
    s_last_fault.user_ss = user_mode ? user_ss : 0u;
    s_last_fault.err = err;
    s_last_fault.cr2 = cr2;
}

static void lf_putc(char* out, unsigned int cap, unsigned int* pos, char c) {
    if (!out || !pos || cap == 0u) return;
    if (*pos + 1u < cap) {
        out[*pos] = c;
        (*pos)++;
    }
    out[*pos < cap ? *pos : cap - 1u] = '\0';
}

static void lf_puts(char* out, unsigned int cap, unsigned int* pos, const char* s) {
    if (!s) return;
    while (*s) {
        lf_putc(out, cap, pos, *s++);
    }
}

static void lf_put_uint(char* out, unsigned int cap, unsigned int* pos, unsigned int value) {
    char buf[16];
    unsigned int len = 0;
    if (value == 0u) {
        lf_putc(out, cap, pos, '0');
        return;
    }
    while (value != 0u && len < sizeof(buf)) {
        buf[len++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (len > 0u) {
        lf_putc(out, cap, pos, buf[--len]);
    }
}

static void lf_put_hex(char* out, unsigned int cap, unsigned int* pos, unsigned int value) {
    static const char hex[] = "0123456789ABCDEF";
    lf_puts(out, cap, pos, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        lf_putc(out, cap, pos, hex[(value >> shift) & 0xFu]);
    }
}

static void lf_put_pf_flags(char* out, unsigned int cap, unsigned int* pos, unsigned int err) {
    lf_puts(out, cap, pos, (err & 0x01u) ? "protection" : "not-present");
    lf_putc(out, cap, pos, ' ');
    lf_puts(out, cap, pos, (err & 0x02u) ? "write" : "read");
    lf_putc(out, cap, pos, ' ');
    lf_puts(out, cap, pos, (err & 0x04u) ? "user" : "supervisor");
    if (err & 0x08u) lf_puts(out, cap, pos, " reserved");
    if (err & 0x10u) lf_puts(out, cap, pos, " instruction");
}

unsigned int idt_lastfault_render(char* out, unsigned int cap) {
    unsigned int pos = 0;

    if (!out || cap == 0u) return 0;
    out[0] = '\0';
    if (s_last_fault.sequence == 0u) {
        lf_puts(out, cap, &pos, "sequence: 0\nstate: none\n");
        return pos;
    }

    lf_puts(out, cap, &pos, "sequence: ");
    lf_put_uint(out, cap, &pos, s_last_fault.sequence);
    lf_puts(out, cap, &pos, "\nvector: ");
    lf_put_uint(out, cap, &pos, s_last_fault.vector);
    lf_puts(out, cap, &pos, "\nmnemonic: ");
    lf_puts(out, cap, &pos, s_last_fault.tag);
    lf_puts(out, cap, &pos, "\npid: ");
    lf_put_uint(out, cap, &pos, s_last_fault.pid);
    lf_puts(out, cap, &pos, "\nprocess: ");
    lf_puts(out, cap, &pos, s_last_fault.process);
    lf_puts(out, cap, &pos, "\nmode: ");
    lf_puts(out, cap, &pos, s_last_fault.user_mode ? "user" : "kernel");
    lf_puts(out, cap, &pos, "\neip: ");
    lf_put_hex(out, cap, &pos, s_last_fault.eip);
    lf_puts(out, cap, &pos, "\ncs: ");
    lf_put_hex(out, cap, &pos, s_last_fault.cs);
    lf_puts(out, cap, &pos, "\neflags: ");
    lf_put_hex(out, cap, &pos, s_last_fault.eflags);
    if (s_last_fault.user_mode) {
        lf_puts(out, cap, &pos, "\nesp: ");
        lf_put_hex(out, cap, &pos, s_last_fault.user_esp);
        lf_puts(out, cap, &pos, "\nss: ");
        lf_put_hex(out, cap, &pos, s_last_fault.user_ss);
    }
    if (s_last_fault.err_valid) {
        lf_puts(out, cap, &pos, "\nerr: ");
        lf_put_hex(out, cap, &pos, s_last_fault.err);
    }
    if (s_last_fault.cr2_valid) {
        lf_puts(out, cap, &pos, "\ncr2: ");
        lf_put_hex(out, cap, &pos, s_last_fault.cr2);
        lf_puts(out, cap, &pos, "\npf_flags: ");
        lf_put_pf_flags(out, cap, &pos, s_last_fault.err);
    }
    lf_putc(out, cap, &pos, '\n');
    return pos;
}

void fault_handler_main(unsigned int esp, unsigned int vector) {
    const fault_meta_t* meta = fault_meta_for_vector(vector);
    const char* tag = meta ? meta->tag : "??";
    unsigned int has_err = meta ? meta->has_err : 0u;
    unsigned int has_cr2 = meta ? meta->has_cr2 : 0u;
    unsigned int err = has_err ? fault_frame_word(esp, 12) : 0;
    unsigned int eip = fault_frame_word(esp, has_err ? 13 : 12);
    unsigned int cs = fault_frame_word(esp, has_err ? 14 : 13);
    unsigned int eflags = fault_frame_word(esp, has_err ? 15 : 14);
    unsigned int user_mode = (cs & 3u) == 3u;
    unsigned int user_esp = user_mode ? fault_frame_word(esp, has_err ? 16 : 15) : 0u;
    unsigned int user_ss = user_mode ? fault_frame_word(esp, has_err ? 17 : 16) : 0u;
    unsigned int cr2 = has_cr2 ? pf_get_cr2() : 0;
    int exit_status = 128 + (int)(meta ? meta->signal : FAULT_SIGSEGV);
    process_t* proc = sched_current();

    if (has_cr2 && has_err && proc && proc->pd != 0 && (cs & 3u) == 3u) {
        if (process_vm_handle_fault(proc, cr2, err)) {
            return;
        }
    }

    last_fault_record(vector, meta, eip, cs, eflags, user_esp, user_ss, err, cr2);

    terminal_puts(tag);
    terminal_puts(" eip=");
    terminal_put_hex(eip);
    terminal_puts(" cs=");
    terminal_put_hex(cs);
    terminal_puts(((cs & 3u) == 3u) ? " user" : " kernel");
    if (has_err) {
        terminal_puts(" err=");
        terminal_put_hex(err);
    }
    if ((cs & 3u) == 3u) {
        terminal_puts(" esp=");
        terminal_put_hex(user_esp);
        terminal_puts(" ss=");
        terminal_put_hex(user_ss);
        terminal_puts(" eflags=");
        terminal_put_hex(eflags);
    }
    if (has_cr2) {
        terminal_puts(" cr2=");
        terminal_put_hex(cr2);
        terminal_puts(" pf=");
        terminal_puts((err & 0x01u) ? "prot" : "np");
        terminal_putc('/');
        terminal_puts((err & 0x02u) ? "w" : "r");
        terminal_putc('/');
        terminal_puts((err & 0x04u) ? "user" : "super");
        if (err & 0x08u) terminal_puts("/rsvd");
        if (err & 0x10u) terminal_puts("/insn");
    }
    terminal_putc('\n');

    /*
     * User faults terminate just the current process so the shell and
     * the rest of the VM stay alive.  Kernel faults still halt hard so
     * we preserve the last error context instead of trying to recover
     * from a potentially corrupted kernel stack.
     */
    if (proc && proc->pd != 0 && (cs & 3u) == 3u) {
        terminal_puts(tag);
        terminal_puts(" term ");
        terminal_puts(proc->name);
        terminal_putc('\n');

        proc->exit_status = exit_status;
        paging_switch(paging_get_kernel_pd());
        sched_exit_current(esp);
    }

    terminal_puts(tag);
    terminal_puts(" kernel panic\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/*
 * irq0_handler_main(esp)
 *
 * Timer IRQ handler.  esp is the kernel stack pointer at the point
 * irq0_stub called us — it points at the saved register frame.
 *
 * We must send EOI before calling sched_tick, because sched_switch may
 * resume a different context that never returns through this function.
 * If EOI were sent after sched_tick, the outgoing context would have
 * its EOI sent when it is eventually rescheduled — but the incoming
 * context would run with IRQ0 still masked in the PIC, meaning no
 * further timer ticks until the original context runs again.
 */
#define SCHED_RESUME_RETADDR_OFFSET 8u

void irq0_handler_main(unsigned int esp) {
    timer_handle_irq();
    sound_timer_tick();
    outb(0x20, 0x20);   /* EOI before sched_tick — see above */
    sched_tick(esp - SCHED_RESUME_RETADDR_OFFSET);
}

void irq1_handler_main(unsigned int esp) {
    outb(0x20, 0x20);   /* EOI first — keep PIC unmasked before IRQ-side work */
    keyboard_handle_irq();
    process_deliver_pending_terminal_interrupt(esp - SCHED_RESUME_RETADDR_OFFSET);
}

void irq5_handler_main(unsigned int esp) {
    (void)esp;
    sound_irq_handler();
    outb(0x20, 0x20);
}

void irq12_handler_main(unsigned int esp) {
    (void)esp;
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
    mouse_handle_irq();
}
