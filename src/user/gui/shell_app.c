#include "shell_app.h"

#include "app_services.h"
#include "canvas.h"
#include "keyboard.h"
#include "shell_window.h"

#define SHELL_LINE_HEIGHT 8
#define SHELL_PADDING 4

static unsigned int shell_text_width(const char* text) {
    unsigned int length = 0;
    while (text && text[length]) length++;
    return length ? length * 6u - 1u : 0u;
}

static void shell_draw_fixed(gfx_surface_t* surface, int x, int y,
                             const char* text, int columns,
                             unsigned int color) {
    char clipped[GUI_SHELL_COLS + 1];
    int count = 0;
    while (count < columns && count < GUI_SHELL_COLS && text[count]) {
        clipped[count] = text[count];
        count++;
    }
    clipped[count] = 0;
    gui_app_services_draw_text(surface, x, y, clipped, color);
}

static void shell_draw(gfx_surface_t* surface, gui_app_context_t* context,
                       int mouse_x, int mouse_y) {
    gui_shell_window_t* shell = gui_app_state(context);
    int width = (int)surface->width;
    int height = (int)surface->height;
    int pty_mode = shell->backend == GUI_SHELL_BACKEND_PTY_CHILD;
    int input_height = pty_mode ? 0 : SHELL_LINE_HEIGHT + 2;
    int visible = (height - input_height - SHELL_PADDING * 2) /
                  SHELL_LINE_HEIGHT;
    int columns = (width - 8) / 6;
    int start;
    int y;
    (void)mouse_x;
    (void)mouse_y;
    if (visible < 1) visible = 1;
    if (columns < 1) columns = 1;
    if (columns > GUI_SHELL_COLS) columns = GUI_SHELL_COLS;
    gui_canvas_fill_rect(surface, 0, 0, width, height, 0x00000000u);
    gui_shell_set_terminal_size(shell, (unsigned int)visible,
                                (unsigned int)columns);
    start = shell->line_count - visible - shell->scroll;
    if (start < 0) start = 0;
    y = SHELL_PADDING;
    for (int i = 0; i < visible && start + i < shell->line_count; i++) {
        shell_draw_fixed(surface, 4, y, shell->lines[start + i], columns,
                         0x00C8C8C8u);
        y += SHELL_LINE_HEIGHT;
    }
    if (pty_mode) {
        int cursor_row = shell->cursor_row - start;
        int cursor_column = shell->cursor_col;
        if (cursor_row >= 0 && cursor_row < visible) {
            if (cursor_column < 0) cursor_column = 0;
            if (cursor_column > columns) cursor_column = columns;
            gui_canvas_fill_rect(surface, 4 + cursor_column * 6,
                SHELL_PADDING + cursor_row * SHELL_LINE_HEIGHT,
                5, 7, 0x00FFFFFFu);
        }
        return;
    }
    {
        char prompt[GUI_SHELL_COLS + 1];
        int input_y = height - input_height - 2;
        int cursor_chars;
        char cursor_prefix[GUI_SHELL_COLS + 1];
        gui_canvas_hline(surface, 0, input_y - 2, width, 0x00404040u);
        gui_shell_format_prompt(shell, prompt, sizeof(prompt));
        gui_app_services_draw_text(surface, 4, input_y, prompt, 0x00FFFFFFu);
        cursor_chars = (int)sizeof(prompt);
        for (int i = 0; i < (int)sizeof(prompt); i++)
            if (!prompt[i]) { cursor_chars = i; break; }
        if (cursor_chars > GUI_SHELL_COLS) cursor_chars = GUI_SHELL_COLS;
        for (int i = 0; i < cursor_chars; i++) cursor_prefix[i] = prompt[i];
        cursor_prefix[cursor_chars] = 0;
        gui_canvas_fill_rect(surface,
            5 + (int)shell_text_width(cursor_prefix), input_y,
            5, 7, 0x00FFFFFFu);
    }
}

static void shell_open(gui_app_context_t* context, const char* argument) {
    (void)argument;
    gui_shell_open(gui_app_state(context));
}

static void shell_close(gui_app_context_t* context) {
    gui_shell_window_t* shell = gui_app_state(context);
    if (shell) gui_shell_close(shell);
}

static unsigned int shell_event(gui_app_context_t* context,
                                const gui_app_event_t* event) {
    gui_shell_window_t* shell = gui_app_state(context);
    if (event->type == GUI_APP_EVENT_KEY) {
        if (event->key == KEY_ESC || (event->ascii & 0xffu) == 27u)
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
        if (gui_shell_handle_key(shell, event->ascii & 0xffu, event->key,
                                 event->modifiers) == GUI_SHELL_KEY_CLOSE)
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_WHEEL) {
        int maximum = shell->line_count - 1;
        if (maximum < 0) maximum = 0;
        shell->scroll += event->wheel * 3;
        if (shell->scroll < 0) shell->scroll = 0;
        if (shell->scroll > maximum) shell->scroll = maximum;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_FD_READY ||
        event->type == GUI_APP_EVENT_TICK)
        return gui_shell_poll(shell) ? GUI_APP_RESULT_REDRAW
                                     : GUI_APP_RESULT_NONE;
    return GUI_APP_RESULT_NONE;
}

static int shell_wait_fd(gui_app_context_t* context) {
    return gui_shell_poll_fd(gui_app_state(context));
}

static const gui_app_descriptor_t DESCRIPTOR = {
    "Shell", sizeof(gui_shell_window_t), 500, 320, 240, 120, 0,
    shell_open, shell_close, shell_draw, shell_event, GUI_APP_SHELL, 1,
    "shell", "Shell", 2, 1, shell_wait_fd
};

const gui_app_descriptor_t* gui_shell_app_descriptor(void) {
    return &DESCRIPTOR;
}
