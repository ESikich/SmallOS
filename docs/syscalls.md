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

Terminates the current ring-3 process. Does not return to the caller. Control returns to the parent via `elf_process_exit()` → `longjmp()`. `elf_process_exit()` executes `sti` before `longjmp()` because SYS_EXIT enters through an interrupt gate and does not unwind through `iretd`.

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

`sys_read_impl` issues `sti` before the wait loop and `cli` before returning so keyboard IRQs fire during the blocking read.

---

### SYS_YIELD (6)

```c
void sys_yield(void);
```

Voluntarily surrenders the current scheduler quantum. The calling process is immediately context-switched out and becomes runnable again on the next scheduler pass.

**Implementation note:** `sys_yield_impl(esp)` receives `(unsigned int)regs` — a pointer to the `isr128_stub` register frame, which is structurally identical to the `irq0_stub` frame (same push order). This makes it valid as a scheduler resume ESP. `sched_yield_now(esp)` bypasses the quantum counter and calls `sched_do_switch(esp)`.

---

### SYS_EXEC (7)

```c
int sys_exec(const char* name, int argc, char** argv);
```

Loads and runs a named ELF program from the FAT16 partition through the current foreground run-and-wait path. The calling process is suspended until the child exits. Returns 0 on success, `-1` if not found or load fails.

`sys_exec_impl` copies `name` to a local kernel stack buffer before any CR3 switches because the load path later switches page directories and must not depend on the caller's user pointer remaining valid. `s_parent_proc`/`s_parent_esp0` statics save the parent context. One-deep only — a child cannot call `SYS_EXEC`. This is still blocking foreground execution, not spawn-style execution.

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
sys_exec(name, argc, argv)
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
* Kernel trusts user pointers (no copy-from-user validation yet)
* `SYS_YIELD` and `SYS_EXEC` use the same `isr128_stub` frame as the timer path — the frame pointer is a valid scheduler resume ESP
* EOI for IRQ1 is sent at the top of `irq1_handler_main` before `keyboard_handle_irq`
* The TSS is owned by the GDT subsystem. Syscall entry uses the currently active `SS0/ESP0`, and scheduler-driven updates to ESP0 go through `tss_set_kernel_stack()` rather than a cached pointer into the packed TSS.

---

## Future Extensions

* `SYS_SLEEP`
* `SYS_ALLOC`
* copy-from-user validation
* per-process file descriptors

---

## Debugging Tips

If syscalls stop working: check `isr128_stub`, check `syscall_regs_t`, verify register order. Test with `SYS_PUTC` first. Common symptom of mismatch: garbage output, only one character prints, crashes after `int 0x80`.