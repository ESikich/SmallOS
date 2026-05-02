# Filesystem

This document defines how the system stores, discovers, reads, and writes files
on disk.

The current implementation stores a **raw FAT16 volume inside an MBR-partitioned disk image**. Sector 0 contains the partition table, and the FAT16 partition starts immediately after the kernel region. The runtime resolves nested FAT16 paths for reads and directory listings, and it can also create/remove directories in place. Regular file writes now work at nested paths too, and `rm` removes files in place. `cat` prints file contents, `touch` creates or truncates files, and the shell keeps a working directory for `cd` / `pwd` and `ls`. `ls` also accepts simple `*` and `?` wildcards, while still sorting directories before files.
The filesystem layer also exposes file metadata through `stat`, and the fd
path now routes file, socket, and console descriptors through a generic
per-process handle table. FAT16-backed file handles and path operations are
wrapped by the small kernel VFS layer in `src/kernel/vfs.c`, which currently
maps directly onto the FAT16 driver. Writable handles support `rename`,
`unlink`, `lseek`, streaming `write`, and flush/close operations for
toolchain-style programs.

---

# Overview

The final disk image is:

```text
LBA 0         boot sector + MBR partition table
LBA 1–4       stage 2 loader
LBA 5+        kernel_padded.bin
LBA 5+ks      FAT16 partition   (raw FAT16 volume data)
```

where:

```text
ks = ceil(kernel.bin / 512)
fat16_lba = 5 + ks
```

The FAT16 start LBA is **not** compiled into the kernel. The Makefile computes it after the kernel size is known, writes it into partition entry 1, and `fat16_init()` reads the partition table back at boot.

---

# On-Disk Layout

`build/bin/fat16.seed.img` is built by `tools/mkfat16.c` as a raw 16 MB FAT16
volume. Normal runs copy that seed to `.state/fat16.img`, which is the mutable
volume appended to `os-image.bin`.

## Fixed geometry

```text
sector size           512 bytes
sectors per cluster   4
cluster size          2048 bytes
reserved sectors      4
number of FATs        2
sectors per FAT       32
root entries          512
root dir sectors      32
volume size           32768 sectors (16 MB)
```

## Internal FAT16 layout

Relative to the start of the FAT16 volume:

```text
Sector   0        FAT16 boot sector / BPB
Sectors  1–3      reserved
Sectors  4–35     FAT 1
Sectors 36–67     FAT 2
Sectors 68–99     root directory
Sectors 100+      data region
```

Cluster numbering follows normal FAT16 rules:

```text
cluster 2 = first data cluster
cluster N = data sector 100 + (N - 2) * 4
```

This layout must match both:
- `tools/mkfat16.c`
- `src/drivers/fat16.c`

Any change to one without the other breaks file lookup.

---

# How the FAT16 Volume Gets Into the Image

The image build contract is:

1. build `kernel.bin`
2. pad it to a 512-byte boundary as `kernel_padded.bin`
3. compute `kernel_sectors = ceil(kernel.bin / 512)`
4. compute `fat16_lba = 5 + kernel_sectors`
5. build `build/bin/fat16.seed.img` and refresh `.state/fat16.img` when needed
6. assemble:

```text
os-image.bin = boot.bin + loader2.bin + kernel_padded.bin + .state/fat16.img
```

## Why kernel padding matters

The FAT16 volume is addressed in sectors. If the kernel were appended without 512-byte padding, the computed `fat16_lba` would no longer point at the real start of the FAT16 boot sector and all filesystem reads would be offset into the wrong bytes.

Padding the kernel is therefore a hard requirement, not an optimization.

---

# FAT16 LBA Entry

The kernel cannot know the FAT16 start LBA at compile time because it depends on the final kernel size.

Instead, the Makefile writes a little-endian `u32` into:

```text
sector 0, partition entry 1
```

That location is in the MBR partition table and does not overlap the boot signature at bytes 510–511.

