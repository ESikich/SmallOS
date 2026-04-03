#ifndef ELF_LOADER_H
#define ELF_LOADER_H

/*
 * elf_run_named(name, argc, argv)
 *
 * Look up `name` in the FAT16 partition, load it into a per-process address space,
 * and create a process, seed its scheduler bootstrap context, enqueue it,
 * and return the created process_t*. Returns 0 on failure.
 */
#include "process.h"

process_t* elf_run_named(const char* name, int argc, char** argv);

/*
 * elf_run_image(image, argc, argv)
 *
 * Load an ELF binary from the pointer `image` (in kernel address space)
 * into a fresh per-process page directory, seed its first-entry scheduler
 * context, enqueue it, and return the created process_t*. Returns 0 on
 * failure.
 *
 * The ELF must be linked at USER_CODE_BASE (0x400000).
 */
process_t* elf_run_image(const unsigned char* image, int argc, char** argv);

#endif