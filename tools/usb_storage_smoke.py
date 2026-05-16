#!/usr/bin/env python3

import argparse
import os
import socket
import sys
import time


DEFAULT_MARKERS = (
    "usbms: ready",
    "ext2: ok",
    "dev=usb0",
    "boot: PASS ext2: volume mounted from USB",
    "SmallOS ready",
    "Launching user shell",
    "SmallOS user shell",
    "/> ",
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


def print_tail(transcript, line_count=80):
    lines = transcript.splitlines()
    if not lines:
        return
    print("\n--- usb storage smoke transcript tail ---", file=sys.stderr)
    for line in lines[-line_count:]:
        print(line, file=sys.stderr)
    print("--- end usb storage smoke transcript tail ---", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--marker", action="append", dest="markers")
    args = parser.parse_args()

    markers = tuple(args.markers) if args.markers else DEFAULT_MARKERS

    if not wait_for_path(args.monitor, args.timeout):
        print(f"timed out waiting for {args.monitor}", file=sys.stderr)
        return 1
    if not wait_for_path(args.serial, args.timeout):
        print(f"timed out waiting for {args.serial}", file=sys.stderr)
        return 1

    sock = connect_monitor(args.monitor, args.timeout)
    deadline = time.time() + args.timeout
    marker_index = 0
    marker_pos = 0
    transcript = ""
    offset = 0

    with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
        while time.time() < deadline:
            log.seek(offset)
            chunk = log.read()
            offset = log.tell()
            if chunk:
                sys.stdout.write(chunk)
                sys.stdout.flush()
                transcript += chunk

                while marker_index < len(markers):
                    found_at = transcript.find(markers[marker_index], marker_pos)
                    if found_at < 0:
                        break
                    marker_pos = found_at + len(markers[marker_index])
                    marker_index += 1
                if marker_index == len(markers):
                    print("[usb-storage-smoke] PASS")
                    return shutdown_qemu(sock, args.pidfile)

                if len(transcript) > 262144:
                    trim_by = len(transcript) - 131072
                    transcript = transcript[-131072:]
                    marker_pos = max(0, marker_pos - trim_by)
            else:
                time.sleep(0.05)

    missing = markers[marker_index] if marker_index < len(markers) else "<none>"
    print(f"timed out waiting for marker: {missing}", file=sys.stderr)
    print_tail(transcript)
    shutdown_qemu(sock, args.pidfile)
    return 1


if __name__ == "__main__":
    sys.exit(main())
