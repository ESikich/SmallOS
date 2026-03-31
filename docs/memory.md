# Memory Model

This document describes the physical memory layout, allocators, and paging architecture.

---

# Physical Memory Map

```text
0x00007C00   bootloader stage 1 (BIOS loads here)
0x00008000   loader2 stage 2 (done after protected-mode jump)
0x00001000   kernel image (loaded by loader2)
0x00006000   kernel .bss start (page tables + PMM bitmap reside here)
~0x0000A008  kernel .bss end
0x00010000   ramdisk (permanent — loaded by loader2, never freed)
0x00090000   kernel stack top (grows downward from here)

0x00100000   bump allocator base — permanent kernel structures
               kmalloc()      — argv arrays, command-line parse buffers
             grows upward to 0x1FFFFF

0x00200000   PMM base — reclaimable frames
               pmm_alloc_frame() — process_t structs, process page directories,
                                   ELF segment frames, user stack frames,
                                   all process-private page tables,
                                   per-process kernel stack frames
             6 MB = 1536 frames; all reclaimed on process exit
0x00800000   PMM ceiling (= identity-map limit)

0x00400000   USER_CODE_BASE — ELF virtual address (per-process mapping)
0xBFFFF000   user stack virtual address (per-process mapping)
```

---

# Two-Allocator Contract

The system has two physical allocators with **disjoint, non-overlapping ranges**:

```text
kmalloc / kmalloc_page   0x100000 – 0x1FFFFF   bump, no free
pmm_alloc_frame          0x200000 – 0x7FFFFF   bitmap, freed on process exit
```

**`kmalloc` / `kmalloc_page` are for permanent kernel-owned structures** — command-line parse buffers and other bump-allocator data with kernel lifetime. These allocations are never freed.

**`pmm_alloc_frame` is for everything reclaimed on exit** — `process_t` structs, process page directories, ELF segment frames, user stack frames, all process-private page tables, and per-process kernel stack frames. All of these are freed by `process_destroy()`.

**PMM ceiling = identity-map limit (0x800000).** Frames above this address are not identity-mapped. The kernel accesses all PMM frames as direct pointers (phys == virt). `PMM_BASE + PMM_SIZE` must not exceed `0x800000`.

**The two ranges are disjoint.** `pmm_alloc_frame()` and `kmalloc_page()` can never return the same address. If they overlapped, the PMM bitmap would hand out frames already claimed by the bump allocator, causing silent corruption.

---

# Bump Allocator (`kmalloc` / `kmalloc_page`)

```c
void  memory_init(unsigned int start);   // set base; called with 0x100000
void* kmalloc(unsigned int size);        // allocate, 4-byte aligned
void* kmalloc_page(void);               // allocate one 4096-byte page-aligned block
unsigned int memory_get_heap_top(void); // current bump pointer (for meminfo)
```

No free. Suitable for structures that live for the lifetime of the kernel: parse buffers and other permanent kernel-owned data. `memory_get_heap_top()` is used by `meminfo` to report heap usage.

---

# Physical Memory Manager (`pmm`)

```c
void pmm_init(void);             // initialise bitmap; all frames start free
u32  pmm_alloc_frame(void);      // return a 4KB-aligned physical address, or 0
void pmm_free_frame(u32 addr);   // reclaim a frame; warns on double-free
u32  pmm_free_count(void);       // current free frame count (used by meminfo)
```

192-byte bitmap in `.bss` (zeroed at boot). Linear scan with a search hint for O(n) worst-case allocation. Detects and logs double-free attempts.

Run `meminfo` before and after `runelf` to confirm the free count is unchanged.

---

# Process Memory Lifecycle

Each `runelf` invocation allocates from the PMM and fully reclaims on exit:

```text
process_create()
  → pmm_alloc_frame()           process_t struct (< PAGE_SIZE, fits in one frame)

process_pd_create()
  → pmm_alloc_frame()           process page directory

paging_map_page() for ELF segments:
  → pmm_alloc_frame()           process-private page table (first use only)
  → pmm_alloc_frame() × N       one frame per ELF page

paging_map_page() for user stack:
  → pmm_alloc_frame()           process-private page table (first use only)
  → pmm_alloc_frame()           stack frame

elf_run_image():
  → pmm_alloc_frame()           per-process kernel stack (TSS ESP0)

--- process runs ---

process_destroy()
  → process_pd_destroy(pd)
      pmm_free_frame() × N      ELF segment frames
      pmm_free_frame()          stack frame  (walked via PD index 767 PTE)
      pmm_free_frame()          each process-private page table
      pmm_free_frame()          page directory itself
  → pmm_free_frame()            kernel stack frame
  → pmm_free_frame()            process_t frame
```

