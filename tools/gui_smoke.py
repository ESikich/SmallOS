#!/usr/bin/env python3

import argparse
import os
import socket
import sys
import time


GUI_ERROR_MARKERS = (
    "gui: framebuffer not available",
    "gui: display is already in use",
    "gui: unsupported display size",
    "gui: out of memory opening display",
    "gui: could not open display",
    "gui: present failed",
)


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


def find_gui_error(buf):
    for marker in GUI_ERROR_MARKERS:
        if marker in buf:
            return marker
    return None


def wait_for_prompt(log, offset, timeout_s):
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


def wait_for_marker_or_error(log, offset, marker, timeout_s):
    deadline = time.time() + timeout_s
    buf = ""

    while time.time() < deadline:
        chunk, offset = read_new(log, offset)
        if chunk:
            tee_stdout(chunk)
            buf += chunk
            error = find_gui_error(buf)
            if error:
                raise RuntimeError(error)
            if marker in buf:
                return offset
            if len(buf) > 8192:
                buf = buf[-4096:]
        else:
            time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for serial marker: {marker}")


def wait_for_prompt_or_error(log, offset, timeout_s):
    deadline = time.time() + timeout_s
    buf = ""

    while time.time() < deadline:
        chunk, offset = read_new(log, offset)
        if chunk:
            tee_stdout(chunk)
            buf += chunk
            error = find_gui_error(buf)
            if error:
                raise RuntimeError(error)
            if "> " in buf:
                return offset
            if len(buf) > 8192:
                buf = buf[-4096:]
        else:
            time.sleep(0.05)
    raise RuntimeError("gui did not return to shell after q")


def run_gui_smoke(args):
    if not wait_for_path(args.monitor, args.timeout):
        raise RuntimeError(f"timed out waiting for {args.monitor}")
    if not wait_for_path(args.serial, args.timeout):
        raise RuntimeError(f"timed out waiting for {args.serial}")

    monitor = connect_monitor(args.monitor, args.timeout)
    ok = False
    try:
        with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
            offset = wait_for_prompt(log, 0, args.timeout)

            tee_stdout("\n[smoke:gui] launch\n")
            send_text(monitor, "gui")
            send_key(monitor, "ret")
            offset = wait_for_marker_or_error(
                log,
                offset,
                "gui: starting",
                args.timeout,
            )

            time.sleep(args.settle)
            send_key(monitor, "q")
            wait_for_prompt_or_error(log, offset, args.timeout)

        tee_stdout("[smoke:gui] PASS\n")
        ok = True
        return 0
    finally:
        rc = shutdown_qemu(monitor, args.pidfile)
        if ok and rc != 0:
            raise SystemExit(rc)


def main():
    parser = argparse.ArgumentParser(description="SmallOS GUI launch smoke")
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle", type=float, default=0.5)
    args = parser.parse_args()

    try:
        return run_gui_smoke(args)
    except Exception as exc:
        print(f"GUI smoke FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
