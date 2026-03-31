#include "exec.h"
#include "terminal.h"

int exec_run_entry(exec_entry_t entry, int argc, char** argv) {
    if (!entry) {
        terminal_puts("exec: null entry\n");
        return 0;
    }

    entry(argc, argv);
    return 1;
}