#ifndef ELF_LOADER_H
#define ELF_LOADER_H

/*
 * elf_run_named(name, argc, argv)
 *
 * Look up `name` in the FAT16 partition, load it into a per-process address space,
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
 * Called by sys_exit() from inside the syscall handler while the current
 * process page directory is still active.
 *
 * Restores the parent address space when one exists, otherwise falls back
 * to the kernel page directory; then destroys the exiting process and
 * longjmps back to the saved exit context in elf_run_image() without
 * unwinding through ring-3 frames.
 *
 * Does not return to its caller.
 */
void elf_process_exit(void);

/*
 * elf_process_running()
 *
 * Returns 1 when the current process exists and is in PROCESS_STATE_RUNNING,
 * 0 otherwise.
 *
 * Note: this reflects the current process state returned by
 * process_get_current(); it is not limited strictly to a foreground ring-3
 * ELF process.
 */
int elf_process_running(void);

#endif