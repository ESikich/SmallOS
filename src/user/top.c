#include "diag_util.h"

typedef struct top_row {
    sys_procinfo_entry_t entry;
    unsigned int cpu_delta;
    unsigned int cpu_percent;
} top_row_t;

static void term_write(const char* s) {
    sys_write(s, str_len(s));
}

static const char* state_name(unsigned int state) {
    switch (state) {
        case 1: return "RUN";
        case 3: return "ZOM";
        case 4: return "WAIT";
        case 5: return "SLEEP";
        default: return "?";
    }
}

static void put_pad(const char* text, unsigned int width) {
    unsigned int len = str_len(text);

    u_puts(text);
    while (len < width) {
        u_putc(' ');
        len++;
    }
}

static void put_uint_width(unsigned int value, unsigned int width) {
    char buf[16];
    unsigned int len = 0;
    unsigned int tmp = value;

    if (tmp == 0) {
        buf[len++] = '0';
    } else {
        while (tmp > 0 && len < sizeof(buf)) {
            buf[len++] = (char)('0' + (tmp % 10u));
            tmp /= 10u;
        }
    }

    for (unsigned int i = len; i < width; i++) {
        u_putc(' ');
    }
    while (len > 0) {
        u_putc(buf[--len]);
    }
}

static int find_prev(const sys_procinfo_t* prev, unsigned int pid) {
    for (unsigned int i = 0; prev && i < prev->out_count; i++) {
        if (prev->entries[i].pid == pid) return (int)i;
    }
    return -1;
}

static void sort_rows(top_row_t* rows, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        for (unsigned int j = i + 1; j < count; j++) {
            if (rows[j].cpu_delta > rows[i].cpu_delta ||
                (rows[j].cpu_delta == rows[i].cpu_delta &&
                 rows[j].entry.ram_bytes > rows[i].entry.ram_bytes)) {
                top_row_t tmp = rows[i];
                rows[i] = rows[j];
                rows[j] = tmp;
            }
        }
    }
}

static int input_available(void) {
    struct pollfd pfd;

    pfd.fd = 0;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return sys_poll(&pfd, 1u, 0) == 1 && (pfd.revents & POLLIN);
}

static int should_quit(void) {
    char c = 0;

    while (input_available()) {
        if (sys_read_raw(&c, 1u) != 1) {
            return 0;
        }
        if (c == 'q' || c == 'Q' || c == 3 || c == 27) {
            return 1;
        }
    }
    return 0;
}

static void print_snapshot(const sys_procinfo_t* cur, const sys_procinfo_t* prev) {
    top_row_t rows[SYS_PROCINFO_MAX];
    unsigned int elapsed = cur->total_ticks;
    unsigned int total_ram = 0;

    if (prev && cur->total_ticks > prev->total_ticks) {
        elapsed = cur->total_ticks - prev->total_ticks;
    }
    if (elapsed == 0) elapsed = 1;

    for (unsigned int i = 0; i < cur->out_count; i++) {
        const sys_procinfo_entry_t* ent = &cur->entries[i];
        int prev_idx = find_prev(prev, ent->pid);
        unsigned int prev_cpu = 0;

        rows[i].entry = *ent;
        if (prev_idx >= 0) {
            prev_cpu = prev->entries[prev_idx].cpu_ticks;
        }
        rows[i].cpu_delta = ent->cpu_ticks >= prev_cpu ? ent->cpu_ticks - prev_cpu
                                                       : ent->cpu_ticks;
        rows[i].cpu_percent = (rows[i].cpu_delta * 100u) / elapsed;
        total_ram += ent->ram_bytes;
    }

    sort_rows(rows, cur->out_count);

    term_write("\x1b[H");
    u_puts("top: tasks ");
    u_put_uint(cur->out_count);
    u_puts("/");
    u_put_uint(cur->total_count);
    u_puts("  ticks ");
    u_put_uint(cur->total_ticks);
    u_puts("  task_ram ");
    u_put_uint(total_ram / 1024u);
    u_puts(" KB  q quit\n");

    u_puts(" PID  PPID ST    CPU%   CPU   RAMK  HEAPK NAME\n");
    for (unsigned int i = 0; i < cur->out_count; i++) {
        const top_row_t* row = &rows[i];

        put_uint_width(row->entry.pid, 4);
        u_putc(' ');
        put_uint_width(row->entry.parent_pid, 5);
        u_putc(' ');
        put_pad(state_name(row->entry.state), 5);
        put_uint_width(row->cpu_percent, 4);
        u_putc(' ');
        put_uint_width(row->entry.cpu_ticks, 5);
        u_putc(' ');
        put_uint_width(row->entry.ram_bytes / 1024u, 6);
        u_putc(' ');
        put_uint_width(row->entry.heap_bytes / 1024u, 6);
        u_putc(' ');
        u_puts(row->entry.name);
        term_write("\x1b[K\n");
    }
    term_write("\x1b[J");
}

void _start(int argc, char** argv) {
    unsigned int refreshes = 0;
    unsigned int interval_ticks = SMALLOS_TIMER_HZ;
    sys_procinfo_t prev;
    sys_procinfo_t cur;
    int have_prev = 0;

    for (int i = 1; i < argc; i++) {
        if (diag_streq(argv[i], "-r") && i + 1 < argc) {
            if (!diag_parse_uint(argv[++i], &refreshes)) {
                u_puts("top: invalid refresh count\n");
                sys_exit(1);
            }
        } else if (diag_streq(argv[i], "-d") && i + 1 < argc) {
            if (!diag_parse_uint(argv[++i], &interval_ticks)) {
                u_puts("top: invalid delay ticks\n");
                sys_exit(1);
            }
        } else if (diag_streq(argv[i], "-h") || diag_streq(argv[i], "--help")) {
            u_puts("usage: top [-r refreshes] [-d delay_ticks]\n");
            u_puts("       q, Esc, or Ctrl+C exits live view\n");
            sys_exit(0);
        } else {
            u_puts("usage: top [-r refreshes] [-d delay_ticks]\n");
            sys_exit(1);
        }
    }

    if (interval_ticks == 0) interval_ticks = 1;

    term_write("\x1b[?25l\x1b[2J");
    for (unsigned int sample = 0; refreshes == 0 || sample < refreshes; sample++) {
        if (sys_procinfo(&cur) < 0) {
            term_write("\x1b[?25h");
            u_puts("top: procinfo unavailable\n");
            sys_exit(1);
        }

        print_snapshot(&cur, have_prev ? &prev : 0);
        prev = cur;
        have_prev = 1;

        if (refreshes != 0 && sample + 1 >= refreshes) {
            break;
        }

        for (unsigned int waited = 0; waited < interval_ticks; waited++) {
            if (should_quit()) {
                term_write("\x1b[?25h\n");
                sys_exit(0);
            }
            sys_sleep(1);
        }
    }

    term_write("\x1b[?25h\n");
    sys_exit(0);
}
