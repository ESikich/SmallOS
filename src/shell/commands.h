#ifndef COMMANDS_H
#define COMMANDS_H

#include "parse.h"

typedef void (*command_fn_t)(command_t* cmd);

typedef struct {
    const char* name;
    command_fn_t fn;
} command_entry_t;

void commands_execute(command_t* cmd);

#endif