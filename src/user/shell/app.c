/*
 * User-space interactive shell for SmallOS.
 *
 * This shell is the normal boot target and owns command dispatch for user
 * programs.
 */

#include "user_lib.h"
#include "sys/wait.h"
#include "dirent.h"

#define SHELL_LINE_MAX     256
#define SHELL_ARG_MAX      16
#define SHELL_PATH_MAX     128
#define SHELL_PIPE_MAX     8
#define SHELL_JOB_MAX      8
#define SHELL_SIGTERM      15
#define SHELL_HISTORY_MAX  16

typedef struct {
    int used;
    int pid;
    unsigned int id;
    char command[SHELL_PATH_MAX];
} shell_job_t;

static shell_job_t s_jobs[SHELL_JOB_MAX];
static unsigned int s_next_job_id = 1;
static char s_history[SHELL_HISTORY_MAX][SHELL_LINE_MAX];
static unsigned int s_history_count = 0;

static int sh_execute_line(const char* text);
static void sh_job_clear(shell_job_t* job);

static int sh_streq(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int sh_has_sep(const char* s) {
    while (*s) {
        if (*s == '/' || *s == '\\') return 1;
        s++;
    }
    return 0;
}

static int sh_has_dot(const char* s) {
    while (*s) {
        if (*s == '.') return 1;
        s++;
    }
    return 0;
}

static void sh_copy(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    if (cap) dst[i] = 0;
}

static unsigned int sh_len(const char* s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static void sh_cat(char* dst, const char* src, unsigned int cap) {
    unsigned int n = sh_len(dst);
    while (n + 1 < cap && *src) {
        dst[n++] = *src++;
    }
    if (cap) dst[n] = 0;
}

static int sh_starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static int sh_is_space(char c) {
    return c == ' ' || c == '\t';
}

static int sh_join3(char* out,
                    unsigned int cap,
                    const char* a,
                    const char* b,
                    const char* c) {
    unsigned int pos = 0;
    const char* parts[3] = { a, b, c };

    if (!out || cap == 0) return 0;

    for (unsigned int part = 0; part < 3; part++) {
        const char* s = parts[part];
        if (!s) continue;
        while (*s) {
            if (pos + 1 >= cap) {
                out[0] = 0;
                return 0;
            }
            out[pos++] = *s++;
        }
    }

    out[pos] = 0;
    return 1;
}

static int sh_file_exists(const char* path) {
    uint32_t size = 0;
    int is_dir = 0;
    return sys_stat(path, &size, &is_dir) == 0 && !is_dir;
}

static int sh_path_exists(const char* path, int* out_is_dir) {
    uint32_t size = 0;
    int is_dir = 0;
    if (sys_stat(path, &size, &is_dir) < 0) return 0;
    if (out_is_dir) *out_is_dir = is_dir;
    return 1;
}

static int sh_resolve_program(const char* name, char* out, unsigned int out_size) {
    char candidate[SHELL_PATH_MAX];
    static const char* const prefixes[] = {
        "/bin/",
        "/usr/bin/",
        "/usr/sbin/",
        "bin/",
        "usr/bin/",
        "usr/sbin/",
    };

    if (!name || !name[0]) return 0;

    if (sh_has_sep(name)) {
        if (sh_file_exists(name)) {
            sh_copy(out, name, out_size);
            return 1;
        }
        if (!sh_has_dot(name) &&
            sh_join3(candidate, sizeof(candidate), name, ".elf", 0) &&
            sh_file_exists(candidate)) {
            sh_copy(out, candidate, out_size);
            return 1;
        }
        return 0;
    }

    for (unsigned int i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (sh_join3(candidate, sizeof(candidate), prefixes[i], name, ".elf") &&
            sh_file_exists(candidate)) {
            sh_copy(out, candidate, out_size);
            return 1;
        }
    }

    if (sh_has_dot(name) && sh_file_exists(name)) {
        sh_copy(out, name, out_size);
        return 1;
    }

    if (!sh_has_dot(name) &&
        sh_join3(candidate, sizeof(candidate), name, ".elf", 0) &&
        sh_file_exists(candidate)) {
        sh_copy(out, candidate, out_size);
        return 1;
    }

    return 0;
}

static int sh_tokenize(char* line, char* argv[], int max_args) {
    int argc = 0;
    char* p = line;

    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) {
            *p = 0;
            p++;
        }
    }

    return argc;
}

static void sh_apply_backspaces(char* line) {
    unsigned int out = 0;

    for (unsigned int i = 0; line[i]; i++) {
        unsigned char ch = (unsigned char)line[i];
        if (ch == '\b' || ch == 127) {
            if (out > 0) out--;
            continue;
        }
        if (ch >= 1 && ch <= 26) {
            line[out++] = (char)('a' + ch - 1);
            continue;
        }
        line[out++] = line[i];
    }

    line[out] = 0;
}

static int sh_parse_uint(const char* s, unsigned int* out) {
    unsigned int value = 0;
    if (!s || !*s || !out) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        value = value * 10u + (unsigned int)(*s - '0');
        s++;
    }
    *out = value;
    return 1;
}

static void sh_put_int(int value) {
    if (value < 0) {
        u_putc('-');
        u_put_uint((unsigned int)(-value));
    } else {
        u_put_uint((unsigned int)value);
    }
}

static void sh_make_prompt(char* out, unsigned int cap) {
    char cwd[128];
    if (!out || cap == 0) return;
    out[0] = 0;
    if (sys_getcwd(cwd, sizeof(cwd)) < 0 || cwd[0] == 0) {
        sh_copy(out, "/> ", cap);
        return;
    }
    if (cwd[0] != '/') sh_cat(out, "/", cap);
    sh_cat(out, cwd, cap);
    sh_cat(out, "> ", cap);
}

static void sh_prompt(void) {
    char prompt[144];
    sh_make_prompt(prompt, sizeof(prompt));
    u_puts(prompt);
}

static void sh_put_cwd(void) {
    char cwd[128];
    if (sys_getcwd(cwd, sizeof(cwd)) < 0 || cwd[0] == 0) {
        u_puts("/");
        return;
    }
    if (cwd[0] != '/') u_putc('/');
    u_puts(cwd);
}

