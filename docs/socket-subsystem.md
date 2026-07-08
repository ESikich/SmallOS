# Socket Subsystem Status

This document records the completed socket subsystem rollout. It is now a
status and reference note, not an implementation handoff.

The rollout started from commit `6b820d9` (`Add cserver ELF service support`),
where SmallOS had a small passive TCP path, a 16-entry process fd table, and a
single global TCP waiter. The phase plan has since been completed through the
cserve/FTP server-side milestone, the basic active-open client path, and the
first close-state hardening pass.

Future work listed here is outside that rollout. It belongs to later TCP
production polish, broader network-mode coverage, TLS/SSH protocol work, or
POSIX edge-semantics work.

## Current Capabilities

- Process fd tables are PMM-backed, start at 16 slots, grow to the default
  128-fd limit, and are freed on process teardown.
- Socket fds point at `socket_t` objects that own TCP listener and connection
  state, UDP datagram bind/receive state for resolver-style traffic, and
  raw-ICMP descriptors for the BusyBox networking compatibility surface.
- Passive TCP listeners live in a small listener table, while accepted and
  active streams live in a PMM-backed global connection table keyed by local IP,
  local port, remote IP, and remote port.
- `listen(backlog)` is capped and wired through the socket/TCP layers.
- Sockets own accept, read, and write wait queues. Blocking `accept`, `recv`,
  socket `read`, `poll`, and `epoll_wait` wait on the relevant object instead
  of a single global TCP waiter.
- Timerfd and signalfd-style handles own read wait queues. Timer IRQs wake
  expired timerfd waiters directly, Ctrl+C queues `SIGINT` to matching
  foreground process-group signalfds, and shell job `kill` queues `SIGTERM`
  to matching job-group signalfds before falling back to force-kill behavior.
- Connected TCP streams allocate lazy PMM-backed 4 KiB RX rings, advertise the
  remaining RX window, and stop ACKing bytes that cannot be queued.
- Connected TCP streams allocate lazy PMM-backed 16 KiB TX rings, retain sent
  bytes until ACKed, retry buffered payloads, send zero-window probes for
  queued unsent data, and report writability from remaining TX capacity.
- RX and TX ring allocation observes global caps.
- Outbound TCP active opens are implemented for `connect()`, including
  ephemeral local ports, SYN retransmission, nonblocking `EINPROGRESS`, and
  `POLLOUT` / `POLLERR` connect readiness.
- `shutdown()` covers the first half-close behavior: `SHUT_RD` reports local
  EOF, `SHUT_WR` drains queued TX before sending FIN, later writes fail with
  `EPIPE`, duplicate peer FINs are ACKed, and late FIN cleanup is covered.
- `netinfo`, `ip`, and `ipconfig /all` report socket-object counts, TCP
  listener and connection counts, RX/TX ring usage, allocated ring capacity,
  and global RX/TX caps. `ip` and `ipconfig` also expose the runtime IPv4
  address, route, DNS, DHCP server, and lease state used by outbound sockets.
- BusyBox `ifconfig`, `route`, `arp`, `hostname`, `ipcalc`, `netstat -r`,
  `nslookup`, `pscan`, `whois`, `ip link`, `ip addr`, `ip route`, `ip neigh`,
  and IPv4 `ping` are enabled through Linux-shaped `eth0`/loopback interface/route
  ioctls, minimal rtnetlink, state-backed `/proc/net/dev`, `/proc/net/route`,
  `/proc/net/arp`, resolver/service helpers, ARP-neighbor reporting, and raw
  ICMP `sendto`/`recvfrom` descriptors. UDP send/receive is available for
  DNS-sized resolver traffic, single-file TFTP client transfers, and the
  BusyBox `udpsvd`/`tftpd` service smoke path. Libc resolver lookups can issue
  DNS A-record queries using the configured DNS server. UDP bind accepts
  `SO_REUSEADDR` for service-style sharing, and connected UDP sockets win
  demux over wildcard bound sockets for matching replies.
  Basic BusyBox TCP/UDP applets are enabled too: `nc`, plain HTTP `wget`,
  `whois`, `ftpget`, `ftpput`, `tftp`, `tcpsvd`, `udpsvd`, `tftpd`, and
  `httpd` run over the existing IPv4 socket path. BusyBox `udhcpc` is enabled
  through a SmallOS-specific shim that calls the native `SYS_NET_OP_DHCP`
  operation for `eth0`.
- The boot-started cserve instance uses `max_conn = 28`; the default cserve
  smoke gate holds 24 keep-alive clients and adds one slow-reader connection.

## Historical Baseline

### File Descriptors

At the baseline, `process_t` owned a fixed `fd_entry_t fds[PROCESS_FD_MAX]`
array. `PROCESS_FD_MAX` was 16, and fds `0`, `1`, and `2` were stdio, leaving
13 user-open slots.

cserve consumes baseline fds for:

- the log file
- the listen socket
- the epoll fd
- the signalfd
- the timerfd
- each accepted client socket
- each static file being sent

That is why the baseline sample config used `max_conn = 4`. The current sample
uses `max_conn = 40` after the fd-table, socket-object, wait-queue, TCP table,
and resource-visibility work.

### TCP

At the baseline, the TCP driver had a small fixed slot model:

- fixed local-port slots
- fixed accepted streams per slot
- fd-local `socket_port` and `socket_conn` transport state
- one global TCP waiter
- minimal per-stream receive buffering

