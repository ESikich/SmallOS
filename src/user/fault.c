#include "user_lib.h"

static int str_eq(const char* a, const char* b) {
    unsigned int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static void trigger_ud(void) {
    /* UD2 is the cleanest way to force an invalid-opcode exception. */
    u_puts("fault: triggering #UD\n");
    __asm__ volatile("ud2");
}

static void trigger_gp(void) {
    /* Intentionally invoke a ring-0-only software interrupt from user mode. */
    u_puts("fault: triggering #GP\n");
    __asm__ volatile("int $0x0D");
}

static void trigger_de(void) {
    /* Divide by zero to trip the CPU's divide-error path. */
    u_puts("fault: triggering #DE\n");
    __asm__ volatile(
        "mov $1, %%eax\n"
        "xor %%edx, %%edx\n"
        "xor %%ecx, %%ecx\n"
        "idiv %%ecx\n"
        :
        :
        : "eax", "ecx", "edx"
    );
}

static void trigger_br(void) {
    /* The BOUND instruction raises #BR when the index is out of range. */
    int idx = 11;
    int bounds[2] = { 0, 10 };

    u_puts("fault: triggering #BR\n");
    __asm__ volatile("bound %0, %1" : "+r"(idx) : "m"(bounds));
}

static void trigger_pf(void) {
    /* Null dereference exercises the user page-fault handler. */
    u_puts("fault: triggering #PF\n");
    *(volatile unsigned int*)0 = 1u;
}

void _start(int argc, char** argv) {
    const char* mode = (argc >= 2) ? argv[1] : "ud";

    /* Default to #UD so a bare `runelf fault.elf` still does something useful. */
    if (str_eq(mode, "gp")) {
        trigger_gp();
    } else if (str_eq(mode, "de")) {
        trigger_de();
    } else if (str_eq(mode, "br")) {
        trigger_br();
    } else if (str_eq(mode, "pf")) {
        trigger_pf();
    } else {
        trigger_ud();
    }

    sys_exit(0);
}
