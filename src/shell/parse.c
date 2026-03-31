#include "parse.h"

command_t parse_command(char* input) {
    command_t cmd;
    cmd.argc = 0;

    // Skip leading spaces
    while (*input == ' ') input++;

    while (*input && cmd.argc < MAX_ARGS) {
        cmd.argv[cmd.argc++] = input;

        while (*input && *input != ' ') {
            input++;
        }

        if (*input == '\0') {
            break;
        }

        *input = '\0';
        input++;

        while (*input == ' ') input++;
    }

    return cmd;
}