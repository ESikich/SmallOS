#include "gui.h"
#include "runtime.h"

static int arg_streq(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int gui_main(int argc, char** argv) {
    gui_runtime_options_t options;

    options.initial_path = 0;
    options.diagnostics = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i] && arg_streq(argv[i], "--diagnostics"))
            options.diagnostics = 1;
        else if (argv[i] && argv[i][0] && !options.initial_path)
            options.initial_path = argv[i];
    }
    return gui_runtime_run(&options);
}
