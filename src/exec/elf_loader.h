#ifndef ELF_LOADER_H
#define ELF_LOADER_H

/*
 * elf_run_named(name, argc, argv)
 *
 * Look up `name` in the ramdisk, load it into a per-process address space,
 * and execute it in ring 3. Does not return until the process calls
 * sys_exit(). Returns 1 on success, 0 on failure.
 */
int elf_run_named(const char* name, int argc, char** argv);

/*
 * elf_run_image(image, argc, argv)
 *
 * Load an ELF binary from the pointer `image` (in kernel address space)
 * into a fresh per-process page directory and execute it in ring 3.
 * Does not return until the process calls sys_exit().
 * Returns 1 on success, 0 on failure.
 *
 * The ELF must be linked at USER_CODE_BASE (0x400000).
 */
int elf_run_image(const unsigned char* image, int argc, char** argv);

/*
 * elf_process_exit()
 *
 * Called by sys_exit() from inside the syscall handler while the process
 * page directory is still active (CR3 still points at process PD).
 *
 * Restores the kernel page directory, destroys the process page directory,
 * and longjmps back to the point in elf_run_image() just after the iret —
 * effectively returning control to the shell without unwinding through
 * ring-3 frames.
 *
 * Does not return to its caller.
 */
void elf_process_exit(void);

/*
 * elf_process_running()
 *
 * Returns 1 while a ring-3 ELF process is active (between iret and
 * elf_process_exit), 0 otherwise.  Used by the keyboard driver to decide
 * whether keystrokes go to the shell or to the process input buffer.
 */
int elf_process_running(void);

#endif