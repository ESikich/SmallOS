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
} shell_event_type_t;

typedef struct {
    shell_event_type_t type;
    char c;
} shell_event_t;

static shell_event_t event_queue[SHELL_EVENT_QUEUE_SIZE];
static volatile int event_head = 0;
static volatile int event_tail = 0;
static volatile int event_count = 0;

static int path_is_sep(char c) {
    return c == '/' || c == '\\';
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
                if (line_editor_backspace(&editor)) {
                    shell_render_current_line();
                }
                break;

            case SHELL_EVENT_MOVE_LEFT:
                line_editor_move_left(&editor);
                terminal_set_cursor(prompt_row, prompt_col + prompt_len + editor.cursor);
                break;

            case SHELL_EVENT_MOVE_RIGHT:
                line_editor_move_right(&editor);
                terminal_set_cursor(prompt_row, prompt_col + prompt_len + editor.cursor);
                break;

            case SHELL_EVENT_MOVE_HOME:
                line_editor_move_home(&editor);
                terminal_set_cursor(prompt_row, prompt_col + prompt_len + editor.cursor);
                break;

            case SHELL_EVENT_MOVE_END:
                line_editor_move_end(&editor);
                terminal_set_cursor(prompt_row, prompt_col + prompt_len + editor.cursor);
                break;

            case SHELL_EVENT_DELETE:
                if (line_editor_delete(&editor)) {
                    shell_render_current_line();
                }
                break;

            case SHELL_EVENT_HISTORY_UP:
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