At runtime:

1. `ata_init()` brings up the ATA PIO driver
2. `fat16_init()` reads ATA sector 0
3. `fat16_init()` extracts the FAT16 LBA from partition entry 1
4. `fat16_init()` reads the FAT16 boot sector at that LBA
5. `fat16_init()` validates the BPB geometry

If the partition entry is missing or malformed, `fat16_init()` prints:

```text
fat16: partition entry not populated
```

---

# Runtime Initialization

The kernel boot path calls:

```text
ata_init()
fat16_init()
```

in that order.

## `ata_init()`

The ATA driver is:
- primary channel only
- master drive only
- polling PIO only
- 28-bit LBA only
- blocking

It performs a software reset and waits for the drive to become ready.

## `fat16_init()`

`fat16_init()` is the one-time mount/validation step. It does **not** accept an LBA argument.

It validates these BPB fields against the hardcoded geometry in `fat16.c`:

- bytes per sector = 512
- sectors per cluster = 4
- reserved sectors = 4
- number of FATs = 2
- root entry count = 512
- FAT size = 32 sectors
- boot signature = `0x55AA`

On success it sets internal state and prints:

```text
fat16: ok  lba=<value>
```

On failure it leaves the driver unusable.

---

# What the Driver Supports

The current FAT16 driver is intentionally narrow.

## Supported

- read access to root-directory and nested-directory files
- raw file display via `cat`; CRLF text relies on terminal `\r` handling instead of file-content normalization
- shell working-directory navigation via `cd` / `pwd`
- directory listing by path with `fat16_ls_path(path)` and `fsls [path]`
- wildcard shell listing with `ls [pattern]`
- directory creation/removal by path with `fat16_mkdir(path)` / `fat16_rmdir(path)`
- file removal by path with `fat16_rm(path)`
- file metadata queries by path with `fat16_stat(path, ...)`
- empty-file creation / truncation via `touch`
- root-directory file creation and overwrite via `fat16_write(name, ...)`
- nested-path file creation and overwrite via `fat16_write_path(path, ...)`
- VFS-backed writable file handles with streaming FAT16 writes via `SYS_OPEN_WRITE`, `SYS_WRITEFD`, `SYS_LSEEK`, and `SYS_FSYNC`
- file removal and rename/move through `SYS_UNLINK` and `SYS_RENAME`
- fd-backed console handles for stdin/stdout/stderr
- socket-backed handles for the current minimal TCP stream path via `SYS_SOCKET`, `SYS_BIND`, `SYS_LISTEN`, `SYS_ACCEPT`, `SYS_SEND`, `SYS_RECV`, `SYS_POLL`, `SYS_SETSOCKOPT`, and `SYS_GETSOCKNAME`
- case-insensitive 8.3 filename matching
- FAT chain following for file reads
- loading one file at a time into a shared static buffer

## Not supported

- long filenames (LFN)
- multiple concurrent file buffers
- arbitrary transport stacks beyond the current minimal TCP stream path
- mounting arbitrary FAT layouts

Directory scan code explicitly skips:
- deleted entries (`0xE5`)
- long filename entries (`attr == 0x0F`)
- volume label entries (`attr & 0x08`)

---

# Filename Rules

`fat16_load(name, &size)` resolves each path component from the root directory
using uppercase 8.3 semantics.

Examples that match the same file:

```text
hello
HELLO
hello.elf
HELLO.ELF
```

Internally the driver:
- uppercases the requested name
- splits at the last `.`
- pads base name and extension with spaces to 11 bytes
- compares against the 11-byte FAT directory entry name
- resolves path components one at a time before applying the 8.3 match

Path components such as `apps/demo/hello.elf` therefore work even though each
component is still matched with FAT16 8.3 rules.

Practical limits:
- base name truncated to 8 characters for matching
- extension truncated to 3 characters for matching
- nested directories are supported for reads and listings, but regular file
  writes can target either the root directory or a nested path

