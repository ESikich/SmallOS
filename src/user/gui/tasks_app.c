#include "native_apps_internal.h"

/* ---------------- Tasks ---------------- */

#define TASKS_CONTROL_TERMINATE 1

typedef struct task_row {
    sys_procinfo_entry_t entry;
    unsigned int cpu_delta;
    unsigned int cpu_percent;
} task_row_t;

typedef struct tasks_state {
    sys_procinfo_t previous;
    task_row_t rows[TASK_MAX];
    unsigned int row_count;
    int have_previous;
    int selection;
    int confirm;
    unsigned int confirm_pid;
    int sort_column;
    int sort_descending;
    char status[80];
} tasks_state_t;

static int native_text_compare(const char* a, const char* b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return *a ? 1 : *b ? -1 : 0;
}

static int tasks_compare(const task_row_t* a, const task_row_t* b,
                         int column) {
    gui_task_model_row_t left = {
        a->entry.pid, a->entry.parent_pid, a->entry.state, a->cpu_delta,
        a->entry.ram_bytes, a->entry.heap_bytes, a->entry.name
    };
    gui_task_model_row_t right = {
        b->entry.pid, b->entry.parent_pid, b->entry.state, b->cpu_delta,
        b->entry.ram_bytes, b->entry.heap_bytes, b->entry.name
    };
    return gui_tasks_compare(&left, &right, column);
}

static void tasks_refresh(tasks_state_t* state) {
    sys_procinfo_t current;
    unsigned int elapsed;
    unsigned int selected_pid = 0;
    if (state->row_count && state->selection >= 0 &&
        state->selection < (int)state->row_count)
        selected_pid = state->rows[state->selection].entry.pid;
    if (sys_procinfo(&current) < 0) {
        copy_text(state->status, "Process information unavailable", sizeof(state->status));
        return;
    }
    elapsed = state->have_previous && current.total_ticks > state->previous.total_ticks
            ? current.total_ticks - state->previous.total_ticks : current.total_ticks;
    if (!elapsed) elapsed = 1;
    state->row_count = current.out_count;
    for (unsigned int i = 0; i < current.out_count; i++) {
        unsigned int old_cpu = 0;
        state->rows[i].entry = current.entries[i];
        if (state->have_previous) {
            for (unsigned int j = 0; j < state->previous.out_count; j++)
                if (state->previous.entries[j].pid == current.entries[i].pid)
                    old_cpu = state->previous.entries[j].cpu_ticks;
        }
        state->rows[i].cpu_delta = current.entries[i].cpu_ticks >= old_cpu
            ? current.entries[i].cpu_ticks - old_cpu : current.entries[i].cpu_ticks;
        state->rows[i].cpu_percent = state->rows[i].cpu_delta * 100u / elapsed;
    }
    for (unsigned int i = 0; i < state->row_count; i++)
        for (unsigned int j = i + 1; j < state->row_count; j++)
            if (tasks_compare(&state->rows[j], &state->rows[i],
                              state->sort_column) != 0 &&
                ((tasks_compare(&state->rows[j], &state->rows[i],
                                state->sort_column) > 0) ==
                 state->sort_descending)) {
                task_row_t swap = state->rows[i]; state->rows[i] = state->rows[j];
                state->rows[j] = swap;
            }
    state->previous = current;
    state->have_previous = 1;
    if (selected_pid) {
        for (unsigned int i = 0; i < state->row_count; i++)
            if (state->rows[i].entry.pid == selected_pid) {
                state->selection = (int)i;
                selected_pid = 0;
                break;
            }
    }
    if (selected_pid || state->selection >= (int)state->row_count)
        state->selection = state->row_count ? (int)state->row_count - 1 : 0;
}

static void tasks_open(gui_app_context_t* context, const char* argument) {
    tasks_state_t* state = gui_app_state(context);
    (void)argument;
    memset(state, 0, sizeof(*state));
    state->sort_column = 3;
    state->sort_descending = 1;
    tasks_refresh(state);
}

static int tasks_protected(const task_row_t* row) {
    gui_task_model_row_t model;
    if (!row) return 1;
    model.pid = row->entry.pid;
    model.parent_pid = row->entry.parent_pid;
    model.state = row->entry.state;
    model.cpu_delta = row->cpu_delta;
    model.ram_bytes = row->entry.ram_bytes;
    model.heap_bytes = row->entry.heap_bytes;
    model.name = row->entry.name;
    return gui_tasks_is_protected(&model, (unsigned int)sys_getpid());
}

