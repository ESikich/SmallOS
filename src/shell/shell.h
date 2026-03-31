#ifndef SHELL_H
#define SHELL_H

void shell_init(void);
void shell_input_char(char c);
void shell_backspace(void);

void shell_move_left(void);
void shell_move_right(void);
void shell_move_home(void);
void shell_move_end(void);
void shell_delete(void);

void shell_history_up(void);
void shell_history_down(void);

#endif