# SmallOS FTP Bring-Up Plan

Current handoff for the next implementation pass.

## Current State

- `make test` is green on the current tree.
- A minimal kernel TCP bring-up path already exists in `src/drivers/tcp.c` and `src/drivers/tcp.h`.
- The hostfwd smoke path is proven end to end with `apps/services/tcpecho.elf`.
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
- A guest-side FTP ELF launcher now exists in `src/user/ftpd.c`.
- The FTP port uses SmallOS compatibility shims in `src/user/ftp_compat.c` plus small headers for `dirent`, `ctype`, and `netinet/in`.
- A tiny user-space TCP echo app exists in `src/user/tcpecho.c`.
- The TCP transmit path is now validated by a host client connecting through QEMU hostfwd and receiving echoed bytes back.
- `ftpd.elf` has been verified as a normal guest ELF, reachable from the host through QEMU user networking on `2121` for control and `30000` for passive data.
- `lftp` is the current least-fussy host-side smoke client; FileZilla and WinSCP both probe with `LIST -a`, so the server accepts that form as a current-directory listing request.
- `third_party/ftp_server/` is present but intentionally untracked and should be treated as vendored input, not as the implementation target.

## Verified Smoke Recipe

Use this exact launch shape when you want to verify the guest FTP daemon over QEMU user networking:

```bash
qemu-system-i386 \
  -drive format=raw,file=build/img/os-image.bin \
  -boot c -m 32 \
  -serial file:/tmp/smallos-serial.log \
  -nic user,model=e1000,mac=52:54:00:12:34:56,hostfwd=tcp::2121-:2121,hostfwd=tcp::30000-:30000 \
  -display none \
  -monitor unix:/tmp/smallos-monitor.sock,server,nowait \
  -daemonize -pidfile /tmp/smallos.pid
```

Inside the guest shell, start the daemon with:

```text
runelf_nowait apps/services/ftpd
```

Expected guest banner:

```text
ftpd: listening on 0.0.0.0:2121
```

Host-side expectations:

- Connect to `127.0.0.1:2121` and verify the banner arrives immediately.
- Log in with `USER ftp` and `PASS ftp`.
- Send `PASV` and connect the data socket to the advertised host/port tuple.
- Confirm `LIST`, `RETR`, and `STOR` all succeed through the passive data port.
- Do not rely on active mode; the daemon is intended to operate passive-only.
- Send `QUIT` and confirm the control connection closes cleanly.
- If using a GUI client, expect it to probe `FEAT`, `OPTS UTF8 ON`, `TYPE A`, and `LIST -a` before the visible directory listing.

Recommended repeatable host smoke:

```bash
lftp -d -e '
set ftp:passive-mode yes
set net:timeout 10
open -u ftp,ftp 127.0.0.1:2121
cls -l
get apps/demo/hello.elf -o /tmp/retr.bin
put /tmp/lftp-smoke.txt -o LFTP_SMOKE.TXT
cls -l
bye
'
```

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
- The first implementation target is the guest-side ELF launcher, not `main.c` from the vendored tree.
- Use the same QEMU hostfwd path that already works with `tcpecho.elf` to validate control and passive data ports.

Important:

- Keep the server in user space.
- Avoid active FTP until everything else is stable.
- Use the existing filesystem syscalls for file operations.
- Keep the vendored FTP session logic unchanged unless a SmallOS shim absolutely requires a tiny adaptation.

Validation:

- `make test`
- then a manual Windows FileZilla or PowerShell smoke test

### Phase 5: QEMU Launch and Docs

Goal:

- Add a Windows-friendly launch path and document it.

Ports to forward:

- control port 2121 -> 2121
- passive data port 30000 -> 30000 first

Notes:

- Keep the guest FTP daemon on `2121`.
- Keep passive data on `30000` until the guest TCP stack supports a wider range cleanly.
- Document the `lftp` smoke as the preferred repeatable host check, with GUI clients as an additional compatibility test.

## Best First Move for the Next Session

- Exercise `build/bin/ftpd.elf` in QEMU over the proven hostfwd path.
- Keep the vendored FTP code as the core session engine.
- If anything regresses, fix the SmallOS shim layer before touching the FTP logic.