static void sh_help(void) {
    u_puts("Commands:\n");
    u_puts("  help            show shell commands\n");
    u_puts("  clear           clear the terminal\n");
    u_puts("  meminfo         show memory summary\n");
    u_puts("  memmap          show physical memory map\n");
    u_puts("  netinfo         show network state\n");
    u_puts("  ip              show or set IPv4 config\n");
    u_puts("  ipconfig        alias for ip\n");
    u_puts("  dhcp            request IPv4 config via DHCP\n");
    u_puts("  netsend         queue a test network frame\n");
    u_puts("  netrecv         receive a test network frame\n");
    u_puts("  arpgw           resolve the default gateway\n");
    u_puts("  ping            ping the default gateway\n");
    u_puts("  pinggw          ping the default gateway\n");
    u_puts("  pingpublic      try public ICMP through gateway\n");
    u_puts("  netcheck        check gateway and public connectivity\n");
    u_puts("  mousetest       print mouse events for 5 seconds\n");
    u_puts("  ataread         read mounted block sector diagnostics\n");
    u_puts("  cd              change the shell working directory\n");
    u_puts("  pwd             print the shell working directory\n");
    u_puts("  runelf          run an ext2 ELF and wait\n");
    u_puts("  runelf_nowait   run an ext2 ELF and return\n");
    u_puts("  runelf_bg       run a reattachable background ELF\n");
    u_puts("  bg              run a reattachable background ELF\n");
    u_puts("  jobs            list reattachable background jobs\n");
    u_puts("  fg              reattach and wait for a job\n");
    u_puts("  kill            terminate a background job\n");
    u_puts("  selftest        run shipped ELF self-tests\n");
    u_puts("  shelltest       run built-in shell command tests\n");
    u_puts("  echo            print arguments\n");
    u_puts("  about           print system information\n");
    u_puts("  halt            halt the machine\n");
    u_puts("  reboot          reboot the machine\n");
    u_puts("  uptime          print uptime\n");
    u_puts("  top             show process CPU and RAM usage\n");
    u_puts("  ls              list an ext2 directory\n");
    u_puts("  tree            print an ext2 directory tree\n");
    u_puts("  fsread          inspect an ext2 file\n");
    u_puts("  cat             print an ext2 file\n");
    u_puts("  more            page stdin or an ext2 file\n");
    u_puts("  mkdir           create an ext2 directory\n");
    u_puts("  rmdir           remove an ext2 directory\n");
    u_puts("  rm              remove an ext2 file\n");
    u_puts("  touch           create or truncate an ext2 file\n");
    u_puts("  cp              copy an ext2 file\n");
    u_puts("  mv              move or rename an ext2 entry\n");
    u_puts("  edit            edit an ext2 file\n");
}

static const char* const s_builtin_names[] = {
    "help", "clear", "ip", "ipconfig", "mousetest", "cd", "pwd", "wd", "runelf",
    "runelf_nowait", "runelf_bg", "bg", "jobs", "fg", "kill",
    "selftest", "shelltest", "echo", "about", "halt", "reboot",
    "uptime", "ls", "tree", "fsread", "cat", "more", "mkdir",
    "rmdir", "rm", "touch", "cp", "mv", "edit", "exit",
};

static void sh_history_add(const char* line) {
    if (!line || !line[0]) return;
    if (s_history_count > 0 &&
        sh_streq(s_history[(s_history_count - 1) % SHELL_HISTORY_MAX], line)) {
        return;
    }

    sh_copy(s_history[s_history_count % SHELL_HISTORY_MAX], line, SHELL_LINE_MAX);
    s_history_count++;
}

static unsigned int sh_history_available(void) {
    return s_history_count < SHELL_HISTORY_MAX ? s_history_count : SHELL_HISTORY_MAX;
}

static const char* sh_history_at(unsigned int view_index) {
    unsigned int avail = sh_history_available();
    unsigned int first;
    if (avail == 0 || view_index >= avail) return 0;
    first = s_history_count - avail;
    return s_history[(first + view_index) % SHELL_HISTORY_MAX];
}

static void sh_common_prefix(char* common, const char* next) {
    unsigned int i = 0;
    while (common[i] && next[i] && common[i] == next[i]) i++;
    common[i] = 0;
}

static void sh_match_add(const char* candidate,
                         const char* prefix,
                         unsigned int* count,
                         char* common,
                         unsigned int common_cap) {
    if (!candidate || !prefix || !sh_starts_with(candidate, prefix)) return;
    if (*count > 0 && sh_streq(common, candidate)) return;
    if (*count == 0) {
        sh_copy(common, candidate, common_cap);
    } else {
        sh_common_prefix(common, candidate);
    }
    (*count)++;
}

static void sh_match_dir_entries(const char* dir_path,
                                 const char* leaf_prefix,
                                 const char* output_prefix,
                                 int add_slash_to_dirs,
                                 unsigned int* count,
                                 char* common,
                                 unsigned int common_cap) {
    DIR* dir = opendir(dir_path && dir_path[0] ? dir_path : ".");
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != 0) {
        char candidate[SHELL_LINE_MAX];
        if (!ent->d_name[0]) continue;
        if (!sh_starts_with(ent->d_name, leaf_prefix)) continue;
        sh_copy(candidate, output_prefix ? output_prefix : "", sizeof(candidate));
        sh_cat(candidate, ent->d_name, sizeof(candidate));
        if (add_slash_to_dirs && ent->d_is_dir) sh_cat(candidate, "/", sizeof(candidate));
        sh_match_add(candidate, "", count, common, common_cap);
    }
    closedir(dir);
}

static void sh_match_program_dir(const char* dir_path,
                                 const char* prefix,
                                 unsigned int* count,
                                 char* common,
                                 unsigned int common_cap) {
    DIR* dir = opendir(dir_path);
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != 0) {
        char name[SHELL_LINE_MAX];
        unsigned int len;
        if (ent->d_is_dir || !ent->d_name[0]) continue;
        sh_copy(name, ent->d_name, sizeof(name));
        len = sh_len(name);
        if (len > 4 &&
            name[len - 4] == '.' &&
            name[len - 3] == 'e' &&
            name[len - 2] == 'l' &&
            name[len - 1] == 'f') {
            name[len - 4] = 0;
        }
        sh_match_add(name, prefix, count, common, common_cap);
    }
    closedir(dir);
}

static int sh_token_is_command(const char* line, unsigned int token_start) {
    for (unsigned int i = 0; i < token_start; i++) {
        if (!sh_is_space(line[i])) return 0;
    }
    return 1;
}

static unsigned int sh_token_start_at_cursor(const char* line, unsigned int cursor) {
    unsigned int start = cursor;
    while (start > 0 && !sh_is_space(line[start - 1])) start--;
    return start;
}

