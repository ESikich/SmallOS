# Filesystem

This document defines how the system stores, discovers, reads, and writes files
on disk.

The current implementation stores a **raw ext2 volume inside an MBR-partitioned disk image**. Sector 0 contains the partition table, and the ext2 partition starts immediately after the kernel region. The runtime resolves nested ext2 paths for reads and directory listings, and it can also create/remove directories in place. Regular file writes now work at nested paths too, and `rm` removes files in place. `cat` prints file contents, `touch` creates or truncates files, `edit` opens a full-screen text editor for ext2 files, and the shell keeps a working directory for `cd` / `pwd` and `ls`. `ls` also accepts simple `*` and `?` wildcards, while still sorting directories before files.
The filesystem layer also exposes file metadata through `stat`, and the fd
path now routes file, socket, and console descriptors through a dynamic generic
per-process handle table. ext2-backed file handles and path operations are
wrapped by the small kernel VFS layer in `src/kernel/vfs.c`, which currently
maps directly onto the ext2 driver. Writable handles support `rename`,
`unlink`, `lseek`, streaming `write`, and flush/close operations for
toolchain-style programs.

---

# Overview

The final disk image is:

```text
LBA 0                     boot sector + MBR partition table
LBA 1 ... loader2_sectors stage 2 loader
LBA kernel_lba ... N      padded kernel
LBA N+1 ...               ext2 partition   (raw ext2 volume data)
```

where:

```text
loader2_sectors = loader2.bin / 512
kernel_lba = 1 + loader2_sectors
kernel_sectors = ceil(kernel.bin / 512)
ext2_lba = kernel_lba + kernel_sectors
```

The ext2 start LBA is **not** compiled into the kernel. The Makefile computes it after the kernel size is known, writes it into partition entry 1, and the selected block-device path reads the partition table back at boot.

---

# On-Disk Layout

`build/bin/ext2.seed.img` is built by `tools/mkext2.c` as a raw 16 MB ext2
volume. Normal runs copy that seed to `.state/ext2.img`, which is the mutable
volume appended to `smallos.img`. The seed includes `/var/log/boot.txt` from
`samples/boot.txt` so the kernel can persist boot diagnostics by overwriting an
existing file after `ext2_init()`.

## Fixed geometry

```text
sector size           512 bytes
block size           4096 bytes
sectors per block       8
volume size          4096 blocks / 32768 sectors (16 MB)
inode size            128 bytes
inode count           256
root inode              2
first data block       12
partition type       0x83
superblock magic   0xEF53
```

## Internal ext2 layout

Relative to the start of the ext2 volume:

```text
byte offset 1024    ext2 superblock
block 1             block group descriptor table
block 2             block bitmap
block 3             inode bitmap
blocks 4-11         inode table
block 12+           file and directory data blocks
```

Block numbering follows normal ext2 rules:

```text
absolute LBA = ext2_lba + block * 8
block 12     = first allocatable data block
```

This layout must match both:
- `tools/mkext2.c`
- `src/drivers/ext2.c`

Any change to one without the other breaks file lookup.

---

# How the ext2 Volume Gets Into the Image

The image build contract is:

1. build `kernel.bin`
2. pad it to a 512-byte boundary as `kernel_padded.bin`
3. compute `kernel_sectors = ceil(kernel.bin / 512)`
4. compute `ext2_lba = kernel_lba + kernel_sectors`
5. build `build/bin/ext2.seed.img` and refresh `.state/ext2.img` when needed
6. assemble:

```text
smallos.img = boot.bin + loader2.bin + kernel_padded.bin + .state/ext2.img
```

## Why kernel padding matters

The ext2 volume is addressed through 512-byte block-device sectors even though
allocation uses 4 KiB blocks. If the kernel were appended without 512-byte
padding, the
computed `ext2_lba` would no longer point at the real start of the ext2 volume
and all filesystem reads would be offset into the wrong bytes.

Padding the kernel is therefore a hard requirement, not an optimization.

---

# ext2 LBA Entry

The kernel cannot know the ext2 start LBA at compile time because it depends on the final kernel size.

Instead, the Makefile writes a little-endian `u32` into:

```text
sector 0, partition entry 1
```

That location is in the MBR partition table and does not overlap the boot signature at bytes 510–511.

At runtime on sector-backed disks:

1. `ata_init()` brings up the ATA driver, including bus-master DMA when available
2. `ext2_init()` reads sector 0 from the selected block device
3. `ext2_init()` extracts the ext2 LBA from partition entry 1
4. `ext2_init()` reads the ext2 superblock from that partition
5. `ext2_init()` validates the ext2 superblock and fixed geometry

