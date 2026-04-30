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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--command", required=True, choices=("reboot", "halt"))
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    if not wait_for_path(args.monitor, args.timeout):
        print(f"timed out waiting for {args.monitor}", file=sys.stderr)
        return 1
    if not wait_for_path(args.serial, args.timeout):
        print(f"timed out waiting for {args.serial}", file=sys.stderr)
        return 1

    sock = connect_monitor(args.monitor, args.timeout)

    deadline = time.time() + args.timeout
    log_offset = 0
    buf = ""

    with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
        while time.time() < deadline:
            chunk, log_offset = read_new(log, log_offset)
            if chunk:
                tee_stdout(chunk)
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

        tee_stdout(f"\n[smoke:{args.command}] ")
        send_text(sock, args.command)
        send_key(sock, "ret")

        saw_marker = False
        saw_reboot_banner = False
        marker = "Rebooting..." if args.command == "reboot" else "System halted."
        reboot_mark = -1

        while time.time() < deadline:
            chunk, log_offset = read_new(log, log_offset)
            if chunk:
                tee_stdout(chunk)
                buf += chunk
                if marker in buf:
                    saw_marker = True
                    if args.command == "halt":
                        tee_stdout("[smoke:halt] PASS\n")
                        return shutdown_qemu(sock, args.pidfile)
                    reboot_mark = buf.rfind(marker)
                if args.command == "reboot" and saw_marker:
                    if buf.find("SmallOS", reboot_mark + 1) >= 0 and buf.find("> ", reboot_mark + 1) >= 0:
                        saw_reboot_banner = True
                        break
                if len(buf) > 8192:
                    buf = buf[-4096:]
            else:
                time.sleep(0.05)

        if args.command == "reboot":
            if not saw_reboot_banner:
                print("timed out waiting for reboot cycle", file=sys.stderr)
                return 1
            tee_stdout("[smoke:reboot] PASS\n")
            return shutdown_qemu(sock, args.pidfile)

        if not saw_marker:
            print("timed out waiting for halt marker", file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
