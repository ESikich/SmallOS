#!/usr/bin/env python3

import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path


SCENARIOS = {
    "no-loader": {
        "command": "usr/libexec/tests/dynhello",
        "markers": (
            "elf: missing interpreter: /lib/ld-smallos.so",
            "usr/libexec/tests/dynhello: failed",
        ),
    },
    "no-libc": {
        "command": "usr/libexec/tests/dynfailprobe",
        "markers": (
            "ld-smallos: library not found",
            "dynfailprobe status: PASS",
            "dynfailprobe PASS",
        ),
    },
}

ERROR_MARKERS = (
    "PANIC",
    "panic:",
    "Page fault",
    "page fault",
    "double fault",
    "Unhandled exception",
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
        elif ch == "\n":
            send_key(sock, "ret")
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


def prompt_after_markers(buf, markers):
    last_marker = -1
    for marker in markers:
        pos = buf.find(marker)
        if pos < 0:
            return False
        last_marker = max(last_marker, pos + len(marker))
    return buf.find("/> ", last_marker) >= 0


def check_errors(buf):
    for marker in ERROR_MARKERS:
        if marker in buf:
            raise RuntimeError(f"guest failure marker seen: {marker}")


def wait_for_prompt(log, offset, deadline):
    buf = ""
    while time.time() < deadline:
        chunk, offset = read_new(log, offset)
        if chunk:
            tee_stdout(chunk)
            buf += chunk
            check_errors(buf)
            if "/> " in buf:
                return offset
            if len(buf) > 65536:
                buf = buf[-32768:]
        else:
            time.sleep(0.05)
    raise RuntimeError("timed out waiting for shell prompt")


def wait_for_markers_and_prompt(log, offset, markers, deadline):
    buf = ""
    while time.time() < deadline:
        chunk, offset = read_new(log, offset)
        if chunk:
            tee_stdout(chunk)
            buf += chunk
            check_errors(buf)
            if prompt_after_markers(buf, markers):
                return offset
            if len(buf) > 65536:
                buf = buf[-32768:]
        else:
            time.sleep(0.05)

    missing = [marker for marker in markers if marker not in buf]
    if missing:
        raise RuntimeError(f"timed out waiting for marker: {missing[0]}")
    raise RuntimeError("timed out waiting for prompt after dynamic-link failure")


def shutdown_qemu(proc, sock):
    try:
        monitor_send(sock, "quit")
    except OSError:
        pass

    try:
        proc.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5.0)


def run_scenario(args):
    scenario = SCENARIOS[args.scenario]
    monitor = Path(args.monitor)
    serial = Path(args.serial)
    pidfile = Path(args.pidfile)
    monitor.parent.mkdir(parents=True, exist_ok=True)
    serial.parent.mkdir(parents=True, exist_ok=True)
    pidfile.parent.mkdir(parents=True, exist_ok=True)

    for path in (monitor, serial, pidfile):
        try:
            path.unlink()
        except FileNotFoundError:
            pass

    cmd = [
        args.qemu,
        "-drive",
        f"format=raw,file={args.image}",
        "-boot",
        "c",
        "-m",
        str(args.memory),
        "-serial",
        f"file:{serial}",
        "-nic",
        f"user,model=e1000,mac={args.net_mac}",
        "-display",
        "none",
        "-monitor",
        f"unix:{monitor},server,nowait",
        "-pidfile",
        str(pidfile),
    ]
    proc = subprocess.Popen(cmd)
    sock = None
    try:
        if not wait_for_path(serial, args.timeout):
            raise RuntimeError(f"timed out waiting for serial log: {serial}")
        sock = connect_monitor(str(monitor), args.timeout)
        deadline = time.time() + args.timeout

        with serial.open("r", encoding="utf-8", errors="replace") as log:
            offset = wait_for_prompt(log, 0, deadline)
            print(f"\n[dynlink-negative:{args.scenario}] {scenario['command']}")
            send_text(sock, scenario["command"])
            send_key(sock, "ret")
            wait_for_markers_and_prompt(log, offset, scenario["markers"], deadline)

        print(f"[dynlink-negative:{args.scenario}] PASS")
        return 0
    finally:
        if sock is not None:
            shutdown_qemu(proc, sock)
            sock.close()
        elif proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5.0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", required=True, choices=sorted(SCENARIOS))
    parser.add_argument("--image", required=True)
    parser.add_argument("--qemu", default="qemu-system-i386")
    parser.add_argument("--monitor", default="/tmp/smallos-dynlink-negative-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-dynlink-negative-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos-dynlink-negative.pid")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--memory", type=int, default=64)
    parser.add_argument("--net-mac", default="52:54:00:12:34:56")
    args = parser.parse_args()

    try:
        return run_scenario(args)
    except Exception as exc:
        print(f"dynlink-negative-smoke: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
