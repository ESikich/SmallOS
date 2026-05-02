#include "shell.h"
#include "terminal.h"
#include "line_editor.h"
#include "system.h"
#include "timer.h"
#include "memory.h"
#include "commands.h"
#include "parse.h"
#include "vfs.h"
#include "klib.h"
#include "keyboard.h"

#define HISTORY_MAX 8
#define SHELL_EVENT_QUEUE_SIZE 64

static line_editor_t editor;

static int prompt_row = 0;
static int prompt_col = 0;
static int prompt_len = 0;

static char history[HISTORY_MAX][LINE_EDITOR_MAX];
static int history_count = 0;
static int history_index = -1;
static char saved_current[LINE_EDITOR_MAX];
static char shell_cwd[SHELL_PATH_MAX];

typedef enum {
    SHELL_EVENT_NONE = 0,
    SHELL_EVENT_INPUT_CHAR,
    SHELL_EVENT_BACKSPACE,
    SHELL_EVENT_MOVE_LEFT,
    SHELL_EVENT_MOVE_RIGHT,
    SHELL_EVENT_MOVE_HOME,
    SHELL_EVENT_MOVE_END,
    SHELL_EVENT_DELETE,
    SHELL_EVENT_HISTORY_UP,
    SHELL_EVENT_HISTORY_DOWN,
    SHELL_EVENT_COMPLETE,
} shell_event_type_t;

typedef struct {
    shell_event_type_t type;
    char c;
} shell_event_t;

typedef enum {
    SHELL_COMPLETION_NONE = 0,
    SHELL_COMPLETION_COMMAND,
    SHELL_COMPLETION_PATH,
} shell_completion_kind_t;

static shell_event_t event_queue[SHELL_EVENT_QUEUE_SIZE];
static volatile int event_head = 0;
static volatile int event_tail = 0;
static volatile int event_count = 0;

static shell_completion_kind_t completion_kind = SHELL_COMPLETION_NONE;
static char completion_prefix[LINE_EDITOR_MAX];
static char completion_dir[SHELL_PATH_MAX];
static int completion_token_start = 0;
static unsigned int completion_next_index = 0;

static int path_is_sep(char c) {
    return c == '/' || c == '\\';
}

static int shell_char_equal_ci(char a, char b) {
    if (a >= 'a' && a <= 'z') {
        a = (char)(a - 32);
    }
    if (b >= 'a' && b <= 'z') {
        b = (char)(b - 32);
    }
    return a == b;
}

static int shell_starts_with_ci(const char* s, const char* prefix) {
    int i = 0;
    while (prefix[i]) {
        if (!shell_char_equal_ci(s[i], prefix[i])) {
            return 0;
        }
        i++;
    }
    return 1;
}

static void shell_completion_reset(void) {
    completion_kind = SHELL_COMPLETION_NONE;
    completion_prefix[0] = '\0';
    completion_dir[0] = '\0';
    completion_token_start = 0;
    completion_next_index = 0;
}

static int path_add_component(char comps[][32], int* count, const char* component) {
    if (*count >= 16) {
        return 0;
    }

    int len = 0;
    while (component[len] != '\0') {
        if (len >= 31) {
            return 0;
        }
        len++;
    }

    for (int i = 0; i < len; i++) {
        comps[*count][i] = component[i];
    }
    comps[*count][len] = '\0';
    (*count)++;
    return 1;
}

static int shell_path_build(const char* base, const char* path, char* out, unsigned int out_size) {
    char comps[16][32];
    int count = 0;

    const char* sources[2];
    int source_count = 0;
    if (base && base[0] != '\0' && (!path || !path_is_sep(path[0]))) {
        sources[source_count++] = base;
    }
    sources[source_count++] = path ? path : "";

    for (int s = 0; s < source_count; s++) {
        const char* cursor = sources[s];
        while (*cursor) {
            while (*cursor && path_is_sep(*cursor)) {
                cursor++;
            }
            if (*cursor == '\0') {
                break;
            }

            char component[32];
            int len = 0;
            while (cursor[len] && !path_is_sep(cursor[len])) {
                if (len >= 31) {
                    return 0;
                }
                component[len] = cursor[len];
                len++;
            }
            component[len] = '\0';
            cursor += len;

            if (k_strcmp(component, ".")) {
                continue;
            }
            if (k_strcmp(component, "..")) {
                if (count > 0) {
                    count--;
                }
                continue;
            }

            if (!path_add_component(comps, &count, component)) {
                return 0;
            }
        }
    }

    if (count == 0) {
        if (out_size == 0) {
            return 0;
        }
        out[0] = '\0';
        return 1;
    }

    unsigned int pos = 0;
    for (int i = 0; i < count; i++) {
        unsigned int len = 0;
        while (comps[i][len] != '\0') {
            len++;
        }

        if (pos + len + (i > 0 ? 1u : 0u) + 1u > out_size) {
            return 0;
        }

        if (i > 0) {
            out[pos++] = '/';
        }

        for (unsigned int j = 0; j < len; j++) {
            out[pos++] = comps[i][j];
        }
    }

    out[pos] = '\0';
    return 1;
}

