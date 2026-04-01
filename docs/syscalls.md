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
edx = arg3

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
* Control returns to the parent (shell or user process) via `elf_process_exit()` → `longjmp()`
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

### SYS_YIELD (6)

```c
int sys_yield(void);
```

* Voluntarily surrenders the remainder of the current scheduler quantum
* The calling process is immediately preempted and the next runnable process is scheduled
* Returns 0 when the process is next scheduled to run
* The tick counter is reset so the next process receives a full quantum

**Implementation note:** `sys_yield_impl(esp)` receives `(unsigned int)regs` — the pointer to the `isr128_stub` register frame on the kernel stack. This is passed directly to `sched_yield_now(esp)`. The `isr128_stub` frame layout (pusha + 4 segment pushes + esp) is structurally identical to an `irq0_stub` frame, so `sched_switch` can save this ESP as the resume point and resume the yielding process via `iretd` exactly as it would a timer-preempted context.

---

### SYS_EXEC (7)

```c
int sys_exec(const char* name, int argc, char** argv);
```

* Loads the named ELF from the ramdisk and runs it as a child process
* Blocks until the child calls `sys_exit()`
* Returns 0 on success, -1 if the program was not found or failed to load

**Argument handling:** `name` and `argv` are user virtual addresses — valid when the syscall fires because the caller's CR3 is still active. `sys_exec_impl` copies `name` into a kernel-side stack buffer before calling `elf_run_named()`, because `ramdisk_find()` runs after `paging_switch()` changes CR3 to the child's page directory — at that point the original user pointer would no longer be mapped. `argv` strings are copied into `process_t.user_arg_data` by `elf_seed_sched_context()` while the kernel PD is active, before CR3 switches.

**Parent context save/restore:** Before launching the child, `elf_run_image()` saves:
* `s_parent_proc` — the calling process's `process_t*`
* `s_parent_esp0` — the calling process's kernel stack top (`kernel_stack_frame + PAGE_SIZE`)

On child exit, `elf_process_exit()` switches CR3 to `s_parent_proc->pd` (the parent's page directory, not the kernel PD), restores TSS ESP0 to `s_parent_esp0`, restores `process_set_current(s_parent_proc)`, and `longjmp`s back into the parent's `elf_run_image()` call frame.

**Why parent's PD, not kernel PD:** After `longjmp` the return chain unwinds through kernel code (mapped in all PDs) and reaches `isr128_stub`'s `iretd`. That instruction jumps to the parent's ring-3 EIP at `0x400000`, which is only mapped in the parent's page directory. Switching to the kernel PD before `longjmp` would cause an immediate page fault on `iretd`.

---

## Kernel Entry Point

All syscalls are dispatched via:

```c
void syscall_handler_main(syscall_regs_t* regs);
```

---

## Register Frame Layout

This **must match** `isr128_stub`.

Current assembly push order in interrupts.asm:

```asm
pusha
push ds
push es
push fs
push gs
push esp        ; pointer to saved syscall frame passed to C
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
sys_write(buf, len)
sys_putc(c)
sys_exit(code)
sys_get_ticks()
sys_read(buf, len)
sys_yield()
sys_exec(name, argc, argv)
```

`user_lib.h` builds on these with higher-level helpers:

```c
u_puts(s)           // sys_write wrapper
u_putc(c)           // sys_putc wrapper
u_put_uint(n)       // decimal integer output
u_readline(buf, n)  // sys_read + null-terminate + strip newline
```

---

## Design Notes

* Programs run in **ring 3** — hardware-enforced privilege separation
* Kernel trusts user pointers (no copy-from-user validation yet)
* `SYS_READ` blocks with STI+HLT in the calling process while timer interrupts remain active
* `SYS_YIELD` uses `(unsigned int)regs` as the scheduler resume ESP — safe because `isr128_stub` and `irq0_stub` build identical frame layouts
* `SYS_EXEC` is one-deep — `s_parent_proc` / `s_parent_esp0` are single statics, not a stack. The current implementation supports one level of nesting (a user process spawning a child). Deeper nesting would require making these into a proper stack.
* EOI for IRQ1 is sent at the **top** of `irq1_handler_main` (before calling `keyboard_handle_irq`) so it is always delivered even when the handler launches a process and never returns through the normal path

---

## Future Extensions

Planned additions:

* SYS_SLEEP
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