static int sh_make_path_parts(const char* token,
                              char* dir_path,
                              unsigned int dir_cap,
                              char* leaf,
                              unsigned int leaf_cap,
                              char* output_prefix,
                              unsigned int output_cap) {
    int slash = -1;
    unsigned int len = sh_len(token);

    for (unsigned int i = 0; i < len; i++) {
        if (token[i] == '/') slash = (int)i;
    }

    if (slash < 0) {
        sh_copy(dir_path, ".", dir_cap);
        sh_copy(leaf, token, leaf_cap);
        sh_copy(output_prefix, "", output_cap);
        return 1;
    }

    if (slash == 0) {
        sh_copy(dir_path, "/", dir_cap);
    } else {
        unsigned int n = 0;
        while (n < (unsigned int)slash && n + 1 < dir_cap) {
            dir_path[n] = token[n];
            n++;
        }
        dir_path[n] = 0;
    }

    {
        unsigned int n = 0;
        while (n <= (unsigned int)slash && n + 1 < output_cap) {
            output_prefix[n] = token[n];
            n++;
        }
        output_prefix[n] = 0;
    }

    sh_copy(leaf, token + slash + 1, leaf_cap);
    return 1;
}

static int sh_complete_line(char* line, unsigned int* len, unsigned int* cursor) {
    unsigned int start = sh_token_start_at_cursor(line, *cursor);
    unsigned int token_len = *cursor - start;
    char token[SHELL_LINE_MAX];
    char common[SHELL_LINE_MAX];
    unsigned int match_count = 0;
    unsigned int insert_len;

    token[0] = 0;
    for (unsigned int i = 0; i < token_len && i + 1 < sizeof(token); i++) {
        token[i] = line[start + i];
        token[i + 1] = 0;
    }
    common[0] = 0;

    if (sh_token_is_command(line, start) && !sh_has_sep(token)) {
        for (unsigned int i = 0; i < sizeof(s_builtin_names) / sizeof(s_builtin_names[0]); i++) {
            sh_match_add(s_builtin_names[i], token, &match_count,
                         common, sizeof(common));
        }
        sh_match_program_dir("/bin", token, &match_count, common, sizeof(common));
        sh_match_program_dir("/usr/bin", token, &match_count, common, sizeof(common));
        sh_match_program_dir("/usr/sbin", token, &match_count, common, sizeof(common));
    } else {
        char dir_path[SHELL_PATH_MAX];
        char leaf[SHELL_PATH_MAX];
        char output_prefix[SHELL_PATH_MAX];
        sh_make_path_parts(token, dir_path, sizeof(dir_path),
                           leaf, sizeof(leaf),
                           output_prefix, sizeof(output_prefix));
        sh_match_dir_entries(dir_path, leaf, output_prefix, 1,
                             &match_count, common, sizeof(common));
    }

    if (match_count == 0) return 0;
    if (sh_len(common) <= token_len && match_count != 1) return 0;

    insert_len = sh_len(common) - token_len;
    if (*len + insert_len + (match_count == 1 ? 1u : 0u) >= SHELL_LINE_MAX) return 0;

    for (int i = (int)*len; i >= (int)*cursor; i--) {
        line[i + insert_len] = line[i];
    }
    for (unsigned int i = 0; i < insert_len; i++) {
        line[*cursor + i] = common[token_len + i];
    }
    *cursor += insert_len;
    *len += insert_len;

    if (match_count == 1 && *len + 1 < SHELL_LINE_MAX) {
        unsigned int common_len = sh_len(common);
        if (common_len == 0 || common[common_len - 1] != '/') {
            for (int i = (int)*len; i >= (int)*cursor; i--) {
                line[i + 1] = line[i];
            }
            line[*cursor] = ' ';
            (*cursor)++;
            (*len)++;
        }
    }

    return 1;
}

typedef enum {
    SH_KEY_CHAR,
    SH_KEY_ENTER,
    SH_KEY_BACKSPACE,
    SH_KEY_DELETE,
    SH_KEY_LEFT,
    SH_KEY_RIGHT,
    SH_KEY_HOME,
    SH_KEY_END,
    SH_KEY_UP,
    SH_KEY_DOWN,
    SH_KEY_TAB,
    SH_KEY_CANCEL,
    SH_KEY_EOF,
    SH_KEY_IGNORE,
} sh_key_kind_t;

typedef struct {
    sh_key_kind_t kind;
    char ch;
} sh_key_t;

static sh_key_t sh_read_key(void) {
    char ch;
    sh_key_t key;
    key.kind = SH_KEY_IGNORE;
    key.ch = 0;

    if (sys_read_raw(&ch, 1u) != 1) {
        key.kind = SH_KEY_EOF;
        return key;
    }

    if (ch == '\n' || ch == '\r') key.kind = SH_KEY_ENTER;
    else if (ch == '\b' || (unsigned char)ch == 127u) key.kind = SH_KEY_BACKSPACE;
    else if (ch == '\t') key.kind = SH_KEY_TAB;
    else if ((unsigned char)ch == 3u) key.kind = SH_KEY_CANCEL;
    else if ((unsigned char)ch == 4u) key.kind = SH_KEY_EOF;
    else if ((unsigned char)ch == 1u) key.kind = SH_KEY_HOME;
    else if ((unsigned char)ch == 5u) key.kind = SH_KEY_END;
    else if ((unsigned char)ch == 21u) key.kind = SH_KEY_IGNORE;
    else if ((unsigned char)ch == 27u) {
        char next;
        if (sys_read_raw(&next, 1u) != 1) return key;
        if (next == '[') {
            char code;
            if (sys_read_raw(&code, 1u) != 1) return key;
            if (code == 'A') key.kind = SH_KEY_UP;
            else if (code == 'B') key.kind = SH_KEY_DOWN;
            else if (code == 'C') key.kind = SH_KEY_RIGHT;
            else if (code == 'D') key.kind = SH_KEY_LEFT;
            else if (code == 'H') key.kind = SH_KEY_HOME;
            else if (code == 'F') key.kind = SH_KEY_END;
            else if (code == '3') {
                char tilde;
                if (sys_read_raw(&tilde, 1u) == 1 && tilde == '~') key.kind = SH_KEY_DELETE;
            }
        }
    } else if ((unsigned char)ch >= 32u && (unsigned char)ch < 127u) {
        key.kind = SH_KEY_CHAR;
        key.ch = ch;
    }

    return key;
}

static void sh_move_cursor_left(unsigned int n) {
    while (n--) u_puts("\x1b[D");
}

static void sh_move_cursor_right(unsigned int n) {
    while (n--) u_puts("\x1b[C");
}

static void sh_redraw_input(const char* prompt,
                            const char* line,
                            unsigned int len,
                            unsigned int cursor) {
    u_putc('\r');
    u_puts(prompt);
    u_puts(line);
    u_puts("\x1b[K");
    if (cursor < len) sh_move_cursor_left(len - cursor);
}

