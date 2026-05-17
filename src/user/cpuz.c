#include "diag_util.h"
#include "uapi_display.h"

typedef struct cpuid_regs {
    unsigned int eax;
    unsigned int ebx;
    unsigned int ecx;
    unsigned int edx;
} cpuid_regs_t;

static int cpu_has_cpuid(void) {
    unsigned int before;
    unsigned int after;

    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0, %1\n\t"
        "xorl $0x200000, %1\n\t"
        "pushl %1\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %1\n\t"
        "pushl %0\n\t"
        "popfl"
        : "=&r"(before), "=&r"(after)
        :
        : "cc");

    return ((before ^ after) & 0x200000u) != 0u;
}

static void cpuid(unsigned int leaf, unsigned int subleaf, cpuid_regs_t* out) {
    __asm__ volatile(
        "cpuid"
        : "=a"(out->eax), "=b"(out->ebx), "=c"(out->ecx), "=d"(out->edx)
        : "a"(leaf), "c"(subleaf)
        : "memory");
}

static void copy_reg(char* dst, unsigned int value) {
    dst[0] = (char)(value & 0xFFu);
    dst[1] = (char)((value >> 8) & 0xFFu);
    dst[2] = (char)((value >> 16) & 0xFFu);
    dst[3] = (char)((value >> 24) & 0xFFu);
}

static void put_feature(const char** sep, int present, const char* name) {
    if (!present) return;
    u_puts(*sep);
    u_puts(name);
    *sep = " ";
}

static void print_cpu(void) {
    cpuid_regs_t r0;
    cpuid_regs_t r1;
    cpuid_regs_t rx;
    char vendor[13];
    char brand[49];
    unsigned int family;
    unsigned int model;
    unsigned int stepping;
    unsigned int max_leaf;
    unsigned int max_ext;
    const char* sep;

    u_puts("CPU\n");
    if (!cpu_has_cpuid()) {
        u_puts("  cpuid: unavailable\n");
        return;
    }

    cpuid(0, 0, &r0);
    max_leaf = r0.eax;
    copy_reg(vendor + 0, r0.ebx);
    copy_reg(vendor + 4, r0.edx);
    copy_reg(vendor + 8, r0.ecx);
    vendor[12] = 0;

    cpuid(1, 0, &r1);
    stepping = r1.eax & 0xFu;
    model = (r1.eax >> 4) & 0xFu;
    family = (r1.eax >> 8) & 0xFu;
    if (family == 0xFu) family += (r1.eax >> 20) & 0xFFu;
    if (family == 0x6u || family == 0xFu) {
        model += ((r1.eax >> 16) & 0xFu) << 4;
    }

    u_puts("  vendor:       ");
    u_puts(vendor);
    u_putc('\n');
    u_puts("  family/model: ");
    u_put_uint(family);
    u_putc('/');
    u_put_uint(model);
    u_puts(" stepping ");
    u_put_uint(stepping);
    u_puts(" type ");
    u_put_uint((r1.eax >> 12) & 0x3u);
    u_putc('\n');
    u_puts("  apic/logical: apic ");
    u_put_uint((r1.ebx >> 24) & 0xFFu);
    u_puts(" logical ");
    u_put_uint((r1.ebx >> 16) & 0xFFu);
    u_puts(" clflush ");
    u_put_uint(((r1.ebx >> 8) & 0xFFu) * 8u);
    u_puts(" bytes\n");

    cpuid(0x80000000u, 0, &rx);
    max_ext = rx.eax;
    if (max_ext >= 0x80000004u) {
        cpuid(0x80000002u, 0, &rx);
        copy_reg(brand + 0, rx.eax);
        copy_reg(brand + 4, rx.ebx);
        copy_reg(brand + 8, rx.ecx);
        copy_reg(brand + 12, rx.edx);
        cpuid(0x80000003u, 0, &rx);
        copy_reg(brand + 16, rx.eax);
        copy_reg(brand + 20, rx.ebx);
        copy_reg(brand + 24, rx.ecx);
        copy_reg(brand + 28, rx.edx);
        cpuid(0x80000004u, 0, &rx);
        copy_reg(brand + 32, rx.eax);
        copy_reg(brand + 36, rx.ebx);
        copy_reg(brand + 40, rx.ecx);
        copy_reg(brand + 44, rx.edx);
        brand[48] = 0;
        u_puts("  brand:        ");
        u_puts(brand);
        u_putc('\n');
    }

    u_puts("  features:    ");
    sep = "";
    put_feature(&sep, (r1.edx & (1u << 0)) != 0, "fpu");
    put_feature(&sep, (r1.edx & (1u << 4)) != 0, "tsc");
    put_feature(&sep, (r1.edx & (1u << 5)) != 0, "msr");
    put_feature(&sep, (r1.edx & (1u << 8)) != 0, "cx8");
    put_feature(&sep, (r1.edx & (1u << 15)) != 0, "cmov");
    put_feature(&sep, (r1.edx & (1u << 23)) != 0, "mmx");
    put_feature(&sep, (r1.edx & (1u << 24)) != 0, "fxsr");
    put_feature(&sep, (r1.edx & (1u << 25)) != 0, "sse");
    put_feature(&sep, (r1.edx & (1u << 26)) != 0, "sse2");
    put_feature(&sep, (r1.ecx & (1u << 0)) != 0, "sse3");
    put_feature(&sep, (r1.ecx & (1u << 9)) != 0, "ssse3");
    put_feature(&sep, (r1.ecx & (1u << 19)) != 0, "sse4.1");
    put_feature(&sep, (r1.ecx & (1u << 20)) != 0, "sse4.2");
    put_feature(&sep, (r1.ecx & (1u << 28)) != 0, "avx");
    if (!sep[0]) u_puts("none");
    u_putc('\n');

    if (max_ext >= 0x80000006u) {
        cpuid(0x80000006u, 0, &rx);
        u_puts("  cache:        l2 ");
        u_put_uint((rx.ecx >> 16) & 0xFFFFu);
        u_puts(" KB line ");
        u_put_uint(rx.ecx & 0xFFu);
        u_puts(" bytes\n");
    } else if (max_leaf >= 4u) {
        cpuid(4, 0, &rx);
        u_puts("  cache:        line ");
        u_put_uint((rx.ebx & 0xFFFu) + 1u);
        u_puts(" bytes\n");
    }
}

