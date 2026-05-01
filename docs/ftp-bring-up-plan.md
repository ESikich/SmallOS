# SmallOS FTP Bring-Up Plan

This is the current handoff for the next implementation pass.

## Current State

- `make test` is green on the current tree.
- A minimal kernel TCP bring-up path already exists in `src/drivers/tcp.c` and `src/drivers/tcp.h`.
- The process/file layer has been refactored into a generic per-process handle seam in:
  - `src/kernel/process.c`
  - `src/kernel/process.h`
  - `src/kernel/syscall.c`
- The FTP daemon itself is not yet integrated.
- `third_party/ftp_server/` is present but intentionally untracked and should be treated as vendored input, not as the implementation target.

## Goal

Run a user-space FTP daemon in the guest with the OS changes limited to:

- kernel TCP/IP plumbing
- socket syscall ABI
- small user-space wrappers

Keep FTP logic in user space. Start with passive mode only.

## Rules

- Run `make test` after every major layer.
- Do not mix kernel networking changes and FTP daemon porting in the same change set.
- If a layer breaks the suite, stop there and fix it before moving on.
- Prefer the smallest possible implementation that proves the path end to end.

## Suggested Implementation Order

### Phase 1: TCP Kernel Path

Goal:

- Minimal TCP subsystem in the kernel.
- Wire IPv4 protocol 6 to TCP demux.
- Support:
  - listening sockets
  - connect
  - accept
  - send
  - recv
  - close
  - basic retransmit and timeout handling

Scope:

- One connection at a time is acceptable if it keeps the code simpler.

Suggested files:

- `src/drivers/tcp.c`
- `src/drivers/tcp.h`
- `src/drivers/ipv4.c`
- `src/drivers/ipv4.h`
- kernel init wiring

Acceptance:

- A tiny kernel or user-space echo test can accept one TCP connection and echo bytes back.
- QEMU hostfwd can reach that service from Windows.

Validation:

- `make test`

### Phase 2: Socket Syscall ABI

Goal:

- Add a small socket-style syscall surface.

Suggested files:

- `src/kernel/uapi_syscall.h`
- `src/kernel/syscall.c`
- `src/kernel/process.h`
- `src/kernel/process.c`
- `src/kernel/uapi_socket.h`
- `src/kernel/uapi_poll.h`

ABI to add:

- `socket`
- `bind`
- `listen`
- `accept`
- `connect`
- `send`
- `recv`
- `close`
- `poll`

Design rules:

- Keep socket state in per-process handle slots.
- Do not use giant kernel-stack structs.
- Return clear errors for unsupported cases.

Acceptance:

- A user program can open a TCP socket, bind, listen, accept, and exchange bytes.

Validation:

- `make test`

### Phase 3: User-Space Wrappers

Goal:

- Add minimal libc-style wrappers so user programs can use the socket ABI naturally.

Suggested files:

- `src/user/user_syscall.h`
- `src/user/user_posix.c`
- `src/user/sys/socket.h`
- `src/user/poll.h`
- `src/user/arpa/inet.h`

Helpers to include:

- dotted-quad IP parsing
- socket address structs
- basic polling wrappers

Acceptance:

- A user-space TCP echo server compiles and runs against the wrappers.

Validation:

- `make test`

### Phase 4: FTP Daemon Port

Goal:

- Port the FTP daemon to user space.
- Keep it passive mode only at first.

Important:

- Keep the server in user space.
- Avoid active FTP until everything else is stable.
- Use the existing filesystem syscalls for file operations.
- Keep the server logic itself unchanged if possible.

Core commands:

- `USER`
- `PASS`
- `SYST`
- `PWD`
- `CWD`
- `LIST`
- `RETR`
- `STOR`
- `QUIT`

Acceptance:

- File listing works.
- Download works.
- Upload works.
- The server can be connected to from Windows through QEMU port forwarding.

Validation:

- `make test`
- then a manual Windows FileZilla or PowerShell smoke test

### Phase 5: QEMU Launch and Docs

Goal:

- Add a Windows-friendly launch path and document it.

Suggested files:

- `Makefile`
- `README.md`
- `docs/build.md`
- `docs/execution.md`
- optionally `CHANGELOG.md`

Ports to forward:

- control port 2121 -> 21
- passive data port 30000 -> 30000 first
- widen only if a client actually needs it

Acceptance:

- You can launch QEMU on Windows with no extra installs.
- An FTP client on Windows can connect to the guest through forwarded ports.

## Commit Order

### Commit 1: TCP kernel bring-up

Touch only the kernel TCP/IP pieces.

Suggested files:

- `src/drivers/tcp.c`
- `src/drivers/tcp.h`
- `src/drivers/ipv4.c`
- `src/drivers/ipv4.h`
- kernel init wiring

Goal:

- one inbound TCP connection
- one listener
- one echo path

After this commit:

- run `make test`

### Commit 2: Socket ABI

Add the syscall and process-state support.

Suggested files:

- `src/kernel/uapi_syscall.h`
- `src/kernel/syscall.c`
- `src/kernel/process.h`
- `src/kernel/process.c`
- `src/kernel/uapi_socket.h`
- `src/kernel/uapi_poll.h`

After this commit:

- run `make test`

### Commit 3: User wrappers

Add the small POSIX-like glue.

Suggested files:

- `src/user/user_syscall.h`
- `src/user/user_posix.c`
- `src/user/sys/socket.h`
- `src/user/poll.h`
- `src/user/arpa/inet.h`

After this commit:

- run `make test`

### Commit 4: Tiny echo app

Add a small user ELF, likely:

- `src/user/tcpecho.c`

Purpose:

- prove the end-to-end TCP path before FTP

After this commit:

- test from Windows through QEMU hostfwd

### Commit 5: FTP daemon port

Add the FTP daemon as a user ELF only after echo works.

Suggested files:

- `src/user/ftpd.c`
- or a small launcher around the vendored FTP code

Keep it:

- passive mode only
- user-space only
- minimal

After this commit:

- run `make test`
- test with a Windows client

### Commit 6: Launch/docs

Update the launch commands and docs last.

Suggested files:

- `Makefile`
- `README.md`
- `docs/build.md`
- `docs/execution.md`
- `CHANGELOG.md` if needed

## Testing Discipline

After each step:

- rebuild
- run `make test`
- only then move to the next layer

If something breaks:

- stop at that layer
- fix the regression
- rerun the full suite

## Best First Move for the Next Session

- Re-add the TCP echo path if it is missing from the current branch state.
- Run `make test`.
- If green, add the socket ABI.

## Practical Session Start Checklist

When starting a new session, do this first:

- confirm the repo is clean or know exactly what changed
- run `make test`
- if green, start with Phase 1 only
- if not green, fix the baseline first