static void sh_replace_input(const char* prompt,
                             char* line,
                             unsigned int* len,
                             unsigned int* cursor,
                             const char* replacement) {
    sh_copy(line, replacement ? replacement : "", SHELL_LINE_MAX);
    *len = sh_len(line);
    *cursor = *len;
    sh_redraw_input(prompt, line, *len, *cursor);
}

static int sh_readline_interactive(char* out, unsigned int cap) {
    char prompt[144];
    char line[SHELL_LINE_MAX];
    char saved[SHELL_LINE_MAX];
    unsigned int len = 0;
    unsigned int cursor = 0;
    unsigned int hist_avail = sh_history_available();
    unsigned int hist_view = hist_avail;

    if (!out || cap == 0) return 0;
    line[0] = 0;
    saved[0] = 0;

    sh_make_prompt(prompt, sizeof(prompt));
    u_puts(prompt);

    for (;;) {
        sh_key_t key = sh_read_key();

        if (key.kind == SH_KEY_EOF) {
            if (len == 0) return 0;
            continue;
        }

        if (key.kind == SH_KEY_ENTER) {
            line[len] = 0;
            u_putc('\n');
            sh_copy(out, line, cap);
            return 1;
        }

        if (key.kind == SH_KEY_CANCEL) {
            u_puts("^C\n");
            line[0] = 0;
            len = 0;
            cursor = 0;
            hist_view = hist_avail;
            sh_make_prompt(prompt, sizeof(prompt));
            u_puts(prompt);
            continue;
        }

        if (key.kind == SH_KEY_CHAR) {
            if (len + 1 >= SHELL_LINE_MAX) continue;
            for (int i = (int)len; i >= (int)cursor; i--) {
                line[i + 1] = line[i];
            }
            line[cursor++] = key.ch;
            len++;
            if (cursor == len) {
                u_putc(key.ch);
            } else {
                sh_redraw_input(prompt, line, len, cursor);
            }
            continue;
        }

        if (key.kind == SH_KEY_BACKSPACE) {
            if (cursor == 0) continue;
            for (unsigned int i = cursor - 1; i < len; i++) {
                line[i] = line[i + 1];
            }
            cursor--;
            len--;
            sh_redraw_input(prompt, line, len, cursor);
            continue;
        }

        if (key.kind == SH_KEY_DELETE) {
            if (cursor >= len) continue;
            for (unsigned int i = cursor; i < len; i++) {
                line[i] = line[i + 1];
            }
            len--;
            sh_redraw_input(prompt, line, len, cursor);
            continue;
        }

        if (key.kind == SH_KEY_LEFT) {
            if (cursor > 0) {
                cursor--;
                sh_move_cursor_left(1);
            }
            continue;
        }

        if (key.kind == SH_KEY_RIGHT) {
            if (cursor < len) {
                cursor++;
                sh_move_cursor_right(1);
            }
            continue;
        }

        if (key.kind == SH_KEY_HOME) {
            if (cursor > 0) {
                sh_move_cursor_left(cursor);
                cursor = 0;
            }
            continue;
        }

        if (key.kind == SH_KEY_END) {
            if (cursor < len) {
                sh_move_cursor_right(len - cursor);
                cursor = len;
            }
            continue;
        }

        if (key.kind == SH_KEY_UP) {
            const char* hist;
            if (hist_avail == 0 || hist_view == 0) continue;
            if (hist_view == hist_avail) sh_copy(saved, line, sizeof(saved));
            hist_view--;
            hist = sh_history_at(hist_view);
            sh_replace_input(prompt, line, &len, &cursor, hist);
            continue;
        }

        if (key.kind == SH_KEY_DOWN) {
            if (hist_avail == 0 || hist_view == hist_avail) continue;
            hist_view++;
            if (hist_view == hist_avail) {
                sh_replace_input(prompt, line, &len, &cursor, saved);
            } else {
                sh_replace_input(prompt, line, &len, &cursor, sh_history_at(hist_view));
            }
            continue;
        }

        if (key.kind == SH_KEY_TAB) {
            if (sh_complete_line(line, &len, &cursor)) {
                sh_redraw_input(prompt, line, len, cursor);
            }
            continue;
        }
    }
}

static void sh_jobs_terminate_all(void) {
    for (unsigned int i = 0; i < SHELL_JOB_MAX; i++) {
        if (s_jobs[i].used) {
            int status = 0;
            (void)sys_kill(s_jobs[i].pid, SHELL_SIGTERM);
            (void)sys_waitpid(s_jobs[i].pid, &status, 0);
            sh_job_clear(&s_jobs[i]);
        }
    }
}

static int sh_wait_foreground(int pid, int* status) {
    int rc = sys_waitpid_foreground(pid, status);
    if (rc >= 0) return rc;
    return sys_waitpid(pid, status, 0);
}

static int sh_launch_wait(const char* path, int argc, char* argv[], const char* label) {
    int pid;
    int status = 0;

    pid = sys_exec_foreground(path, argc, argv);
    if (pid < 0) {
        u_puts(label);
        u_puts(": failed\n");
        return 0;
    }

    if (sh_wait_foreground(pid, &status) < 0) {
        u_puts(label);
        u_puts(": wait failed\n");
        return 0;
    }

    return 1;
}

static int sh_run_external(int argc, char* argv[]) {
    char path[SHELL_PATH_MAX];

    if (!sh_resolve_program(argv[0], path, sizeof(path))) {
        u_puts("Unknown command: ");
        u_puts(argv[0]);
        u_putc('\n');
        return 0;
    }

    return sh_launch_wait(path, argc, argv, argv[0]);
}

static int sh_runelf(int argc, char* argv[]) {
    char path[SHELL_PATH_MAX];

    if (argc < 2) {
        u_puts("Usage: runelf <n>\n");
        return 0;
    }

    if (!sh_resolve_program(argv[1], path, sizeof(path))) {
        u_puts("runelf: failed\n");
        return 0;
    }

    return sh_launch_wait(path, argc - 1, &argv[1], "runelf");
}

static int sh_runelf_nowait(int argc, char* argv[]) {
    char path[SHELL_PATH_MAX];
    int pid;

    if (argc < 2) {
        u_puts("Usage: runelf_nowait <n>\n");
        return 0;
    }

    if (!sh_resolve_program(argv[1], path, sizeof(path))) {
        u_puts("runelf_nowait: failed\n");
        return 0;
    }

    pid = sys_exec(path, argc - 1, &argv[1]);
    if (pid < 0) {
        u_puts("runelf_nowait: failed\n");
        return 0;
    }

    return 1;
}

