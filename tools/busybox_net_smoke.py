#!/usr/bin/env python3

import argparse
import http.server
import os
import socket
import socketserver
import sys
import threading
import time


WGET_BODY = b"busybox-net-ok\n"
NC_BODY = b"smallos-nc-ok\n"
WHOIS_BODY = b"Domain Name: smallos.test\r\nsmallos-whois-ok\r\n"
FTPGET_BODY = b"smallos-ftpget-ok\n"
FTPPUT_BODY = b"smallos-ftpput-ok\n"
TFTP_BODY = b"smallos-tftp-ok\n"
TCPSVD_BODY = b"smallos-tcpsvd-ok\n"
TFTPD_BODY = b"smallos-tftpd-ok"


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
        "_": "shift-minus",
        ".": "dot",
        "/": "slash",
        "-": "minus",
        ":": "shift-semicolon",
        "'": "apostrophe",
        "|": "shift-backslash",
        ">": "shift-dot",
        "<": "shift-comma",
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
                return offset, buf
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
                return offset, buf
            if len(buf) > 16384:
                buf = buf[-8192:]
        else:
            time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for serial marker: {marker}")


def run_guest_command(monitor, log, offset, command, timeout_s, marker=None):
    send_text(monitor, command)
    send_key(monitor, "ret")
    if marker:
        offset, buf = wait_for_log(log, offset, marker, timeout_s)
        return offset, buf
    return wait_for_prompt(monitor, log, timeout_s, offset)


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


class QuietTCPServer(socketserver.TCPServer):
    allow_reuse_address = True


class SmokeHTTPHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/busybox.txt":
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(WGET_BODY)))
        self.end_headers()
        self.wfile.write(WGET_BODY)

    def log_message(self, fmt, *args):
        return


