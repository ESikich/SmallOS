#include "modal_manager.h"

#include "file_picker.h"
#include "keyboard.h"
#include "smallos_input.h"
#include "theme.h"
#include "user_lib.h"
#include "widgets.h"

#define GUI_MODAL_TASKBAR_HEIGHT 24

typedef struct gui_modal_button_runtime {
    unsigned int id;
    char label[32];
    int is_default;
    int is_cancel;
} gui_modal_button_runtime_t;

typedef struct gui_modal_runtime {
    int screen_width;
    int screen_height;
    gui_modal_services_t services;
    int kind;
    gui_window_t* owner;
    gui_file_picker_t picker;
    gui_file_filter_t picker_filter;
    unsigned int request_id;
    char title[64];
    char message[160];
    int has_text;
    gui_text_input_t input;
    gui_modal_button_runtime_t buttons[GUI_DIALOG_BUTTON_CAPACITY];
    unsigned int button_count;
    int focus;
    int pressed;
} gui_modal_runtime_t;

static gui_modal_runtime_t g_modal;

static const gui_file_picker_style_t PICKER_STYLE = {
    COL_WIN_BG, COL_FRAME, COL_TEXT, COL_SUBTEXT,
    COL_HILIGHT, COL_HILIGHT_T
};

static unsigned int string_length(const char* text) {
    unsigned int length = 0;
    while (text && text[length]) length++;
    return length;
}

static void string_copy(char* destination, const char* source,
                        unsigned int capacity) {
    unsigned int index = 0;
    if (!source) source = "";
    while (index + 1 < capacity && source[index]) {
        destination[index] = source[index];
        index++;
    }
    if (capacity) destination[index] = 0;
}

static int string_equal(const char* left, const char* right) {
    while (*left && *left == *right) { left++; right++; }
    return *left == *right;
}

static int ends_with(const char* path, const char* suffix) {
    unsigned int path_length = string_length(path);
    unsigned int suffix_length = string_length(suffix);
    return suffix_length <= path_length &&
           string_equal(path + path_length - suffix_length, suffix);
}

static int text_path(const char* path) {
    return ends_with(path, ".txt") || ends_with(path, ".TXT") ||
           ends_with(path, ".c") || ends_with(path, ".h") ||
           ends_with(path, ".md") || ends_with(path, ".ini") ||
           ends_with(path, ".log") || ends_with(path, ".html");
}

static void invalidate(void) {
    if (g_modal.services.invalidate)
        g_modal.services.invalidate(g_modal.services.opaque);
}

static gui_rect_t picker_bounds(void) {
    int width = g_modal.screen_width - 80;
    int height = g_modal.screen_height - GUI_MODAL_TASKBAR_HEIGHT - 80;
    if (width > 560) width = 560;
    if (height > 400) height = 400;
    if (width < 280) width = 280;
    if (height < 180) height = 180;
    return gui_rect_make((g_modal.screen_width - width) / 2,
                         (g_modal.screen_height - GUI_MODAL_TASKBAR_HEIGHT -
                          height) / 2,
                         width, height);
}

static gui_rect_t dialog_bounds(void) {
    int width = g_modal.screen_width - 48;
    int height = g_modal.has_text ? 116 : 94;
    if (width > 380) width = 380;
    if (width < 240) width = 240;
    return gui_rect_make((g_modal.screen_width - width) / 2,
                         (g_modal.screen_height - GUI_MODAL_TASKBAR_HEIGHT -
                          height) / 2,
                         width, height);
}

static gui_rect_t text_bounds(void) {
    gui_rect_t bounds = dialog_bounds();
    return gui_rect_make(bounds.x + 10, bounds.y + 45,
                         bounds.w - 20, 18);
}

static gui_rect_t button_bounds(unsigned int index) {
    gui_rect_t bounds = dialog_bounds();
    int gap = 8;
    int width = (bounds.w - 20 -
                 ((int)g_modal.button_count - 1) * gap) /
                (int)g_modal.button_count;
    int total;
    if (width > 104) width = 104;
    total = width * (int)g_modal.button_count +
            gap * ((int)g_modal.button_count - 1);
    return gui_rect_make(bounds.x + (bounds.w - total) / 2 +
                         (int)index * (width + gap),
                         bounds.y + bounds.h - 29, width, 20);
}

void gui_modal_init(int screen_width, int screen_height,
                    const gui_modal_services_t* services) {
    memset(&g_modal, 0, sizeof(g_modal));
    g_modal.screen_width = screen_width;
    g_modal.screen_height = screen_height;
    if (services) g_modal.services = *services;
}

void gui_modal_resize(int screen_width, int screen_height) {
    g_modal.screen_width = screen_width;
    g_modal.screen_height = screen_height;
}

int gui_modal_active(void) { return g_modal.kind != 0; }

