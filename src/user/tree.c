#include "dirent.h"
#include "user_lib.h"

#define TREE_MAX_ENTRIES 128
#define TREE_PATH_MAX 256
#define TREE_LINE_MAX (TREE_PATH_MAX + NAME_MAX + 32)
#define TREE_OUT_BUF_SIZE 4096u
#define TREE_VERT "\xE2\x94\x82"
#define TREE_MID "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 "
#define TREE_LAST "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 "

typedef struct {
    char name[NAME_MAX + 1];
    int is_dir;
} tree_entry_t;

static unsigned int s_dir_count = 0;
static unsigned int s_file_count = 0;
static char s_out_buf[TREE_OUT_BUF_SIZE];
static unsigned int s_out_pos = 0;
static int s_out_error = 0;

static int flush_output(void) {
    if (s_out_error) {
        return 0;
    }
    if (s_out_pos == 0u) {
        return 1;
    }
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

    if (!s) {
        return 0;
    }
    if (s_out_error) {
        return 0;
    }

    while (done < len) {
        unsigned int avail = TREE_OUT_BUF_SIZE - s_out_pos;
        unsigned int chunk = len - done;

        if (avail == 0u) {
            if (!flush_output()) {
                return 0;
            }
            avail = TREE_OUT_BUF_SIZE;
        }
        if (chunk > avail) {
            chunk = avail;
        }
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

static int buffered_put_uint(unsigned int value) {
    char buf[16];
    unsigned int n = 0;

    if (value == 0u) {
        return buffered_putc('0');
    }

    while (value > 0u && n < sizeof(buf)) {
        buf[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n > 0u) {
        if (!buffered_putc(buf[--n])) {
            return 0;
        }
    }
    return 1;
}

static void print_error_prefix(const char* msg) {
    flush_output();
    u_puts(msg);
}

static int is_root_path(const char* path) {
    return !path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0');
}

static void copy_string(char* dst, unsigned int dst_size, const char* src) {
    unsigned int i = 0;

    if (!dst || dst_size == 0u) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    while (i + 1u < dst_size && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
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

static int entry_cmp(const tree_entry_t* a, const tree_entry_t* b) {
    if (a->is_dir != b->is_dir) {
        return a->is_dir ? -1 : 1;
    }
    return name_cmp(a->name, b->name);
}

static void sort_entries(tree_entry_t* entries, unsigned int count) {
    for (unsigned int i = 1; i < count; i++) {
        tree_entry_t key = entries[i];
        unsigned int j = i;
        while (j > 0 && entry_cmp(&key, &entries[j - 1]) < 0) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }
}

static int join_path(char* out, unsigned int out_size, const char* base, const char* name) {
    unsigned int pos = 0;

    if (!out || out_size == 0u || !name || name[0] == '\0') {
        return 0;
    }

    out[0] = '\0';
    if (is_root_path(base)) {
        if (out_size < 2u) {
            return 0;
        }
        out[pos++] = '/';
    } else if (base && !(base[0] == '.' && base[1] == '\0')) {
        while (base[pos]) {
            if (pos + 1u >= out_size) {
                out[0] = '\0';
                return 0;
            }
            out[pos] = base[pos];
            pos++;
        }
        if (pos > 0 && out[pos - 1u] != '/') {
            if (pos + 1u >= out_size) {
                out[0] = '\0';
                return 0;
            }
            out[pos++] = '/';
        }
    }

    for (unsigned int i = 0; name[i]; i++) {
        if (pos + 1u >= out_size) {
            out[0] = '\0';
            return 0;
        }
        out[pos++] = name[i];
    }

    out[pos] = '\0';
    return 1;
}

static int build_child_prefix(char* out,
                              unsigned int out_size,
                              const char* prefix,
                              int is_last) {
    unsigned int pos = 0;
    const char* suffix = is_last ? "    " : TREE_VERT "   ";

    if (!out || out_size == 0u) {
        return 0;
    }

    out[0] = '\0';
    if (prefix) {
        while (prefix[pos]) {
            if (pos + 1u >= out_size) {
                out[0] = '\0';
                return 0;
            }
            out[pos] = prefix[pos];
            pos++;
        }
    }

    for (unsigned int i = 0; suffix[i]; i++) {
        if (pos + 1u >= out_size) {
            out[0] = '\0';
            return 0;
        }
        out[pos++] = suffix[i];
    }

    out[pos] = '\0';
    return 1;
}

static int append_string(char* out, unsigned int out_size, unsigned int* pos, const char* s) {
    if (!out || !pos || !s) {
        return 0;
    }

    for (unsigned int i = 0; s[i]; i++) {
        if (*pos + 1u >= out_size) {
            return 0;
        }
        out[(*pos)++] = s[i];
    }
    out[*pos] = '\0';
    return 1;
}

static int append_char(char* out, unsigned int out_size, unsigned int* pos, char ch) {
    if (!out || !pos || *pos + 1u >= out_size) {
        return 0;
    }

    out[(*pos)++] = ch;
    out[*pos] = '\0';
    return 1;
}

static int write_entry_line(const char* prefix, const tree_entry_t* entry, int is_last) {
    char line[TREE_LINE_MAX];
    unsigned int pos = 0;
    uint32_t name_len;

    line[0] = '\0';
    if (prefix && !append_string(line, sizeof(line), &pos, prefix)) {
        return 0;
    }
    if (!append_string(line, sizeof(line), &pos, is_last ? TREE_LAST : TREE_MID) ||
        !append_string(line, sizeof(line), &pos, entry->name)) {
        return 0;
    }

    name_len = str_len(entry->name);
    if (entry->is_dir && (name_len == 0u || entry->name[name_len - 1u] != '/')) {
        if (!append_char(line, sizeof(line), &pos, '/')) {
            return 0;
        }
    }
    if (!append_char(line, sizeof(line), &pos, '\n')) {
        return 0;
    }

    return buffered_write(line, pos);
}

static int collect_entries(const char* path, tree_entry_t* entries, unsigned int* out_count) {
    DIR* dir = opendir(path);
    unsigned int count = 0;
    struct dirent* ent;

    if (!dir || !entries || !out_count) {
        if (dir) {
            closedir(dir);
        }
        return 0;
    }

    while ((ent = readdir(dir)) != 0) {
        if ((ent->d_name[0] == '.' && ent->d_name[1] == '\0') ||
            (ent->d_name[0] == '.' && ent->d_name[1] == '.' && ent->d_name[2] == '\0')) {
            continue;
        }

        if (count >= TREE_MAX_ENTRIES) {
            closedir(dir);
            return 0;
        }

        copy_string(entries[count].name, sizeof(entries[count].name), ent->d_name);
        entries[count].is_dir = ent->d_is_dir;
        count++;
    }

    closedir(dir);
    sort_entries(entries, count);
    *out_count = count;
    return 1;
}

static int print_tree(const char* path, const char* prefix) {
    tree_entry_t* entries = (tree_entry_t*)malloc(sizeof(tree_entry_t) * TREE_MAX_ENTRIES);
    unsigned int count = 0;

    if (!entries) {
        print_error_prefix("tree: out of memory\n");
        return 0;
    }

    if (!collect_entries(path, entries, &count)) {
        free(entries);
        print_error_prefix("tree: failed to read: ");
        u_puts(path);
        u_putc('\n');
        return 0;
    }

    for (unsigned int i = 0; i < count; i++) {
        tree_entry_t* entry = &entries[i];
        int is_last = (i + 1u == count);

        if (!write_entry_line(prefix, entry, is_last)) {
            free(entries);
            print_error_prefix(s_out_error ? "tree: write failed\n" : "tree: line too long\n");
            return 0;
        }
        if (entry->is_dir) {
            char child_path[TREE_PATH_MAX];
            char child_prefix[TREE_PATH_MAX];

            s_dir_count++;
            if (!join_path(child_path, sizeof(child_path), path, entry->name) ||
                !build_child_prefix(child_prefix, sizeof(child_prefix), prefix, is_last)) {
                free(entries);
                print_error_prefix("tree: path too long\n");
                return 0;
            }
            if (!print_tree(child_path, child_prefix)) {
                free(entries);
                return 0;
            }
        } else {
            s_file_count++;
        }
    }

    free(entries);
    return 1;
}

static void print_start_label(const char* path) {
    char cwd[TREE_PATH_MAX];

    if (is_root_path(path)) {
        buffered_puts("/\n");
        return;
    }

    if (path && path[0] == '.' && path[1] == '\0') {
        if (u_getcwd(cwd, sizeof(cwd)) >= 0) {
            buffered_puts(cwd);
            buffered_putc('\n');
            return;
        }
    }

    buffered_puts(path);
    buffered_putc('\n');
}

void _start(int argc, char** argv) {
    const char* path = argc >= 2 ? argv[1] : ".";
    uint32_t size = 0;
    int is_dir = 0;

    if (u_stat(path, &size, &is_dir) < 0) {
        u_puts("tree: not found: ");
        u_puts(path);
        u_putc('\n');
        sys_exit(1);
    }

    print_start_label(path);
    if (!is_dir) {
        s_file_count = 1;
    } else if (!print_tree(path, "")) {
        sys_exit(1);
    }

    if (!buffered_putc('\n') ||
        !buffered_put_uint(s_dir_count) ||
        !buffered_puts(" directories, ") ||
        !buffered_put_uint(s_file_count) ||
        !buffered_puts(" files\n") ||
        !flush_output()) {
        sys_exit(1);
    }
    sys_exit(0);
}
