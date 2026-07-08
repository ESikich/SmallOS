#!/usr/bin/env python3

import argparse
import os
import shlex
import socket
import subprocess
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
    keymap = {
        " ": "spc",
        ".": "dot",
        "/": "slash",
        "-": "minus",
        ":": "shift-semicolon",
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


def wait_for_prompt(monitor, log, timeout_s, offset=0):
    deadline = time.time() + timeout_s
    buf = ""
    sent_login = False
    while time.time() < deadline:
        chunk, offset = read_new(log, offset)
        if chunk:
            tee_stdout(chunk)
            buf += chunk
            if "SmallOS login:" in buf and not sent_login:
                send_text(monitor, "root")
                send_key(monitor, "ret")
                sent_login = True
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
                return offset
            if len(buf) > 8192:
                buf = buf[-4096:]
        else:
            time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for serial marker: {marker}")


def run_guest_command(monitor, log, offset, command, timeout_s, marker=None):
    send_text(monitor, command)
    send_key(monitor, "ret")
    if marker:
        return wait_for_log(log, offset, marker, timeout_s)
    return wait_for_prompt(monitor, log, timeout_s, offset)


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


def ssh_base_args(port, identity):
    return [
        "ssh",
        "-p",
        str(port),
        "-i",
        identity,
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "UserKnownHostsFile=/dev/null",
        "-o",
        "LogLevel=ERROR",
        "-o",
        "BatchMode=yes",
        "root@127.0.0.1",
    ]


def log_ssh_result(log, title, cmd, returncode, stdout, stderr):
    print(f"--- {title} ---", file=log)
    print("$ " + " ".join(shlex.quote(arg) for arg in cmd), file=log)
    print(f"returncode={returncode}", file=log)
    if stdout:
        print("--- stdout ---", file=log)
        print(stdout, end="" if stdout.endswith("\n") else "\n", file=log)
    if stderr:
        print("--- stderr ---", file=log)
        print(stderr, end="" if stderr.endswith("\n") else "\n", file=log)
    log.flush()


def run_ssh_until_ok(port, identity, timeout_s, ssh_log):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        cmd = ssh_base_args(port, identity) + ["echo tinyssh-ok"]
        proc = subprocess.run(cmd, text=True, capture_output=True, timeout=20)
        log_ssh_result(ssh_log, "login probe", cmd, proc.returncode, proc.stdout, proc.stderr)
        last = proc
        if proc.returncode == 0 and "tinyssh-ok" in proc.stdout:
            return
        time.sleep(0.5)
    detail = ""
    if last is not None:
        detail = f" last rc={last.returncode} stdout={last.stdout!r} stderr={last.stderr!r}"
    raise RuntimeError(f"timed out waiting for TinySSH login.{detail}")


def run_pty_check(port, identity, ssh_log):
    cmd = ssh_base_args(port, identity)
    cmd.insert(1, "-tt")
    cmd += ["echo tinyssh-pty-ok"]
    proc = subprocess.run(cmd, text=True, capture_output=True, timeout=20)
    output = (proc.stdout or "") + (proc.stderr or "")
    log_ssh_result(ssh_log, "pty check", cmd, proc.returncode, proc.stdout, proc.stderr)
    if proc.returncode != 0 or "tinyssh-pty-ok" not in output:
        raise RuntimeError(f"interactive PTY check failed: rc={proc.returncode} output={output!r}")


def run_default_shell_check(port, identity, ssh_log):
    cmd = ssh_base_args(port, identity)
    cmd.insert(1, "-tt")
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(15)
    try:
        stdout, stderr = proc.communicate("echo tinyssh-shell-ok\nexit\n", timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()
    output = (stdout or "") + (stderr or "")
    log_ssh_result(ssh_log, "default shell check", cmd, proc.returncode, stdout, stderr)
    if (
        proc.returncode != 0
        or "SmallOS user shell" not in output
        or "tinyssh-shell-ok" not in output
    ):
        raise RuntimeError(
            f"default shell check failed: rc={proc.returncode} output={output!r}"
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--port", type=int, default=2222)
    parser.add_argument("--identity", default=".state/tinyssh-smoke-ed25519")
    parser.add_argument("--ssh-log", default="/tmp/smallos-tinyssh.log")
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()

    monitor = None
    exit_status = 1
    try:
        if not wait_for_path(args.serial, args.timeout):
            raise RuntimeError(f"timed out waiting for serial log: {args.serial}")
        monitor = connect_monitor(args.monitor, args.timeout)
        with open(args.ssh_log, "w", encoding="utf-8") as ssh_log, open(
            args.serial, "r", encoding="utf-8", errors="replace"
        ) as log:
            print(f"tinyssh-smoke SSH transcript log: {args.ssh_log}", file=ssh_log)
            ssh_log.flush()
            offset = wait_for_prompt(monitor, log, args.timeout)
            offset = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox udhcpc -n -q -i eth0",
                args.timeout,
                "lease acquired via SmallOS DHCP",
            )
            run_ssh_until_ok(args.port, args.identity, args.timeout, ssh_log)
            run_pty_check(args.port, args.identity, ssh_log)
            run_default_shell_check(args.port, args.identity, ssh_log)
        exit_status = 0
    finally:
        if monitor is not None:
            exit_status |= shutdown_qemu(monitor, args.pidfile)
    return exit_status


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"tinyssh-smoke: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
