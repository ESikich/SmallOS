# User Runtime

This document describes the user-space runtime contract for SmallOS ELF
programs. It covers the boundary between raw syscalls, POSIX-like wrappers,
stdio streams, directory traversal, and the hosted-ish expectations used by
TinyCC.

---

# Runtime Layers

SmallOS user programs are freestanding ELF binaries linked with the SmallOS
runtime objects from `src/user/`.

The layers are:

```text
program code
  -> POSIX-ish wrappers: open/read/write/stat/access/realpath/opendir/fopen
  -> raw syscall helpers in user_syscall.h
  -> int 0x80 syscall ABI
  -> kernel syscall dispatcher
  -> VFS / FAT16 / console / socket backends
```

Programs may call raw `sys_*` helpers directly, but higher-level code should
prefer the POSIX-like wrappers when it wants normal `errno` behavior.

---

# Syscalls And Errno

Raw syscalls return SmallOS kernel results directly:

- non-negative values mean success
- negative values mean `-errno`

For example, `sys_open("missing")` returns `-ENOENT`.

POSIX-like wrappers translate raw syscall errors to:

```text
return -1
errno = positive errno value
```

For example, `open("missing", O_RDONLY)` returns `-1` and sets
`errno = ENOENT`.

This split is intentional. Low-level probes such as `ptrguard` and `fileread`
exercise raw syscall results, while hosted-ish code such as TinyCC relies on
the wrapper convention.

---

# Cwd And Paths

Each process has a current working directory stored in its `process_t`.

Kernel path-taking syscalls resolve paths against that cwd before touching the
filesystem. Absolute paths start at the FAT16 root. Relative paths start at the
calling process cwd.

Path normalization supports:

- repeated separators
- `.`
- `..`
- root clamping when `..` would walk above `/`

Userland `realpath(path, resolved)` mirrors this normalization for programs
that compare paths before opening them. It returns canonical absolute paths
with a leading slash, such as:

```text
cwd: /apps/demo
realpath("../demo/./hello.elf") -> /apps/demo/hello.elf
```

Current runtime limits are intentionally small:

- canonical paths fit in 128 bytes
- components fit in 31 bytes
- normalization supports up to 16 path components

---

# File Descriptors

Every process has a fixed descriptor table in `process_t`.

Descriptor layout:

```text
0  stdin   console read
1  stdout  console write
2  stderr  console write
3+ user-opened files or sockets
```

The kernel dispatches descriptors through per-handle ops:

```text
read / write / seek / poll / flush / close
```

FAT16-backed file descriptors support:

- read-only opens
- write/create/truncate opens
- append opens
- read/write opens
- seek
- flush
- close-time writeback

The user-visible fd API is preserved while FAT writes are still cache-backed
internally. Flushes stream sectors from those cache pages into FAT16 rather
than requiring one contiguous kernel writeback buffer.

---

# POSIX-Like Wrappers

The runtime provides a small POSIX-shaped surface:

- `open`, `close`
- `read`, `write`
- `lseek`
- `stat`, `lstat`, `fstat`
- `access`
- `unlink`, `remove`
- `rename`
- `mkdir`, `rmdir`
- `getcwd`, `chdir`
- socket and polling wrappers used by guest services

`access(path, mode)` validates mode bits and checks existence through
`SYS_STAT`. SmallOS does not currently model Unix permission bits, so `R_OK`,
`W_OK`, and `X_OK` are existence/type checks rather than permission checks.

---

# Stdio

`FILE` streams are fd-backed runtime objects.

Supported operations include:

- `fopen`, `fdopen`, `freopen`, `fclose`
- `fread`, `fwrite`
- `fgetc`, `fgets`, `getc`, `getchar`
- `fputc`, `fputs`, `putchar`, `puts`
- `ungetc`
- `fflush`
- `feof`, `ferror`, `clearerr`
- `fseek`, `ftell`
- `printf` family helpers

The implementation is deliberately unbuffered at the user-runtime level:
`fread` and `fwrite` call into fd syscalls directly. Stream state is still
tracked normally:

- EOF is set when a read attempts to read past the end of file.
- Error is set for invalid stream operations and syscall failures.
- `clearerr` clears both EOF and error bits.
- `ungetc` clears EOF for the pushed-back byte.

`fflush(stream)` calls `SYS_FSYNC` for writable file streams. That makes it
meaningful even though userland stdio is unbuffered: it asks the kernel VFS
layer to flush any dirty file cache to FAT16. Console streams treat `fflush`
as success.

---

# Directory Traversal

Directory traversal is provided through:

```c
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
```

The implementation uses `SYS_STAT` to validate that `opendir` targets an
existing directory, then uses `SYS_DIRLIST` for iteration.

Current `struct dirent` contains:

```c
char d_name[NAME_MAX + 1];
unsigned int d_size;
int d_is_dir;
```

FAT16 display names follow the existing filesystem presentation rules:

- short 8.3 names are returned in display form
- directory names include a trailing `/`
- iteration returns `NULL` at EOF
- invalid handles set `errno = EBADF`

The FTP compatibility layer uses the same public directory runtime, so FTP
directory listing behavior should stay aligned with `dirprobe`.

---

# TinyCC Expectations

`tools/tcc.elf` is built from vendored TinyCC sources plus the SmallOS runtime.
It links `src/user/user_crt0.c`, so the kernel still enters `_start(argc, argv)`
while TinyCC itself runs through its upstream `main(argc, argv)` path. It relies
on normal runtime behavior:

- cwd-aware file opens
- normalized path handling through `realpath`
- fd-backed stdio streams
- meaningful `fflush`
- directory traversal through `opendir` / `readdir`
- FAT16 writeback through normal fd writes and flush/close

The TinyCC acceptance gate is the guest compiler suite:

```text
tinycc_math
tinycc_agg
tinycc_tree
tinycc_compile
```

Any runtime or filesystem change should keep those tests passing unless the
change explicitly updates the TinyCC contract and tests in the same patch.

---

# Tests

Runtime coverage currently lives in guest ELF probes:

- `fileread` - raw fd read/EOF/bad-fd behavior
- `fileprobe` - POSIX open modes, write/readback, seek, append, rename/delete
- `cwdprobe` - process cwd, relative opens, `realpath`, `access`
- `statprobe` - `SYS_STAT`, POSIX `stat`, `access`
- `stdioprobe` - EOF/error state, `clearerr`, `fflush`, invalid stdio ops
- `dirprobe` - root and nested directory iteration, EOF, invalid/missing dirs
- `errnoprobe` - wrapper `errno` behavior

Run the full acceptance suite with:

```bash
make test
```

For docs-only changes, a lightweight sanity check is:

```bash
make
rg "user-runtime" docs
rg "SYS_FSYNC|stdio|dirent|TinyCC" docs/user-runtime.md
```
