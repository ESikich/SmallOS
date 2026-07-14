#include "widgets.h"

int gui_widget_hit(gui_rect_t b, int x, int y) {
    return b.w > 0 && b.h > 0 && x >= b.x && y >= b.y &&
           x < b.x + b.w && y < b.y + b.h;
}

void gui_widget_label(gfx_surface_t* s, gui_rect_t b, const char* text,
                      unsigned int color, gui_widget_text_fn draw_text) {
    if (draw_text && text) draw_text(s, b.x, b.y, text, color);
}

void gui_widget_button(gfx_surface_t* s, gui_rect_t b, const char* text,
                       gui_widget_state_t state,
                       const gui_widget_theme_t* theme,
                       gui_widget_text_fn draw_text) {
    unsigned int face;
    if (!s || !theme) return;
    face = state.disabled ? theme->disabled :
           state.pressed ? theme->face_pressed :
           state.hovered ? theme->face_hover : theme->face;
    gui_canvas_fill_rect(s, b.x, b.y, b.w, b.h, face);
    gui_canvas_rect(s, b.x, b.y, b.w, b.h,
                    state.focused ? theme->accent : theme->frame);
    if (draw_text && text)
        draw_text(s, b.x + 4, b.y + (b.h - 7) / 2, text, theme->text);
}

void gui_widget_checkbox(gfx_surface_t* s, gui_rect_t b, const char* text,
                         int checked, gui_widget_state_t state,
                         const gui_widget_theme_t* theme,
                         gui_widget_text_fn draw_text) {
    gui_rect_t box = gui_rect_make(b.x, b.y + (b.h - 11) / 2, 11, 11);
    if (!s || !theme) return;
    gui_canvas_fill_rect(s, box.x, box.y, box.w, box.h,
                         state.disabled ? theme->disabled : theme->face);
    gui_canvas_rect(s, box.x, box.y, box.w, box.h,
                    state.focused ? theme->accent : theme->frame);
    if (checked) {
        gui_canvas_fill_rect(s, box.x + 3, box.y + 5, 2, 2, theme->accent);
        gui_canvas_fill_rect(s, box.x + 5, box.y + 3, 2, 4, theme->accent);
        gui_canvas_fill_rect(s, box.x + 7, box.y + 2, 2, 2, theme->accent);
    }
    if (draw_text && text)
        draw_text(s, b.x + 15, b.y + (b.h - 7) / 2, text, theme->text);
}

gui_rect_t gui_widget_scroll_thumb(gui_rect_t track, int total,
                                   int visible, int offset) {
    int thumb_h;
    int max_offset;
    if (track.h <= 0 || total <= 0 || visible >= total) return track;
    if (visible < 1) visible = 1;
    thumb_h = track.h * visible / total;
    if (thumb_h < 8) thumb_h = 8;
    if (thumb_h > track.h) thumb_h = track.h;
    max_offset = total - visible;
    if (offset < 0) offset = 0;
    if (offset > max_offset) offset = max_offset;
    return gui_rect_make(track.x + 2,
                         track.y + (track.h - thumb_h) * offset / max_offset,
                         track.w > 4 ? track.w - 4 : track.w, thumb_h);
}

int gui_widget_scroll_offset(gui_rect_t track, int total, int visible,
                             int pointer_y, int grab_offset) {
    gui_rect_t thumb;
    int travel;
    int max_offset;
    int thumb_y;
    if (total <= 0 || visible >= total || track.h <= 0) return 0;
    thumb = gui_widget_scroll_thumb(track, total, visible, 0);
    travel = track.h - thumb.h;
    max_offset = total - visible;
    if (travel <= 0 || max_offset <= 0) return 0;
    thumb_y = pointer_y - grab_offset - track.y;
    if (thumb_y < 0) thumb_y = 0;
    if (thumb_y > travel) thumb_y = travel;
    return (thumb_y * max_offset + travel / 2) / travel;
}

void gui_widget_scrollbar(gfx_surface_t* s, gui_rect_t track,
                          int total, int visible, int offset,
                          gui_widget_state_t state,
                          const gui_widget_theme_t* theme) {
    gui_rect_t thumb;
    if (!s || !theme) return;
    gui_canvas_fill_rect(s, track.x, track.y, track.w, track.h, theme->face);
    gui_canvas_rect(s, track.x, track.y, track.w, track.h, theme->frame);
    thumb = gui_widget_scroll_thumb(track, total, visible, offset);
    gui_canvas_fill_rect(s, thumb.x, thumb.y, thumb.w, thumb.h,
                         state.pressed ? theme->face_pressed :
                         state.hovered ? theme->face_hover : theme->accent);
}

