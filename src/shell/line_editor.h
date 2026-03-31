#ifndef LINE_EDITOR_H
#define LINE_EDITOR_H

#define LINE_EDITOR_MAX 128

typedef struct {
    char buf[LINE_EDITOR_MAX];
    int len;
    int cursor;
} line_editor_t;

void line_editor_init(line_editor_t* ed);
void line_editor_clear(line_editor_t* ed);

int line_editor_insert(line_editor_t* ed, char c);
int line_editor_backspace(line_editor_t* ed);
int line_editor_delete(line_editor_t* ed);

void line_editor_move_left(line_editor_t* ed);
void line_editor_move_right(line_editor_t* ed);
void line_editor_move_home(line_editor_t* ed);
void line_editor_move_end(line_editor_t* ed);

#endif