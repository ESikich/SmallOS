# Syscall Interface (int 0x80)

This document defines the syscall ABI between ELF programs and the kernel.

⚠️ This is a **strict contract**. Any change here must be reflected in both:

* `interrupts.asm` (`isr128_stub`)
* `syscall_regs_t` in `syscall.h`

---

## Invocation

Syscalls are invoked using:

```asm
int 0x80
```

---

## Register Convention

```text
eax = syscall number
ebx = arg1
ecx = arg2
edx = arg3 (reserved for future use)

return value → eax
```

---

## Current Syscalls

### SYS_WRITE (1)

```c
int sys_write(const char* buf, uint32_t len);
```

* Writes `len` bytes from `buf` to terminal
* Returns number of bytes written
* Returns `-1` on error

---

### SYS_EXIT (2)

```c
void sys_exit(int code);
```

* Terminates the current ring-3 process
* Does not return to the caller
* Control returns to the shell via `elf_process_exit()` → `longjmp()` after foreground process teardown
* `elf_process_exit()` explicitly executes `sti` before `longjmp()` because `SYS_EXIT` entered through an interrupt gate and does not return through `iretd`

---

### SYS_GET_TICKS (3)

```c
uint32_t sys_get_ticks(void);
```

* Returns number of PIT ticks since boot

---

### SYS_PUTC (4)

```c
int sys_putc(char c);
```

* Writes a single character
* Returns 1

---

### SYS_READ (5)

```c
int sys_read(char* buf, uint32_t len);
```

* Reads up to `len` bytes of keyboard input into `buf`
* Blocks (STI + HLT loop) until input is available
* Echoes each character to the terminal as it is received
* Terminates early on newline (`\n`) — newline is included in the returned data
* Returns number of bytes read, or `-1` on error
* `buf` is a user virtual address — valid because the process PD is still active
  when the syscall runs

**Interrupt handling note:** The syscall gate is an interrupt gate — the CPU clears
IF on entry. `sys_read_impl` issues `sti` before the wait loop and `cli` before
returning so keyboard IRQs can fire during the blocking read.

---

## Kernel Entry Point

All syscalls are dispatched via:

```c
void syscall_handler_main(syscall_regs_t* regs);
```

---

## Register Frame Layout

This **must match** `isr128_stub`.

Current assembly push order:

```asm
pusha
push ds
push es
push fs
push gs
push esp   ; pointer passed to C
```

Because the stack grows downward, the last item pushed (`gs`) sits at the lowest address — which is where the struct pointer points. Reading the struct from its base therefore gives fields in reverse push order: `gs` first, `eax` last.

Therefore, C struct:

```c
typedef struct syscall_regs {
    unsigned int gs;   // last pushed → lowest address (stack grows down)
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
    unsigned int eax;  // first pushed by pusha → highest address
} syscall_regs_t;
```

---

## ⚠️ Critical Rule

If you change **anything** in `isr128_stub`:

👉 You MUST update `syscall_regs_t` to match.

Failure to do this will:

* corrupt arguments
* break return values
* silently break syscalls (hard to debug)

---

## Userspace Helpers

Defined in:

```c
user_syscall.h
```

Provides:

```c
sys_write(...)
sys_putc(...)
sys_exit(...)
sys_get_ticks(...)
sys_read(...)
```

`user_lib.h` builds on these with higher-level helpers:

```c
u_puts(...)       // sys_write wrapper
u_putc(...)       // sys_putc wrapper
u_put_uint(...)   // decimal integer output
u_readline(...)   // sys_read + null-terminate + strip newline
```

---

## Design Notes

* Programs run in **ring 3** — hardware-enforced privilege separation
* Kernel trusts user pointers (no copy-from-user validation yet)
* `SYS_READ` blocks with STI+HLT in the calling process while timer interrupts remain active
* EOI for IRQ1 is sent at the **top** of `irq1_handler_main` (before calling
  `keyboard_handle_irq`) so it is always delivered even when the handler
  launches a process and never returns through the normal path

---

## Future Extensions

Planned additions:

* SYS_SLEEP
* SYS_EXEC
* SYS_ALLOC

Long-term:

* copy-from-user validation
* per-process file descriptors (read from multiple sources)

---

## Debugging Tips

If syscalls stop working:

1. Check `isr128_stub`
2. Check `syscall_regs_t`
3. Verify register order
4. Test with `SYS_PUTC` first (simplest path)

Common symptom of mismatch:

* garbage output
* only one character prints
* random crashes after `int 0x80`