void gui_widget_text_field(gfx_surface_t* s, gui_rect_t b, const char* text,
                           int cursor, gui_widget_state_t state,
                           const gui_widget_theme_t* theme,
                           gui_widget_text_fn draw_text) {
    int len = 0;
    if (!s || !theme) return;
    gui_canvas_fill_rect(s, b.x, b.y, b.w, b.h,
                         state.disabled ? theme->disabled : theme->face);
    gui_canvas_rect(s, b.x, b.y, b.w, b.h,
                    state.focused ? theme->accent : theme->frame);
    if (draw_text && text) draw_text(s, b.x + 3, b.y + 3, text, theme->text);
    while (text && text[len]) len++;
    if (state.focused) {
        if (cursor < 0) cursor = 0;
        if (cursor > len) cursor = len;
        gui_canvas_vline(s, b.x + 3 + cursor * 6, b.y + 2,
                         b.h - 4, theme->accent);
    }
}

void gui_widget_menu(gfx_surface_t* s, gui_rect_t b,
                     const gui_menu_item_t* items, int count, int selected,
                     const gui_widget_theme_t* theme,
                     gui_widget_text_fn draw_text) {
    int row_h;
    if (!s || !items || !theme || count <= 0) return;
    row_h = b.h / count;
    if (row_h < 1) row_h = 1;
    gui_canvas_fill_rect(s, b.x, b.y, b.w, b.h, theme->face);
    gui_canvas_rect(s, b.x, b.y, b.w, b.h, theme->frame);
    for (int i = 0; i < count; i++) {
        int y = b.y + i * row_h;
        unsigned int color = items[i].enabled ? theme->text : theme->disabled;
        if (i == selected && items[i].enabled)
            gui_canvas_fill_rect(s, b.x + 1, y, b.w - 2, row_h,
                                 theme->accent);
        if (items[i].checked && draw_text)
            draw_text(s, b.x + 3, y + (row_h - 7) / 2, "x", color);
        if (draw_text && items[i].label)
            draw_text(s, b.x + 13, y + (row_h - 7) / 2,
                      items[i].label, color);
    }
}

void gui_widget_list_row(gfx_surface_t* s, gui_rect_t b, const char* text,
                         int selected, gui_widget_state_t state,
                         const gui_widget_theme_t* theme,
                         gui_widget_text_fn draw_text) {
    if (!s || !theme) return;
    gui_canvas_fill_rect(s, b.x, b.y, b.w, b.h,
                         selected ? theme->accent :
                         state.hovered ? theme->face_hover : theme->face);
    if (state.focused)
        gui_canvas_rect(s, b.x, b.y, b.w, b.h, theme->frame);
    if (draw_text && text)
        draw_text(s, b.x + 3, b.y + (b.h - 7) / 2, text, theme->text);
}

void gui_widget_table_header(gfx_surface_t* s, gui_rect_t b,
                             const gui_table_column_t* columns, int count,
                             int sorted_column, int descending,
                             const gui_widget_theme_t* theme,
                             gui_widget_text_fn draw_text) {
    int x = b.x;
    if (!s || !columns || !theme) return;
    gui_canvas_fill_rect(s, b.x, b.y, b.w, b.h, theme->face_pressed);
    for (int i = 0; i < count && x < b.x + b.w; i++) {
        int width = columns[i].width;
        if (width < 1) width = 1;
        gui_canvas_rect(s, x, b.y, width, b.h, theme->frame);
        if (draw_text && columns[i].text)
            draw_text(s, x + 3, b.y + (b.h - 7) / 2,
                      columns[i].text, theme->text);
        if (i == sorted_column && draw_text)
            draw_text(s, x + width - 9, b.y + (b.h - 7) / 2,
                      descending ? "v" : "^", theme->accent);
        x += width;
    }
}

void gui_widget_radio(gfx_surface_t* s, gui_rect_t b, const char* text,
                      int selected, gui_widget_state_t state,
                      const gui_widget_theme_t* theme,
                      gui_widget_text_fn draw_text) {
    int cy;
    if (!s || !theme) return;
    cy = b.y + b.h / 2;
    gui_canvas_rect(s, b.x, cy - 5, 11, 11,
                    state.focused ? theme->accent : theme->frame);
    if (selected) gui_canvas_fill_rect(s, b.x + 3, cy - 2, 5, 5,
                                       theme->accent);
    if (draw_text && text)
        draw_text(s, b.x + 15, b.y + (b.h - 7) / 2, text,
                  state.disabled ? theme->disabled : theme->text);
}