static void tasks_draw(gfx_surface_t* s, gui_app_context_t* context,
                       int mx, int my) {
    tasks_state_t* state = gui_app_state(context);
    int bx = 0, by = 0;
    int width = (int)s->width;
    int bh = (int)s->height;
    int visible = (bh - 48) / TASK_ROW_H;
    (void)mx; (void)my;
    gui_canvas_fill_rect(s, bx, by, width, bh, g_ui.window_bg);
    {
        gui_table_column_t columns[] = {
            {"PID", 42}, {"NAME", 112}, {"STATE", 48},
            {"CPU", 42}, {"RAM", 54}, {"HEAP", 54}
        };
        gui_widget_table_header(s, gui_rect_make(bx + 2, by + 2,
                                width - 4, 14), columns, 6,
                                state->sort_column, state->sort_descending,
                                g_ui.widget_theme, g_ui.draw_text);
    }
    for (int i = 0; i < visible && i < (int)state->row_count; i++) {
        int y = by + 17 + i * TASK_ROW_H;
        task_row_t* row = &state->rows[i];
        if (i == state->selection)
            gui_canvas_fill_rect(s, bx + 2, y, width - 4, TASK_ROW_H,
                                 g_ui.highlight);
        unsigned int color = i == state->selection ? g_ui.highlight_text : g_ui.text;
        char n[16];
        uint_text(row->entry.pid, n);
        g_ui.draw_text(s, bx + 5, y + 3, n, color);
        g_ui.draw_text(s, bx + 45, y + 3, row->entry.name, color);
        uint_text(row->entry.state, n);
        g_ui.draw_text(s, bx + 157, y + 3, n, color);
        uint_text(row->cpu_percent, n);
        g_ui.draw_text(s, bx + 205, y + 3, n, color);
        uint_text(row->entry.ram_bytes / 1024u, n);
        g_ui.draw_text(s, bx + 247, y + 3, n, color);
        uint_text(row->entry.heap_bytes / 1024u, n);
        g_ui.draw_text(s, bx + 301, y + 3, n, color);
    }
    gui_widget_button(s, gui_rect_make(bx + 4, by + bh - 25, 76, 18), "Terminate",
                      (gui_widget_state_t){0,
                          gui_app_captured_control(context) == TASKS_CONTROL_TERMINATE,
                          gui_app_focused_control(context) == TASKS_CONTROL_TERMINATE,
                          state->row_count == 0 ||
                          tasks_protected(&state->rows[state->selection])},
                      g_ui.widget_theme, g_ui.draw_text);
    g_ui.draw_text(s, bx + 86, by + bh - 19, state->status, g_ui.subtext);
}

static void tasks_request_terminate(gui_app_context_t* context,
                                    tasks_state_t* state) {
    static const gui_dialog_button_t buttons[] = {
        {1u, "Terminate", 1, 0}, {2u, "Cancel", 0, 1}
    };
    gui_dialog_request_t request;
    if (!state->row_count ||
        tasks_protected(&state->rows[state->selection])) return;
    state->confirm_pid = state->rows[state->selection].entry.pid;
    request.request_id = 1u;
    request.title = "Terminate Process";
    request.message = "Send SIGTERM to the selected process?";
    request.initial_text = 0;
    request.buttons = buttons;
    request.button_count = 2;
    if (!gui_app_open_dialog(context, &request))
        copy_text(state->status, "Another dialog is already open",
                  sizeof(state->status));
}

static unsigned int tasks_event(gui_app_context_t* context,
                                const gui_app_event_t* event) {
    tasks_state_t* state = gui_app_state(context);
    int width = 0, bh = 0;
    gui_app_client_size(context, &width, &bh);
    if (event->type == GUI_APP_EVENT_TICK) {
        tasks_refresh(state);
        gui_app_invalidate(context, 0, 0, width, bh);
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_DIALOG_RESULT &&
        event->request_id == 1u) {
        if (event->button_id == 1u) {
            if (sys_kill((int)state->confirm_pid, SIGTERM) < 0)
                copy_text(state->status, "Terminate failed", sizeof(state->status));
            else
                copy_text(state->status, "SIGTERM sent", sizeof(state->status));
        }
        state->confirm_pid = 0;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN) {
        if (event->y >= 2 && event->y < 16) {
            static const int edges[] = {42, 154, 202, 244, 298, 352};
            int column = 5;
            for (int i = 0; i < 6; i++)
                if (event->x - 2 < edges[i]) { column = i; break; }
            if (state->sort_column == column)
                state->sort_descending = !state->sort_descending;
            else {
                state->sort_column = column;
                state->sort_descending = column != 0 && column != 1;
            }
            tasks_refresh(state);
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        if (event->y >= 17 && event->y < bh - 30) {
            int row = (event->y - 17) / TASK_ROW_H;
            if (row >= 0 && row < (int)state->row_count) state->selection = row;
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        if (inside(event->x, event->y, gui_rect_make(4, bh - 25, 76, 18)) &&
            state->row_count && !tasks_protected(&state->rows[state->selection])) {
            gui_app_focus_control(context, TASKS_CONTROL_TERMINATE);
            gui_app_capture_pointer(context, TASKS_CONTROL_TERMINATE);
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP &&
        gui_app_captured_control(context) == TASKS_CONTROL_TERMINATE) {
        gui_app_release_pointer(context);
        if (inside(event->x, event->y, gui_rect_make(4, bh - 25, 76, 18)) &&
            state->row_count && !tasks_protected(&state->rows[state->selection]))
            tasks_request_terminate(context, state);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_KEY) {
        if (event->key == KEY_TAB) gui_app_focus_control(context, TASKS_CONTROL_TERMINATE);
        else if (event->key == KEY_UP && state->selection > 0) state->selection--;
        else if (event->key == KEY_DOWN && state->selection + 1 < (int)state->row_count)
            state->selection++;
        else if ((event->key == KEY_ENTER || event->key == KEY_DELETE) &&
                 state->row_count && !tasks_protected(&state->rows[state->selection]))
            tasks_request_terminate(context, state);
        else return GUI_APP_RESULT_NONE;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    return GUI_APP_RESULT_NONE;
}

static const gui_app_descriptor_t TASKS_DESCRIPTOR = {
    "Tasks", sizeof(tasks_state_t), 520, 360, 360, 220, SMALLOS_TIMER_HZ,
    tasks_open, 0, tasks_draw, tasks_event, GUI_APP_TASKS, 0,
    "tasks", "Tasks", 4, 1
};

const gui_app_descriptor_t* gui_tasks_app_descriptor(void) {
    return &TASKS_DESCRIPTOR;
}