static shell_job_t* sh_job_alloc(int pid, const char* command) {
    for (unsigned int i = 0; i < SHELL_JOB_MAX; i++) {
        if (!s_jobs[i].used) {
            s_jobs[i].used = 1;
            s_jobs[i].pid = pid;
            s_jobs[i].id = s_next_job_id++;
            sh_copy(s_jobs[i].command, command, sizeof(s_jobs[i].command));
            return &s_jobs[i];
        }
    }
    return 0;
}

static shell_job_t* sh_job_find(unsigned int id) {
    for (unsigned int i = 0; i < SHELL_JOB_MAX; i++) {
        if (s_jobs[i].used && s_jobs[i].id == id) return &s_jobs[i];
    }
    return 0;
}

static void sh_job_clear(shell_job_t* job) {
    if (!job) return;
    job->used = 0;
    job->pid = 0;
    job->id = 0;
    job->command[0] = 0;
}

static int sh_runelf_bg(int argc, char* argv[]) {
    char path[SHELL_PATH_MAX];
    int pid;
    shell_job_t* job;

    if (argc < 2) {
        u_puts("Usage: ");
        u_puts(argv[0]);
        u_puts(" <n>\n");
        return 0;
    }

    if (!sh_resolve_program(argv[1], path, sizeof(path))) {
        u_puts(argv[0]);
        u_puts(": failed\n");
        return 0;
    }

    pid = sys_exec(path, argc - 1, &argv[1]);
    if (pid < 0) {
        u_puts(argv[0]);
        u_puts(": failed\n");
        return 0;
    }

    job = sh_job_alloc(pid, path);
    if (!job) {
        u_puts(argv[0]);
        u_puts(": job table full\n");
        sys_kill(pid, SHELL_SIGTERM);
        return 0;
    }

    u_puts("[");
    u_put_uint(job->id);
    u_puts("] ");
    u_puts(path);
    u_putc('\n');
    return 1;
}

static void sh_jobs(void) {
    int any = 0;

    for (unsigned int i = 0; i < SHELL_JOB_MAX; i++) {
        int status = 0;
        if (!s_jobs[i].used) continue;
        any = 1;
        u_puts("[");
        u_put_uint(s_jobs[i].id);
        u_puts("] ");
        if (sys_waitpid(s_jobs[i].pid, &status, WNOHANG) == s_jobs[i].pid) {
            u_puts("done  ");
            u_puts(s_jobs[i].command);
            u_putc('\n');
            sh_job_clear(&s_jobs[i]);
        } else {
            u_puts("running  ");
            u_puts(s_jobs[i].command);
            u_putc('\n');
        }
    }

    if (!any) u_puts("jobs: none\n");
}

static void sh_jobs_reap_completed(void) {
    for (unsigned int i = 0; i < SHELL_JOB_MAX; i++) {
        int status = 0;
        if (!s_jobs[i].used) continue;
        if (sys_waitpid(s_jobs[i].pid, &status, WNOHANG) == s_jobs[i].pid) {
            u_puts("[");
            u_put_uint(s_jobs[i].id);
            u_puts("] done  ");
            u_puts(s_jobs[i].command);
            u_putc('\n');
            sh_job_clear(&s_jobs[i]);
        }
    }
}

static int sh_fg(int argc, char* argv[]) {
    unsigned int id = 0;
    shell_job_t* job;
    int status = 0;

    if (argc < 2 || !sh_parse_uint(argv[1], &id)) {
        u_puts("usage: fg <jobid>\n");
        return 0;
    }

    job = sh_job_find(id);
    if (!job) {
        u_puts("fg: no such job\n");
        return 0;
    }

    u_puts("fg: ");
    u_puts(job->command);
    u_putc('\n');
    if (sh_wait_foreground(job->pid, &status) < 0) {
        u_puts("fg: wait failed\n");
        return 0;
    }

    sh_job_clear(job);
    return 1;
}

static int sh_kill_job(int argc, char* argv[]) {
    unsigned int id = 0;
    shell_job_t* job;

    if (argc < 2 || !sh_parse_uint(argv[1], &id)) {
        u_puts("usage: kill <jobid>\n");
        return 0;
    }

    job = sh_job_find(id);
    if (!job) {
        u_puts("kill: no such job\n");
        return 0;
    }

    sys_kill(job->pid, SHELL_SIGTERM);
    sh_job_clear(job);
    return 1;
}

static int sh_run_pipeline(int argc, char* argv[]) {
    char* stage_argv[SHELL_PIPE_MAX][SHELL_ARG_MAX + 1];
    int stage_argc[SHELL_PIPE_MAX];
    char paths[SHELL_PIPE_MAX][SHELL_PATH_MAX];
    int pids[SHELL_PIPE_MAX];
    int pipes[SHELL_PIPE_MAX - 1][2];
    int stage_count = 0;
    int arg_start = 0;

    for (int i = 0; i < SHELL_PIPE_MAX; i++) {
        stage_argc[i] = 0;
        pids[i] = -1;
    }
    for (int i = 0; i < SHELL_PIPE_MAX - 1; i++) {
        pipes[i][0] = -1;
        pipes[i][1] = -1;
    }

    for (int i = 0; i <= argc; i++) {
        if (i == argc || sh_streq(argv[i], "|")) {
            int count = i - arg_start;
            if (count <= 0 || stage_count >= SHELL_PIPE_MAX) {
                u_puts("pipeline: syntax error\n");
                return 0;
            }
            stage_argc[stage_count] = count;
            for (int j = 0; j < count; j++) {
                stage_argv[stage_count][j] = argv[arg_start + j];
            }
            stage_argv[stage_count][count] = 0;
            if (!sh_resolve_program(stage_argv[stage_count][0],
                                    paths[stage_count],
                                    sizeof(paths[stage_count]))) {
                u_puts("pipeline: command not found: ");
                u_puts(stage_argv[stage_count][0]);
                u_putc('\n');
                return 0;
            }
            stage_count++;
            arg_start = i + 1;
        }
    }

    for (int i = 0; i < stage_count - 1; i++) {
        if (sys_pipe(pipes[i]) < 0) {
            u_puts("pipeline: pipe failed\n");
            return 0;
        }
    }

    for (int i = 0; i < stage_count; i++) {
        int pid = sys_fork();
        if (pid < 0) {
            u_puts("pipeline: fork failed\n");
            return 0;
        }
        if (pid == 0) {
            if (i > 0) (void)sys_dup2(pipes[i - 1][0], 0);
            if (i < stage_count - 1) (void)sys_dup2(pipes[i][1], 1);
            for (int p = 0; p < stage_count - 1; p++) {
                if (pipes[p][0] >= 0) sys_close(pipes[p][0]);
                if (pipes[p][1] >= 0) sys_close(pipes[p][1]);
            }
            sys_execve(paths[i], stage_argv[i], 0);
            u_puts("pipeline: exec failed: ");
            u_puts(paths[i]);
            u_putc('\n');
            sys_exit(127);
        }
        pids[i] = pid;
    }

    for (int i = 0; i < stage_count - 1; i++) {
        if (pipes[i][0] >= 0) sys_close(pipes[i][0]);
        if (pipes[i][1] >= 0) sys_close(pipes[i][1]);
    }

    for (int i = 0; i < stage_count; i++) {
        int status = 0;
        if (pids[i] >= 0) (void)sys_waitpid(pids[i], &status, 0);
    }

    return 1;
}