void gui_widget_progress(gfx_surface_t* s, gui_rect_t b, int value,
                         int maximum, const gui_widget_theme_t* theme) {
    int fill;
    if (!s || !theme || b.w <= 0 || b.h <= 0) return;
    if (maximum < 1) maximum = 1;
    if (value < 0) value = 0;
    if (value > maximum) value = maximum;
    fill = (b.w - 2) * value / maximum;
    gui_canvas_fill_rect(s, b.x, b.y, b.w, b.h, theme->face);
    gui_canvas_rect(s, b.x, b.y, b.w, b.h, theme->frame);
    if (fill > 0) gui_canvas_fill_rect(s, b.x + 1, b.y + 1,
                                      fill, b.h - 2, theme->accent);
}

void gui_widget_tooltip(gfx_surface_t* s, gui_rect_t b, const char* text,
                        const gui_widget_theme_t* theme,
                        gui_widget_text_fn draw_text) {
    if (!s || !theme) return;
    gui_canvas_fill_rect(s, b.x, b.y, b.w, b.h, theme->face_hover);
    gui_canvas_rect(s, b.x, b.y, b.w, b.h, theme->frame);
    if (draw_text && text) draw_text(s, b.x + 4, b.y + 4, text, theme->text);
}

void gui_widget_modal(gfx_surface_t* s, gui_rect_t b, const char* title,
                      const char* message, const gui_widget_theme_t* theme,
                      gui_widget_text_fn draw_text) {
    if (!s || !theme) return;
    gui_canvas_fill_rect(s, b.x, b.y, b.w, b.h, theme->face);
    gui_canvas_rect(s, b.x, b.y, b.w, b.h, theme->accent);
    gui_canvas_fill_rect(s, b.x + 1, b.y + 1, b.w - 2, 16,
                         theme->face_pressed);
    if (draw_text && title) draw_text(s, b.x + 5, b.y + 5,
                                      title, theme->text);
    if (draw_text && message) draw_text(s, b.x + 7, b.y + 25,
                                        message, theme->text);
}

int gui_widget_focus_next(int current, int count, int reverse,
                          const unsigned char* enabled) {
    int position = current;
    if (count <= 0) return -1;
    for (int step = 0; step < count; step++) {
        position += reverse ? -1 : 1;
        if (position < 0) position = count - 1;
        if (position >= count) position = 0;
        if (!enabled || enabled[position]) return position;
    }
    return -1;
}

unsigned int gui_widget_command_for_key(const gui_command_t* commands,
                                        int count, unsigned int key,
                                        unsigned int modifiers,
                                        unsigned int modifier_mask) {
    if (!commands || count <= 0) return 0;
    for (int i = 0; i < count; i++) {
        if (commands[i].enabled && commands[i].key == key &&
            (commands[i].modifiers & modifier_mask) ==
            (modifiers & modifier_mask)) return commands[i].id;
    }
    return 0;
}

int gui_widget_key_activates(unsigned int key) {
    return key == 28u || key == 57u;
}

void gui_text_input_init(gui_text_input_t* input, const char* text) {
    int length = 0;
    if (!input) return;
    while (text && text[length] && length + 1 < GUI_TEXT_INPUT_CAPACITY) {
        input->text[length] = text[length];
        length++;
    }
    input->text[length] = '\0';
    input->length = length;
    input->cursor = length;
}

int gui_text_input_insert(gui_text_input_t* input, char ch) {
    if (!input || (unsigned char)ch < 32u ||
        input->length + 1 >= GUI_TEXT_INPUT_CAPACITY) return 0;
    for (int i = input->length + 1; i > input->cursor; i--)
        input->text[i] = input->text[i - 1];
    input->text[input->cursor++] = ch;
    input->length++;
    return 1;
}

int gui_text_input_command(gui_text_input_t* input,
                           gui_text_input_command_t command) {
    if (!input) return 0;
    if (command == GUI_TEXT_INPUT_LEFT) {
        if (input->cursor == 0) return 0;
        input->cursor--;
    } else if (command == GUI_TEXT_INPUT_RIGHT) {
        if (input->cursor >= input->length) return 0;
        input->cursor++;
    } else if (command == GUI_TEXT_INPUT_HOME) {
        if (input->cursor == 0) return 0;
        input->cursor = 0;
    } else if (command == GUI_TEXT_INPUT_END) {
        if (input->cursor == input->length) return 0;
        input->cursor = input->length;
    } else if (command == GUI_TEXT_INPUT_BACKSPACE) {
        if (input->cursor == 0) return 0;
        for (int i = input->cursor - 1; i < input->length; i++)
            input->text[i] = input->text[i + 1];
        input->cursor--;
        input->length--;
    } else if (command == GUI_TEXT_INPUT_DELETE) {
        if (input->cursor >= input->length) return 0;
        for (int i = input->cursor; i < input->length; i++)
            input->text[i] = input->text[i + 1];
        input->length--;
    } else {
        return 0;
    }
    return 1;
}
