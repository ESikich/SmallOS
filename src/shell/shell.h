#ifndef SHELL_H
#define SHELL_H

void shell_init(void);
void shell_poll(void);
void shell_task_main(void);

/*
 * shell_register_consumer()
 *
 * Register the shell's keyboard consumer with the keyboard driver.
 * Called once from shell_init(), and again by process_set_foreground()
 * when a user process releases the foreground.
 */
void shell_register_consumer(void);

#endif