# Syscall Interface (int 0x80)

This document defines the syscall ABI between ELF programs and the kernel.

⚠️ This is a **strict contract**. Any change here must be reflected in both:

* `interrupts.asm` (`isr128_stub`)
* `syscall_regs_t` in `syscall.h`

---

## Invocation

```asm
int 0x80
```

---

## Register Convention

```text
eax = syscall number
ebx = arg1
ecx = arg2
edx = arg3

return value → eax
```

---

## Current Syscalls

### SYS_WRITE (1)

```c
int sys_write(const char* buf, uint32_t len);
```

Writes `len` bytes from `buf` to terminal. Returns bytes written or `-1`.

---

### SYS_EXIT (2)

```c
void sys_exit(int code);
```

Terminates the current ring-3 process. Does not return to the caller. `sys_exit_impl()` switches to the kernel page directory and calls `sched_exit_current((unsigned int)regs)`. The task becomes `PROCESS_STATE_ZOMBIE`, is dequeued, and execution switches to the next runnable task. Destruction is deferred to a later waiter such as `process_wait()`.

---

### SYS_GET_TICKS (3)

```c
uint32_t sys_get_ticks(void);
```

Returns PIT tick count since boot.

---

### SYS_PUTC (4)

```c
int sys_putc(char c);
```

Writes a single character. Returns 1.

---

### SYS_READ (5)

```c
int sys_read(char* buf, uint32_t len);
```

Blocks until keyboard input is available, echoing each character. Terminates early on newline (included in returned data). Returns bytes read or `-1`.

`sys_read_impl` uses true scheduler-aware blocking: when `kb_buf` is empty it sets the process state to `PROCESS_STATE_WAITING` and registers it as the keyboard waiter via `keyboard_set_waiting_process()`, then executes `hlt`. The timer IRQ fires normally; `sched_tick` sees the task is `WAITING`, skips it, and switches to another runnable task. When a keypress arrives, `process_key_consumer()` pushes the character into `kb_buf`, sets the waiting process back to `PROCESS_STATE_RUNNING`, and clears the waiter slot. On the next scheduler pass the process is selected, resumes after the `hlt`, and drains the buffer normally.

`sti` is issued before the first `hlt` so IRQ1 can fire during the wait. `cli` is restored before returning, matching the IF=0 postcondition expected by the syscall gate.

---

### SYS_YIELD (6)

```c
void sys_yield(void);
```

Voluntarily surrenders the current scheduler quantum. The calling process is immediately context-switched out and becomes runnable again on the next scheduler pass.

**Implementation note:** `sys_yield_impl(esp)` receives `(unsigned int)regs` from `isr128_stub`, but the true resume-frame base is `esp - 8` because the stub passes ESP via `push esp` and then `call` adds a return address. `sched_yield_now(esp - 8)` bypasses the quantum counter and calls `sched_do_switch()` with the real resume ESP.

---

### SYS_SLEEP (11)

```c
int sys_sleep(uint32_t ticks);
```

Blocks the calling process for at least `ticks` timer ticks. The task marks itself `PROCESS_STATE_SLEEPING`, yields to the scheduler, and is woken by the timer IRQ once the deadline is reached.

`sys_sleep_impl` uses the same scheduler-owned preemption path as `SYS_YIELD`, but the task remains unrunnable until `timer_get_ticks()` reaches the stored wake deadline. If no other runnable task exists, the kernel still idles in a `hlt` loop until the wake condition is met.

---

### SYS_WRITEFILE (12)

```c
int sys_writefile(const char* name, const char* buf, uint32_t len);
```

Creates or overwrites a root-directory FAT16 file in one shot. Returns `0` on success, `-1` on failure.

This is the historical root-only persistence primitive for generated artifacts such as compiler output, assembly listings, or other build products. The kernel validates both the filename and the byte range before calling into `fat16_write()`.

---

### SYS_WRITEFILE_PATH (15)

```c
int sys_writefile_path(const char* path, const char* buf, uint32_t len);
```

Creates or overwrites a FAT16 file at an arbitrary path. Returns `0` on success, `-1` on failure.

This is the preferred persistence primitive for build tools and compilers because it can emit directly into nested directories. The kernel validates both the path and the byte range before calling into `fat16_write_path()`.

---

### SYS_EXEC (7)

```c
int sys_exec(const char* name, int argc, char** argv);
```

Loads and asynchronously spawns a named ELF program from the FAT16 partition. Returns `0` on success, `-1` if not found or load fails.

`sys_exec_impl` copies `name` to a local kernel stack buffer before any FAT16 or ELF work so the loader does not depend on the caller's user pointer remaining valid. It then calls `elf_run_named()`, which creates the process, seeds its scheduler bootstrap context, enqueues it, and returns immediately.

