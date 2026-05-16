#include "diag_util.h"

void _start(int argc, char** argv) {
    unsigned int lba = 0;
    unsigned char sector[512];

    if (argc >= 2 && !diag_parse_uint(argv[1], &lba)) {
        u_puts("ataread: invalid lba\n");
        sys_exit(1);
    }

    if (sys_block_read_sector(lba, sector) < 0) {
        u_puts("ataread: read failed\n");
        sys_exit(1);
    }

    u_puts("lba ");
    u_put_uint(lba);
    u_puts(" bytes 0-31:\n  ");
    for (unsigned int i = 0; i < 32u; i++) {
        diag_put_hex_digit((sector[i] >> 4) & 0xFu);
        diag_put_hex_digit(sector[i] & 0xFu);
        u_putc(' ');
        if (i == 15u) u_puts("\n  ");
    }
    u_putc('\n');

    if (lba == 0u) {
        unsigned int entry_off = 446u + 16u;
        unsigned int partition_lba = sector[entry_off + 8u]
            | ((unsigned int)sector[entry_off + 9u] << 8)
            | ((unsigned int)sector[entry_off + 10u] << 16)
            | ((unsigned int)sector[entry_off + 11u] << 24);
        u_puts("sig: 0x");
        diag_put_hex_digit((sector[510] >> 4) & 0xFu);
        diag_put_hex_digit(sector[510] & 0xFu);
        u_putc(' ');
        u_puts("0x");
        diag_put_hex_digit((sector[511] >> 4) & 0xFu);
        diag_put_hex_digit(sector[511] & 0xFu);
        u_puts(" (expect 0x55 0xAA)\n");
        u_puts("ext2 partition lba: ");
        u_put_uint(partition_lba);
        u_putc('\n');
    }

    sys_exit(0);
}
