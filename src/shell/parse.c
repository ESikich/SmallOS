#include "parse.h"
#include "memory.h"

static int is_space(char c) {
    return c == ' ' || c == '\t';
}

command_t parse_command(const char* input) {
    command_t cmd;
    cmd.argc = 0;
    cmd.argv = (char**)kmalloc(sizeof(char*) * MAX_ARGS);

    int i = 0;

    while (input[i] != '\0') {
        while (is_space(input[i])) {
            i++;
        }

        if (input[i] == '\0') {
            break;
        }

        if (cmd.argc >= MAX_ARGS) {
            break;
        }

        int start = i;

        while (input[i] != '\0' && !is_space(input[i])) {
            i++;
        }

        int len = i - start;

        char* arg = (char*)kmalloc(len + 1);

        for (int j = 0; j < len; j++) {
            arg[j] = input[start + j];
        }
        arg[len] = '\0';

        cmd.argv[cmd.argc++] = arg;
    }

    return cmd;
}