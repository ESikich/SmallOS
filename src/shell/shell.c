#include "shell.h"
#include "terminal.h"
#include "line_editor.h"
#include "system.h"
#include "timer.h"
#include "memory.h"
#include "commands.h"
#include "parse.h"

#define HISTORY_MAX 8
#define SHELL_EVENT_QUEUE_SIZE 64

static line_editor_t editor;

static int prompt_row = 0;
static int prompt_col = 0;

static char history[HISTORY_MAX][LINE_EDITOR_MAX];
static int history_count = 0;
static int history_index = -1;
static char saved_current[LINE_EDITOR_MAX];

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
static int event_head = 0;
static int event_tail = 0;
static int event_count = 0;

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int str_starts_with(const char* s, const char* prefix) {
    int i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static void str_copy(char* dst, const char* src, int max_len) {
    int i = 0;
    while (i < max_len - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
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

static void shell_render_current_line(void) {
    for (int i = 0; i < 78; i++) {
        terminal_write_at(prompt_row, prompt_col + 2 + i, ' ');
    }

    for (int i = 0; i < editor.len && (prompt_col + 2 + i) < 80; i++) {
        terminal_write_at(prompt_row, prompt_col + 2 + i, editor.buf[i]);
    }

    terminal_set_cursor(prompt_row, prompt_col + 2 + editor.cursor);
}

static void shell_start_prompt(void) {
    terminal_puts("> ");
    prompt_row = terminal_get_row();
    prompt_col = terminal_get_col() - 2;

    line_editor_clear(&editor);

    history_index = -1;
    saved_current[0] = '\0';
}

static void shell_history_add(const char* cmd) {
    if (str_eq(cmd, "")) {
        return;
    }

    if (history_count > 0 && str_eq(history[history_count - 1], cmd)) {
        return;
    }

    if (history_count < HISTORY_MAX) {
        str_copy(history[history_count], cmd, LINE_EDITOR_MAX);
        history_count++;
        return;
    }

    for (int i = 1; i < HISTORY_MAX; i++) {
        str_copy(history[i - 1], history[i], LINE_EDITOR_MAX);
    }

    str_copy(history[HISTORY_MAX - 1], cmd, LINE_EDITOR_MAX);
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
    command_t cmd = parse_command(input);
    commands_execute(&cmd);
}

void shell_init(void) {
    line_editor_init(&editor);
    history_count = 0;
    history_index = -1;
    event_head = 0;
    event_tail = 0;
    event_count = 0;
    shell_start_prompt();
}

void shell_poll(void) {
    shell_event_t ev;

    while (shell_dequeue_event(&ev)) {
        switch (ev.type) {
            case SHELL_EVENT_INPUT_CHAR:
                if (ev.c == '\n') {
                    terminal_set_cursor(prompt_row, prompt_col + 2 + editor.len);
                    terminal_putc('\n');

                    shell_history_add(editor.buf);
                    shell_execute(editor.buf);

                    shell_start_prompt();
                } else if (ev.c != '\t') {
                    if (line_editor_insert(&editor, ev.c)) {
                        shell_render_current_line();
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
                terminal_set_cursor(prompt_row, prompt_col + 2 + editor.cursor);
                break;

            case SHELL_EVENT_MOVE_RIGHT:
                line_editor_move_right(&editor);
                terminal_set_cursor(prompt_row, prompt_col + 2 + editor.cursor);
                break;

            case SHELL_EVENT_MOVE_HOME:
                line_editor_move_home(&editor);
                terminal_set_cursor(prompt_row, prompt_col + 2 + editor.cursor);
                break;

            case SHELL_EVENT_MOVE_END:
                line_editor_move_end(&editor);
                terminal_set_cursor(prompt_row, prompt_col + 2 + editor.cursor);
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
                    str_copy(saved_current, editor.buf, LINE_EDITOR_MAX);
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
        shell_poll();
    }
}

void shell_input_char(char c) {
    shell_enqueue_event(SHELL_EVENT_INPUT_CHAR, c);
}

void shell_backspace(void) {
    shell_enqueue_event(SHELL_EVENT_BACKSPACE, 0);
}

void shell_delete(void) {
    shell_enqueue_event(SHELL_EVENT_DELETE, 0);
}

void shell_move_left(void) {
    shell_enqueue_event(SHELL_EVENT_MOVE_LEFT, 0);
}

void shell_move_right(void) {
    shell_enqueue_event(SHELL_EVENT_MOVE_RIGHT, 0);
}

void shell_move_home(void) {
    shell_enqueue_event(SHELL_EVENT_MOVE_HOME, 0);
}

void shell_move_end(void) {
    shell_enqueue_event(SHELL_EVENT_MOVE_END, 0);
}

void shell_history_up(void) {
    shell_enqueue_event(SHELL_EVENT_HISTORY_UP, 0);
}

void shell_history_down(void) {
    shell_enqueue_event(SHELL_EVENT_HISTORY_DOWN, 0);
}