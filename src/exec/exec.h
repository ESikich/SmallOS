#ifndef EXEC_H
#define EXEC_H

typedef void (*exec_entry_t)(int argc, char** argv);

int exec_run_entry(exec_entry_t entry, int argc, char** argv);

#endif