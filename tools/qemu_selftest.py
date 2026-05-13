#!/usr/bin/env python3

import argparse
import importlib
import os
import pkgutil
import socket
import sys
import threading
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
TEST_PACKAGES = ("tests.shell", "tests.elfs")
TRANSCRIPT_LIMIT = 262144
TRANSCRIPT_TRIM = 131072
STATUS_WIDTH = 38
BOOT_SPLASH_MARKERS = (
    "SmallOS boot diagnostics",
    "boot: PASS terminal: VGA text and serial console",
    "boot: PASS memory map: E820 available",
    "boot: PASS gdt: TSS selector loaded",
    "boot: PASS pmm: free frame baseline sane",
    "boot: PASS ata: primary channel ready",
    "boot: PASS tcp: service task queued",
    "boot: PASS ext2: volume mounted",
    "boot: PASS boot sequence: task queued",
    "SmallOS ready",
)
CONNECTPROBE_PORT = 45123


sys.path.insert(0, str(REPO_ROOT))


class TranscriptTimeout(RuntimeError):
    def __init__(self, message, transcript):
        super().__init__(message)
        self.transcript = transcript


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


class HostEchoServer:
    def __init__(self, port):
        self.port = port
        self.ready = threading.Event()
        self.done = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()
        if not self.ready.wait(2.0):
            raise RuntimeError("host echo server did not start")

    def _run(self):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
                srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                srv.bind(("127.0.0.1", self.port))
                srv.listen(1)
                srv.settimeout(15.0)
                self.ready.set()

                conn, _ = srv.accept()
                with conn:
                    conn.settimeout(5.0)
                    data = conn.recv(1024)
                    if data:
                        conn.sendall(data)
        except Exception as exc:
            self.error = exc
        finally:
            self.ready.set()
            self.done.set()

    def wait(self, timeout_s):
        if not self.done.wait(timeout_s):
            raise RuntimeError("host echo server did not finish")
        if self.error:
            raise RuntimeError(f"host echo server failed: {self.error}")


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


def clear_prompt_line(sock, count=16):
    for _ in range(count):
        send_key(sock, "backspace")
        time.sleep(0.01)


def read_new(log, offset):
    log.seek(offset)
    chunk = log.read()
    return chunk, log.tell()


def tee_stdout(text):
    if text:
        sys.stdout.write(text)
        sys.stdout.flush()


def status_begin(label):
    print(f"{label:<{STATUS_WIDTH}} ", end="", flush=True)


def status_end(ok, detail=None):
    suffix = "PASS" if ok else "FAIL"
    if detail:
        suffix += f" ({detail})"
    print(suffix, flush=True)


def status_line(label, ok, detail=None):
    status_begin(label)
    status_end(ok, detail)


def print_transcript_tail(title, transcript, line_count=80):
    lines = transcript.splitlines()
    if not lines:
        return
    print(f"\n--- {title} transcript tail ---", file=sys.stderr)
    for line in lines[-line_count:]:
        print(line, file=sys.stderr)
    print(f"--- end {title} transcript tail ---", file=sys.stderr)


def saw_prompt_after(buf, marker):
    marker_at = buf.rfind(marker)
    prompt_at = buf.rfind("/> ")
    return marker_at >= 0 and prompt_at > marker_at


def load_cases():
    cases = []

    for package_name in TEST_PACKAGES:
        pkg = importlib.import_module(package_name)
        pkg_path = Path(pkg.__file__).resolve().parent
        suite = package_name.rsplit(".", 1)[-1]
        if suite == "elfs":
            suite = "elf"

        for modinfo in sorted(pkgutil.iter_modules([str(pkg_path)]), key=lambda m: m.name):
            if modinfo.name.startswith("_"):
                continue
            mod = importlib.import_module(f"{package_name}.{modinfo.name}")
            for case in getattr(mod, "CASES", []):
                loaded_case = dict(case)
                loaded_case["suite"] = suite
                cases.append(loaded_case)

    return cases


def normalize_case(case):
    normalized = {
        "name": case["name"],
        "suite": case.get("suite", "case"),
        "must_contain": list(case.get("must_contain", [])),
        "interactive": list(case.get("interactive", [])),
        "timeout": float(case.get("timeout", 30.0)),
    }
    return normalized


def collect_selftest_transcript(sock, log, start_offset, deadline, cases, echo=True):
    buf = ""
    log_offset = start_offset

    if echo:
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
            if echo:
                tee_stdout(chunk)
            buf += chunk

            for marker, response in interactive:
                if marker in buf and marker not in responded:
                    time.sleep(0.15)
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

    raise TranscriptTimeout("timed out waiting for guest selftest", buf)


