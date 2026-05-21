#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "term_keys.h"
#include "unistd.h"

#define MORE_BUF_SIZE 1024u
#define MORE_DEFAULT_ROWS 25u
#define MORE_DEFAULT_COLS 80u

static int write_all(const char* buf, unsigned int len) {
    unsigned int off = 0;

    while (off < len) {
        int n = write(STDOUT_FILENO, buf + off, len - off);
        if (n <= 0) {
            return -1;
        }
        off += (unsigned int)n;
    }

    return 0;
}

static void more_puts(const char* s) {
    (void)write_all(s, strlen(s));
}

static void clear_screen(void) {
    more_puts("\x1b[2J\x1b[H");
}

static unsigned int pause_for_more(unsigned int page_lines) {
    int key;

    more_puts("\r--More--");
    key = term_key_read_console(1);
    more_puts("\r        \r");

    if (key == 'q' || key == 'Q') {
        return 0;
    }
    if (key == TERM_KEY_ENTER) {
        return 1u;
    }
    clear_screen();
    return page_lines;
}

static int page_fd(int fd) {
    char buf[MORE_BUF_SIZE];
    unsigned int rows = MORE_DEFAULT_ROWS;
    unsigned int cols = MORE_DEFAULT_COLS;
    unsigned int page_lines;
    unsigned int lines_left;
    unsigned int col = 0;

    if (term_get_size(&rows, &cols) < 0 || rows == 0u) {
        rows = MORE_DEFAULT_ROWS;
    }
    if (cols == 0u) {
        cols = MORE_DEFAULT_COLS;
    }

    page_lines = rows > 1u ? rows - 1u : 1u;
    lines_left = page_lines;
    clear_screen();

    for (;;) {
        int n = read(fd, buf, sizeof(buf));
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
    int fd = STDIN_FILENO;
    int rc;

    if (argc > 2) {
        more_puts("usage: more [path]\n");
        exit(1);
    }

    if (argc == 2) {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            more_puts("more: failed\n");
            exit(1);
        }
    }

    rc = page_fd(fd);
    if (argc == 2) {
        close(fd);
    }
    exit(rc);
}