When ATA is unavailable, or when ATA resets successfully but ext2 cannot be
mounted from sector 0, the kernel tries USB mass storage through the generic
block-device interface. The USB path exposes the first supported OHCI
Bulk-Only Transport device as `usb0` and mounts it read-only. If USB storage
also fails, the kernel retries with the loader2-published boot RAM fallback when
one exists. The RAM-backed path is for marginal storage hardware and non-USB
BIOS disks; it accepts in-memory writes, but those writes are not persisted back
to the disk image. The default `BOOT_RAMDISK_FALLBACK=never` build policy skips
the loader2 fallback preload for normal VM/IDE boots. `BOOT_RAMDISK_FALLBACK=auto`
preloads only when EDD does not identify the boot drive as USB or ATA.

The explicit USB build/run targets force `BOOT_RAMDISK_FALLBACK=always` so real
USB images still boot when protected-mode USB storage cannot validate ext2 on a
specific controller.

If the partition entry is missing or malformed, `ext2_init()` prints:

```text
ext2: partition entry not populated
```

---

# Runtime Initialization

The kernel boot path calls:

```text
ata_init()
pci_init()
nic_init()
dhcp_configure()
tcp_init()
ntp_sync()   # best-effort CLOCK_REALTIME setup; warning-only on failure
ext2_init()
```

in that order. The DHCP-provided network config is runtime state; after boot,
`/bin/ip.elf` and `/bin/ipconfig.elf` can inspect or replace it without writing
anything to the filesystem. The storage policy tries ATA first, USB mass storage
second, and the loader2-published boot RAM fallback last. During the early
storage probe, only timer IRQ0 is temporarily unmasked so boot timestamps and
USB/OHCI waits advance without letting keyboard/mouse/process IRQ paths run
before the scheduler has a current task.

## `ata_init()`

The ATA driver is:
- primary channel only
- master drive only
- bus-master DMA when available, with polling PIO fallback
- 28-bit LBA only
- blocking

It performs a software reset, waits for the drive to become ready, and returns
failure if the ready poll times out.

## `ext2_init()`

`ext2_init()` is the one-time mount/validation step. It does **not** accept an LBA argument.

It validates the MBR partition entry and ext2 superblock against the hardcoded
geometry in `ext2.c`:

- MBR signature = `0x55AA`
- partition entry 1 type = `0x83`
- partition entry 1 has a non-zero LBA and sector count
- superblock magic = `0xEF53`
- block size = 4096 bytes
- inode size = 128 bytes
- inode count = 256
- root inode = 2

On success it sets internal state and prints:

```text
ext2: ok  lba=<value> dev=<device>
```

On failure it leaves the driver unusable.

---

# What the Driver Supports

The current ext2 driver is intentionally narrow.

## Supported

- read access to root-directory and nested-directory files
- raw file display via `cat`; CRLF text relies on terminal `\r` handling instead of file-content normalization
- shell working-directory navigation via `cd` / `pwd`
- directory listing by path with `ext2_ls_path(path)` and `ls [path]`
- wildcard shell listing with `ls [pattern]`
- recursive directory display with `tree [path]`, including UTF-8 branch glyphs on the terminal path
- directory creation/removal by path with `ext2_mkdir(path)` / `ext2_rmdir(path)`
- file removal by path with `ext2_rm(path)`
- file metadata queries by path with `ext2_stat(path, ...)`
- empty-file creation / truncation via `touch`
- root-directory file creation and overwrite via `ext2_write(name, ...)`
- nested-path file creation and overwrite via `ext2_write_path(path, ...)`
- VFS-backed writable file handles with streaming ext2 writes via `SYS_OPEN_WRITE`, `SYS_WRITEFD`, `SYS_LSEEK`, and `SYS_FSYNC`
- file removal and rename/move through `SYS_UNLINK` and `SYS_RENAME`
- fd-backed console handles for stdin/stdout/stderr
- socket-backed handles for the current passive TCP stream path via `SYS_SOCKET`, `SYS_BIND`, `SYS_LISTEN`, `SYS_ACCEPT`, `SYS_ACCEPT4`, `SYS_SEND`, `SYS_RECV`, `SYS_POLL`, `SYS_SETSOCKOPT`, `SYS_SHUTDOWN`, `SYS_GETSOCKNAME`, and `SYS_GETPEERNAME`
- long, case-sensitive native ext2 names
- direct, single-indirect, and double-indirect block mapping
- loading one file at a time into a shared static buffer

