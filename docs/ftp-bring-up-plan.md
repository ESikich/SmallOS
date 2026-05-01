# SmallOS FTP Bring-Up Plan

Current handoff for the next implementation pass.

## Current State

- `make test` is green on the current tree.
- A minimal kernel TCP bring-up path already exists in `src/drivers/tcp.c` and `src/drivers/tcp.h`.
- The process/file layer now has a generic per-process handle seam in:
  - `src/kernel/process.c`
  - `src/kernel/process.h`
  - `src/kernel/syscall.c`
- A first-pass socket syscall ABI exists:
  - `socket`
  - `bind`
  - `listen`
  - `accept`
  - `connect` stubbed for now
  - `send`
- `recv`
- `poll`
- The FTP daemon itself is not yet integrated.
- A tiny user-space TCP echo app exists in `src/user/tcpecho.c`.
- `third_party/ftp_server/` is present but intentionally untracked and should be treated as vendored input, not as the implementation target.

## Goal

Run a user-space FTP daemon in the guest with OS changes limited to:

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

Validation:

- `make test`

### Phase 2: Socket Syscall ABI

Goal:

- Add a small socket-style syscall surface.

ABI:

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

Validation:

- `make test`

### Phase 3: User-Space Wrappers

Goal:

- Add minimal libc-style wrappers so user programs can use the socket ABI naturally.

Helpers:

- dotted-quad IP parsing
- socket address structs
- basic polling wrappers

Validation:

- `make test`

### Phase 4: FTP Daemon Port

Goal:

- Port the FTP daemon to user space.
- Keep it passive mode only at first.
- The TCP echo app should remain the quick smoke test before any FTP porting.

Important:

- Keep the server in user space.
- Avoid active FTP until everything else is stable.
- Use the existing filesystem syscalls for file operations.

Validation:

- `make test`
- then a manual Windows FileZilla or PowerShell smoke test

### Phase 5: QEMU Launch and Docs

Goal:

- Add a Windows-friendly launch path and document it.

Ports to forward:

- control port 2121 -> 21
- passive data port 30000 -> 30000 first

## Best First Move for the Next Session

- Re-add the TCP echo path if it is missing from the current branch state.
- Run `make test`.
- If green, add the socket ABI.