static void shell_set_cwd_root(void) {
    shell_cwd[0] = '\0';
}


static int shell_enqueue_event(shell_event_type_t type, char c) {
    if (event_count >= SHELL_EVENT_QUEUE_SIZE) {
        return 0;
    }

    event_queue[event_head].type = type;
    event_queue[event_head].c = c;
    event_head = (event_head + 1) % SHELL_EVENT_QUEUE_SIZE;
    event_count++;
    return 1;
}

static int shell_dequeue_event(shell_event_t* ev) {
    if (event_count == 0) {
        return 0;
    }

    *ev = event_queue[event_tail];
    event_tail = (event_tail + 1) % SHELL_EVENT_QUEUE_SIZE;
    event_count--;
    return 1;
}

static int shell_prompt_length(void) {
    const char* cwd = shell_get_cwd();
    int len = 3; /* "/> " */
    if (cwd) {
        while (*cwd) {
            len++;
            cwd++;
        }
    }
    return len;
}

static void shell_put_prompt(void) {
    terminal_putc('/');
    const char* cwd = shell_get_cwd();
    if (cwd && cwd[0] != '\0') {
        terminal_puts(cwd);
    }
    terminal_puts("> ");
}

static void shell_render_current_line(void) {
    int start = prompt_col + prompt_len;
    for (int i = 0; start + i < 80; i++) {
        terminal_write_at(prompt_row, start + i, ' ');
    }

    for (int i = 0; i < editor.len && (start + i) < 80; i++) {
        terminal_write_at(prompt_row, start + i, editor.buf[i]);
    }

    terminal_set_cursor(prompt_row, start + editor.cursor);
}

static int shell_can_fast_echo_append(void) {
    int cursor_col = prompt_col + prompt_len + editor.cursor;
    return editor.cursor == editor.len && cursor_col >= 0 && cursor_col < 79;
}

static void shell_start_prompt(void) {
    prompt_len = shell_prompt_length();
    shell_put_prompt();
    prompt_row = terminal_get_row();
    prompt_col = terminal_get_col() - prompt_len;

    line_editor_clear(&editor);
    shell_completion_reset();

    history_index = -1;
    saved_current[0] = '\0';
}

const char* shell_get_cwd(void) {
    return shell_cwd;
}

int shell_resolve_path(const char* path, char* out, unsigned int out_size) {
    return shell_path_build(shell_cwd, path, out, out_size);
}

int shell_set_cwd(const char* path) {
    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(path, resolved, sizeof(resolved))) {
        return 0;
    }

    if (!vfs_is_dir(resolved)) {
        return 0;
    }

    if (resolved[0] == '\0') {
        shell_set_cwd_root();
        return 1;
    }

    k_strncpy(shell_cwd, resolved, sizeof(shell_cwd));
    shell_cwd[sizeof(shell_cwd) - 1] = '\0';
    return 1;
}

static void shell_history_add(const char* cmd) {
    if (k_strcmp(cmd, "")) {
        return;
    }

    if (history_count > 0 && k_strcmp(history[history_count - 1], cmd)) {
        return;
    }

    if (history_count < HISTORY_MAX) {
        k_strncpy(history[history_count], cmd, LINE_EDITOR_MAX);
        history_count++;
        return;
    }

    for (int i = 1; i < HISTORY_MAX; i++) {
        k_strncpy(history[i - 1], history[i], LINE_EDITOR_MAX);
    }

    k_strncpy(history[HISTORY_MAX - 1], cmd, LINE_EDITOR_MAX);
}

static void shell_set_editor_text(const char* s) {
    line_editor_clear(&editor);

    for (int i = 0; s[i] != '\0'; i++) {
        if (!line_editor_insert(&editor, s[i])) {
            break;
        }
    }

    line_editor_move_end(&editor);
    shell_render_current_line();
}

static void shell_insert_text(const char* s) {
    int changed = 0;

    for (int i = 0; s[i] != '\0'; i++) {
        if (!line_editor_insert(&editor, s[i])) {
            break;
        }
        changed = 1;
    }

    if (changed) {
        shell_render_current_line();
    }
}