def start_http_server(port):
    server = QuietTCPServer(("127.0.0.1", port), SmokeHTTPHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def start_echo_server(port):
    ready = threading.Event()
    done = threading.Event()
    state = {"error": None}

    def worker():
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
                srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                srv.bind(("127.0.0.1", port))
                srv.listen(1)
                ready.set()
                conn, _ = srv.accept()
                with conn:
                    conn.settimeout(5.0)
                    data = b""
                    while b"\n" not in data:
                        chunk = conn.recv(128)
                        if not chunk:
                            break
                        data += chunk
                    if NC_BODY.strip() not in data:
                        raise RuntimeError(f"nc payload mismatch: {data!r}")
                    conn.sendall(NC_BODY)
        except Exception as exc:
            state["error"] = exc
        finally:
            done.set()

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    if not ready.wait(5.0):
        raise RuntimeError("timed out starting echo server")
    return done, state


def start_whois_server(port):
    ready = threading.Event()
    done = threading.Event()
    state = {"error": None}

    def worker():
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
                srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                srv.bind(("127.0.0.1", port))
                srv.listen(1)
                ready.set()
                conn, _ = srv.accept()
                with conn:
                    conn.settimeout(5.0)
                    data = b""
                    while b"\n" not in data:
                        chunk = conn.recv(128)
                        if not chunk:
                            break
                        data += chunk
                    if b"smallos.test" not in data:
                        raise RuntimeError(f"whois query mismatch: {data!r}")
                    conn.sendall(WHOIS_BODY)
        except Exception as exc:
            state["error"] = exc
        finally:
            done.set()

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    if not ready.wait(5.0):
        raise RuntimeError("timed out starting whois server")
    return done, state


def _ftp_readline(sock):
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            break
        data += chunk
    return data.decode("ascii", errors="replace").strip()


def _ftp_sendline(sock, line):
    sock.sendall((line + "\r\n").encode("ascii"))


def start_ftp_server(control_port, data_port, mode):
    ready = threading.Event()
    done = threading.Event()
    state = {"error": None, "uploaded": b"", "saw_upload": False}

    def worker():
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as data_srv:
                data_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                data_srv.bind(("127.0.0.1", data_port))
                data_srv.listen(1)
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as ctrl_srv:
                    ctrl_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    ctrl_srv.bind(("127.0.0.1", control_port))
                    ctrl_srv.listen(1)
                    ready.set()
                    ctrl, _ = ctrl_srv.accept()
                    with ctrl:
                        ctrl.settimeout(5.0)
                        _ftp_sendline(ctrl, "220 smallos ftp")
                        while True:
                            line = _ftp_readline(ctrl)
                            if not line:
                                break
                            verb = line.split(" ", 1)[0].upper()
                            if verb == "USER":
                                _ftp_sendline(ctrl, "230 logged in")
                            elif verb == "PASS":
                                _ftp_sendline(ctrl, "230 logged in")
                            elif verb == "TYPE":
                                _ftp_sendline(ctrl, "200 type ok")
                            elif verb == "PASV":
                                p1 = data_port // 256
                                p2 = data_port % 256
                                _ftp_sendline(
                                    ctrl,
                                    f"227 Entering Passive Mode (10,0,2,2,{p1},{p2})",
                                )
                            elif verb == "SIZE":
                                _ftp_sendline(ctrl, f"213 {len(FTPGET_BODY)}")
                            elif verb == "RETR" and mode == "get":
                                data_conn, _ = data_srv.accept()
                                with data_conn:
                                    _ftp_sendline(ctrl, "150 opening data")
                                    data_conn.sendall(FTPGET_BODY)
                                _ftp_sendline(ctrl, "226 done")
                            elif verb == "STOR" and mode == "put":
                                data_conn, _ = data_srv.accept()
                                uploaded = b""
                                with data_conn:
                                    _ftp_sendline(ctrl, "150 opening data")
                                    data_conn.settimeout(5.0)
                                    while True:
                                        chunk = data_conn.recv(512)
                                        if not chunk:
                                            break
                                        uploaded += chunk
                                state["uploaded"] = uploaded
                                state["saw_upload"] = True
                                if FTPPUT_BODY.strip() not in uploaded:
                                    raise RuntimeError(f"ftpput payload mismatch: {uploaded!r}")
                                _ftp_sendline(ctrl, "226 done")
                            elif verb == "QUIT":
                                _ftp_sendline(ctrl, "221 bye")
                                break
                            else:
                                raise RuntimeError(f"unexpected FTP command in {mode}: {line}")
                        if mode == "put" and not state["saw_upload"]:
                            raise RuntimeError("ftpput did not upload any data")
        except Exception as exc:
            state["error"] = exc
        finally:
            done.set()

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    if not ready.wait(5.0):
        raise RuntimeError("timed out starting ftp server")
    return done, state


def start_tftp_get_server(port):
    ready = threading.Event()
    done = threading.Event()
    state = {"error": None}

    def worker():
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as srv:
                srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                srv.bind(("127.0.0.1", port))
                srv.settimeout(10.0)
                ready.set()
                packet, peer = srv.recvfrom(1024)
                if not packet.startswith(b"\x00\x01") or b"busybox.txt\x00" not in packet:
                    raise RuntimeError(f"unexpected tftp rrq: {packet!r}")
                srv.sendto(b"\x00\x03\x00\x01" + TFTP_BODY, peer)
                ack, _ = srv.recvfrom(1024)
                if ack != b"\x00\x04\x00\x01":
                    raise RuntimeError(f"unexpected tftp ack: {ack!r}")
        except Exception as exc:
            state["error"] = exc
        finally:
            done.set()

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    if not ready.wait(5.0):
        raise RuntimeError("timed out starting tftp server")
    return done, state


def fetch_guest_http(port, timeout_s):
    with connect_tcp("127.0.0.1", port, timeout_s) as sock:
        sock.sendall(
            b"GET / HTTP/1.0\r\n"
            b"Host: 127.0.0.1\r\n"
            b"Connection: close\r\n"
            b"\r\n"
        )
        data = b""
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            data += chunk
        if b"200" not in data.split(b"\r\n", 1)[0]:
            raise RuntimeError(f"busybox httpd returned unexpected response: {data[:80]!r}")
        if b"cserve on SmallOS" not in data:
            raise RuntimeError("busybox httpd response did not include expected body marker")
        print(f"busybox httpd fetch: {len(data)} bytes")


def check_guest_tcpsvd(port, timeout_s):
    with connect_tcp("127.0.0.1", port, timeout_s) as sock:
        sock.sendall(TCPSVD_BODY)
        data = b""
        deadline = time.time() + timeout_s
        while time.time() < deadline and TCPSVD_BODY not in data:
            try:
                chunk = sock.recv(512)
            except socket.timeout:
                continue
            if not chunk:
                break
            data += chunk
        if TCPSVD_BODY not in data:
            raise RuntimeError(f"tcpsvd echo mismatch: {data!r}")
        print(f"busybox tcpsvd echo: {len(data)} bytes")


def fetch_guest_tftpd(port, timeout_s):
    request = b"\x00\x01tmp/bbtftpd.txt\x00octet\x00"
    data = b""
    block = 1
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(1.0)
        sock.sendto(request, ("127.0.0.1", port))
        deadline = time.time() + timeout_s
        peer = None
        while time.time() < deadline:
            try:
                packet, peer = sock.recvfrom(1024)
            except socket.timeout:
                if peer is None:
                    sock.sendto(request, ("127.0.0.1", port))
                continue
            if len(packet) < 4:
                raise RuntimeError(f"short tftpd packet: {packet!r}")
            opcode = int.from_bytes(packet[0:2], "big")
            pkt_block = int.from_bytes(packet[2:4], "big")
            if opcode == 5:
                raise RuntimeError(f"tftpd error packet: {packet[4:]!r}")
            if opcode != 3 or pkt_block != block:
                raise RuntimeError(f"unexpected tftpd packet: {packet!r}")
            payload = packet[4:]
            data += payload
            sock.sendto(b"\x00\x04" + packet[2:4], peer)
            if len(payload) < 512:
                break
            block += 1
        else:
            raise RuntimeError("timed out waiting for guest tftpd")
    if TFTPD_BODY not in data:
        raise RuntimeError(f"tftpd body mismatch: {data!r}")
    print(f"busybox tftpd fetch: {len(data)} bytes")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--guest-http-port", type=int, default=18081)
    parser.add_argument("--host-http-port", type=int, default=18080)
    parser.add_argument("--host-echo-port", type=int, default=18082)
    parser.add_argument("--host-whois-port", type=int, default=18083)
    parser.add_argument("--host-ftp-port", type=int, default=18084)
    parser.add_argument("--host-ftp-data-port", type=int, default=18085)
    parser.add_argument("--host-tftp-port", type=int, default=18086)
    parser.add_argument("--guest-tcpsvd-port", type=int, default=18087)
    parser.add_argument("--guest-tftpd-port", type=int, default=18088)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    http_server = start_http_server(args.host_http_port)
    monitor = None
    exit_status = 1
    try:
        if not wait_for_path(args.serial, args.timeout):
            raise RuntimeError(f"timed out waiting for serial log: {args.serial}")
        monitor = connect_monitor(args.monitor, args.timeout)
        with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
            offset, _ = wait_for_prompt(monitor, log, args.timeout)

            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox udhcpc -n -q -i eth0",
                args.timeout,
                "lease acquired via SmallOS DHCP",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"bg usr/bin/busybox httpd -f -p {args.guest_http_port} -h /var/www",
                args.timeout,
            )
            fetch_guest_http(args.guest_http_port, args.timeout)

            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"usr/bin/busybox wget -q -O /tmp/bbwget.txt http://10.0.2.2:{args.host_http_port}/busybox.txt",
                args.timeout,
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox grep busybox-net-ok /tmp/bbwget.txt",
                args.timeout,
                "busybox-net-ok",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"usr/bin/busybox pscan -p {args.host_http_port} -P {args.host_http_port} 10.0.2.2 | usr/bin/busybox grep open",
                args.timeout,
                "\ttcp\topen\t",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox hostname",
                args.timeout,
                "smallos",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox ipcalc -m 10.0.2.15 | usr/bin/busybox grep NETMASK",
                args.timeout,
                "NETMASK=",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox netstat -r | usr/bin/busybox grep eth0",
                args.timeout,
                "10.0.2.2",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox netstat | usr/bin/busybox grep Active",
                args.timeout,
                "Internet connections",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox nslookup localhost | usr/bin/busybox grep Address",
                args.timeout,
                "127.0.0.1",
            )
            whois_done, whois_state = start_whois_server(args.host_whois_port)
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"usr/bin/busybox whois -h 10.0.2.2 -p {args.host_whois_port} smallos.test | usr/bin/busybox grep whois-ok",
                args.timeout,
                "smallos-whois-ok",
            )
            if not whois_done.wait(5.0):
                raise RuntimeError("timed out waiting for host whois server")
            if whois_state["error"] is not None:
                raise whois_state["error"]

            ftp_done, ftp_state = start_ftp_server(
                args.host_ftp_port,
                args.host_ftp_data_port,
                "get",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"usr/bin/busybox ftpget -P {args.host_ftp_port} 10.0.2.2 /tmp/bbftpget.txt busybox.txt",
                args.timeout,
            )
            if not ftp_done.wait(5.0):
                raise RuntimeError("timed out waiting for host ftpget server")
            if ftp_state["error"] is not None:
                raise ftp_state["error"]
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox grep ftpget-ok /tmp/bbftpget.txt",
                args.timeout,
                "smallos-ftpget-ok",
            )

            ftp_done, ftp_state = start_ftp_server(
                args.host_ftp_port,
                args.host_ftp_data_port,
                "put",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox printf smallos-ftpput-ok | usr/bin/busybox tee /tmp/bbftpput.txt",
                args.timeout,
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"usr/bin/busybox ftpput -P {args.host_ftp_port} 10.0.2.2 uploaded.txt /tmp/bbftpput.txt",
                args.timeout,
            )
            if not ftp_done.wait(5.0):
                raise RuntimeError("timed out waiting for host ftpput server")
            if ftp_state["error"] is not None:
                raise ftp_state["error"]

            tftp_done, tftp_state = start_tftp_get_server(args.host_tftp_port)
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"usr/bin/busybox tftp -g -l /tmp/bbtftp.txt -r busybox.txt 10.0.2.2 {args.host_tftp_port}",
                args.timeout,
            )
            if not tftp_done.wait(5.0):
                raise RuntimeError("timed out waiting for host tftp server")
            if tftp_state["error"] is not None:
                raise tftp_state["error"]
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox grep tftp-ok /tmp/bbtftp.txt",
                args.timeout,
                "smallos-tftp-ok",
            )

            echo_done, echo_state = start_echo_server(args.host_echo_port)
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"echo smallos-nc-ok | usr/bin/busybox nc 10.0.2.2 {args.host_echo_port} | usr/bin/busybox grep smallos-nc-ok",
                args.timeout,
            )
            if not echo_done.wait(5.0):
                raise RuntimeError("timed out waiting for host echo server")
            if echo_state["error"] is not None:
                raise echo_state["error"]

            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"bg usr/bin/busybox tcpsvd -E 0.0.0.0 {args.guest_tcpsvd_port} usr/bin/busybox cat",
                args.timeout,
            )
            check_guest_tcpsvd(args.guest_tcpsvd_port, args.timeout)

            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox printf smallos-tftpd-ok | usr/bin/busybox tee /tmp/bbtftpd.txt",
                args.timeout,
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                f"bg usr/bin/busybox udpsvd -E 0.0.0.0 {args.guest_tftpd_port} usr/bin/busybox tftpd",
                args.timeout,
            )
            fetch_guest_tftpd(args.guest_tftpd_port, args.timeout)

            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "arpgw",
                args.timeout,
                "arpgw:",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox ip neigh show | usr/bin/busybox grep 10.0.2.2",
                args.timeout,
                "52:55:0a:00:02:02",
            )
            offset, _ = run_guest_command(
                monitor,
                log,
                offset,
                "usr/bin/busybox arp -n | usr/bin/busybox grep 52:55",
                args.timeout,
                "on eth0",
            )
            print("busybox network smoke PASS")
            exit_status = 0
    except Exception as exc:
        print(f"busybox network smoke FAIL: {exc}", file=sys.stderr)
        exit_status = 1
    finally:
        http_server.shutdown()
        http_server.server_close()
        if monitor is not None:
            shutdown_qemu(monitor, args.pidfile)
            try:
                monitor.close()
            except OSError:
                pass
    return exit_status


if __name__ == "__main__":
    raise SystemExit(main())
