#!/usr/bin/env python3

import argparse
import os
import re
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


def wait_for_prompt(log, timeout_s):
    deadline = time.time() + timeout_s
    offset = 0
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


class FtpClient:
    def __init__(self, host, port, timeout_s):
        self.host = host
        self.port = port
        self.timeout_s = timeout_s
        self.sock = connect_tcp(host, port, timeout_s)
        self.file = self.sock.makefile("rwb", buffering=0)

    def close(self):
        try:
            self.file.close()
        finally:
            self.sock.close()

    def read_reply(self):
        first = self.file.readline().decode("ascii", errors="replace").rstrip("\r\n")
        if not first:
            raise RuntimeError("FTP connection closed unexpectedly")

        lines = [first]
        if len(first) >= 4 and first[3] == "-":
            code = first[:3]
            while True:
                line = self.file.readline().decode("ascii", errors="replace").rstrip("\r\n")
                if not line:
                    raise RuntimeError("FTP connection closed during multiline reply")
                lines.append(line)
                if len(line) >= 4 and line.startswith(code + " "):
                    break
        return lines

    def command(self, text, expect=None):
        self.file.write((text + "\r\n").encode("ascii"))
        lines = self.read_reply()
        if expect and not lines[-1].startswith(expect):
            raise RuntimeError(f"{text} expected {expect}, got: {' | '.join(lines)}")
        print(f"{text}: {' | '.join(lines)}")
        return lines

    def pasv(self):
        lines = self.command("PASV", "227")
        match = re.search(r"\((\d+),(\d+),(\d+),(\d+),(\d+),(\d+)\)", lines[-1])
        if not match:
            raise RuntimeError(f"PASV reply did not contain endpoint: {lines[-1]}")
        octets = [int(match.group(i)) for i in range(1, 7)]
        port = octets[4] * 256 + octets[5]
        print(f"PASV data endpoint: 127.0.0.1:{port}")
        return connect_tcp("127.0.0.1", port, self.timeout_s)

    def download(self, command):
        data = self.pasv()
        self.command(command, "150")
        chunks = []
        deadline = time.time() + self.timeout_s
        while time.time() < deadline:
            try:
                chunk = data.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            chunks.append(chunk)
        data.close()
        payload = b"".join(chunks)
        self.read_expect("226", command)
        return payload

    def upload(self, command, payload):
        data = self.pasv()
        self.command(command, "150")
        data.sendall(payload)
        data.shutdown(socket.SHUT_WR)
        data.close()
        self.read_expect("226", command)

    def read_expect(self, prefix, context):
        lines = self.read_reply()
        if not lines[-1].startswith(prefix):
            raise RuntimeError(f"{context} completion expected {prefix}, got: {' | '.join(lines)}")
        print(f"{context} done: {' | '.join(lines)}")
        return lines


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


def run_ftp_smoke(args):
    ftp = FtpClient(args.host, args.port, args.timeout)
    try:
        banner = ftp.read_reply()
        if not banner[-1].startswith("220"):
            raise RuntimeError(f"banner expected 220, got: {' | '.join(banner)}")
        print(f"banner: {' | '.join(banner)}")

        ftp.command("USER nope", "331")
        ftp.command("PASS nope", "530")
        ftp.command("USER ftp", "331")
        ftp.command("PASS ftp", "230")
        ftp.command("SYST", "2")
        ftp.command("PWD", "257")
        ftp.command("CWD /", "250")
        ftp.command("CWD /NO_SUCH_DIR", "550")
        ftp.command("DELE NO_SUCH.TXT", "550")
        ftp.command("RMD NO_SUCH_DIR", "550")

        listing = ftp.download("LIST")
        list_text = listing.decode("ascii", errors="replace")
        print(f"LIST bytes: {len(listing)}")
        if "APPS" not in list_text.upper():
            raise RuntimeError("LIST did not include APPS")

        retr = ftp.download(f"RETR {args.retr_path}")
        print(f"RETR bytes: {len(retr)}")
        if len(retr) < 4 or retr[:4] != b"\x7fELF":
            raise RuntimeError("RETR did not return an ELF payload")

        payload = args.stor_payload.encode("ascii")
        ftp.upload(f"STOR {args.stor_path}", payload)
        print(f"STOR uploaded bytes: {len(payload)}")

        uploaded = ftp.download(f"RETR {args.stor_path}")
        if uploaded != payload:
            raise RuntimeError(
                "RETR uploaded root file did not match STOR payload: "
                f"expected {len(payload)} {payload!r}, got {len(uploaded)} {uploaded[:64]!r}"
            )

        listing = ftp.download("LIST")
        if args.stor_path.upper() not in listing.decode("ascii", errors="replace").upper():
            raise RuntimeError("LIST after STOR did not include uploaded file")

        ftp.command(f"DELE {args.stor_path}", "250")

        nested_payload = args.nested_payload.encode("ascii")
        ftp.command(f"MKD {args.nested_dir}", "257")
        ftp.command(f"CWD {args.nested_dir}", "250")
        ftp.command("PWD", "257")
        ftp.upload(f"STOR {args.nested_file}", nested_payload)
        nested = ftp.download(f"RETR {args.nested_file}")
        if nested != nested_payload:
            raise RuntimeError(
                "RETR nested uploaded file did not match STOR payload: "
                f"expected {len(nested_payload)} {nested_payload!r}, got {len(nested)} {nested[:64]!r}"
            )

        nested_listing = ftp.download("LIST")
        if args.nested_file.upper() not in nested_listing.decode("ascii", errors="replace").upper():
            raise RuntimeError("nested LIST did not include uploaded file")

        ftp.command(f"DELE {args.nested_file}", "250")
        ftp.command("CWD /", "250")
        ftp.command(f"RMD {args.nested_dir}", "250")

        ftp.command("QUIT", "221")
        print("FTP smoke PASS")
    finally:
        ftp.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2121)
    parser.add_argument("--retr-path", default="apps/demo/hello.elf")
    parser.add_argument("--stor-path", default="PY_SMOKE.TXT")
    parser.add_argument("--stor-payload", default="ftp smoke payload\r\n")
    parser.add_argument("--nested-dir", default="PYFTP")
    parser.add_argument("--nested-file", default="NEST.TXT")
    parser.add_argument("--nested-payload", default="nested ftp smoke payload\r\n")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    if not wait_for_path(args.monitor, args.timeout):
        print(f"timed out waiting for {args.monitor}", file=sys.stderr)
        return 1
    if not wait_for_path(args.serial, args.timeout):
        print(f"timed out waiting for {args.serial}", file=sys.stderr)
        return 1

    monitor = connect_monitor(args.monitor, args.timeout)
    ok = False
    try:
        with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
            offset = wait_for_prompt(log, args.timeout)
            send_text(monitor, "runelf_nowait apps/services/ftpd")
            send_key(monitor, "ret")
            wait_for_log(log, offset, "ftpd: listening", args.timeout)
        run_ftp_smoke(args)
        ok = True
        return 0
    except Exception as exc:
        print(f"FTP smoke FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        rc = shutdown_qemu(monitor, args.pidfile)
        if ok and rc != 0:
            raise SystemExit(rc)


if __name__ == "__main__":
    sys.exit(main())
