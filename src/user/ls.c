#include "dirent.h"
#include "user_lib.h"

#define LS_MAX_ENTRIES 128
#define LS_LINE_MAX (NAME_MAX + 32)
#define LS_OUT_BUF_SIZE 4096u

typedef struct {
    char name[NAME_MAX + 1];
    unsigned int size;
    int is_dir;
} ls_entry_t;

static ls_entry_t s_entries[LS_MAX_ENTRIES];
static char s_out_buf[LS_OUT_BUF_SIZE];
static unsigned int s_out_pos = 0;
static int s_out_error = 0;

static int flush_output(void) {
    if (s_out_error) return 0;
    if (s_out_pos == 0u) return 1;
    if (sys_write(s_out_buf, s_out_pos) != (int)s_out_pos) {
        s_out_pos = 0;
        s_out_error = 1;
        return 0;
    }
    s_out_pos = 0;
    return 1;
}

static int buffered_write(const char* s, unsigned int len) {
    unsigned int done = 0;

    if (!s || s_out_error) return 0;
    while (done < len) {
        unsigned int avail = LS_OUT_BUF_SIZE - s_out_pos;
        unsigned int chunk = len - done;

        if (avail == 0u) {
            if (!flush_output()) return 0;
            avail = LS_OUT_BUF_SIZE;
        }
        if (chunk > avail) chunk = avail;
        memcpy(s_out_buf + s_out_pos, s + done, chunk);
        s_out_pos += chunk;
        done += chunk;
    }
    return 1;
}

static int buffered_puts(const char* s) {
    return buffered_write(s, str_len(s));
}

static int buffered_putc(char ch) {
    return buffered_write(&ch, 1u);
}

static void print_error_prefix(const char* msg) {
    flush_output();
    u_puts(msg);
}

static int is_root_path(const char* path) {
    return !path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0');
}

static int has_wildcard(const char* s) {
    if (!s) return 0;
    while (*s) {
        if (*s == '*' || *s == '?') return 1;
        s++;
    }
    return 0;
}

static int wildmatch(const char* pattern, const char* text) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*text) {
                if (wildmatch(pattern, text)) return 1;
                text++;
            }
            return 0;
        }
        if (*pattern == '?') {
            if (!*text) return 0;
            pattern++;
            text++;
            continue;
        }
        char pc = *pattern;
        char tc = *text;
        if (pc >= 'a' && pc <= 'z') pc = (char)(pc - 32);
        if (tc >= 'a' && tc <= 'z') tc = (char)(tc - 32);
        if (pc != tc) return 0;
        pattern++;
        text++;
    }
    return *text == '\0';
}

static int name_cmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return (int)ca - (int)cb;
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int entry_cmp(const ls_entry_t* a, const ls_entry_t* b) {
    if (a->is_dir != b->is_dir) {
        return a->is_dir ? -1 : 1;
    }
    return name_cmp(a->name, b->name);
}

static void sort_entries(ls_entry_t* entries, unsigned int count) {
    for (unsigned int i = 1; i < count; i++) {
        ls_entry_t key = entries[i];
        unsigned int j = i;
        while (j > 0 && entry_cmp(&key, &entries[j - 1]) < 0) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }
}

static int append_string(char* out, unsigned int out_size, unsigned int* pos, const char* s) {
    if (!out || !pos || !s) return 0;
    for (unsigned int i = 0; s[i]; i++) {
        if (*pos + 1u >= out_size) return 0;
        out[(*pos)++] = s[i];
    }
    out[*pos] = '\0';
    return 1;
}

static int append_char(char* out, unsigned int out_size, unsigned int* pos, char ch) {
    if (!out || !pos || *pos + 1u >= out_size) return 0;
    out[(*pos)++] = ch;
    out[*pos] = '\0';
    return 1;
}

