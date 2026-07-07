#!/usr/bin/env python3

import argparse
import http.server
import os
import socket
import socketserver
import sys
import threading
import time


WGET_BODY = b"busybox-net-ok\n"
NC_BODY = b"smallos-nc-ok\n"


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
    keymap = {
        " ": "spc",
        "_": "shift-minus",
        ".": "dot",
        "/": "slash",
        "-": "minus",
        ":": "shift-semicolon",
        "'": "apostrophe",
        "|": "shift-backslash",
        ">": "shift-dot",
        "<": "shift-comma",
    }
    for ch in text:
        key = keymap.get(ch)
        if key is None:
            if "a" <= ch <= "z" or "0" <= ch <= "9":
                key = ch
            elif "A" <= ch <= "Z":
                key = "shift-" + ch.lower()
            else:
                raise RuntimeError(f"unsupported key for send_text: {ch!r}")
        send_key(sock, key)
        time.sleep(0.04)


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
                return offset, buf
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


def run_guest_command(monitor, log, offset, command, timeout_s, marker=None):
    send_text(monitor, command)
    send_key(monitor, "ret")
    if marker:
        offset, buf = wait_for_log(log, offset, marker, timeout_s)
        return offset, buf
    return wait_for_prompt(log, timeout_s, offset)


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


class QuietTCPServer(socketserver.TCPServer):
    allow_reuse_address = True


class SmokeHTTPHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/busybox.txt":
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(WGET_BODY)))
        self.end_headers()
        self.wfile.write(WGET_BODY)

    def log_message(self, fmt, *args):
        return


def start_http_server(port):
    server = QuietTCPServer(("127.0.0.1", port), SmokeHTTPHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def start_echo_server(port):
    ready = threading.Event()
    done = threading.Event()
    state = {"error": None}

    def worker():
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
                srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                srv.bind(("127.0.0.1", port))
                srv.listen(1)
                ready.set()
                conn, _ = srv.accept()
                with conn:
                    conn.settimeout(5.0)
                    data = b""
                    while b"\n" not in data:
                        chunk = conn.recv(128)
                        if not chunk:
                            break
                        data += chunk
                    if NC_BODY.strip() not in data:
                        raise RuntimeError(f"nc payload mismatch: {data!r}")
                    conn.sendall(NC_BODY)
        except Exception as exc:
            state["error"] = exc
        finally:
            done.set()

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    if not ready.wait(5.0):
        raise RuntimeError("timed out starting echo server")
    return done, state


def fetch_guest_http(port, timeout_s):
    with connect_tcp("127.0.0.1", port, timeout_s) as sock:
        sock.sendall(
            b"GET / HTTP/1.0\r\n"
            b"Host: 127.0.0.1\r\n"
            b"Connection: close\r\n"
            b"\r\n"
        )
        data = b""
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            data += chunk
        if b"200" not in data.split(b"\r\n", 1)[0]:
            raise RuntimeError(f"busybox httpd returned unexpected response: {data[:80]!r}")
        if b"cserve on SmallOS" not in data:
            raise RuntimeError("busybox httpd response did not include expected body marker")
        print(f"busybox httpd fetch: {len(data)} bytes")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--guest-http-port", type=int, default=18081)
    parser.add_argument("--host-http-port", type=int, default=18080)
    parser.add_argument("--host-echo-port", type=int, default=18082)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    http_server = start_http_server(args.host_http_port)
    monitor = None
    exit_status = 1
    try:
        if not wait_for_path(args.serial, args.timeout):
            raise RuntimeError(f"timed out waiting for serial log: {args.serial}")
        monitor = connect_monitor(args.monitor, args.timeout)
        with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
            offset, _ = wait_for_prompt(log, args.timeout)

            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"bg usr/bin/busybox httpd -f -p {args.guest_http_port} -h /var/www",
                args.timeout,
            )
            fetch_guest_http(args.guest_http_port, args.timeout)

            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"usr/bin/busybox wget -q -O /tmp/bbwget.txt http://10.0.2.2:{args.host_http_port}/busybox.txt",
                args.timeout,
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox grep busybox-net-ok /tmp/bbwget.txt",
                args.timeout,
                "busybox-net-ok",
            )

            echo_done, echo_state = start_echo_server(args.host_echo_port)
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"echo smallos-nc-ok | usr/bin/busybox nc 10.0.2.2 {args.host_echo_port} | usr/bin/busybox grep smallos-nc-ok",
                args.timeout,
                "smallos-nc-ok",
            )
            if not echo_done.wait(5.0):
                raise RuntimeError("timed out waiting for host echo server")
            if echo_state["error"] is not None:
                raise echo_state["error"]

            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "arpgw",
                args.timeout,
                "arpgw:",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox ip neigh show | usr/bin/busybox grep 10.0.2.2",
                args.timeout,
                "10.0.2.2",
            )
            print("busybox network smoke PASS")
            exit_status = 0
    except Exception as exc:
        print(f"busybox network smoke FAIL: {exc}", file=sys.stderr)
        exit_status = 1
    finally:
        http_server.shutdown()
        http_server.server_close()
        if monitor is not None:
            shutdown_qemu(monitor, args.pidfile)
            try:
                monitor.close()
            except OSError:
                pass
    return exit_status


if __name__ == "__main__":
    raise SystemExit(main())