static void sh_builtin_mousetest(void) {
    sys_mouse_state_t mouse;
    unsigned int deadline;
    unsigned int last_sequence;
    unsigned int events = 0;
    int usb_open = 0;

    if (sys_usb_mouse_op(SYS_USB_MOUSE_OP_OPEN, 0u) > 0) {
        usb_open = 1;
        u_puts("mousetest: usb poll active\n");
    }

    if (sys_mouse_read(&mouse) < 0) {
        if (usb_open) {
            sys_usb_mouse_op(SYS_USB_MOUSE_OP_CLOSE, 0u);
        }
        u_puts("mousetest: mouse unavailable\n");
        return;
    }

    last_sequence = mouse.sequence;
    deadline = sys_get_ticks() + (5u * SMALLOS_TIMER_HZ);
    u_puts("mousetest: move/click mouse for 5 seconds\n");

    while ((int)(sys_get_ticks() - deadline) < 0) {
        if (usb_open && sys_usb_mouse_op(SYS_USB_MOUSE_OP_POLL, 0u) < 0) {
            usb_open = 0;
            u_puts("mousetest: usb poll stopped\n");
        }
        if (sys_mouse_read(&mouse) < 0) {
            if (usb_open) {
                sys_usb_mouse_op(SYS_USB_MOUSE_OP_CLOSE, 0u);
            }
            u_puts("mousetest: mouse became unavailable\n");
            return;
        }
        if (mouse.sequence != last_sequence ||
            mouse.dx != 0 || mouse.dy != 0 || mouse.wheel != 0) {
            last_sequence = mouse.sequence;
            events++;
            u_puts("mousetest: seq=");
            u_put_uint(mouse.sequence);
            u_puts(" dx=");
            sh_put_int(mouse.dx);
            u_puts(" dy=");
            sh_put_int(mouse.dy);
            u_puts(" wheel=");
            sh_put_int(mouse.wheel);
            u_puts(" buttons=");
            u_put_uint(mouse.buttons);
            u_putc('\n');
        }
        sys_sleep(1);
    }

    if (usb_open) {
        sys_usb_mouse_op(SYS_USB_MOUSE_OP_CLOSE, 0u);
    }
    u_puts("mousetest: events=");
    u_put_uint(events);
    u_putc('\n');
}

static int sh_builtin(int argc, char* argv[], int* should_exit) {
    if (argc == 0) return 1;

    if (sh_streq(argv[0], "help")) {
        sh_help();
        return 1;
    }
    if (sh_streq(argv[0], "exit")) {
        *should_exit = 1;
        return 1;
    }
    if (sh_streq(argv[0], "pwd") || sh_streq(argv[0], "wd")) {
        u_puts("pwd: ");
        sh_put_cwd();
        u_putc('\n');
        return 1;
    }
    if (sh_streq(argv[0], "clear")) {
        u_puts("\x1b[2J\x1b[H");
        return 1;
    }
    if (sh_streq(argv[0], "cd")) {
        const char* target;
        if (argc < 2) {
            u_puts("usage: cd <path>\n");
            return 1;
        }
        target = argv[1];
        if (sys_chdir(target) < 0) {
            u_puts("cd: failed\n");
        } else {
            u_puts("cd: ");
            sh_put_cwd();
            u_putc('\n');
        }
        return 1;
    }
    if (sh_streq(argv[0], "runelf")) {
        sh_runelf(argc, argv);
        return 1;
    }
    if (sh_streq(argv[0], "runelf_nowait")) {
        sh_runelf_nowait(argc, argv);
        return 1;
    }
    if (sh_streq(argv[0], "runelf_bg") || sh_streq(argv[0], "bg")) {
        sh_runelf_bg(argc, argv);
        return 1;
    }
    if (sh_streq(argv[0], "jobs")) {
        sh_jobs();
        return 1;
    }
    if (sh_streq(argv[0], "fg")) {
        sh_fg(argc, argv);
        return 1;
    }
    if (sh_streq(argv[0], "kill")) {
        sh_kill_job(argc, argv);
        return 1;
    }
    if (sh_streq(argv[0], "mousetest")) {
        sh_builtin_mousetest();
        return 1;
    }
    if (sh_streq(argv[0], "shelltest")) {
        extern void sh_shelltest(void);
        sh_shelltest();
        return 1;
    }
    if (sh_streq(argv[0], "selftest")) {
        extern void sh_selftest(void);
        sh_selftest();
        return 1;
    }

    return 0;
}

static int sh_has_pipeline(int argc, char* argv[]) {
    for (int i = 0; i < argc; i++) {
        if (sh_streq(argv[i], "|")) return 1;
    }
    return 0;
}

static int sh_execute_argv(int argc, char* argv[], int* should_exit) {
    if (argc == 0) return 1;
    if (sh_has_pipeline(argc, argv)) return sh_run_pipeline(argc, argv);
    if (sh_builtin(argc, argv, should_exit)) return 1;
    return sh_run_external(argc, argv);
}

static int sh_execute_line(const char* text) {
    char line[SHELL_LINE_MAX];
    char* argv[SHELL_ARG_MAX + 1];
    int argc;
    int should_exit = 0;

    sh_copy(line, text, sizeof(line));
    argc = sh_tokenize(line, argv, SHELL_ARG_MAX);
    argv[argc] = 0;
    return sh_execute_argv(argc, argv, &should_exit);
}

static void sh_shelltest_begin(const char* name) {
    u_puts("shelltest: ");
    u_puts(name);
    u_puts(" begin\n");
}

static void sh_shelltest_end(const char* name) {
    u_puts("shelltest: ");
    u_puts(name);
    u_puts(" end\n");
}

static void sh_shelltest_exec(const char* name, const char* command) {
    sh_shelltest_begin(name);
    (void)sh_execute_line(command);
    sh_shelltest_end(name);
}