static int append_uint(char* out, unsigned int out_size, unsigned int* pos, unsigned int value) {
    char tmp[16];
    unsigned int n = 0;

    if (value == 0) {
        return append_char(out, out_size, pos, '0');
    }

    while (value > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (n > 0) {
        if (!append_char(out, out_size, pos, tmp[--n])) return 0;
    }
    return 1;
}

static int split_wildcard(const char* path,
                          char* dir,
                          unsigned int dir_size,
                          const char** pattern) {
    const char* last_sep = 0;
    unsigned int len;

    if (!path || !dir || dir_size == 0u || !pattern) return 0;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last_sep = p;
    }
    if (!last_sep) {
        dir[0] = '.';
        dir[1] = '\0';
        *pattern = path;
        return 1;
    }
    len = (unsigned int)(last_sep - path);
    if (len == 0) {
        if (dir_size < 2u) return 0;
        dir[0] = '/';
        dir[1] = '\0';
    } else {
        if (len + 1u > dir_size) return 0;
        for (unsigned int i = 0; i < len; i++) dir[i] = path[i];
        dir[len] = '\0';
    }
    *pattern = last_sep + 1;
    return 1;
}

static int print_entry(const ls_entry_t* ent) {
    char line[LS_LINE_MAX];
    unsigned int pos = 0;

    line[0] = '\0';
    if (!append_string(line, sizeof(line), &pos, "  ") ||
        !append_string(line, sizeof(line), &pos, ent->name)) {
        return 0;
    }
    if (ent->is_dir && ent->name[str_len(ent->name) - 1] != '/') {
        if (!append_char(line, sizeof(line), &pos, '/')) return 0;
    }
    if (!append_string(line, sizeof(line), &pos, "  ")) return 0;
    if (ent->is_dir) {
        if (!append_string(line, sizeof(line), &pos, "0-")) return 0;
    } else {
        if (!append_uint(line, sizeof(line), &pos, ent->size) ||
            !append_string(line, sizeof(line), &pos, " B")) {
            return 0;
        }
    }
    if (!append_char(line, sizeof(line), &pos, '\n')) return 0;
    return buffered_write(line, pos);
}

static int list_path(const char* path, const char* pattern) {
    const char* use_path = path ? path : "";
    DIR* dir = opendir(use_path);
    unsigned int count = 0;
    if (!dir) {
        print_error_prefix("ext2: not found: ");
        u_puts(use_path);
        u_putc('\n');
        return 1;
    }

    if (use_path[0] == '.' && use_path[1] == '\0') {
        char cwd[128];
        if (u_getcwd(cwd, sizeof(cwd)) < 0) {
            cwd[0] = '/';
            cwd[1] = '\0';
        }
        if (is_root_path(cwd)) {
            buffered_puts("ext2 root directory:\n");
        } else {
            buffered_puts("ext2 directory: ");
            buffered_puts(cwd[0] == '/' ? cwd + 1 : cwd);
            buffered_putc('\n');
        }
    } else if (is_root_path(use_path)) {
        buffered_puts("ext2 root directory:\n");
    } else {
        buffered_puts("ext2 directory: ");
        buffered_puts(use_path);
        buffered_putc('\n');
    }
    buffered_puts("  name  size\n");

    struct dirent* ent;
    while ((ent = readdir(dir)) != 0) {
        if (pattern && !wildmatch(pattern, ent->d_name)) {
            continue;
        }
        if (count >= LS_MAX_ENTRIES) {
            closedir(dir);
            print_error_prefix("ls: too many entries\n");
            return 1;
        }
        for (unsigned int i = 0; i < sizeof(s_entries[count].name) - 1u && ent->d_name[i]; i++) {
            s_entries[count].name[i] = ent->d_name[i];
            s_entries[count].name[i + 1u] = '\0';
        }
        s_entries[count].size = ent->d_size;
        s_entries[count].is_dir = ent->d_is_dir;
        count++;
    }
    closedir(dir);

    sort_entries(s_entries, count);
    for (unsigned int i = 0; i < count; i++) {
        if (!print_entry(&s_entries[i])) {
            print_error_prefix(s_out_error ? "ls: write failed\n" : "ls: line too long\n");
            return 1;
        }
    }
    return flush_output() ? 0 : 1;
}

void _start(int argc, char** argv) {
    const char* path = argc >= 2 ? argv[1] : ".";
    char dir[128];
    const char* pattern = 0;

    if (has_wildcard(path)) {
        if (!split_wildcard(path, dir, sizeof(dir), &pattern)) {
            u_puts("ls: failed\n");
            sys_exit(1);
        }
        sys_exit(list_path(dir, pattern));
    }

    sys_exit(list_path(path, 0));
}