int gui_modal_open_picker(gui_window_t* owner,
                          gui_file_request_mode_t mode,
                          gui_file_filter_t filter,
                          const char* initial_path) {
    if (!owner || gui_modal_active()) return 0;
    gui_file_picker_init(&g_modal.picker,
        mode == GUI_FILE_REQUEST_SAVE ? GUI_FILE_PICKER_SAVE
                                      : GUI_FILE_PICKER_OPEN,
        initial_path && initial_path[0] ? initial_path : "/");
    g_modal.kind = 1;
    g_modal.owner = owner;
    g_modal.picker_filter = filter;
    invalidate();
    return 1;
}

int gui_modal_open_dialog(gui_window_t* owner,
                          const gui_dialog_request_t* request) {
    unsigned int default_index = 0;
    if (!owner || !request || !request->buttons ||
        request->button_count == 0 ||
        request->button_count > GUI_DIALOG_BUTTON_CAPACITY ||
        gui_modal_active()) return 0;
    g_modal.kind = 2;
    g_modal.owner = owner;
    g_modal.request_id = request->request_id;
    string_copy(g_modal.title, request->title, sizeof(g_modal.title));
    string_copy(g_modal.message, request->message, sizeof(g_modal.message));
    g_modal.has_text = request->initial_text != 0;
    if (g_modal.has_text)
        gui_text_input_init(&g_modal.input, request->initial_text);
    g_modal.button_count = request->button_count;
    g_modal.pressed = 0;
    for (unsigned int i = 0; i < request->button_count; i++) {
        g_modal.buttons[i].id = request->buttons[i].id;
        g_modal.buttons[i].is_default = request->buttons[i].is_default;
        g_modal.buttons[i].is_cancel = request->buttons[i].is_cancel;
        string_copy(g_modal.buttons[i].label, request->buttons[i].label,
                    sizeof(g_modal.buttons[i].label));
        if (request->buttons[i].is_default) default_index = i;
    }
    g_modal.focus = g_modal.has_text ? -1 : (int)default_index;
    invalidate();
    return 1;
}

void gui_modal_dismiss_owner(gui_window_t* owner) {
    if (gui_modal_active() && g_modal.owner == owner) {
        g_modal.kind = 0;
        g_modal.owner = 0;
        invalidate();
    }
}

static void dispatch_completion(gui_app_event_t* completion) {
    gui_window_t* owner = g_modal.owner;
    g_modal.kind = 0;
    g_modal.owner = 0;
    if ((!g_modal.services.owner_active ||
         g_modal.services.owner_active(owner, g_modal.services.opaque)) &&
        g_modal.services.dispatch)
        g_modal.services.dispatch(owner, completion, g_modal.services.opaque);
    invalidate();
}

static void finish_dialog(unsigned int button_id,
                          const gui_app_event_t* source) {
    gui_app_event_t completion;
    memset(&completion, 0, sizeof(completion));
    completion.type = GUI_APP_EVENT_DIALOG_RESULT;
    completion.request_id = g_modal.request_id;
    completion.button_id = button_id;
    completion.text = g_modal.has_text ? g_modal.input.text : 0;
    completion.ticks = source ? source->ticks : 0;
    dispatch_completion(&completion);
}

static int edit_key(const gui_app_event_t* event) {
    if (event->key == KEY_LEFT)
        return gui_text_input_command(&g_modal.input, GUI_TEXT_INPUT_LEFT);
    if (event->key == KEY_RIGHT)
        return gui_text_input_command(&g_modal.input, GUI_TEXT_INPUT_RIGHT);
    if (event->key == KEY_HOME)
        return gui_text_input_command(&g_modal.input, GUI_TEXT_INPUT_HOME);
    if (event->key == KEY_END)
        return gui_text_input_command(&g_modal.input, GUI_TEXT_INPUT_END);
    if (event->key == KEY_BACKSPACE)
        return gui_text_input_command(&g_modal.input, GUI_TEXT_INPUT_BACKSPACE);
    if (event->key == KEY_DELETE)
        return gui_text_input_command(&g_modal.input, GUI_TEXT_INPUT_DELETE);
    if ((event->ascii & 0xffu) >= 32u)
        return gui_text_input_insert(&g_modal.input,
                                     (char)(event->ascii & 0xffu));
    return 0;
}