void sh_shelltest(void) {
    u_puts("shelltest: start\n");

    sh_shelltest_exec("help", "help");
    sh_shelltest_exec("clear", "clear");
    sh_shelltest_exec("echo", "echo alpha beta gamma");
    sh_shelltest_exec("pipeline", "echo pipeline-ok | cat");
    sh_shelltest_exec("pipeline3", "echo pipeline3-ok | cat | cat");
    sh_shelltest_exec("more_pipe", "echo more-pipe-ok | more");
    sh_shelltest_exec("about", "about");
    sh_shelltest_exec("uptime", "uptime");
    sh_shelltest_exec("meminfo", "meminfo");
    sh_shelltest_exec("top", "top -r 1");
    sh_shelltest_exec("memmap", "memmap");
    sh_shelltest_exec("netinfo", "netinfo");
    sh_shelltest_exec("ip", "ip");
    sh_shelltest_exec("ip_addr", "ip addr show");
    sh_shelltest_exec("ip_route", "ip route show");
    sh_shelltest_exec("ip_dns", "ip dns show");
    sh_shelltest_exec("ipconfig", "ipconfig /all");
    sh_shelltest_exec("netsend", "netsend");
    sh_shelltest_exec("netrecv", "netrecv");
    sh_shelltest_exec("arpgw", "arpgw");
    sh_shelltest_exec("ping", "ping");
    sh_shelltest_exec("pinggw", "pinggw");
    sh_shelltest_exec("ataread", "ataread 0");
    sh_shelltest_exec("usbports", "usbports");
    sh_shelltest_exec("usbdiag", "usbdiag");
    sh_shelltest_exec("ls_abs_root", "ls /");
    sh_shelltest_exec("ls_path", "ls usr/bin");
    sh_shelltest_exec("tree", "tree");
    sh_shelltest_exec("tree_path", "tree usr/bin");
    sh_shelltest_exec("fsread", "fsread usr/bin/hello.elf");
    sh_shelltest_exec("fsread_path", "fsread usr/bin/hello.elf");
    sh_shelltest_exec("cat", "cat var/tmp/compiler.out");
    sh_shelltest_exec("touch", "touch var/tmp/EMPTY.TXT");
    sh_shelltest_exec("fsread_touch", "fsread var/tmp/EMPTY.TXT");
    sh_shelltest_exec("mkdir", "mkdir TESTDIR");
    sh_shelltest_exec("ls_newdir", "ls TESTDIR");
    sh_shelltest_exec("rmdir", "rmdir TESTDIR");
    sh_shelltest_exec("ls_removed", "ls TESTDIR");
    sh_shelltest_exec("mkdir_nested_parent", "mkdir NESTPARENT");
    sh_shelltest_exec("mkdir_nested_child", "mkdir NESTPARENT/CHILD");
    sh_shelltest_exec("ls_nested", "ls NESTPARENT");
    sh_shelltest_exec("rmdir_nested_child", "rmdir NESTPARENT/CHILD");
    sh_shelltest_exec("rmdir_nested_parent", "rmdir NESTPARENT");
    sh_shelltest_exec("ls_nested_removed", "ls NESTPARENT");
    sh_shelltest_exec("compiler_demo", "runelf usr/libexec/tests/compiler_demo");
    sh_shelltest_exec("tinycc", "runelf usr/bin/tcc.elf -v");
    sh_shelltest_exec("mkdir_var_tmp", "mkdir var/tmp");
    sh_shelltest_exec("mkdir_work", "mkdir var/tmp/WORK");
    sh_shelltest_exec("mkdir_samples", "mkdir var/tmp/samples");
    sh_shelltest_exec("mv_tccmath", "mv usr/share/examples/tinycc/tccmath.c var/tmp/samples");
    sh_shelltest_exec("mv_tccagg", "mv usr/share/examples/tinycc/tccagg.c var/tmp/samples");
    sh_shelltest_exec("mv_tcctree", "mv usr/share/examples/tinycc/tcctree.c var/tmp/samples");
    sh_shelltest_exec("mv_tccmini", "mv usr/share/examples/tinycc/tccmini.c var/tmp/samples");
    sh_shelltest_exec("ls_samples", "ls var/tmp/samples");
    sh_shelltest_exec("tccmath_build", "runelf usr/bin/tcc.elf -nostdlib -o var/tmp/tccmath.elf var/tmp/samples/tccmath.c");
    sh_shelltest_exec("tccmath_run", "runelf var/tmp/tccmath");
    sh_shelltest_exec("tccagg_build", "runelf usr/bin/tcc.elf -nostdlib -o var/tmp/tccagg.elf var/tmp/samples/tccagg.c");
    sh_shelltest_exec("tccagg_run", "runelf var/tmp/tccagg");
    sh_shelltest_exec("tcctree_build", "runelf usr/bin/tcc.elf -nostdlib -o var/tmp/tcctree.elf var/tmp/samples/tcctree.c");
    sh_shelltest_exec("tcctree_run", "runelf var/tmp/tcctree");
    sh_shelltest_exec("tccmini_build", "runelf usr/bin/tcc.elf -nostdlib -o var/tmp/tccmini.elf var/tmp/samples/tccmini.c");
    sh_shelltest_exec("tccmini_run", "runelf var/tmp/tccmini");
    sh_shelltest_exec("cat", "cat var/tmp/compiler.out");
    sh_shelltest_exec("touch", "touch var/tmp/EMPTY.TXT");
    sh_shelltest_exec("fsread_touch", "fsread var/tmp/EMPTY.TXT");
    sh_shelltest_exec("edit", "edit var/tmp/EDIT.TXT -c c -c a -c first-line -c second-line -c . -c wq");
    sh_shelltest_exec("cat_edit", "cat var/tmp/EDIT.TXT");
    sh_shelltest_exec("cd", "cd usr/bin");
    sh_shelltest_exec("pwd", "pwd");
    sh_shelltest_exec("ls", "ls");
    sh_shelltest_exec("fsread_rel", "fsread hello.elf");
    sh_shelltest_exec("cat_rel", "cat ../../var/tmp/compiler.out");
    sh_shelltest_exec("touch_rel", "touch ../../var/tmp/LOCAL.TXT");
    sh_shelltest_exec("fsread_touch_rel", "fsread ../../var/tmp/LOCAL.TXT");
    sh_shelltest_exec("runelf_rel", "runelf hello alpha beta");
    sh_shelltest_exec("runelf", "runelf usr/bin/hello alpha beta");
    sh_shelltest_exec("cd_root", "cd /");
    sh_shelltest_exec("pwd_root", "pwd");
    sh_shelltest_exec("runelf_path", "runelf usr/bin/hello alpha beta");
    sh_shelltest_exec("ls_root", "ls");
    sh_shelltest_exec("ls_glob", "ls *.elf");
    sh_shelltest_exec("cp", "cp var/tmp/compiler.out var/tmp/compiler.copy");
    sh_shelltest_exec("fsread_copy", "fsread var/tmp/compiler.copy");
    sh_shelltest_exec("mv", "mv var/tmp/compiler.copy var/tmp/compiler.moved");
    sh_shelltest_exec("fsread_moved", "fsread var/tmp/compiler.moved");
    sh_shelltest_exec("cp_dir", "cp var/tmp/compiler.out var/tmp/WORK");
    sh_shelltest_exec("fsread_dir_copy", "fsread var/tmp/WORK/compiler.out");
    sh_shelltest_exec("rm_dir", "rm var/tmp/WORK/compiler.out");
    sh_shelltest_exec("fsread_dir_removed", "fsread var/tmp/WORK/compiler.out");
    sh_shelltest_exec("mv_dir", "mv var/tmp/compiler.moved var/tmp/WORK");
    sh_shelltest_exec("runelf_nowait", "runelf_nowait usr/libexec/tests/ticks");
    sys_sleep(10);

    u_puts("shelltest: PASS\n");
}