def verify_cases(cases, transcript, report=True):
    overall_pass = True
    failures = []
    for case in cases:
        missing = [marker for marker in case["must_contain"] if marker not in transcript]
        if missing:
            if report:
                status_line(f"{case['suite']}: {case['name']}", False)
                for marker in missing:
                    print(f"  missing: {marker}")
            failures.append((case, missing))
            overall_pass = False
        else:
            if report:
                status_line(f"{case['suite']}: {case['name']}", True)
    return overall_pass, failures


def report_case_failures(failures):
    for case, missing in failures:
        print(f"  {case['suite']}: {case['name']}: missing {len(missing)} marker(s)")
        for marker in missing[:5]:
            print(f"    missing: {marker}")
        if len(missing) > 5:
            print(f"    ... {len(missing) - 5} more")


def report_cases(cases, failures):
    failed = {(case["suite"], case["name"]) for case, _ in failures}
    for case in cases:
        case_key = (case["suite"], case["name"])
        status_line(f"{case['suite']}: {case['name']}", case_key not in failed)


def verify_boot_splash(transcript, report=True):
    missing = [marker for marker in BOOT_SPLASH_MARKERS if marker not in transcript]
    ok = not missing
    if report:
        detail = f"{len(BOOT_SPLASH_MARKERS) - len(missing)}/{len(BOOT_SPLASH_MARKERS)} markers"
        status_line("boot: diagnostics", ok, detail)
        for marker in missing:
            print(f"  missing: {marker}")
    return ok, missing


def run_interactive_regressions(sock, log, start_offset, deadline, echo=True, report=True):
    buf = ""
    log_offset = start_offset

    if echo:
        tee_stdout("\n[interactive-regression] ")
    send_text(sock, "runelf poop")
    send_key(sock, "ret")

    saw_failed = False
    while time.time() < deadline:
        chunk, log_offset = read_new(log, log_offset)
        if chunk:
            if echo:
                tee_stdout(chunk)
            buf += chunk
            if "runelf: failed" in buf and not saw_failed:
                saw_failed = True
                send_text(sock, "pwd")
                send_key(sock, "ret")
            if saw_failed and saw_prompt_after(buf, "pwd: /"):
                if report:
                    print("[runelf_missing_keeps_input] PASS")
                return True, log_offset
            if len(buf) > TRANSCRIPT_LIMIT:
                buf = buf[-TRANSCRIPT_TRIM:]
        else:
            time.sleep(0.05)

    if report:
        print("[runelf_missing_keeps_input] FAIL")
    return False, log_offset


def run_ctrl_c_regression(sock, log, start_offset, deadline, echo=True, report=True):
    buf = ""
    log_offset = start_offset

    if echo:
        tee_stdout("\n[ctrl-c-regression] ")
    send_text(sock, "runelf usr/libexec/tests/spinwkr late ctrlc.txt 500")
    send_key(sock, "ret")
    time.sleep(0.5)
    send_key(sock, "ctrl-c")

    saw_interrupt = False
    sent_probe = False
    while time.time() < deadline:
        chunk, log_offset = read_new(log, log_offset)
        if chunk:
            if echo:
                tee_stdout(chunk)
            buf += chunk

            if "^C" in buf and not saw_interrupt:
                saw_interrupt = True

            if saw_interrupt and not sent_probe:
                send_text(sock, "pwd")
                send_key(sock, "ret")
                sent_probe = True

            if saw_interrupt and saw_prompt_after(buf, "pwd: /"):
                if report:
                    print("[foreground_ctrl_c] PASS")
                return True, log_offset

            if "spinwkr late PASS" in buf:
                if report:
                    print("[foreground_ctrl_c] FAIL")
                return False, log_offset

            if len(buf) > TRANSCRIPT_LIMIT:
                buf = buf[-TRANSCRIPT_TRIM:]
        else:
            time.sleep(0.05)

    if report:
        print("[foreground_ctrl_c] FAIL")
    return False, log_offset


