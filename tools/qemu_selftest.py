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
        elif ch == "\n":
            send_key(sock, "ret")
        elif "a" <= ch <= "z" or "0" <= ch <= "9":
            send_key(sock, ch)
        else:
            raise RuntimeError(f"unsupported key for send_text: {ch!r}")


def read_new(log, offset):
    log.seek(offset)
    chunk = log.read()
    return chunk, log.tell()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    if not wait_for_path(args.monitor, args.timeout):
        print(f"timed out waiting for {args.monitor}", file=sys.stderr)
        return 1
    if not wait_for_path(args.serial, args.timeout):
        print(f"timed out waiting for {args.serial}", file=sys.stderr)
        return 1

    sock = connect_monitor(args.monitor, args.timeout)

    log_offset = 0
    deadline = time.time() + args.timeout
    buf = ""

    with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
        while time.time() < deadline:
            chunk, log_offset = read_new(log, log_offset)
            if chunk:
                buf += chunk
                if "> " in buf:
                    break
                if len(buf) > 8192:
                    buf = buf[-4096:]
            else:
                time.sleep(0.05)
        else:
            print("timed out waiting for shell prompt", file=sys.stderr)
            return 1

    send_text(sock, "selftest")
    send_key(sock, "ret")

    seen_first_readline = False
    seen_second_readline = False
    seen_result = False
    result_pass = False

    with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
        while time.time() < deadline:
            chunk, log_offset = read_new(log, log_offset)
            if chunk:
                buf += chunk
                if "Enter your name:" in buf and not seen_first_readline:
                    send_text(sock, "erik")
                    send_key(sock, "ret")
                    seen_first_readline = True
                if "Type a line (max 127 chars):" in buf and not seen_second_readline:
                    send_text(sock, "smallos")
                    send_key(sock, "ret")
                    seen_second_readline = True
                if "selftest: PASS" in buf:
                    seen_result = True
                    result_pass = True
                    break
                if "selftest: FAIL" in buf:
                    seen_result = True
                    result_pass = False
                    break
                if len(buf) > 8192:
                    buf = buf[-4096:]
            else:
                time.sleep(0.05)

    if not seen_result:
        print("timed out waiting for selftest result", file=sys.stderr)
        try:
            monitor_send(sock, "quit")
        except OSError:
            pass
        return 1

    try:
        monitor_send(sock, "quit")
    except OSError:
        pass

    if not wait_for_path(args.pidfile, 1.0):
        return 0 if result_pass else 1

    deadline = time.time() + 5.0
    try:
        with open(args.pidfile, "r", encoding="utf-8") as f:
            pid = int(f.read().strip())
    except Exception:
        return 0 if result_pass else 1

    while time.time() < deadline:
        try:
            os.kill(pid, 0)
        except OSError:
            return 0 if result_pass else 1
        time.sleep(0.05)

    print("qemu did not exit after quit", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