---

### SYS_OPEN (8)

```c
int sys_open(const char* name);
```

Opens a file from the FAT16 root directory by name (case-insensitive 8.3 matching). Allocates the lowest free slot in the calling process's fd table (fd ≥ 3) and records the filename, file size, and an initial read offset of 0. fds 0/1/2 are reserved.

Returns the fd (≥ 3) on success, or `-1` if the file is not found, the fd table is full (`PROCESS_FD_MAX = 8`), or the name pointer fails user-space validation.

`sys_open_impl` validates the name with page-aware user checks, copies it into a kernel buffer bounded by `PROCESS_FD_NAME_MAX` (128 bytes), then calls `fat16_stat()` to confirm the file exists without loading its data.

---

### SYS_CLOSE (9)

```c
int sys_close(int fd);
```

Closes an open file descriptor, freeing its slot for reuse. Returns `0` on success, `-1` if the fd is out of range or not currently open.

---

### SYS_FREAD (10)

```c
int sys_fread(int fd, char* buf, uint32_t len);
```

Reads up to `len` bytes from the open file at `fd` into `buf`, starting at the current file position. Advances the position by the number of bytes actually read. Returns the number of bytes read, `0` at end-of-file, or `-1` on error (bad fd, invalid buffer, read failure).

`sys_fread_impl` loads the file once into PMM-backed per-fd cache pages on first use, then copies the requested slice from that cache into the validated user buffer. The cache stays live until `sys_close()` or process teardown, so repeated reads from the same descriptor avoid extra ATA traffic.

---

## Kernel Entry Point

```c
void syscall_handler_main(syscall_regs_t* regs);
```

---

## Register Frame Layout

This **must match** `isr128_stub`.

Assembly push order:

```asm
pusha
push ds
push es
push fs
push gs
push esp   ; pointer passed to C
```

C struct (fields in reverse push order — last pushed is at lowest address):

```c
typedef struct syscall_regs {
    unsigned int gs;
    unsigned int fs;
    unsigned int es;
    unsigned int ds;
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;
} syscall_regs_t;
```

---

## ⚠️ Critical Rule

If you change **anything** in `isr128_stub`, you MUST update `syscall_regs_t` to match. Failure silently corrupts arguments and return values.

---

## Userspace Helpers

`user_syscall.h`:

```c
sys_write(buf, len)
sys_putc(c)
sys_exit(code)
sys_get_ticks()
sys_read(buf, len)
sys_yield()
sys_sleep(ticks)
sys_exec(name, argc, argv)
sys_writefile(name, buf, len)
sys_open(name)
sys_close(fd)
sys_fread(fd, buf, len)
```

`user_lib.h` higher-level wrappers:

```c
u_puts(...)        sys_write wrapper
u_putc(...)        sys_putc wrapper
u_put_uint(...)    decimal integer output
u_readline(...)    sys_read + null-terminate + strip newline
```

---

## Design Notes

* Programs run in ring 3 — hardware-enforced privilege separation
* All pointer arguments to syscalls are validated with page-aware user checks before kernel dereference — pointers below `USER_CODE_BASE` (0x400000), spanning above `USER_STACK_TOP` (0xC0000000), or touching an unmapped user page are rejected with `-1`
* `SYS_EXEC` copies the user `name` and `argv[]` strings into kernel buffers before handing them to the ELF loader, so the loader never depends on caller memory staying stable after validation
* `SYS_YIELD` and the timer path use the same stub layout, but the real scheduler resume ESP is `esp - 8`, not raw `esp`
* EOI for IRQ1 is sent at the top of `irq1_handler_main` before `keyboard_handle_irq`
* The TSS is owned by the GDT subsystem. Syscall entry uses the currently active `SS0/ESP0`, and scheduler-driven updates to ESP0 go through `tss_set_kernel_stack()` rather than a cached pointer into the packed TSS.
* fd 0/1/2 are reserved by convention; user-opened files start at fd 3. The fd table (`fd_entry_t fds[PROCESS_FD_MAX]`) lives inside `process_t` and is zero-initialized by `process_create()`; no explicit close-on-exit is needed since the entire frame is freed by `pmm_free_frame` on process destruction.
* `SYS_WRITEFILE` is the simplest root-only persistence path for user tools that want to emit a generated artifact without managing an fd-based write stream.
* `SYS_WRITEFILE_PATH` is the preferred path-aware persistence primitive for compilers and build tools.

---

## Future Extensions

* `SYS_ALLOC`

---

## Debugging Tips

If syscalls stop working: check `isr128_stub`, check `syscall_regs_t`, verify register order. Test with `SYS_PUTC` first. Common symptom of mismatch: garbage output, only one character prints, crashes after `int 0x80`.
