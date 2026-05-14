#ifndef SMALLOS_GUI_SHELL_WINDOW_H
#define SMALLOS_GUI_SHELL_WINDOW_H

#define GUI_SHELL_COLS  78
#define GUI_SHELL_LINES 64
#define GUI_SHELL_INPUT 256
#define GUI_SHELL_HISTORY 16
#define GUI_SHELL_CSI_ARGS 4

typedef enum {
    GUI_SHELL_BACKEND_EMBEDDED = 1,
    GUI_SHELL_BACKEND_PIPE_CHILD,
    GUI_SHELL_BACKEND_PTY_CHILD,
} gui_shell_backend_t;

typedef enum {
    GUI_SHELL_KEY_OK = 0,
    GUI_SHELL_KEY_CLOSE,
} gui_shell_key_result_t;

typedef struct {
    gui_shell_backend_t backend;
    int pid;
    int stdin_fd;
    int stdout_fd;
    char cwd[256];
    char lines[GUI_SHELL_LINES][GUI_SHELL_COLS + 1];
    int line_count;
    int term_rows;
    int term_cols;
    int cursor_row;
    int cursor_col;
    char pending[GUI_SHELL_COLS + 1];
    int pending_len;
    int pending_cursor;
    int esc_state;
    int csi_args[GUI_SHELL_CSI_ARGS];
    int csi_count;
    int csi_value;
    int csi_has_value;
    int csi_private;
    unsigned char utf8_buf[4];
    int utf8_len;
    int utf8_need;
    char input[GUI_SHELL_INPUT];
    int input_len;
    char history[GUI_SHELL_HISTORY][GUI_SHELL_INPUT];
    unsigned int history_count;
    unsigned int history_view;
    char saved_input[GUI_SHELL_INPUT];
    int scroll;
} gui_shell_window_t;

void gui_shell_open(gui_shell_window_t* shell);
void gui_shell_close(gui_shell_window_t* shell);
int gui_shell_poll(gui_shell_window_t* shell);
void gui_shell_set_terminal_size(gui_shell_window_t* shell,
                                 unsigned int rows,
                                 unsigned int cols);
gui_shell_key_result_t gui_shell_handle_key(gui_shell_window_t* shell,
                                            unsigned int ascii,
                                            unsigned int key,
                                            unsigned int flags);
void gui_shell_format_prompt(gui_shell_window_t* shell,
                             char* out,
                             unsigned int cap);

#endif
