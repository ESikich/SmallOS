#ifndef PROGRAMS_H
#define PROGRAMS_H

typedef void (*program_entry_t)(int argc, char** argv);

typedef struct {
    const char* name;
    program_entry_t entry;
} program_entry_desc_t;

void programs_run(const char* name, int argc, char** argv);

#endif