static void shell_common_prefix(char* common, unsigned int common_size, const char* name) {
    unsigned int i = 0;

    if (!common || common_size == 0u || !name) {
        return;
    }
    (void)common_size;

    while (common[i] && name[i] && shell_char_equal_ci(common[i], name[i])) {
        i++;
    }
    common[i] = '\0';
}

static void shell_replace_token(int token_start, const char* text) {
    if (token_start < 0 || token_start > editor.cursor) {
        return;
    }

    while (editor.cursor > token_start) {
        line_editor_backspace(&editor);
    }

    shell_insert_text(text);
}

static int shell_command_match_at(const char* prefix,
                                  unsigned int start,
                                  char* out,
                                  unsigned int out_size,
                                  unsigned int* out_next_index) {
    unsigned int count = commands_count();

    for (unsigned int i = start; i < count; i++) {
        const char* name = commands_name_at(i);
        if (name && k_starts_with(name, prefix)) {
            k_strncpy(out, name, out_size);
            if (out_next_index) {
                *out_next_index = i + 1u;
            }
            return 1;
        }
    }

    return 0;
}

static void shell_complete_command(const char* prefix, int token_start) {
    char common[LINE_EDITOR_MAX];
    char match[LINE_EDITOR_MAX];
    unsigned int matches = 0;
    unsigned int count = commands_count();

    common[0] = '\0';
    for (unsigned int i = 0; i < count; i++) {
        const char* name = commands_name_at(i);
        if (!name || !k_starts_with(name, prefix)) {
            continue;
        }

        if (matches == 0) {
            k_strncpy(common, name, sizeof(common));
        } else {
            shell_common_prefix(common, sizeof(common), name);
        }
        matches++;
    }

    if (matches == 0) {
        return;
    }

    int prefix_len = k_strlen(prefix);
    if (matches == 1) {
        shell_insert_text(common + prefix_len);
        shell_insert_text(" ");
        return;
    }

    if (k_strlen(common) > prefix_len) {
        shell_insert_text(common + prefix_len);
        return;
    }

    if (!shell_command_match_at(prefix, 0, match, sizeof(match), &completion_next_index)) {
        return;
    }

    completion_kind = SHELL_COMPLETION_COMMAND;
    k_strncpy(completion_prefix, prefix, sizeof(completion_prefix));
    completion_dir[0] = '\0';
    completion_token_start = token_start;
    shell_replace_token(token_start, match);
}

static int shell_split_completion_path(const char* token,
                                       char* dir_input,
                                       unsigned int dir_input_size,
                                       const char** leaf_prefix) {
    const char* last_sep = 0;
    unsigned int dir_len;

    if (!token || !dir_input || dir_input_size == 0u || !leaf_prefix) {
        return 0;
    }

    for (const char* p = token; *p; p++) {
        if (path_is_sep(*p)) {
            last_sep = p;
        }
    }

    if (!last_sep) {
        dir_input[0] = '\0';
        *leaf_prefix = token;
        return 1;
    }

    if (last_sep == token) {
        dir_input[0] = '/';
        dir_input[1] = '\0';
        *leaf_prefix = last_sep + 1;
        return 1;
    }

    dir_len = (unsigned int)(last_sep - token);
    if (dir_len + 1u > dir_input_size) {
        return 0;
    }

    for (unsigned int i = 0; i < dir_len; i++) {
        dir_input[i] = token[i];
    }
    dir_input[dir_len] = '\0';
    *leaf_prefix = last_sep + 1;
    return 1;
}

static int shell_path_match_at(const char* dir,
                               const char* prefix,
                               unsigned int start,
                               char* out,
                               unsigned int out_size,
                               unsigned int* out_next_index) {
    char name[64];
    unsigned int size = 0;
    int is_dir = 0;

    for (unsigned int index = start; vfs_dirent_at(dir, index, name, sizeof(name), &size, &is_dir); index++) {
        (void)size;
        (void)is_dir;
        if (shell_starts_with_ci(name, prefix)) {
            k_strncpy(out, name, out_size);
            if (out_next_index) {
                *out_next_index = index + 1u;
            }
            return 1;
        }
    }

    return 0;
}