static void sh_selftest_exec(const char* label, const char* command) {
    u_puts("selftest: ");
    u_puts(label);
    u_puts(" ... ");
    u_puts("selftest: ");
    u_puts(label);
    u_puts(" launched\n");
    (void)sh_execute_line(command);
    u_puts("selftest: ");
    u_puts(label);
    u_puts(" woke\n");
    u_puts("PASS\n");
}

void sh_selftest(void) {
    u_puts("selftest: start\n");
    sh_shelltest_begin("overall");
    sh_shelltest();
    sh_shelltest_end("overall");

    sh_selftest_exec("hello", "runelf usr/bin/hello alpha beta");
    sh_selftest_exec("ticks", "runelf usr/libexec/tests/ticks alpha beta");
    sh_selftest_exec("args", "runelf usr/libexec/tests/args alpha beta");
    sh_selftest_exec("runelf_test", "runelf usr/libexec/tests/runelf_test alpha beta gamma");
    sh_selftest_exec("readline", "runelf usr/libexec/tests/readline alpha beta");
    sh_selftest_exec("exec_test", "runelf usr/libexec/tests/exec_test alpha beta");
    sh_selftest_exec("waitprobe", "runelf usr/libexec/tests/waitprobe alpha beta");
    sh_selftest_exec("fileread", "runelf usr/libexec/tests/fileread alpha beta");
    sh_selftest_exec("compiler_demo", "runelf usr/libexec/tests/compiler_demo alpha beta");
    sh_selftest_exec("heapprobe", "runelf usr/libexec/tests/heapprobe alpha beta");
    sh_selftest_exec("statprobe", "runelf usr/libexec/tests/statprobe alpha beta");
    sh_selftest_exec("fileprobe", "runelf usr/libexec/tests/fileprobe alpha beta");
    sh_selftest_exec("cwdprobe", "runelf usr/libexec/tests/cwdprobe alpha beta");
    sh_selftest_exec("stdioprobe", "runelf usr/libexec/tests/stdioprobe alpha beta");
    sh_selftest_exec("dirprobe", "runelf usr/libexec/tests/dirprobe alpha beta");
    sh_selftest_exec("errnoprobe", "runelf usr/libexec/tests/errnoprobe alpha beta");
    sh_selftest_exec("badptrprobe", "runelf usr/libexec/tests/badptrprobe alpha beta");
    sh_selftest_exec("sleep_test", "runelf usr/libexec/tests/sleep_test alpha beta");
    sh_selftest_exec("timerfdprobe", "runelf usr/libexec/tests/timerfdprobe alpha beta");
    sh_selftest_exec("ptrguard", "runelf usr/libexec/tests/ptrguard alpha beta");
    sh_selftest_exec("preempt_test", "runelf usr/libexec/tests/preempt_test alpha beta");
    sh_selftest_exec("crtprobe", "runelf usr/libexec/tests/crtprobe.elf alpha nested/path longish-argument-0123456789abcdef");
    sh_selftest_exec("inputprobe", "runelf usr/libexec/tests/inputprobe alpha beta");
    sh_selftest_exec("pipeprobe", "runelf usr/libexec/tests/pipeprobe");
    sh_selftest_exec("dupprobe", "runelf usr/libexec/tests/dupprobe");
    sh_selftest_exec("forkprobe", "runelf usr/libexec/tests/forkprobe");
    sh_selftest_exec("execveprobe", "runelf usr/libexec/tests/execveprobe");
    sh_selftest_exec("fault ud", "runelf usr/libexec/tests/fault ud");
    sh_selftest_exec("fault gp", "runelf usr/libexec/tests/fault gp");
    sh_selftest_exec("fault de", "runelf usr/libexec/tests/fault de");
    sh_selftest_exec("fault br", "runelf usr/libexec/tests/fault br");
    sh_selftest_exec("fault pf", "runelf usr/libexec/tests/fault pf");

    u_puts("selftest: PASS\n");
}

int shell_main(int argc, char** argv) {
    char line[SHELL_LINE_MAX];
    int should_exit = 0;
    int line_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i] && sh_streq(argv[i], "--line-mode")) {
            line_mode = 1;
        }
    }

    u_puts("SmallOS user shell\n");
    u_puts("type 'help' for commands\n");

    while (!should_exit) {
        char* cmd_argv[SHELL_ARG_MAX + 1];
        int cmd_argc;
        int has_input = 0;

        if (line_mode) {
            sh_prompt();
            if (u_readline(line, sizeof(line)) <= 0) {
                u_putc('\n');
                break;
            }
            sh_apply_backspaces(line);
        } else {
            if (!sh_readline_interactive(line, sizeof(line))) {
                u_putc('\n');
                break;
            }
        }

        for (unsigned int i = 0; line[i]; i++) {
            if (!sh_is_space(line[i])) {
                has_input = 1;
                break;
            }
        }
        if (!has_input) continue;

        sh_history_add(line);
        cmd_argc = sh_tokenize(line, cmd_argv, SHELL_ARG_MAX);
        cmd_argv[cmd_argc] = 0;

        (void)sh_execute_argv(cmd_argc, cmd_argv, &should_exit);
        sh_jobs_reap_completed();
    }

    sh_jobs_terminate_all();
    u_puts("user shell exiting\n");
    return 0;
}
