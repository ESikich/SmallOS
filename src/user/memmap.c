#include "diag_util.h"

static const char* e820_type_name(unsigned int type) {
    if (type == 1u) return "usable";
    if (type == 2u) return "reserved";
    if (type == 3u) return "acpi";
    if (type == 4u) return "nvs";
    if (type == 5u) return "bad";
    return "other";
}

void _start(int argc, char** argv) {
    sys_e820_entry_t entry;
    int count;

    (void)argc;
    (void)argv;

    count = sys_e820_entry(0, &entry);
    if (count <= 0) {
        u_puts("memmap: E820 unavailable\n");
        sys_exit(1);
    }

    u_puts("memmap: ");
    u_put_uint((unsigned int)count);
    u_puts(" E820 entries\n");

    for (int i = 0; i < count; i++) {
        if (sys_e820_entry((unsigned int)i, &entry) < 0) break;
        u_put_uint((unsigned int)i);
        u_puts(": base ");
        diag_put_u64_hex(entry.base);
        u_puts("  length ");
        diag_put_u64_hex(entry.length);
        u_puts("  type ");
        u_put_uint(entry.type);
        u_putc(' ');
        u_puts(e820_type_name(entry.type));
        u_putc('\n');
    }

    sys_exit(0);
}