After a clean `runelf`, `pmm_free_count()` returns the same value as before the call.

---

# Process Paging Allocation Model

All paging structures associated with user processes are allocated from the Physical Memory Manager (PMM), not the bump allocator.

## Rules

* `kernel_page_directory` may still reference long-lived kernel-owned tables
* Process page directories are allocated with `pmm_alloc_frame()`
* Process-private user page tables are allocated with `pmm_alloc_frame()`
* Process-private frames (ELF pages, user stack, kernel stack, `process_t`) are allocated with `pmm_alloc_frame()`

## Rationale

Previously, a process-private page table could come from `kmalloc_page()` and survive process teardown, creating a per-process leak in kernel heap pages. Using PMM for all process-owned paging structures makes allocation and reclamation symmetric.

## Teardown

`process_pd_destroy()` now:

1. Walks the user PDE range
2. Frees every mapped user frame referenced by present PTEs
3. Frees every process-private page table frame
4. Frees the page directory frame itself

Kernel-shared mappings are left intact.

# Paging Architecture

## Kernel page directory

Three static arrays in `.bss`, zeroed at boot by `kernel_entry.asm`:

```text
kernel_page_directory[1024]   master PD (CR3 value during kernel execution)
low_page_table_0[1024]        PD index 0 → 0x000000–0x3FFFFF (identity, supervisor)
low_page_table_1[1024]        PD index 1 → 0x400000–0x7FFFFF (identity, supervisor)
```

After `paging_init()`: virtual == physical for all addresses 0x000000–0x7FFFFF.

## Per-process page directories

`process_pd_create()` allocates a fresh PD from the PMM and copies the kernel's entries (PD indices 0 and 2–1023) into it so kernel code, VGA, ramdisk, and heap remain accessible after CR3 switch. PD index 1 is left empty — each process gets its own private ELF mapping there.

### Page table allocation policy

`paging_map_page()` uses different allocators depending on the PD index:

| PD index | Virtual range        | Page table source  | Freed on exit? |
|----------|---------------------|--------------------|----------------|
| user range | per-process mappings | `pmm_alloc_frame`  | yes            |
| kernel range | shared from kernel | shared from kernel | n/a            |

The physical **frames** pointed to by user PTEs are PMM-allocated and freed by `process_pd_destroy()`. Process-private page tables are also PMM-allocated and freed there.

## Virtual address space per process

```text
0x000000 – 0x3FFFFF   kernel (shared, supervisor-only — ring 3 cannot access)
0x400000 – 0x7FFFFF   user ELF segments (private, PAGE_USER | PAGE_WRITE)
0x800000 – 0xBFFEFFFF (unmapped)
0xBFFFF000            user stack page (private, PAGE_USER | PAGE_WRITE)
0xC0000000+           kernel heap region (shared, supervisor-only)
```

## CR3 transitions

```text
boot               → kernel_page_directory (CR3 set by paging_init)
runelf launch      → process PD (paging_switch before iret)
sys_exit           → kernel_page_directory (paging_switch in elf_process_exit,
                     before process_destroy touches any kernel structures)
```

Always switch CR3 back to the kernel PD **before** freeing the process PD.

---

## Disk Image Layout

```text
LBA 0         boot.bin         (512 bytes)
LBA 1–4       loader2.bin      (2048 bytes)
LBA 5+        kernel.bin       (padded to 512-byte boundary)
after kernel  ramdisk.rd
```

---

## Current Limitations

* No scheduler — shell blocks during program execution
* ELF programs linked at fixed address 0x400000 — no PIE/relocation
* No filesystem — programs must be in ramdisk at build time
* Bump allocator has no free — permanent kernel structures only

---

## Direction

Next steps:

* Preemptive scheduler — timer IRQ context switch using per-process kernel stacks and `process_t`
* Filesystem-backed storage (FAT12 or custom FS via ATA PIO)

---

## License

Personal / educational project.