def run_signalfd_regression(sock, log, start_offset, deadline, echo=True, report=True):
    buf = ""
    log_offset = start_offset

    if echo:
        tee_stdout("\n[signalfd-regression] ")
    send_text(sock, "runelf usr/libexec/tests/signalfdprobe")
    send_key(sock, "ret")

    sent_interrupt = False
    sent_retry = False
    retry_at = 0.0
    while time.time() < deadline:
        chunk, log_offset = read_new(log, log_offset)
        if chunk:
            if echo:
                tee_stdout(chunk)
            buf += chunk

            if "signalfdprobe waiting" in buf and not sent_interrupt:
                send_key(sock, "ctrl-c")
                sent_interrupt = True
                retry_at = time.time() + 2.0

            if "signalfdprobe PASS" in buf and saw_prompt_after(buf, "signalfdprobe PASS"):
                if report:
                    print("[signalfd_ctrl_c] PASS")
                return True, log_offset

            if "signalfdprobe FAIL" in buf:
                if report:
                    print("[signalfd_ctrl_c] FAIL")
                return False, log_offset

            if sent_interrupt and "^C" in buf and "signalfdprobe PASS" not in buf:
                pass

            if len(buf) > TRANSCRIPT_LIMIT:
                buf = buf[-TRANSCRIPT_TRIM:]
        else:
            if (sent_interrupt and not sent_retry and time.time() >= retry_at and
                "signalfdprobe PASS" not in buf and
                "signalfdprobe FAIL" not in buf):
                send_key(sock, "ctrl-c")
                sent_retry = True
            time.sleep(0.05)

    if report:
        print("[signalfd_ctrl_c] FAIL")
    return False, log_offset


def run_process_group_ctrl_c_regression(sock, log, start_offset, deadline, echo=True, report=True):
    buf = ""
    log_offset = start_offset
    no_child_pass_until = 0.0
    saw_interrupt = False
    sent_interrupt = False
    sent_probe = False

    if echo:
        tee_stdout("\n[pgrp-ctrl-c-regression] ")
    clear_prompt_line(sock)
    send_text(sock, "runelf usr/libexec/tests/pgrpprobe")
    send_key(sock, "ret")

    while time.time() < deadline:
        chunk, log_offset = read_new(log, log_offset)
        if chunk:
            if echo:
                tee_stdout(chunk)
            buf += chunk

            if "pgrpprobe waiting" in buf and not sent_interrupt:
                send_key(sock, "ctrl-c")
                sent_interrupt = True

            if "^C" in buf and not saw_interrupt:
                saw_interrupt = True
                no_child_pass_until = time.time() + 2.0

            if saw_interrupt and not sent_probe:
                send_text(sock, "pwd")
                send_key(sock, "ret")
                sent_probe = True

            if "spinwkr late PASS" in buf:
                if report:
                    print("[process_group_ctrl_c] FAIL")
                return False, log_offset

            if (saw_interrupt and sent_probe and saw_prompt_after(buf, "pwd: /") and
                time.time() >= no_child_pass_until):
                if report:
                    print("[process_group_ctrl_c] PASS")
                return True, log_offset

            if len(buf) > TRANSCRIPT_LIMIT:
                buf = buf[-TRANSCRIPT_TRIM:]
        else:
            if (saw_interrupt and sent_probe and
                saw_prompt_after(buf, "pwd: /") and
                time.time() >= no_child_pass_until):
                if report:
                    print("[process_group_ctrl_c] PASS")
                return True, log_offset
            time.sleep(0.05)

    if report:
        print("[process_group_ctrl_c] FAIL")
    return False, log_offset


