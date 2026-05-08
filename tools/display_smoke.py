#!/usr/bin/env python3

import argparse
import os
import socket
import sys
import time
from pathlib import Path


FRAMEBUFFER_MARKER = "boot: PASS terminal: framebuffer console"
FORCED_VGA_MARKER = "boot: PASS terminal: VGA text forced"
SHELL_PROMPT = "/> "
MIN_NON_BLACK_PIXELS = 128


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


def read_new(log, offset):
    log.seek(offset)
    chunk = log.read()
    return chunk, log.tell()


def tee_stdout(text):
    if text:
        sys.stdout.write(text)
        sys.stdout.flush()


def wait_for_serial_truth(serial_path, mode, timeout_s):
    deadline = time.time() + timeout_s
    offset = 0
    buf = ""

    with open(serial_path, "r", encoding="utf-8", errors="replace") as log:
        while time.time() < deadline:
            chunk, offset = read_new(log, offset)
            if chunk:
                tee_stdout(chunk)
                buf += chunk

                if mode == "vga" and FRAMEBUFFER_MARKER in buf:
                    raise RuntimeError("framebuffer backend selected despite DISPLAY_BACKEND=vga")

                saw_prompt = SHELL_PROMPT in buf or "> " in buf
                if mode == "framebuffer":
                    if saw_prompt and FRAMEBUFFER_MARKER in buf:
                        return buf
                else:
                    if saw_prompt and FORCED_VGA_MARKER in buf:
                        return buf

                if len(buf) > 131072:
                    buf = buf[-65536:]
            else:
                time.sleep(0.05)

    if SHELL_PROMPT not in buf and "> " not in buf:
        raise RuntimeError("timed out waiting for shell prompt")
    if mode == "framebuffer":
        raise RuntimeError(f"missing framebuffer boot marker: {FRAMEBUFFER_MARKER}")
    raise RuntimeError(f"missing forced-VGA boot marker: {FORCED_VGA_MARKER}")


def ppm_next_token(data, index):
    n = len(data)
    while index < n:
        ch = data[index]
        if ch in b" \t\r\n":
            index += 1
            continue
        if ch == ord("#"):
            while index < n and data[index] not in b"\r\n":
                index += 1
            continue
        break

    if index >= n:
        raise RuntimeError("truncated PPM header")

    start = index
    while index < n and data[index] not in b" \t\r\n#":
        index += 1
    return data[start:index], index


def parse_ppm(path):
    ppm = Path(path)
    if not ppm.exists():
        raise RuntimeError(f"screenshot not created: {path}")

    data = ppm.read_bytes()
    token, index = ppm_next_token(data, 0)
    if token != b"P6":
        raise RuntimeError(f"unsupported PPM format {token!r}; expected P6")

    width_token, index = ppm_next_token(data, index)
    height_token, index = ppm_next_token(data, index)
    maxval_token, index = ppm_next_token(data, index)

    try:
        width = int(width_token)
        height = int(height_token)
        maxval = int(maxval_token)
    except ValueError as exc:
        raise RuntimeError("invalid numeric field in PPM header") from exc

    if maxval != 255:
        raise RuntimeError(f"unsupported PPM max value {maxval}; expected 255")

    if index >= len(data) or data[index] not in b" \t\r\n":
        raise RuntimeError("PPM header is not followed by raster data")
    index += 1

    expected = width * height * 3
    raster = data[index:]
    if len(raster) < expected:
        raise RuntimeError(f"truncated PPM raster: got {len(raster)} bytes, expected {expected}")

    non_black = 0
    for i in range(0, expected, 3):
        if raster[i] or raster[i + 1] or raster[i + 2]:
            non_black += 1

    return width, height, non_black


def take_screenshot(sock, path, timeout_s):
    screenshot = Path(path).resolve()
    screenshot.parent.mkdir(parents=True, exist_ok=True)
    try:
        screenshot.unlink()
    except FileNotFoundError:
        pass

    monitor_send(sock, f"screendump {screenshot}")

    deadline = time.time() + timeout_s
    last_size = -1
    stable_since = None
    while time.time() < deadline:
        if screenshot.exists() and screenshot.stat().st_size > 0:
            size = screenshot.stat().st_size
            now = time.time()
            if size == last_size:
                if stable_since is not None and now - stable_since >= 0.25:
                    return
            else:
                last_size = size
                stable_since = now
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for screendump: {screenshot}")


def shutdown_qemu(sock, pidfile):
    pid = None
    try:
        with open(pidfile, "r", encoding="utf-8") as f:
            pid = int(f.read().strip())
    except Exception:
        pid = None

    if sock is not None:
        try:
            monitor_send(sock, "quit")
        except OSError:
            pass

    if pid is None:
        return

    deadline = time.time() + 5.0
    while time.time() < deadline:
        try:
            os.kill(pid, 0)
        except OSError:
            return
        time.sleep(0.05)

    try:
        os.kill(pid, 15)
    except OSError:
        pass


def run(args):
    if not wait_for_path(args.monitor, args.timeout):
        raise RuntimeError(f"timed out waiting for {args.monitor}")
    if not wait_for_path(args.serial, args.timeout):
        raise RuntimeError(f"timed out waiting for {args.serial}")

    sock = connect_monitor(args.monitor, args.timeout)
    try:
        wait_for_serial_truth(args.serial, args.mode, args.timeout)
        take_screenshot(sock, args.screenshot, args.timeout)
        width, height, non_black = parse_ppm(args.screenshot)

        if non_black < args.min_non_black:
            raise RuntimeError(
                f"screenshot is blank or suspiciously tiny: {non_black} non-black pixels"
            )

        if args.mode == "framebuffer":
            if (width, height) != (1024, 768):
                raise RuntimeError(f"framebuffer screenshot dimensions were {width}x{height}, expected 1024x768")

        print(f"display {args.mode} smoke PASS ({width}x{height}, {non_black} non-black pixels)")
    finally:
        shutdown_qemu(sock, args.pidfile)


def main():
    parser = argparse.ArgumentParser(description="SmallOS display screenshot smoke")
    parser.add_argument("--mode", choices=("framebuffer", "vga"), required=True)
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--screenshot", required=True)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--min-non-black", type=int, default=MIN_NON_BLACK_PIXELS)
    args = parser.parse_args()

    try:
        run(args)
        return 0
    except Exception as exc:
        print(f"display {args.mode} smoke FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
