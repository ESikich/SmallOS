#include "user_lib.h"

#define MORE_BUF_SIZE 1024u
#define MORE_DEFAULT_ROWS 25u
#define MORE_DEFAULT_COLS 80u

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

static void more_puts(const char* s) {
    (void)write_all(s, str_len(s));
}

static void clear_screen(void) {
    more_puts("\x1b[2J\x1b[H");
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

    more_puts("\r--More--");
    key = read_pager_key();
    more_puts("\r        \r");

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
    char buf[MORE_BUF_SIZE];
    uint32_t rows = MORE_DEFAULT_ROWS;
    uint32_t cols = MORE_DEFAULT_COLS;
    uint32_t page_lines;
    uint32_t lines_left;
    uint32_t col = 0;

    if (sys_terminal_size(&rows, &cols) < 0 || rows == 0u) {
        rows = MORE_DEFAULT_ROWS;
    }
    if (cols == 0u) {
        cols = MORE_DEFAULT_COLS;
    }

    page_lines = rows > 1u ? rows - 1u : 1u;
    lines_left = page_lines;
    clear_screen();

    for (;;) {
        int n = sys_fread(fd, buf, sizeof(buf));
        if (n < 0) {
            more_puts("more: read failed\n");
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

void _start(int argc, char** argv) {
    int fd = 0;
    int rc;

    if (argc > 2) {
        more_puts("usage: more [path]\n");
        sys_exit(1);
    }

    if (argc == 2) {
        fd = sys_open(argv[1]);
        if (fd < 0) {
            more_puts("more: failed\n");
            sys_exit(1);
        }
    }

    rc = page_fd(fd);
    if (argc == 2) {
        sys_close(fd);
    }
    sys_exit(rc);
}
