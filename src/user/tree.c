#include "dirent.h"
#include "user_lib.h"

#define TREE_MAX_ENTRIES 128
#define TREE_PATH_MAX 256
#define TREE_VERT "\xE2\x94\x82"
#define TREE_MID "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 "
#define TREE_LAST "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 "

typedef struct {
    char name[NAME_MAX + 1];
    int is_dir;
} tree_entry_t;

static unsigned int s_dir_count = 0;
static unsigned int s_file_count = 0;

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
        u_puts("tree: out of memory\n");
        return 0;
    }

    if (!collect_entries(path, entries, &count)) {
        free(entries);
        u_puts("tree: failed to read: ");
        u_puts(path);
        u_putc('\n');
        return 0;
    }

    for (unsigned int i = 0; i < count; i++) {
        tree_entry_t* entry = &entries[i];
        int is_last = (i + 1u == count);

        if (prefix) {
            u_puts(prefix);
        }
        u_puts(is_last ? TREE_LAST : TREE_MID);
        u_puts(entry->name);
        if (entry->is_dir) {
            char child_path[TREE_PATH_MAX];
            char child_prefix[TREE_PATH_MAX];
            uint32_t name_len = str_len(entry->name);

            s_dir_count++;
            if (name_len == 0u || entry->name[name_len - 1u] != '/') {
                u_putc('/');
            }
            u_putc('\n');
            if (!join_path(child_path, sizeof(child_path), path, entry->name) ||
                !build_child_prefix(child_prefix, sizeof(child_prefix), prefix, is_last)) {
                free(entries);
                u_puts("tree: path too long\n");
                return 0;
            }
            if (!print_tree(child_path, child_prefix)) {
                free(entries);
                return 0;
            }
        } else {
            s_file_count++;
            u_putc('\n');
        }
    }

    free(entries);
    return 1;
}

static void print_start_label(const char* path) {
    char cwd[TREE_PATH_MAX];

    if (is_root_path(path)) {
        u_puts("/\n");
        return;
    }

    if (path && path[0] == '.' && path[1] == '\0') {
        if (u_getcwd(cwd, sizeof(cwd)) >= 0) {
            u_puts(cwd);
            u_putc('\n');
            return;
        }
    }

    u_puts(path);
    u_putc('\n');
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

    u_putc('\n');
    u_put_uint(s_dir_count);
    u_puts(" directories, ");
    u_put_uint(s_file_count);
    u_puts(" files\n");
    sys_exit(0);
}
