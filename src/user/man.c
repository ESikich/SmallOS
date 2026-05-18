#include "user_lib.h"

#define MAN_PATH_MAX 160u
#define MAN_BUF_SIZE 1024u
#define MAN_DEFAULT_ROWS 25u
#define MAN_DEFAULT_COLS 80u

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int is_section(const char* s) {
    if (!s || !s[0] || s[1]) {
        return 0;
    }
    return s[0] >= '1' && s[0] <= '8';
}

static void path_copy(char* dst, const char* src, uint32_t cap) {
    uint32_t i = 0;
    while (i + 1u < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    if (cap) {
        dst[i] = '\0';
    }
}

static int path_append(char* dst, const char* src, uint32_t cap) {
    uint32_t n = str_len(dst);
    while (*src) {
        if (n + 1u >= cap) {
            return 0;
        }
        dst[n++] = *src++;
    }
    dst[n] = '\0';
    return 1;
}

static int build_man_path(char* out,
                          uint32_t cap,
                          const char* section,
                          const char* name) {
    path_copy(out, "/usr/share/man/man", cap);
    return path_append(out, section, cap) &&
           path_append(out, "/", cap) &&
           path_append(out, name, cap) &&
           path_append(out, ".", cap) &&
           path_append(out, section, cap);
}

static int open_page(const char* section, const char* name, char* path) {
    int fd;

    if (!build_man_path(path, MAN_PATH_MAX, section, name)) {
        return -1;
    }

    fd = sys_open(path);
    if (fd >= 0) {
        return fd;
    }
    return -1;
}

static int find_page(const char* section, const char* name, char* path) {
    static const char* const sections[] = {
        "1", "8", "5", "7", "2", "3", "4", "6",
    };

    if (section) {
        return open_page(section, name, path);
    }

    for (uint32_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        int fd = open_page(sections[i], name, path);
        if (fd >= 0) {
            return fd;
        }
    }

    return -1;
}

static int write_all(const char* buf, uint32_t len) {
    uint32_t off = 0;

    while (off < len) {
        int n = sys_write(buf + off, len - off);
        if (n <= 0) {
            return -1;
        }
        off += (uint32_t)n;
    }

    return 0;
}

static void man_puts(const char* s) {
    (void)write_all(s, str_len(s));
}

static void clear_screen(void) {
    man_puts("\x1b[2J\x1b[H");
}

static int read_pager_key(void) {
    sys_input_event_t ev;

    for (;;) {
        int n = sys_input_read(&ev, 1u, 0u);
        if (n < 0) {
            return 'q';
        }
        if (n == 1 &&
            ev.type == SYS_INPUT_TYPE_KEY &&
            (ev.flags & SYS_INPUT_KEY_PRESSED) != 0u) {
            if (ev.ascii) {
                return (int)(ev.ascii & 0xFFu);
            }
            return 0;
        }
    }
}

static uint32_t pause_for_more(uint32_t page_lines) {
    int key;

    man_puts("\r--Man--");
    key = read_pager_key();
    man_puts("\r       \r");

    if (key == 'q' || key == 'Q') {
        return 0;
    }
    if (key == '\n' || key == '\r') {
        return 1u;
    }
    clear_screen();
    return page_lines;
}

static int page_fd(int fd) {
    char buf[MAN_BUF_SIZE];
    uint32_t rows = MAN_DEFAULT_ROWS;
    uint32_t cols = MAN_DEFAULT_COLS;
    uint32_t page_lines;
    uint32_t lines_left;
    uint32_t col = 0;

    if (sys_terminal_size(&rows, &cols) < 0 || rows == 0u) {
        rows = MAN_DEFAULT_ROWS;
    }
    if (cols == 0u) {
        cols = MAN_DEFAULT_COLS;
    }

    page_lines = rows > 1u ? rows - 1u : 1u;
    lines_left = page_lines;
    clear_screen();

    for (;;) {
        int n = sys_fread(fd, buf, sizeof(buf));
        if (n < 0) {
            man_puts("man: read failed\n");
            return 1;
        }
        if (n == 0) {
            return 0;
        }

        for (int i = 0; i < n; i++) {
            char ch = buf[i];

            if (write_all(&ch, 1u) < 0) {
                return 1;
            }

            if (ch == '\n') {
                col = 0;
                if (lines_left > 0u) {
                    lines_left--;
                }
            } else if (ch == '\r') {
                col = 0;
            } else {
                col += (ch == '\t') ? 4u : 1u;
                if (col >= cols) {
                    col = 0;
                    if (lines_left > 0u) {
                        lines_left--;
                    }
                }
            }

            if (lines_left == 0u) {
                lines_left = pause_for_more(page_lines);
                if (lines_left == 0u) {
                    return 0;
                }
            }
        }
    }
}

static void usage(void) {
    man_puts("usage: man [section] name\n");
}

void _start(int argc, char** argv) {
    const char* section = 0;
    const char* name = 0;
    char path[MAN_PATH_MAX];
    int fd;
    int rc;

    if (argc == 2) {
        name = argv[1];
    } else if (argc == 3 && is_section(argv[1])) {
        section = argv[1];
        name = argv[2];
    } else {
        usage();
        sys_exit(1);
    }

    if (str_eq(name, "")) {
        usage();
        sys_exit(1);
    }

    fd = find_page(section, name, path);
    if (fd < 0) {
        man_puts("man: no manual entry for ");
        man_puts(name);
        man_puts("\n");
        sys_exit(1);
    }

    rc = page_fd(fd);
    sys_close(fd);
    sys_exit(rc);
}