---

# Reading a File

The read path for `fat16_load()` is:

```text
fat16_load(name, &size)
  ↓
resolve path components from the root directory downward
  ↓
find matching 8.3 entry
  ↓
read start cluster and file size
  ↓
follow FAT chain through FAT 1
  ↓
read each cluster via ATA PIO
  ↓
copy file bytes into static s_load_buf
  ↓
return pointer to s_load_buf and set *out_size
```

## Buffer ownership

`fat16_load()` returns a pointer into a **single static internal buffer**:

```text
s_load_buf[1 MB]
```

This buffer:
- lives in BSS
- is reused on every `fat16_load()` call
- must not be freed by the caller
- must not be assumed stable across another filesystem load

Code that needs to read a file without flattening it into this 1 MB buffer can
use the sink-oriented or read-at paths. VFS keeps the PMM page cache for small
fd reads, then falls back to direct FAT16 read-at for files larger than the fd
cache.

The driver also uses a separate static cluster scratch buffer so it does not
need a large stack allocation while copying cluster data.

## Size limit

The whole-file load helper rejects files larger than:

```text
1 MB
```

If a file is larger, `fat16_load()` fails with:

```text
fat16: file too large
```

Small fd reads can currently use a page-backed cache up to:

```text
4 MB
```

That fd cache is page-backed, not one contiguous kernel buffer. Larger fd reads
stream from FAT16 sectors directly. Fd writes no longer require caching the
whole file; practical write size is bounded by FAT/free space and the current
15 MB safety limit for this 16 MB test volume.

## Empty files

The current implementation supports zero-length regular files. `touch` uses
the normal write path with size `0`, records a directory entry with no starting
cluster, and readback reports a zero-byte file.

---

# Writing a File

`fat16_write(name, data, size)` creates or overwrites a root-directory file from
a contiguous buffer. The shell's `touch` and compiler-style write paths use
`fat16_write_path(path, ...)` when they need to target nested directories.
Fd-backed writes use `fat16_write_at_path(...)`, writing user chunks directly to
the target file offset without first caching the whole file. The older
`fat16_write_path_from_source(...)` whole-file rewrite path remains for simple
path helpers such as `touch`, `cp`, and `SYS_WRITEFILE_PATH`.

The FAT16 write path is intentionally narrow:

- 8.3 filename matching
- fd writes stream through FAT16 write-at and update the descriptor size/offset
- FAT16 data reads can stream into a caller-provided sink callback
- partial-sector writes read/patch/write existing sectors so surrounding bytes are preserved
- seek-past-EOF writes zero-fill the gap before writing new data
- no long filenames
- no concurrent writer support

At runtime, the kernel:

1. loads FAT1 and the root directory into temporary working buffers
2. resolves the destination entry or finds a free slot in the target parent
3. allocates additional clusters only when the write extends the chain
4. writes full sectors directly and patches partial sectors in place
5. commits only the FAT sectors that changed and the directory sector containing the file entry

This small VFS boundary is enough for compiler-style tools to emit generated artifacts such as `compiler.out` without exposing FAT16 details to every syscall path.
It is also enough for SmallOS-hosted compiler binaries to create temp files, rename outputs into place, and inspect candidate paths before writing. The next filesystem rewrite can grow this boundary into mount-style backends without pushing that complexity back into `syscall.c`.

## Creating and Removing Directories

`fat16_mkdir(path)` creates a new empty directory entry and writes the
initial `.` / `..` records into a fresh data cluster. `fat16_rmdir(path)`
removes an existing empty directory entry.

Rules:

- directories use the same path-aware 8.3 lookup as reads and listings
- the target must not already exist for `mkdir`
- `rmdir` only succeeds on empty directories
- the root directory cannot be removed
- directory entries are deleted with the standard FAT16 `0xE5` marker

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

