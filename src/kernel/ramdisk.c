#include "ramdisk.h"
#include "terminal.h"

static const rd_header_t* rd_header = 0;
static const rd_entry_t*  rd_entries = 0;

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static void print_hex_byte(unsigned char b) {
    static const char hex[] = "0123456789ABCDEF";
    terminal_putc(hex[b >> 4]);
    terminal_putc(hex[b & 0xF]);
}

int ramdisk_init(u32 base) {
    const unsigned char* raw = (const unsigned char*)base;

    /* DEBUG: print first 8 bytes at base so we can verify magic */
    terminal_puts("ramdisk @ 0x");
    print_hex_byte((unsigned char)(base >> 24));
    print_hex_byte((unsigned char)(base >> 16));
    print_hex_byte((unsigned char)(base >>  8));
    print_hex_byte((unsigned char)(base));
    terminal_puts(" bytes: ");
    for (int i = 0; i < 8; i++) {
        print_hex_byte(raw[i]);
        terminal_putc(' ');
    }
    terminal_putc('\n');

    const rd_header_t* hdr = (const rd_header_t*)base;

    if (hdr->magic != RAMDISK_MAGIC) {
        terminal_puts("ramdisk: bad magic\n");
        return 0;
    }

    rd_header  = hdr;
    rd_entries = (const rd_entry_t*)(base + sizeof(rd_header_t));

    terminal_puts("ramdisk: ok  files=");
    {
        unsigned int n = hdr->count;
        char buf[12];
        int i = 0;
        if (n == 0) { buf[i++] = '0'; }
        while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
        while (i > 0) terminal_putc(buf[--i]);
    }
    terminal_putc('\n');

    return 1;
}

int ramdisk_find(const char* name, const u8** out_data, u32* out_size) {
    if (!rd_header) {
        terminal_puts("ramdisk: not initialised\n");
        return 0;
    }

    for (u32 i = 0; i < rd_header->count; i++) {
        if (str_eq(name, rd_entries[i].name)) {
            *out_data = (const u8*)rd_header + rd_entries[i].offset;
            *out_size = rd_entries[i].size;
            return 1;
        }
    }

    return 0;
}