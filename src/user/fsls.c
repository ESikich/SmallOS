#include "dirent.h"
#include "user_lib.h"

#define FSLS_MAX_ENTRIES 128

typedef struct {
    char name[NAME_MAX + 1];
    unsigned int size;
    int is_dir;
} fsls_entry_t;

static fsls_entry_t s_entries[FSLS_MAX_ENTRIES];

static int is_root_path(const char* path) {
    return !path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0');
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

static int entry_cmp(const fsls_entry_t* a, const fsls_entry_t* b) {
    if (a->is_dir != b->is_dir) {
        return a->is_dir ? -1 : 1;
    }
    return name_cmp(a->name, b->name);
}

static void sort_entries(fsls_entry_t* entries, unsigned int count) {
    for (unsigned int i = 1; i < count; i++) {
        fsls_entry_t key = entries[i];
        unsigned int j = i;
        while (j > 0 && entry_cmp(&key, &entries[j - 1]) < 0) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }
}

static void print_entry(const fsls_entry_t* ent) {
    u_puts("  ");
    u_puts(ent->name);
    if (ent->is_dir && ent->name[str_len(ent->name) - 1] != '/') {
        u_putc('/');
    }
    u_puts("  ");
    if (ent->is_dir) {
        u_puts("0-");
    } else {
        u_put_uint(ent->size);
        u_puts(" B");
    }
    u_putc('\n');
}

void _start(int argc, char** argv) {
    const char* path = argc >= 2 ? argv[1] : "";
    DIR* dir = opendir(path);
    unsigned int count = 0;
    if (!dir) {
        u_puts("fat16: not found: ");
        u_puts(path);
        u_putc('\n');
        sys_exit(1);
    }

    if (is_root_path(path)) {
        u_puts("fat16 root directory:\n");
    } else {
        u_puts("fat16 directory: ");
        u_puts(path);
        u_putc('\n');
    }
    u_puts("  name  size\n");

    struct dirent* ent;
    while ((ent = readdir(dir)) != 0) {
        if (count >= FSLS_MAX_ENTRIES) {
            closedir(dir);
            u_puts("fsls: too many entries\n");
            sys_exit(1);
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
        print_entry(&s_entries[i]);
    }
    sys_exit(0);
}