def run_connect_regression(sock, log, start_offset, deadline, echo=True, report=True):
    buf = ""
    log_offset = start_offset
    server = HostEchoServer(CONNECTPROBE_PORT)

    try:
        server.start()
    except RuntimeError as exc:
        if report:
            print(f"[outbound_connect] FAIL: {exc}")
        return False, log_offset

    if echo:
        tee_stdout("\n[connect-regression] ")
    clear_prompt_line(sock)
    send_text(sock, "runelf usr/libexec/tests/connectprobe")
    send_key(sock, "ret")

    while time.time() < deadline:
        chunk, log_offset = read_new(log, log_offset)
        if chunk:
            if echo:
                tee_stdout(chunk)
            buf += chunk

            if "connectprobe PASS" in buf and saw_prompt_after(buf, "connectprobe PASS"):
                try:
                    server.wait(2.0)
                except RuntimeError as exc:
                    if report:
                        print(f"[outbound_connect] FAIL: {exc}")
                    return False, log_offset
                if report:
                    print("[outbound_connect] PASS")
                return True, log_offset

            if "connectprobe FAIL" in buf:
                if report:
                    print("[outbound_connect] FAIL")
                return False, log_offset

            if len(buf) > TRANSCRIPT_LIMIT:
                buf = buf[-TRANSCRIPT_TRIM:]
        else:
            time.sleep(0.05)

    if report:
        print("[outbound_connect] FAIL")
    return False, log_offset


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
    parser.add_argument(
        "--summary",
        action="store_true",
        help="print a concise phase summary instead of the full serial transcript",
    )
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
    echo = not args.summary

    with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
        if args.summary:
            status_begin("boot: shell prompt")
        while time.time() < deadline:
            chunk, log_offset = read_new(log, log_offset)
            if chunk:
                if echo:
                    tee_stdout(chunk)
                buf += chunk
                if "> " in buf:
                    if args.summary:
                        status_end(True)
                    break
                if len(buf) > TRANSCRIPT_LIMIT:
                    buf = buf[-TRANSCRIPT_TRIM:]
            else:
                time.sleep(0.05)
        else:
            if args.summary:
                status_end(False)
                print_transcript_tail("boot", buf)
            print("timed out waiting for shell prompt", file=sys.stderr)
            return shutdown_qemu(sock, args.pidfile, False)

        boot_pass, boot_missing = verify_boot_splash(buf, report=not args.summary)
        if args.summary:
            detail = f"{len(BOOT_SPLASH_MARKERS) - len(boot_missing)}/{len(BOOT_SPLASH_MARKERS)} markers"
            status_line("boot: diagnostics", boot_pass, detail)
            for marker in boot_missing:
                print(f"  missing: {marker}")
        if not boot_pass:
            print_transcript_tail("boot", buf)
            return shutdown_qemu(sock, args.pidfile, False)

        if args.summary:
            status_begin("guest: selftest")
        try:
            transcript, log_offset = collect_selftest_transcript(
                sock, log, log_offset, deadline, cases, echo=echo
            )
        except RuntimeError as exc:
            if args.summary:
                status_end(False)
                print_transcript_tail("selftest", getattr(exc, "transcript", buf))
            print(str(exc), file=sys.stderr)
            return shutdown_qemu(sock, args.pidfile, False)

        guest_pass = "selftest: PASS" in transcript and "selftest: FAIL" not in transcript
        if args.summary:
            status_end(guest_pass)
            if not guest_pass:
                print_transcript_tail("selftest", transcript)

        case_pass, failures = verify_cases(cases, transcript, report=not args.summary)
        if args.summary:
            report_cases(cases, failures)
            detail = f"{len(cases) - len(failures)}/{len(cases)} cases"
            status_line("guest: expected markers", case_pass, detail)
            if failures:
                report_case_failures(failures)

        overall_pass = guest_pass and case_pass

        if args.summary:
            status_begin("interactive: runelf missing")
        interactive_pass, log_offset = run_interactive_regressions(
            sock, log, log_offset, deadline, echo=echo, report=not args.summary
        )
        if args.summary:
            status_end(interactive_pass)
        overall_pass = overall_pass and interactive_pass

        if args.summary:
            status_begin("interactive: foreground ctrl-c")
        ctrl_c_pass, log_offset = run_ctrl_c_regression(
            sock, log, log_offset, deadline, echo=echo, report=not args.summary
        )
        if args.summary:
            status_end(ctrl_c_pass)
        overall_pass = overall_pass and ctrl_c_pass

        if args.summary:
            status_begin("interactive: signalfd ctrl-c")
        signalfd_pass, log_offset = run_signalfd_regression(
            sock, log, log_offset, deadline, echo=echo, report=not args.summary
        )
        if args.summary:
            status_end(signalfd_pass)
        overall_pass = overall_pass and signalfd_pass

        if args.summary:
            status_begin("interactive: process group ctrl-c")
        pgrp_pass, log_offset = run_process_group_ctrl_c_regression(
            sock, log, log_offset, deadline, echo=echo, report=not args.summary
        )
        if args.summary:
            status_end(pgrp_pass)
        overall_pass = overall_pass and pgrp_pass

        if args.summary:
            status_begin("interactive: outbound connect")
        connect_pass, log_offset = run_connect_regression(
            sock, log, log_offset, deadline, echo=echo, report=not args.summary
        )
        if args.summary:
            status_end(connect_pass)
        overall_pass = overall_pass and connect_pass

    result = shutdown_qemu(sock, args.pidfile, overall_pass)
    if args.summary:
        status_line("result", result == 0)
    return result


if __name__ == "__main__":
    sys.exit(main())
