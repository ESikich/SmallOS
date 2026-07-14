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