static void shell_complete_path(const char* token, int token_start) {
    char dir_input[SHELL_PATH_MAX];
    char dir[SHELL_PATH_MAX];
    char name[64];
    char common[64];
    char match[64];
    const char* leaf_prefix = 0;
    int leaf_start = token_start;
    unsigned int matches = 0;
    unsigned int size = 0;
    int is_dir = 0;

    if (!shell_split_completion_path(token, dir_input, sizeof(dir_input), &leaf_prefix)) {
        return;
    }
    leaf_start = token_start + (int)(leaf_prefix - token);
    if (!shell_resolve_path(dir_input, dir, sizeof(dir))) {
        return;
    }

    common[0] = '\0';
    for (unsigned int index = 0; vfs_dirent_at(dir, index, name, sizeof(name), &size, &is_dir); index++) {
        (void)size;
        (void)is_dir;
        if (!shell_starts_with_ci(name, leaf_prefix)) {
            continue;
        }

        if (matches == 0) {
            k_strncpy(common, name, sizeof(common));
        } else {
            shell_common_prefix(common, sizeof(common), name);
        }
        matches++;
    }

    if (matches == 0) {
        return;
    }

    int prefix_len = k_strlen(leaf_prefix);
    if (matches == 1) {
        shell_insert_text(common + prefix_len);
        if (common[k_strlen(common) - 1] != '/') {
            shell_insert_text(" ");
        }
        return;
    }

    if (k_strlen(common) > prefix_len) {
        shell_insert_text(common + prefix_len);
        return;
    }

    if (!shell_path_match_at(dir, leaf_prefix, 0, match, sizeof(match), &completion_next_index)) {
        return;
    }

    completion_kind = SHELL_COMPLETION_PATH;
    k_strncpy(completion_prefix, leaf_prefix, sizeof(completion_prefix));
    k_strncpy(completion_dir, dir, sizeof(completion_dir));
    completion_token_start = leaf_start;
    shell_replace_token(leaf_start, match);
}

static int shell_cycle_completion(void) {
    char match[LINE_EDITOR_MAX];
    unsigned int next_index = 0;

    if (completion_kind == SHELL_COMPLETION_COMMAND) {
        if (!shell_command_match_at(completion_prefix, completion_next_index,
                                    match, sizeof(match), &next_index)) {
            if (!shell_command_match_at(completion_prefix, 0,
                                        match, sizeof(match), &next_index)) {
                shell_completion_reset();
                return 0;
            }
        }
    } else if (completion_kind == SHELL_COMPLETION_PATH) {
        if (!shell_path_match_at(completion_dir, completion_prefix, completion_next_index,
                                 match, sizeof(match), &next_index)) {
            if (!shell_path_match_at(completion_dir, completion_prefix, 0,
                                     match, sizeof(match), &next_index)) {
                shell_completion_reset();
                return 0;
            }
        }
    } else {
        return 0;
    }

    completion_next_index = next_index;
    shell_replace_token(completion_token_start, match);
    return 1;
}

static void shell_complete(void) {
    int token_start = editor.cursor;
    int command_position = 1;
    char token[LINE_EDITOR_MAX];
    int token_len = 0;

    while (token_start > 0 && editor.buf[token_start - 1] != ' ') {
        token_start--;
    }

    for (int i = 0; i < token_start; i++) {
        if (editor.buf[i] != ' ') {
            command_position = 0;
            break;
        }
    }

    while (token_start + token_len < editor.cursor && token_len < LINE_EDITOR_MAX - 1) {
        token[token_len] = editor.buf[token_start + token_len];
        token_len++;
    }
    token[token_len] = '\0';

    if (completion_kind != SHELL_COMPLETION_NONE &&
        completion_token_start >= token_start &&
        completion_token_start <= editor.cursor &&
        shell_cycle_completion()) {
        return;
    }

    shell_completion_reset();

    if (command_position) {
        shell_complete_command(token, token_start);
    } else {
        shell_complete_path(token, token_start);
    }
}

static void shell_execute(const char* input) {
    char buf[LINE_EDITOR_MAX];
    int i = 0;

    while (input[i] != '\0' && i < LINE_EDITOR_MAX - 1) {
        buf[i] = input[i];
        i++;
    }
    buf[i] = '\0';

    command_t cmd = parse_command(buf);
    commands_execute(&cmd);
}

/* ------------------------------------------------------------------ */
/* Keyboard consumer                                                  */
/* ------------------------------------------------------------------ */

/*
 * shell_key_consumer — the shell's registered keyboard_consumer_fn.
 *
 * Called from keyboard_handle_irq() for every key-press event while
 * the shell holds the input consumer registration.  Translates key
 * events into shell_event_t entries and enqueues them for shell_poll()
 * to drain on the shell task's own stack, outside IRQ context.
 */