static int dialog_event(const gui_app_event_t* event) {
    if (event->type == GUI_APP_EVENT_KEY) {
        if (event->key == KEY_ESC) {
            for (unsigned int i = 0; i < g_modal.button_count; i++)
                if (g_modal.buttons[i].is_cancel) {
                    finish_dialog(g_modal.buttons[i].id, event);
                    return 1;
                }
        } else if (event->key == KEY_TAB) {
            int reverse = (event->modifiers & SYS_INPUT_KEY_SHIFT) != 0;
            if (g_modal.has_text && g_modal.focus < 0)
                g_modal.focus = reverse ? (int)g_modal.button_count - 1 : 0;
            else if (g_modal.has_text &&
                     ((reverse && g_modal.focus == 0) ||
                      (!reverse && g_modal.focus + 1 >=
                       (int)g_modal.button_count)))
                g_modal.focus = -1;
            else
                g_modal.focus = gui_widget_focus_next(
                    g_modal.focus, (int)g_modal.button_count, reverse, 0);
        } else if (event->key == KEY_ENTER ||
                   (event->key == KEY_SPACE && g_modal.focus >= 0)) {
            int selected = g_modal.focus;
            if (selected < 0) {
                for (unsigned int i = 0; i < g_modal.button_count; i++)
                    if (g_modal.buttons[i].is_default) selected = (int)i;
            }
            if (selected >= 0 && selected < (int)g_modal.button_count) {
                finish_dialog(g_modal.buttons[selected].id, event);
                return 1;
            }
        } else if (g_modal.has_text && g_modal.focus < 0) {
            (void)edit_key(event);
        }
        invalidate();
        return 1;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN) {
        if (g_modal.has_text && gui_widget_hit(text_bounds(),
                                               event->x, event->y)) {
            g_modal.focus = -1;
            g_modal.pressed = 0;
        } else {
            for (unsigned int i = 0; i < g_modal.button_count; i++)
                if (gui_widget_hit(button_bounds(i), event->x, event->y)) {
                    g_modal.focus = (int)i;
                    g_modal.pressed = (int)i + 1;
                    break;
                }
        }
        invalidate();
        return 1;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP) {
        int pressed = g_modal.pressed - 1;
        g_modal.pressed = 0;
        if (pressed >= 0 && pressed < (int)g_modal.button_count &&
            gui_widget_hit(button_bounds((unsigned int)pressed),
                           event->x, event->y))
            finish_dialog(g_modal.buttons[pressed].id, event);
        else
            invalidate();
    }
    return 1;
}

static int picker_path_allowed(const char* path) {
    if (g_modal.picker_filter == GUI_FILE_FILTER_ANY) return 1;
    if (g_modal.picker_filter == GUI_FILE_FILTER_BMP)
        return ends_with(path, ".bmp") || ends_with(path, ".BMP");
    return text_path(path);
}

static int picker_event(const gui_app_event_t* event) {
    gui_file_picker_result_t result = gui_file_picker_event(
        &g_modal.picker, event, picker_bounds());
    if (result == GUI_FILE_PICKER_RESULT_ACCEPT &&
        !picker_path_allowed(gui_file_picker_path(&g_modal.picker))) {
        if (g_modal.services.notice)
            g_modal.services.notice(
                g_modal.picker_filter == GUI_FILE_FILTER_BMP
                ? "Choose a BMP file" : "Choose a text file",
                g_modal.services.opaque);
        invalidate();
        return 1;
    }
    if (result == GUI_FILE_PICKER_RESULT_ACCEPT ||
        result == GUI_FILE_PICKER_RESULT_CANCEL) {
        gui_app_event_t completion;
        memset(&completion, 0, sizeof(completion));
        completion.type = result == GUI_FILE_PICKER_RESULT_ACCEPT
                        ? GUI_APP_EVENT_FILE_SELECTED
                        : GUI_APP_EVENT_FILE_CANCELLED;
        completion.path = result == GUI_FILE_PICKER_RESULT_ACCEPT
                        ? gui_file_picker_path(&g_modal.picker) : 0;
        completion.file_filter = (unsigned int)g_modal.picker_filter;
        completion.ticks = event->ticks;
        dispatch_completion(&completion);
    } else if (result == GUI_FILE_PICKER_RESULT_REDRAW) {
        invalidate();
    }
    return 1;
}

int gui_modal_event(const gui_app_event_t* event) {
    if (!gui_modal_active() || !event) return 0;
    return g_modal.kind == 1 ? picker_event(event) : dialog_event(event);
}

void gui_modal_draw(gfx_surface_t* surface) {
    if (g_modal.kind == 1) {
        gui_file_picker_draw(surface, &g_modal.picker, picker_bounds(),
                             &gui_retro_widget_theme, &PICKER_STYLE,
                             gui_theme_draw_text);
    } else if (g_modal.kind == 2) {
        gui_rect_t bounds = dialog_bounds();
        gui_widget_modal(surface, bounds, g_modal.title, g_modal.message,
                         &gui_retro_widget_theme, gui_theme_draw_text);
        if (g_modal.has_text)
            gui_widget_text_field(surface, text_bounds(), g_modal.input.text,
                g_modal.input.cursor,
                (gui_widget_state_t){0, 0, g_modal.focus < 0, 0},
                &gui_retro_widget_theme, gui_theme_draw_text);
        for (unsigned int i = 0; i < g_modal.button_count; i++)
            gui_widget_button(surface, button_bounds(i),
                g_modal.buttons[i].label,
                (gui_widget_state_t){0, g_modal.pressed == (int)i + 1,
                                     g_modal.focus == (int)i, 0},
                &gui_retro_widget_theme, gui_theme_draw_text);
    }
}

gui_rect_t gui_modal_bounds(void) {
    if (g_modal.kind == 1) return picker_bounds();
    if (g_modal.kind == 2) return dialog_bounds();
    return gui_rect_make(0, 0, 0, 0);
}