static void print_memory(void) {
    sys_meminfo_t mem;
    sys_e820_entry_t ent;
    unsigned long long usable = 0;
    unsigned long long reserved = 0;
    int count;

    u_puts("\nMemory\n");
    if (sys_meminfo(&mem) < 0) {
        u_puts("  unavailable\n");
        return;
    }

    u_puts("  physical:     ");
    u_put_uint(mem.pmm_total_frames * 4u);
    u_puts(" KB total, ");
    u_put_uint(mem.pmm_free_frames * 4u);
    u_puts(" KB free\n");
    u_puts("  kernel heap:  ");
    diag_put_hex32(mem.heap_base);
    u_puts(" - ");
    diag_put_hex32(mem.heap_top);
    u_puts(" (");
    u_put_uint((mem.heap_top - mem.heap_base) / 1024u);
    u_puts(" KB used)\n");

    count = sys_e820_entry(0, &ent);
    if (count <= 0) {
        u_puts("  e820:         unavailable\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        if (sys_e820_entry((unsigned int)i, &ent) < 0) break;
        if (ent.type == 1u) usable += ent.length;
        else reserved += ent.length;
    }
    u_puts("  e820:         ");
    u_put_uint((unsigned int)count);
    u_puts(" entries, usable ");
    u_put_uint((unsigned int)(usable >> 20));
    u_puts(" MB, reserved ");
    u_put_uint((unsigned int)(reserved >> 20));
    u_puts(" MB\n");
}

static void print_display(void) {
    sys_display_info_t disp;

    u_puts("\nDisplay\n");
    if (sys_display_info(&disp) < 0) {
        u_puts("  framebuffer:  unavailable\n");
        return;
    }

    u_puts("  framebuffer:  ");
    u_put_uint(disp.width);
    u_putc('x');
    u_put_uint(disp.height);
    u_puts(" pitch ");
    u_put_uint(disp.pitch);
    u_puts(" bpp ");
    u_put_uint(disp.bpp);
    u_puts(" format ");
    u_put_uint(disp.format);
    if (disp.format == SYS_DISPLAY_FORMAT_XRGB8888) {
        u_puts(" xrgb8888");
    }
    u_putc('\n');
}

static void print_usb(void) {
    sys_usbinfo_t usb;

    u_puts("\nUSB\n");
    if (sys_usbinfo(&usb) < 0) {
        u_puts("  unavailable\n");
        return;
    }

    u_puts("  controllers:  ");
    u_put_uint(usb.controller_count);
    u_puts(" (uhci ");
    u_put_uint(usb.uhci_count);
    u_puts(", ohci ");
    u_put_uint(usb.ohci_count);
    u_puts(", ehci ");
    u_put_uint(usb.ehci_count);
    u_puts(", xhci ");
    u_put_uint(usb.xhci_count);
    u_puts(")\n");
    u_puts("  devices:      keyboard ");
    u_puts(usb.keyboard_active ? "yes" : "no");
    u_puts(", mouse ");
    u_puts(usb.mouse_active ? "yes" : "no");
    u_puts(", storage ");
    u_puts(usb.storage_active ? "yes" : "no");
    u_puts(", powered ports ");
    u_put_uint(usb.powered_port_count);
    u_putc('\n');
}

static void print_network(void) {
    sys_netinfo_t net;

    u_puts("\nNetwork\n");
    if (sys_netinfo(&net) < 0) {
        u_puts("  unavailable\n");
        return;
    }

    u_puts("  adapter:      ");
    u_puts(net.net_driver);
    u_puts(" mac ");
    diag_put_mac(net.mac);
    u_puts(" link ");
    u_puts(net.net_link_up ? "up" : "down");
    u_putc('\n');
    if (net.ipv4_configured) {
        u_puts("  ipv4:         ");
        diag_put_ip(net.ip);
        u_puts(" gw ");
        diag_put_ip(net.gateway);
        u_putc('\n');
    } else {
        u_puts("  ipv4:         unconfigured\n");
    }
}

static void print_storage(void) {
    unsigned char sector[512];
    int rc;

    u_puts("\nStorage\n");
    rc = sys_block_read_sector(0, sector);
    if (rc < 0) {
        u_puts("  boot disk:    unavailable\n");
        return;
    }

    u_puts("  boot disk:    sector 0 readable, signature ");
    diag_put_hex_byte(sector[510]);
    diag_put_hex_byte(sector[511]);
    if (sector[510] == 0x55u && sector[511] == 0xAAu) {
        u_puts(" mbr");
    }
    u_putc('\n');
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("cpuz: SmallOS hardware summary\n");
    print_cpu();
    print_memory();
    print_display();
    print_usb();
    print_network();
    print_storage();

    sys_exit(0);
}
