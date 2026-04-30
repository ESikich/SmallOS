#ifndef SHELL_H
#define SHELL_H

#define SHELL_PATH_MAX 128

void shell_init(void);
void shell_poll(void);
void shell_task_main(void);
const char* shell_get_cwd(void);
int shell_set_cwd(const char* path);
int shell_resolve_path(const char* path, char* out, unsigned int out_size);

/*
 * shell_register_consumer()
 *
 * Register the shell's keyboard consumer with the keyboard driver.
 * Called once from shell_init(), and again by process_set_foreground()
 * when a user process releases the foreground.
 */
void shell_register_consumer(void);

#endif