This is safe with the static FAT16 buffer because `elf_run_image()` copies ELF segment data out of `s_load_buf` into PMM-backed frames before entering ring 3.

That means the filesystem buffer can be reused after the load path completes.

Important invariant:

- callers may use the returned pointer only as a temporary source buffer
- no code may keep that pointer as persistent file-backed storage

---

# Root Directory Listing

`fat16_ls()` prints all non-empty root-directory entries, grouped with
directories first and files second.

For each file it prints:

```text
<NAME.EXT>  <size> bytes  cluster <start_cluster>
```

`fat16_ls_path(path)` descends into nested directories before listing. The
listing is derived directly from directory entries and keeps the same
directory-first alphabetical order. The shell `ls` command can also apply a
wildcard pattern after resolving the path prefix.

---

# Failure Modes

## `fat16: not initialised`

A filesystem API was called before `fat16_init()` succeeded.

## `fat16: cannot read sector 0`

ATA could not read the image boot sector, so the FAT16 start LBA could not be discovered.

## `fat16: bad MBR signature`

The disk image does not contain the expected MBR signature at the end of sector 0.

## `fat16: MBR partition type mismatch`

The FAT16 partition entry does not have the expected partition type.

## `fat16: partition entry not populated`

The FAT16 partition entry is empty or has a zero start LBA.

## `fat16: cannot read FAT16 boot sector`

The discovered FAT16 LBA does not point at a readable FAT16 volume.

## `fat16: bad boot signature`

The sector at `fat16_lba` does not end with `0x55AA`.

## `fat16: BPB mismatch`

The FAT16 image was built with geometry that does not match `fat16.c`.

## `fat16: not found: <name>`

No matching 8.3 entry exists for the requested path component chain.

## `fat16: already exists: <name>`

`mkdir` was asked to create a directory that is already present.

## `fat16: directory full`

The parent directory has no free slot for a new entry.

## `fat16: directory not empty`

`rmdir` was asked to remove a directory that still contains files or subdirectories.

## `fat16: cannot remove root`

`rmdir` was asked to remove the root directory or a root-like path.

## `fat16: invalid directory name`

The final path component is not a valid 8.3 directory name.

## `fat16: dir read error` / `fat16: cluster read error` / `fat16: FAT read error`

ATA read failed during directory scan, cluster read, or FAT traversal.

## `fat16: chain ended early`

The directory entry file size requires more data than the FAT chain provides. The volume is malformed or inconsistent.

---

# Code Ownership / Source of Truth

The filesystem behavior is jointly defined by these files:

- `tools/mkfat16.c` — host-side FAT16 image builder
- `src/drivers/ata.[ch]` — ATA PIO sector reads
- `src/drivers/fat16.[ch]` — FAT16 runtime driver
- `src/kernel/vfs.[ch]` — kernel VFS shim for FAT16-backed file handles and path operations
- `Makefile` — image layout, kernel padding, FAT16 LBA patch
- `src/exec/elf_loader.c` — runtime ELF loader that reads program images through `vfs_load_file()`

When changing the filesystem, check all of them together.

---

# Non-Negotiable Invariants

The following must stay true unless the implementation is changed everywhere:

- FAT16 volume starts at `5 + kernel_sectors`
- kernel image is padded to a 512-byte boundary before FAT16 is appended
- FAT16 start LBA is patched into sector 0 offset 504
- `fat16_init()` reads sector 0 to discover the start LBA at runtime
- FAT16 geometry in `fat16.c` matches `mkfat16.c`
- `vfs_load_file()` currently returns the FAT16 driver's reused static buffer
- callers copy data out before another file load occurs
- nested reads/listings use path-aware 8.3 lookup; regular file writes can target the root or a nested path
- fd-backed writes use FAT16 write-at paths and do not cache the whole output file before committing sectors

Breaking any of these produces either immediate mount failure or silent file corruption.
