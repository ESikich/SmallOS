#!/usr/bin/env python3

import argparse
import os
import socket
import sys
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
INDEX_PATH = REPO_ROOT / "samples" / "cserve_index.html"
BODY_MARKER = b"cserve on SmallOS"


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
            sock.settimeout(1.0)
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


def recv_some(sock, count, deadline):
    while time.time() < deadline:
        try:
            chunk = sock.recv(count)
        except socket.timeout:
            continue
        if not chunk:
            raise RuntimeError("connection closed while reading response")
        return chunk
    raise RuntimeError("timed out reading HTTP response")


def read_http_response(sock, timeout_s, slow=False):
    deadline = time.time() + timeout_s
    data = b""
    header_step = 1 if slow else 4096

    while b"\r\n\r\n" not in data:
        data += recv_some(sock, header_step, deadline)
        if len(data) > 65536:
            raise RuntimeError("HTTP headers exceeded smoke limit")

    head, body = data.split(b"\r\n\r\n", 1)
    lines = head.decode("iso-8859-1").split("\r\n")
    parts = lines[0].split(" ", 2)
    if len(parts) < 2 or not parts[1].isdigit():
        raise RuntimeError(f"bad HTTP status line: {lines[0]!r}")
    status = int(parts[1])

    headers = {}
    for line in lines[1:]:
        if ":" not in line:
            continue
        name, value = line.split(":", 1)
        headers[name.strip().lower()] = value.strip()

    content_length = headers.get("content-length")
    if content_length is not None:
        expected = int(content_length)
        while len(body) < expected:
            step = 128 if slow else min(4096, expected - len(body))
            body += recv_some(sock, step, deadline)
            if slow:
                time.sleep(0.02)
        body = body[:expected]
    else:
        while time.time() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            body += chunk

    return status, headers, body


def send_http_get(sock, host, port, path, connection):
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "User-Agent: SmallOS-cserve-smoke/1\r\n"
        "Accept: text/html,*/*\r\n"
        f"Connection: {connection}\r\n"
        "\r\n"
    )
    sock.sendall(request.encode("ascii"))


def expect_static_response(label, status, headers, body, expected_len):
    if status != 200:
        raise RuntimeError(f"{label}: expected HTTP 200, got {status}")
    if BODY_MARKER not in body:
        raise RuntimeError(f"{label}: response body missing fixture marker")
    if expected_len and len(body) != expected_len:
        raise RuntimeError(
            f"{label}: expected {expected_len} body bytes, got {len(body)}"
        )
    clen = headers.get("content-length")
    if expected_len and clen != str(expected_len):
        raise RuntimeError(
            f"{label}: expected Content-Length {expected_len}, got {clen!r}"
        )


def fetch_once(args, path, connection="close", slow=False):
    sock = connect_tcp(args.host, args.port, args.timeout)
    try:
        send_http_get(sock, args.host, args.port, path, connection)
        return read_http_response(sock, args.timeout, slow=slow)
    finally:
        sock.close()


def open_keepalive(args, index, expected_len):
    sock = connect_tcp(args.host, args.port, args.timeout)
    send_http_get(sock, args.host, args.port, "/", "keep-alive")
    status, headers, body = read_http_response(sock, args.timeout)
    expect_static_response(f"keepalive {index}", status, headers, body, expected_len)
    return sock


def run_cserve_http_checks(args):
    expected_len = INDEX_PATH.stat().st_size if INDEX_PATH.exists() else 0
    held = []

    query = "q=" + ("smallos" * 128)
    status, headers, body = fetch_once(args, f"/?{query}", connection="keep-alive")
    expect_static_response("query keep-alive fetch", status, headers, body, expected_len)
    print(f"query keep-alive fetch: {len(body)} bytes")

    status, headers, body = fetch_once(args, "/index.html")
    expect_static_response("index fetch", status, headers, body, expected_len)
    print(f"index fetch: {len(body)} bytes")

    status, headers, body = fetch_once(args, "/favicon.ico")
    if status != 404:
        raise RuntimeError(f"favicon fetch: expected HTTP 404, got {status}")
    print(f"favicon fetch: HTTP {status}, {len(body)} body bytes")

    try:
        for i in range(args.clients):
            held.append(open_keepalive(args, i + 1, expected_len))
        print(f"held keep-alive clients: {len(held)}")

        status, headers, body = fetch_once(args, "/index.html", slow=True)
        expect_static_response("slow reader", status, headers, body, expected_len)
        print(f"slow reader: {len(body)} bytes")
    finally:
        for sock in held:
            try:
                sock.close()
            except OSError:
                pass


def run_cserve_smoke(args):
    if not wait_for_path(args.monitor, args.timeout):
        raise RuntimeError(f"monitor socket did not appear: {args.monitor}")
    if not wait_for_path(args.serial, args.timeout):
        raise RuntimeError(f"serial log did not appear: {args.serial}")

    monitor = connect_monitor(args.monitor, args.timeout)
    ok = False
    try:
        with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
            offset = wait_for_prompt(log, args.timeout)
            send_text(monitor, "runelf_nowait usr/sbin/cserve --config /etc/cserve.ini")
            send_key(monitor, "ret")
            offset = wait_for_prompt(log, args.timeout, offset)

            run_cserve_http_checks(args)

            send_text(monitor, "netinfo")
            send_key(monitor, "ret")
            offset, _ = wait_for_log(log, offset, "tcp buffers:", args.timeout)
            print("cserve netinfo captured")

        print("cserve smoke PASS")
        ok = True
        return 0
    except Exception as exc:
        print(f"cserve smoke FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        rc = shutdown_qemu(monitor, args.pidfile)
        monitor.close()
        if ok and rc != 0:
            raise SystemExit(rc)


def main():
    parser = argparse.ArgumentParser(description="SmallOS cserve HTTP smoke")
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--clients", type=int, default=24)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    return run_cserve_smoke(args)


if __name__ == "__main__":
    sys.exit(main())