## Not supported

- permission enforcement, ownership semantics, or timestamps
- multiple concurrent file buffers
- arbitrary transport stacks beyond the current passive TCP stream path
- mounting arbitrary ext2 layouts

Directory scan code skips entries with inode `0` and only exposes regular
files and directories.

---

# Filename Rules

`ext2_load(name, &size)` resolves each path component from the root directory
using ext2 native directory names. Names are long and case-sensitive.

Examples that are distinct names:

```text
hello
hello.elf
Hello.elf
HELLO.ELF
```

Internally the driver:
- splits paths into slash-separated components
- rejects empty components and names longer than 255 bytes
- compares each component byte-for-byte against ext2 directory entries
- uses the directory entry `file_type` field plus inode mode bits to distinguish files and directories

Path components such as `usr/bin/hello.elf` therefore work even though each
component is matched independently.

Practical limits:
- path strings copied through syscall buffers are bounded by the current kernel
  path buffer sizes
- each path component may be up to 255 bytes
- nested directories are supported for reads, writes, listings, rename, and removal

---

# Reading a File

The read path for `ext2_load()` is:

```text
ext2_load(name, &size)
  ↓
resolve path components from the root directory downward
  ↓
find matching native entry
  ↓
read start block and file size
  ↓
map logical file blocks through direct/single-indirect/double-indirect pointers
  ↓
read each 4 KiB block via block-device sectors
  ↓
copy file bytes into static s_load_buf
  ↓
return pointer to s_load_buf and set *out_size
```

## Buffer ownership

`ext2_load()` returns a pointer into a **single static internal buffer**:

```text
s_load_buf[1 MB]
```

This buffer:
- lives in BSS
- is reused on every `ext2_load()` call
- must not be freed by the caller
- must not be assumed stable across another filesystem load

Code that needs to read a file without flattening it into this 1 MB buffer can
use the sink-oriented or read-at paths. VFS keeps the PMM page cache for small
fd reads, then falls back to direct ext2 read-at for files larger than the fd
cache.

The driver also uses a separate static block scratch buffer so it does not
need a large stack allocation while copying block data.

## Size limit

The whole-file load helper rejects files larger than:

```text
1 MB
```

If a file is larger, `ext2_load()` fails with:

```text
ext2: file too large
```

Small fd reads can currently use a page-backed cache up to:

```text
4 MB
```

That fd cache is page-backed, not one contiguous kernel buffer. Larger fd reads
stream from ext2 blocks directly. Fd writes no longer require caching the
whole file; practical write size is bounded by ext2 free space and the current
15 MB safety limit for this 16 MB test volume.

## Empty files

The current implementation supports zero-length regular files. `touch` uses
the normal write path with size `0`, records a directory entry with no starting
block, and readback reports a zero-byte file.

---

# Writing a File

`ext2_write(name, data, size)` creates or overwrites a root-directory file from
a contiguous buffer. The shell's `touch` and compiler-style write paths use
`ext2_write_path(path, ...)` when they need to target nested directories.
Fd-backed writes use `ext2_write_at_path(...)`, writing user chunks directly to
the target file offset without first caching the whole file. The older
`ext2_write_path_from_source(...)` whole-file rewrite path remains for simple
path helpers such as `touch`, `cp`, and `SYS_WRITEFILE_PATH`.

The ext2 write path is intentionally narrow:

- native ext2 filename matching
- fd writes stream through ext2 write-at and update the shared file description size/offset
- ext2 data reads can stream into a caller-provided sink callback
- partial-block writes read/patch/write existing blocks so surrounding bytes are preserved
- seek-past-EOF writes zero-fill the gap before writing new data
- no concurrent writer support

At runtime, the kernel:

1. reads block and inode bitmaps into temporary working buffers
2. resolves the destination entry or finds a free slot in the target parent
3. allocates additional data or pointer blocks only when the write extends the file
4. writes full blocks directly and patches partial blocks in place
5. commits changed bitmaps, inodes, directory blocks, and file blocks

This small VFS boundary is enough for compiler-style tools to emit generated artifacts such as `/var/tmp/compiler.out` without exposing ext2 details to every syscall path.
It is also enough for SmallOS-hosted compiler binaries to create temp files, rename outputs into place, and inspect candidate paths before writing. The next filesystem rewrite can grow this boundary into mount-style backends without pushing that complexity back into `syscall.c`.

## Creating and Removing Directories

