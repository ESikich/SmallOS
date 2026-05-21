#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "term_keys.h"
#include "unistd.h"

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

static void path_copy(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    while (i + 1u < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    if (cap) {
        dst[i] = '\0';
    }
}

static int path_append(char* dst, const char* src, unsigned int cap) {
    unsigned int n = strlen(dst);
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
                          unsigned int cap,
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

    fd = open(path, O_RDONLY);
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

    for (unsigned int i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        int fd = open_page(sections[i], name, path);
        if (fd >= 0) {
            return fd;
        }
    }

    return -1;
}

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

static void man_puts(const char* s) {
    (void)write_all(s, strlen(s));
}

static void clear_screen(void) {
    man_puts("\x1b[2J\x1b[H");
}

static unsigned int pause_for_more(unsigned int page_lines) {
    int key;

    man_puts("\r--Man--");
    key = term_key_read_console(1);
    man_puts("\r       \r");

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
    char buf[MAN_BUF_SIZE];
    unsigned int rows = MAN_DEFAULT_ROWS;
    unsigned int cols = MAN_DEFAULT_COLS;
    unsigned int page_lines;
    unsigned int lines_left;
    unsigned int col = 0;

    if (term_get_size(&rows, &cols) < 0 || rows == 0u) {
        rows = MAN_DEFAULT_ROWS;
    }
    if (cols == 0u) {
        cols = MAN_DEFAULT_COLS;
    }

    page_lines = rows > 1u ? rows - 1u : 1u;
    lines_left = page_lines;
    clear_screen();

    for (;;) {
        int n = read(fd, buf, sizeof(buf));
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
        exit(1);
    }

    if (str_eq(name, "")) {
        usage();
        exit(1);
    }

    fd = find_page(section, name, path);
    if (fd < 0) {
        man_puts("man: no manual entry for ");
        man_puts(name);
        man_puts("\n");
        exit(1);
    }

    rc = page_fd(fd);
    close(fd);
    exit(rc);
}
