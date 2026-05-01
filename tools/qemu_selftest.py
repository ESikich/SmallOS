#!/usr/bin/env python3

import argparse
import importlib
import os
import pkgutil
import socket
import sys
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
TEST_PACKAGES = ("tests.shell", "tests.elfs")
TRANSCRIPT_LIMIT = 262144
TRANSCRIPT_TRIM = 131072


sys.path.insert(0, str(REPO_ROOT))


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


def load_cases():
    cases = []

    for package_name in TEST_PACKAGES:
        pkg = importlib.import_module(package_name)
        pkg_path = Path(pkg.__file__).resolve().parent

        for modinfo in sorted(pkgutil.iter_modules([str(pkg_path)]), key=lambda m: m.name):
            if modinfo.name.startswith("_"):
                continue
            mod = importlib.import_module(f"{package_name}.{modinfo.name}")
            cases.extend(getattr(mod, "CASES", []))

    return cases


def normalize_case(case):
    normalized = {
        "name": case["name"],
        "must_contain": list(case.get("must_contain", [])),
        "interactive": list(case.get("interactive", [])),
        "timeout": float(case.get("timeout", 30.0)),
    }
    return normalized


def collect_selftest_transcript(sock, log, start_offset, deadline, cases):
    buf = ""
    log_offset = start_offset

    tee_stdout("\n[selftest] ")
    send_text(sock, "selftest")
    send_key(sock, "ret")

    interactive = []
    for case in cases:
        interactive.extend(case["interactive"])
    responded = set()

    seen_result = False
    while time.time() < deadline:
        chunk, log_offset = read_new(log, log_offset)
        if chunk:
            tee_stdout(chunk)
            buf += chunk

            for marker, response in interactive:
                if marker in buf and marker not in responded:
                    send_text(sock, response)
                    send_key(sock, "ret")
                    responded.add(marker)

            if "selftest: PASS" in buf:
                seen_result = True
            if "selftest: FAIL" in buf:
                seen_result = True
            if seen_result and "> " in buf:
                return buf, log_offset
            if len(buf) > TRANSCRIPT_LIMIT:
                buf = buf[-TRANSCRIPT_TRIM:]
        else:
            time.sleep(0.05)

    raise RuntimeError("timed out waiting for guest selftest")


def verify_cases(cases, transcript):
    overall_pass = True
    for case in cases:
        missing = [marker for marker in case["must_contain"] if marker not in transcript]
        if missing:
            print(f"[{case['name']}] FAIL")
            for marker in missing:
                print(f"  missing: {marker}")
            overall_pass = False
        else:
            print(f"[{case['name']}] PASS")
    return overall_pass


def shutdown_qemu(sock, pidfile, result_pass):
    try:
        monitor_send(sock, "quit")
    except OSError:
        pass

    if not wait_for_path(pidfile, 1.0):
        return 0 if result_pass else 1

    deadline = time.time() + 5.0
    try:
        with open(pidfile, "r", encoding="utf-8") as f:
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
    cases = [normalize_case(case) for case in load_cases()]

    log_offset = 0
    deadline = time.time() + args.timeout
    buf = ""

    with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
        while time.time() < deadline:
            chunk, log_offset = read_new(log, log_offset)
            if chunk:
                tee_stdout(chunk)
                buf += chunk
                if "> " in buf:
                    break
                if len(buf) > TRANSCRIPT_LIMIT:
                    buf = buf[-TRANSCRIPT_TRIM:]
            else:
                time.sleep(0.05)
        else:
            print("timed out waiting for shell prompt", file=sys.stderr)
            return 1

        transcript, log_offset = collect_selftest_transcript(sock, log, log_offset, deadline, cases)
        overall_pass = verify_cases(cases, transcript)

    return shutdown_qemu(sock, args.pidfile, overall_pass)


if __name__ == "__main__":
    sys.exit(main())