`ext2_mkdir(path)` creates a new empty directory entry and writes the
initial `.` / `..` records into a fresh data block. `ext2_rmdir(path)`
removes an existing empty directory entry.

Rules:

- directories use the same path-aware native ext2 lookup as reads and listings
- the target must not already exist for `mkdir`
- `rmdir` only succeeds on empty directories
- the root directory cannot be removed
- directory entries are deleted by clearing the ext2 directory entry inode field

---

# ELF Program Loading Contract

The main consumer of the filesystem today is the ELF loader.

Launch path:

```text
runelf <name>
  ↓
elf_run_named(name, argc, argv)
  ↓
vfs_load_file(name, &size)
  ↓
elf_run_image(image, argc, argv)
```

This is safe with the static ext2 buffer because `elf_run_image()` copies ELF segment data out of `s_load_buf` into PMM-backed frames before entering ring 3.

That means the filesystem buffer can be reused after the load path completes.

Important invariant:

- callers may use the returned pointer only as a temporary source buffer
- no code may keep that pointer as persistent file-backed storage

---

# Root Directory Listing

`ext2_ls()` prints all non-empty root-directory entries, grouped with
directories first and files second.

For each file it prints:

```text
<NAME.EXT>  <size> bytes  block <start_block>
```

`ext2_ls_path(path)` descends into nested directories before listing. The
listing is derived directly from directory entries and keeps the same
directory-first alphabetical order. The shell `ls` command can also apply a
wildcard pattern after resolving the path prefix.

---

# Failure Modes

## `ext2: not initialised`

A filesystem API was called before `ext2_init()` succeeded.

## `ext2: cannot read sector 0`

The selected block device could not read the image boot sector, so the ext2
start LBA could not be discovered.

## `ext2: bad MBR signature`

The disk image does not contain the expected MBR signature at the end of sector 0.

## `ext2: MBR partition type mismatch`

The ext2 partition entry does not have the expected partition type.

## `ext2: partition entry not populated`

The ext2 partition entry is empty or has a zero start LBA.

## `ext2: cannot read superblock`

The discovered ext2 LBA does not point at a readable ext2 volume.

## `ext2: bad superblock magic`

The discovered partition does not contain an ext2 superblock with magic
`0xEF53`.

## `ext2: unsupported geometry`

The ext2 image was built with geometry that does not match `ext2.c`.

## `ext2: not found: <name>`

No matching native entry exists for the requested path component chain.

## `ext2: already exists: <name>`

`mkdir` was asked to create a directory that is already present.

## `ext2: directory full`

The parent directory has no free slot for a new entry.

## `ext2: directory not empty`

`rmdir` was asked to remove a directory that still contains files or subdirectories.

## `ext2: cannot remove root`

`rmdir` was asked to remove the root directory or a root-like path.

## `ext2: invalid directory name`

The final path component is not a valid native directory name.

## `ext2: dir read error` / `ext2: block read error`

The selected storage backend failed during a directory scan or block read.

## `ext2: block map failed`

The file requires a logical block that cannot be resolved through its ext2
direct or indirect pointers. The volume is malformed or inconsistent.

---

# Code Ownership / Source of Truth

The filesystem behavior is jointly defined by these files:

- `tools/mkext2.c` — host-side ext2 image builder
- `src/drivers/ata.[ch]` — ATA sector reads/writes, DMA setup, and PIO fallback
- `src/drivers/ext2.[ch]` — ext2 runtime driver
- `src/kernel/vfs.[ch]` — kernel VFS shim for ext2-backed file handles and path operations
- `Makefile` — image layout, kernel padding, ext2 LBA patch
- `src/exec/elf_loader.c` — runtime ELF loader that reads program images through `vfs_load_file()`

When changing the filesystem, check all of them together.

---

# Non-Negotiable Invariants

The following must stay true unless the implementation is changed everywhere:

- ext2 volume starts at `kernel_lba + kernel_sectors`
- kernel image is padded to a 512-byte boundary before ext2 is appended
- ext2 partition metadata is written into MBR partition entry 1
- `ext2_init()` reads sector 0 to discover the start LBA at runtime
- ext2 geometry in `ext2.c` matches `mkext2.c`
- `vfs_load_file()` currently returns the ext2 driver's reused static buffer
- callers copy data out before another file load occurs
- nested reads/listings use path-aware native lookup; regular file writes can target the root or a nested path
- fd-backed writes require a writable ext2 source, use ext2 write-at paths, and do not cache the whole output file before committing sectors

Breaking any of these produces either immediate mount failure or silent file corruption.
