#!/usr/bin/env python3

import argparse
import os
import socket
import sys
import time


def wait_for_path(path, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.05)
    return False


def connect_monitor(path, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(path)
            sock.settimeout(0.1)
            return sock
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for monitor socket: {path}")


def monitor_send(sock, cmd):
    sock.sendall((cmd + "\n").encode("ascii"))


def send_key(sock, key):
    monitor_send(sock, f"sendkey {key}")


def send_text(sock, text):
    for ch in text:
        if ch == " ":
            send_key(sock, "spc")
        elif ch == "_":
            send_key(sock, "shift-minus")
        elif ch == ".":
            send_key(sock, "dot")
        elif ch == "/":
            send_key(sock, "slash")
        elif ch == "-":
            send_key(sock, "minus")
        elif "a" <= ch <= "z" or "0" <= ch <= "9":
            send_key(sock, ch)
        else:
            raise RuntimeError(f"unsupported key for send_text: {ch!r}")
        time.sleep(0.05)


def read_new(log, offset):
    log.seek(offset)
    chunk = log.read()
    return chunk, log.tell()


def tee_stdout(text):
    if text:
        sys.stdout.write(text)
        sys.stdout.flush()


def wait_for_prompt(log, timeout_s, offset=0):
    deadline = time.time() + timeout_s
    buf = ""
    while time.time() < deadline:
        chunk, offset = read_new(log, offset)
        if chunk:
            tee_stdout(chunk)
            buf += chunk
            if "> " in buf:
                return offset
            if len(buf) > 8192:
                buf = buf[-4096:]
        else:
            time.sleep(0.05)
    raise RuntimeError("timed out waiting for shell prompt")


def wait_for_log(log, offset, marker, timeout_s):
    deadline = time.time() + timeout_s
    buf = ""
    while time.time() < deadline:
        chunk, offset = read_new(log, offset)
        if chunk:
            tee_stdout(chunk)
            buf += chunk
            if marker in buf:
                return offset, buf
            if len(buf) > 16384:
                buf = buf[-8192:]
        else:
            time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for serial marker: {marker}")


def connect_tcp(host, port, timeout_s):
    deadline = time.time() + timeout_s
    last_error = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection((host, port), timeout=1.0)
            sock.settimeout(0.5)
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)
    raise RuntimeError(f"timed out connecting to {host}:{port}: {last_error}")


def shutdown_qemu(sock, pidfile):
    pid = None
    try:
        with open(pidfile, "r", encoding="utf-8") as f:
            pid = int(f.read().strip())
    except Exception:
        pid = None

    try:
        monitor_send(sock, "quit")
    except OSError:
        pass

    if pid is None:
        return 0

    deadline = time.time() + 5.0
    while time.time() < deadline:
        try:
            os.kill(pid, 0)
        except OSError:
            return 0
        time.sleep(0.05)

    print("qemu did not exit after quit", file=sys.stderr)
    return 1


def capture_netinfo(monitor, log, offset, timeout_s, label):
    print(f"netinfo {label}:")
    send_text(monitor, "netinfo")
    send_key(monitor, "ret")
    offset, _ = wait_for_log(log, offset, "tcp buffers:", timeout_s)
    return offset


def recv_exact(sock, count, timeout_s):
    deadline = time.time() + timeout_s
    data = b""
    while len(data) < count and time.time() < deadline:
        try:
            chunk = sock.recv(count - len(data))
        except socket.timeout:
            continue
        if not chunk:
            raise RuntimeError("connection closed while reading echo response")
        data += chunk
    if len(data) != count:
        raise RuntimeError(f"timed out reading echo response: {len(data)}/{count}")
    return data


def payload_for(client_index, round_index, size):
    base = f"SmallOS echo client={client_index:02d} round={round_index:02d}\n"
    data = base.encode("ascii")
    if len(data) >= size:
        return data[:size]
    repeats = (size + len(data) - 1) // len(data)
    return (data * repeats)[:size]


def open_echo_clients(args):
    clients = []
    try:
        for i in range(args.clients):
            clients.append(connect_tcp(args.host, args.port, args.timeout))
            print(f"connected echo client {i + 1}/{args.clients}")
        return clients
    except Exception:
        close_echo_clients(clients)
        raise


def close_echo_clients(clients):
    for sock in clients:
        try:
            sock.close()
        except OSError:
            pass


def run_echo_rounds(args, clients):
    for round_index in range(args.rounds):
        expected = []
        for client_index, sock in enumerate(clients):
            payload = payload_for(client_index, round_index, args.payload_size)
            expected.append(payload)
            sock.sendall(payload)

        for client_index, sock in enumerate(clients):
            got = recv_exact(sock, len(expected[client_index]), args.timeout)
            if got != expected[client_index]:
                raise RuntimeError(
                    "echo mismatch for client "
                    f"{client_index} round {round_index}: "
                    f"expected {expected[client_index]!r}, got {got!r}"
                )
        print(f"echo round {round_index + 1}/{args.rounds}: PASS")


def run_socket_parallel_smoke(args):
    if not wait_for_path(args.monitor, args.timeout):
        raise RuntimeError(f"monitor socket did not appear: {args.monitor}")
    if not wait_for_path(args.serial, args.timeout):
        raise RuntimeError(f"serial log did not appear: {args.serial}")

    monitor = connect_monitor(args.monitor, args.timeout)
    ok = False
    try:
        with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
            offset = wait_for_prompt(log, args.timeout)
            send_text(monitor, "runelf_nowait apps/services/tcpecho")
            send_key(monitor, "ret")
            offset, _ = wait_for_log(log, offset, "tcpecho: listening", args.timeout)

            offset = capture_netinfo(monitor, log, offset, args.timeout, "before")
            clients = open_echo_clients(args)
            try:
                run_echo_rounds(args, clients)
                if args.hold_seconds > 0:
                    time.sleep(args.hold_seconds)
                offset = capture_netinfo(monitor, log, offset, args.timeout, "active")
            finally:
                close_echo_clients(clients)
            time.sleep(0.5)
            offset = capture_netinfo(monitor, log, offset, args.timeout, "after")

        print("socket parallel smoke PASS")
        ok = True
        return 0
    except Exception as exc:
        print(f"socket parallel smoke FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        rc = shutdown_qemu(monitor, args.pidfile)
        monitor.close()
        if ok and rc != 0:
            raise SystemExit(rc)


def main():
    parser = argparse.ArgumentParser(description="SmallOS parallel TCP echo smoke")
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2323)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--payload-size", type=int, default=64)
    parser.add_argument("--hold-seconds", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    if args.clients < 1:
        print("--clients must be at least 1", file=sys.stderr)
        return 1
    if args.payload_size < 1:
        print("--payload-size must be at least 1", file=sys.stderr)
        return 1

    return run_socket_parallel_smoke(args)


if __name__ == "__main__":
    sys.exit(main())
