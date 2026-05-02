#ifndef COMMANDS_H
#define COMMANDS_H

#include "parse.h"

typedef void (*command_fn_t)(command_t* cmd);

typedef struct {
    const char* name;
    const char* help;
    command_fn_t fn;
} command_entry_t;

void commands_execute(command_t* cmd);
unsigned int commands_count(void);
const char* commands_name_at(unsigned int index);

#endif
