#ifndef PARSE_H
#define PARSE_H

#define MAX_ARGS 16

typedef struct {
    int argc;
    char* argv[MAX_ARGS];
} command_t;

command_t parse_command(char* input);

#endif