static void shell_key_consumer(key_event_t ev) {
    if (ev.ascii) {
        if (ev.ascii == '\b') {
            shell_enqueue_event(SHELL_EVENT_BACKSPACE, 0);
        } else if (ev.ascii == '\t') {
            shell_enqueue_event(SHELL_EVENT_COMPLETE, 0);
        } else {
            shell_enqueue_event(SHELL_EVENT_INPUT_CHAR, ev.ascii);
        }
        return;
    }

    switch (ev.key) {
        case KEY_LEFT:      shell_enqueue_event(SHELL_EVENT_MOVE_LEFT,    0); break;
        case KEY_RIGHT:     shell_enqueue_event(SHELL_EVENT_MOVE_RIGHT,   0); break;
        case KEY_HOME:      shell_enqueue_event(SHELL_EVENT_MOVE_HOME,    0); break;
        case KEY_END:       shell_enqueue_event(SHELL_EVENT_MOVE_END,     0); break;
        case KEY_DELETE:    shell_enqueue_event(SHELL_EVENT_DELETE,       0); break;
        case KEY_UP:        shell_enqueue_event(SHELL_EVENT_HISTORY_UP,   0); break;
        case KEY_DOWN:      shell_enqueue_event(SHELL_EVENT_HISTORY_DOWN, 0); break;
        default: break;
    }
}

void shell_register_consumer(void) {
    keyboard_set_consumer(shell_key_consumer);
}

void shell_init(void) {
    line_editor_init(&editor);
    history_count = 0;
    history_index = -1;
    event_head = 0;
    event_tail = 0;
    event_count = 0;
    shell_set_cwd_root();
    keyboard_set_consumer(shell_key_consumer);
    shell_start_prompt();
}

void shell_poll(void) {
    shell_event_t ev;

    while (shell_dequeue_event(&ev)) {
        switch (ev.type) {
            case SHELL_EVENT_INPUT_CHAR:
                shell_completion_reset();
                if (ev.c == '\n') {
                    terminal_set_cursor(prompt_row, prompt_col + prompt_len + editor.len);
                    terminal_putc('\n');

                    shell_history_add(editor.buf);
                    shell_execute(editor.buf);

                    shell_start_prompt();
                } else if (ev.c != '\t') {
                    int fast_echo = shell_can_fast_echo_append();
                    if (line_editor_insert(&editor, ev.c)) {
                        if (fast_echo) {
                            terminal_putc(ev.c);
                        } else {
                            shell_render_current_line();
                        }
                    }
                }
                break;

            case SHELL_EVENT_BACKSPACE:
                shell_completion_reset();
                if (line_editor_backspace(&editor)) {
                    shell_render_current_line();
                }
                break;

            case SHELL_EVENT_MOVE_LEFT:
                shell_completion_reset();
                line_editor_move_left(&editor);
                terminal_set_cursor(prompt_row, prompt_col + prompt_len + editor.cursor);
                break;

            case SHELL_EVENT_MOVE_RIGHT:
                shell_completion_reset();
                line_editor_move_right(&editor);
                terminal_set_cursor(prompt_row, prompt_col + prompt_len + editor.cursor);
                break;

            case SHELL_EVENT_MOVE_HOME:
                shell_completion_reset();
                line_editor_move_home(&editor);
                terminal_set_cursor(prompt_row, prompt_col + prompt_len + editor.cursor);
                break;

            case SHELL_EVENT_MOVE_END:
                shell_completion_reset();
                line_editor_move_end(&editor);
                terminal_set_cursor(prompt_row, prompt_col + prompt_len + editor.cursor);
                break;

            case SHELL_EVENT_DELETE:
                shell_completion_reset();
                if (line_editor_delete(&editor)) {
                    shell_render_current_line();
                }
                break;

            case SHELL_EVENT_HISTORY_UP:
                shell_completion_reset();
                if (history_count == 0) {
                    break;
                }

                if (history_index == -1) {
                    k_strncpy(saved_current, editor.buf, LINE_EDITOR_MAX);
                    history_index = history_count - 1;
                } else if (history_index > 0) {
                    history_index--;
                }

                shell_set_editor_text(history[history_index]);
                break;

            case SHELL_EVENT_HISTORY_DOWN:
                shell_completion_reset();
                if (history_index == -1) {
                    break;
                }

                if (history_index < history_count - 1) {
                    history_index++;
                    shell_set_editor_text(history[history_index]);
                } else {
                    history_index = -1;
                    shell_set_editor_text(saved_current);
                }
                break;

            case SHELL_EVENT_COMPLETE:
                shell_complete();
                break;

            default:
                break;
        }
    }
}

void shell_task_main(void) {
    shell_init();

    for (;;) {
        if (event_count == 0) {
            __asm__ __volatile__("sti; hlt");
        }
        shell_poll();
    }
}