The current implementation moved beyond that model. Socket objects own waits,
fd entries point at sockets, listeners and streams live in separate TCP tables,
and connected streams allocate lazy RX/TX rings on first use.

### Process Allocation

`process_t` is expected to fit in one 4 KiB frame. Expandable state such as the
fd table now lives behind pointers instead of growing the process struct with
large inline arrays.

## Implementation Notes

### Ownership Boundaries

- `process.c` owns fd table lifetime and generic handle dispatch.
- `socket.c` owns kernel socket objects, socket state, backlog policy, and
  socket wait queues.
- `tcp.c` owns passive TCP listeners, the global 4-tuple connection table, RX/TX
  rings, active-open handshakes, retransmission timers, FIN paths, and TCP
  resource accounting.
- `syscall.c` owns user pointer validation and syscall-level argument handling
  before dispatching to process/socket/TCP helpers.

### Resource Limits

- Process fd initial capacity: 16.
- Process fd default limit: 128.
- Process fd hard cap: 256.
- Socket objects: 256.
- TCP listeners: 8.
- TCP connections: 256.
- TCP backlog cap: 32.
- TCP RX ring: 64 KiB per active receiving connection.
- TCP TX ring: 16 KiB per active writing connection.
- Global TCP RX ring cap: 512 KiB.
- Global TCP TX ring cap: 1 MiB.
- cserve sample `max_conn`: 40.
- Default cserve smoke hold: 24 keep-alive clients plus one slow reader.

### TCP Behavior Notes

- The connection table is keyed by the full 4-tuple, so multiple remote clients
  can share one local listener without fd-local stream arrays.
- RX windows are derived from ring space. Data that cannot be queued is not
  acknowledged.
- Socket readiness checks drain pending NIC RX descriptors before reporting
  TCP readability, so user-space services can consume already-arrived packets
  without waiting for the background TCP task's next timer wakeup.
- TX data remains in the ring until ACKed, which supports retry and makes
  writable readiness reflect actual queued capacity.
- Zero-window probes cover queued unsent data.
- Peer FIN remains visible long enough for userland to observe EOF and
  `POLLHUP` after buffered payload is drained.
- Closing after a final write sends FIN after queued TX drains.
- Active-open smoke uses QEMU user networking to reach a host echo endpoint at
  QEMU's DHCP-provided gateway, normally `10.0.2.2`.

## Regression Matrix

The rollout is covered by the normal build and smoke matrix:

- `make`
- `make test`
- `make socket-eof-smoke`
- `make socket-parallel-smoke`
- `make ftp-smoke`
- `make ftp-loop-smoke`
- `make cserve-smoke`
- `make verify-network`

Coverage highlights:

- `make test` includes the fd exhaustion probe, signalfd-backed Ctrl+C delivery,
  and an outbound TCP `connect()` probe.
- `make socket-eof-smoke` covers host-first half-close, payload-before-EOF,
  guest response shutdown, and guest `close()` sending FIN after a final write.
- `make socket-parallel-smoke` drives parallel `tcpecho` clients.
- `make ftp-smoke` verifies the FTP control and passive data path.
- `make ftp-loop-smoke` repeats passive `LIST`, `RETR`, and `STOR` cycles.
- `make cserve-smoke` uses the boot-started cserve instance, fetches the large
  static fixture, checks a 404, holds keep-alive clients, exercises a slow
  reader, and captures `netinfo`.
- `make busybox-net-smoke` checks BusyBox `udhcpc`, `httpd`, plain HTTP
  `wget`, `pscan`, `whois`, `ftpget`, `ftpput`, `tftp`, `nc`, `tcpsvd`,
  `udpsvd`, `tftpd`, and ARP/neighbor reporting through host/guest TCP and UDP
  paths. It does not start BusyBox `udhcpd`.
- `make tinyssh-smoke` checks boot-started TinySSH on guest port `22`, root
  public-key login, forced-PTY command execution, and an idle default-shell
  interactive session. Its host-side SSH transcript is written to
  `/tmp/smallos-tinyssh.log` by default.
- `make verify-network` runs the socket EOF, socket parallel, FTP, FTP loop,
  cserve, BusyBox network, and TinySSH smoke targets in sequence.

## Future Work

There is no remaining implementation work in the completed socket rollout.
Later networking work can build on it in these areas:

- DHCP-backed outbound `connect()` coverage in QEMU user networking, plus
  TAP-mode coverage for bridged or routed host setups.
- Broader UDP behavior beyond DNS, single-file TFTP transfers, and the basic
  `udpsvd`/`tftpd` service path.
- Broader BusyBox client/server coverage beyond the current `udhcpc`, `nc`,
  plain HTTP `wget`, `whois`, `pscan`, `ftpget`, `ftpput`, `tftp`, `tcpsvd`,
  `udpsvd`, `tftpd`, and `httpd` host smoke, including DHCP server behavior,
  telnet, inetd, and other service applets.
- Broader rtnetlink coverage for BusyBox `ip rule`, tunnel modes,
  multi-interface systems, and IPv6 once those kernel features exist.
- Production TCP polish, including broader congestion, window, and recovery
  behavior.
- Broader close-state and retransmission fuzzing beyond the focused EOF smoke.
- TLS and SSH protocol implementation.
- POSIX-perfect socket semantics for less common edge cases.
