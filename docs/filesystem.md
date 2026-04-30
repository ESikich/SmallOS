# Filesystem

This document defines how the system stores, discovers, and reads files from disk.

The current implementation is a **raw FAT16 volume** appended directly to the OS image. There is no partition table, no subdirectory traversal, and no VFS layer. Root-directory file writes are supported for compiler-style output and similar generated artifacts.

---

# Overview

The final disk image is:

```text
LBA 0         boot sector for stage 1 bootloader
LBA 1–4       stage 2 loader
LBA 5+        kernel_padded.bin
LBA 5+ks      fat16.img   (raw FAT16 volume, no partition table)
```

where:

```text
ks = ceil(kernel.bin / 512)
fat16_lba = 5 + ks
```

The FAT16 start LBA is **not** compiled into the kernel. The Makefile computes it after the kernel size is known, patches it into disk sector 0 at byte offset 504, and `fat16_init()` reads it back at boot.

---

# On-Disk Layout

`fat16.img` is built by `tools/mkfat16.c` as a raw 16 MB FAT16 volume.

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
5. build `fat16.img`
6. assemble:

```text
os-image.bin = boot.bin + loader2.bin + kernel_padded.bin + fat16.img
```

## Why kernel padding matters

The FAT16 volume is addressed in sectors. If the kernel were appended without 512-byte padding, the computed `fat16_lba` would no longer point at the real start of the FAT16 boot sector and all filesystem reads would be offset into the wrong bytes.

Padding the kernel is therefore a hard requirement, not an optimization.

---

# FAT16 LBA Patch

The kernel cannot know the FAT16 start LBA at compile time because it depends on the final kernel size.

Instead, the Makefile patches a little-endian `u32` into:

```text
sector 0, byte offset 504
```

That location is in the zero-padded tail of the boot sector and does not overlap the boot signature at bytes 510–511.

At runtime:

1. `ata_init()` brings up the ATA PIO driver
2. `fat16_init()` reads ATA sector 0
3. `fat16_init()` extracts the FAT16 LBA from byte offset 504
4. `fat16_init()` reads the FAT16 boot sector at that LBA
5. `fat16_init()` validates the BPB geometry

If the patched value is zero, `fat16_init()` prints:

```text
fat16: LBA not patched (zero)
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

- read and write access to root-directory files
- root directory scan
- case-insensitive 8.3 filename matching
- FAT chain following for file reads
- root-directory file creation and overwrite
- loading one file at a time into a shared static buffer
- listing root-directory files with `fat16_ls()`

## Not supported

- subdirectories
- long filenames (LFN)
- multiple concurrent file buffers
- general-purpose file descriptors
- mounting arbitrary FAT layouts

Directory scan code explicitly skips:
- deleted entries (`0xE5`)
- long filename entries (`attr == 0x0F`)
- volume label entries (`attr & 0x08`)

---

# Filename Rules

`fat16_load(name, &size)` matches names against root directory entries using uppercase 8.3 semantics.

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

Practical limits:
- base name truncated to 8 characters for matching
- extension truncated to 3 characters for matching
- only the root directory is searched

---

# Reading a File

The read path for `fat16_load()` is:

```text
fat16_load(name, &size)
  ↓
scan root directory sectors
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
s_load_buf[256 KB]
```

This buffer:
- lives in BSS
- is reused on every `fat16_load()` call
- must not be freed by the caller
- must not be assumed stable across another filesystem load

The driver also uses a separate static cluster scratch buffer:

```text
s_cluster_buf[2048]
```

so it does not need a large stack allocation while copying cluster data.

## Size limit

The driver rejects files larger than:

```text
256 KB
```

If a file is larger, `fat16_load()` fails with:

```text
fat16: file too large
```

## Empty files

The current implementation rejects zero-length files:

```text
fat16: file is empty
```

That is a behavior choice in the current code, not a FAT16 requirement.

---

# Writing a File

`fat16_write(name, data, size)` creates or overwrites a root-directory file in one shot.

The write path is intentionally narrow:

- root directory only
- 8.3 filename matching
- no subdirectories
- no long filenames
- no concurrent writer support

At runtime, the kernel:

1. loads FAT1 and the root directory into temporary working buffers
2. resolves the destination root entry or finds a free slot
3. allocates enough free clusters for the new file contents
4. writes the data clusters to disk
5. commits the updated FAT copies and root directory entry

This is enough for compiler-style tools to emit generated artifacts such as `compiler.out` without a full VFS or buffered fd write API.

---

# ELF Program Loading Contract

The main consumer of the filesystem today is the ELF loader.

Launch path:

```text
runelf <name>
  ↓
elf_run_named(name, argc, argv)
  ↓
fat16_load(name, &size)
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

`fat16_ls()` prints all non-empty root-directory entries.

For each file it prints:

```text
<NAME.EXT>  <size> bytes  cluster <start_cluster>
```

The listing is derived directly from directory entries. It is not sorted and it does not descend into subdirectories.

---

# Failure Modes

## `fat16: not initialised`

A filesystem API was called before `fat16_init()` succeeded.

## `fat16: cannot read sector 0`

ATA could not read the image boot sector, so the FAT16 start LBA could not be discovered.

## `fat16: LBA not patched (zero)`

The Makefile patch step did not run or wrote the wrong bytes.

## `fat16: cannot read FAT16 boot sector`

The discovered FAT16 LBA does not point at a readable FAT16 volume.

## `fat16: bad boot signature`

The sector at `fat16_lba` does not end with `0x55AA`.

## `fat16: BPB mismatch`

The FAT16 image was built with geometry that does not match `fat16.c`.

## `fat16: not found: <name>`

No matching 8.3 root-directory entry exists.

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
- `Makefile` — image layout, kernel padding, FAT16 LBA patch
- `src/exec/elf_loader.c` — main runtime consumer of `fat16_load()`

When changing the filesystem, check all of them together.

---

# Non-Negotiable Invariants

The following must stay true unless the implementation is changed everywhere:

- FAT16 volume starts at `5 + kernel_sectors`
- kernel image is padded to a 512-byte boundary before FAT16 is appended
- FAT16 start LBA is patched into sector 0 offset 504
- `fat16_init()` reads sector 0 to discover the start LBA at runtime
- FAT16 geometry in `fat16.c` matches `mkfat16.c`
- `fat16_load()` returns a pointer into a reused static buffer
- callers copy data out before another file load occurs
- only root-directory 8.3 lookup is supported

Breaking any of these produces either immediate mount failure or silent file corruption.
