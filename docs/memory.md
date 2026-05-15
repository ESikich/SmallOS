# Memory Model

This document describes the memory layout and allocator behavior used by SmallOS.

---

# Overview

SmallOS currently uses three distinct memory pools:

```text
0x100000 – 0x1FFFFF   kernel .bss, bump heap, and boot stack arena
0x200000 – 0x7FFFFFF  reclaimable PMM frame pool, capped at 128 MB
0x10000000+           per-process user heap (SYS_BRK)
```

The kernel's static storage starts at 1 MiB. The kernel heap begins after
the linked `.bss` end, is permanent, and never shrinks. The PMM frame pool is
reclaimed on process exit. The user heap is managed per process through
`SYS_BRK`, with a user-space allocator layered on top of it.

---

# Kernel Heap

`memory_init(PAGE_ALIGN(&bss_end))` seeds the kernel bump heap immediately
after high kernel `.bss`.

Use this heap for:

- permanent kernel structures
- boot-time tables
- long-lived singletons

Do not use it for transient buffers. It has no free path.

Verification rule:

```text
meminfo
runelf usr/bin/hello
meminfo
```

The reported heap top should remain unchanged across user ELF runs when no kernel allocation work is happening.

---

# PMM Frame Pool

`pmm_init()` manages the fixed reclaimable window from `0x200000`
through `0x7FFFFFF`, but the bitmap is initialized from BIOS E820 data.
All frames start used, usable E820 RAM ranges inside the fixed window are
marked free, and SmallOS-owned boot/runtime reservations are marked used again.
If E820 is unavailable, PMM falls back to the old 30 MB fixed-window assumption.

The protected ranges that are never handed to PMM include low BIOS memory,
the boot sector, loader2, boot info at `0x90000`, the copied boot font at
`0x91000`, the loader2-published boot ramdisk when present, high kernel `.bss`
plus the bump heap, the boot stack, and the framebuffer range when framebuffer
boot info is valid.

Use PMM frames for:

- `process_t` objects
- process page directories
- process-private page tables
- kernel stack frames for processes
- ELF segment frames
- other memory that should be freed on process exit
- dynamic per-process handle tables referenced by `process_t`
- TCP listener and global 4-tuple connection tables
- lazy TCP receive and transmit rings for accepted socket streams

Do not allocate process-owned paging structures with `kmalloc_page()`. They must come from PMM so the process can be torn down cleanly.

---

# User Heap

User programs now have their own heap region and can grow or shrink it with `SYS_BRK`.

The user-side runtime exposes:

- `sys_brk(new_brk)`
- `malloc()`
- `free()`
- `realloc()`
- `calloc()`

This is what makes compiler-style tools practical inside the guest: they can allocate token buffers, AST nodes, symbol tables, file buffers, and other large working sets without consuming kernel heap permanently.

The current user heap base is:

```text
USER_HEAP_BASE = 0x10000000
```

The user allocator is simple and deterministic rather than fragmentation-optimized. That is intentional for compiler and tooling workloads.

---

# ext2 Load Buffer

The ext2 driver keeps one permanent load buffer allocated during
`ext2_init()`:

```text
s_load_buf[1 MB]
```

It is reused for file loads and ELF loading. It is allocated once from the
kernel bump heap, not per `runelf`, so repeated program launches do not keep
consuming heap.

The loader copies ELF segment data out of this buffer into PMM-backed frames before returning, so the buffer can be reused immediately after `elf_run_named()`.

Loader2 also preloads the entire ext2 partition to physical `0x800000` as a
boot-storage fallback. With the current 16 MB seed volume, that reservation is
16 MB. QEMU and VMware normally mount the writable ATA path instead, but BIOS
USB boots and hardware with failing ATA sector reads can mount this RAM-backed
copy. Writes made through the fallback are not persisted to the disk image.

Fd-backed file reads and writes do not need to fit inside this static buffer.
Small fd reads still use reclaimable PMM cache pages owned by the shared
open-file description, while larger reads use ext2 read-at directly. Fd writes
stream through ext2 write-at and invalidate that shared read cache, so large
uploads and compiler outputs are bounded by ext2 free space rather than by the
fd cache. VFS accesses PMM cache frames through the high kernel PMM alias
(`KERNEL_PMM_MAP_BASE`), so file-cache frames remain reachable under every
process page directory without switching CR3.

---

# Memory Safety Rules

## Kernel

- Use `kmalloc` / `kmalloc_page` only for permanent structures
- Use `pmm_alloc_frame()` for anything that must be reclaimed
- Treat `pmm_alloc_frame()` results as physical addresses; translate with
  `paging_phys_to_kernel_virt()` before dereferencing
- Keep `.bss` zeroed before paging and PMM initialization
- Keep high kernel `.bss` plus the initial bump pointer below the boot stack
  guard checked by `kernel_selfcheck()`

## User space

- Do not rely on shell input buffers for process lifetime data
- Copy argv and any toolchain inputs into process-owned storage
- Use the user heap for large compiler/runtime working sets

## Process teardown

- user processes are freed after they reach `PROCESS_STATE_ZOMBIE`
- the reaper task reclaims unclaimed children
- `process_destroy()` must only run from a safe stack

---

# Related Tests

The following programs exercise the current memory model:

- `heapprobe` - allocator behavior and reuse
- `fileprobe` - file-handle and path write helpers
- `statprobe` - path probing and file metadata
- `compiler_demo` - file write/readback flows
- the TinyCC guest tests - compiler-style allocation and file output

---

# Debugging

Use `meminfo` to verify that:

- the kernel heap top does not drift unexpectedly
- the frame pool shrinks and grows as processes come and go
- user tools can allocate and free without leaking kernel heap
