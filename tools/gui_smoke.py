#!/usr/bin/env python3

import argparse
import os
import re
import socket
import sys
import time


GUI_ERROR_MARKERS = (
    "gui: framebuffer not available",
    "gui: display is already in use",
    "gui: unsupported display size",
    "gui: out of memory opening display",
    "gui: could not open display",
    "gui: present failed",
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


def capture_screen(sock, path, timeout_s):
    path = os.path.abspath(path)
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    monitor_send(sock, f"screendump {path}")
    if not wait_for_path(path, timeout_s):
        raise RuntimeError(f"timed out waiting for screenshot: {path}")
    return read_ppm(path)


def read_ppm(path):
    with open(path, "rb") as image:
        if image.readline().strip() != b"P6":
            raise RuntimeError(f"unexpected screenshot format: {path}")
        tokens = []
        while len(tokens) < 3:
            line = image.readline()
            if not line:
                raise RuntimeError(f"truncated screenshot header: {path}")
            line = line.split(b"#", 1)[0]
            tokens.extend(line.split())
        width, height, maximum = (int(token) for token in tokens[:3])
        if maximum != 255:
            raise RuntimeError(f"unsupported screenshot depth: {maximum}")
        pixels = image.read(width * height * 3)
        if len(pixels) != width * height * 3:
            raise RuntimeError(f"truncated screenshot pixels: {path}")
        return width, height, pixels


def changed_bytes(before, after):
    if before[:2] != after[:2]:
        raise RuntimeError("GUI screenshots changed dimensions")
    return sum(a != b for a, b in zip(before[2], after[2]))


def pixel_at(image, x, y):
    width, height, pixels = image
    if x < 0 or y < 0 or x >= width or y >= height:
        raise RuntimeError("screenshot pixel coordinate is out of bounds")
    offset = (y * width + x) * 3
    return pixels[offset:offset + 3]


def send_key(sock, key):
    monitor_send(sock, f"sendkey {key}")


def move_mouse(sock, dx, dy):
    monitor_send(sock, f"mouse_move {dx} {dy}")


def set_mouse_buttons(sock, buttons):
    monitor_send(sock, f"mouse_button {buttons}")


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


def find_gui_error(buf):
    for marker in GUI_ERROR_MARKERS:
        if marker in buf:
            return marker
    return None


def wait_for_prompt(monitor, log, offset, timeout_s):
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


def wait_for_marker_or_error(log, offset, marker, timeout_s):
    deadline = time.time() + timeout_s
    buf = ""

    while time.time() < deadline:
        chunk, offset = read_new(log, offset)
        if chunk:
            tee_stdout(chunk)
            buf += chunk
            error = find_gui_error(buf)
            if error:
                raise RuntimeError(error)
            if marker in buf:
                return offset
            if len(buf) > 8192:
                buf = buf[-4096:]
        else:
            time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for serial marker: {marker}")


def wait_for_prompt_or_error(log, offset, timeout_s):
    deadline = time.time() + timeout_s
    buf = ""

    while time.time() < deadline:
        chunk, offset = read_new(log, offset)
        if chunk:
            tee_stdout(chunk)
            buf += chunk
            error = find_gui_error(buf)
            if error:
                raise RuntimeError(error)
            if "> " in buf:
                return offset
            if len(buf) > 8192:
                buf = buf[-4096:]
        else:
            time.sleep(0.05)
    raise RuntimeError("gui did not return to shell after q")


def read_last_gui_metrics(path):
    with open(path, "r", encoding="utf-8", errors="replace") as log:
        lines = [line for line in log if line.startswith("gui: metrics ")]
    if not lines:
        raise RuntimeError("diagnostic GUI run did not emit exit metrics")
    return {
        key: int(value)
        for key, value in re.findall(r"([a-z_]+)=([0-9]+)", lines[-1])
    }


def run_gui_smoke(args):
    if not wait_for_path(args.monitor, args.timeout):
        raise RuntimeError(f"timed out waiting for {args.monitor}")
    if not wait_for_path(args.serial, args.timeout):
        raise RuntimeError(f"timed out waiting for {args.serial}")

    monitor = connect_monitor(args.monitor, args.timeout)
    ok = False
    try:
        with open(args.serial, "r", encoding="utf-8", errors="replace") as log:
            offset = wait_for_prompt(monitor, log, 0, args.timeout)

            tee_stdout("\n[smoke:gui] create editor fixture\n")
            send_text(
                monitor,
                "edit tmp/gui_edit.txt -c a -c seed -c . -c wq",
            )
            send_key(monitor, "ret")
            offset = wait_for_prompt_or_error(log, offset, args.timeout)

            tee_stdout("[smoke:gui] create Files scrollbar fixtures\n")
            for index in range(12):
                send_text(monitor, f"mkdir gui_scroll_{index}")
                send_key(monitor, "ret")
                offset = wait_for_prompt_or_error(log, offset, args.timeout)

            tee_stdout("\n[smoke:gui] launch\n")
            send_text(monitor, "gui --diagnostics tmp/gui_edit.txt")
            send_key(monitor, "ret")
            offset = wait_for_marker_or_error(
                log,
                offset,
                "gui: starting",
                args.timeout,
            )

            time.sleep(args.settle)
            os.makedirs(args.screenshot_dir, exist_ok=True)
            editor = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-editor.ppm"),
                args.timeout,
            )

            tee_stdout("[smoke:gui] edit, guard close, select, copy, paste, and save\n")
            send_text(monitor, "saved_")
            send_key(monitor, "f2")
            send_text(monitor, "dirty_")
            send_key(monitor, "esc")
            time.sleep(args.settle)
            confirm = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-editor-confirm.ppm"),
                args.timeout,
            )
            if changed_bytes(editor, confirm) < 1000:
                raise RuntimeError("dirty editor close did not show confirmation")
            # The desktop modal consumes Ctrl+Esc before the Start shortcut;
            # its Escape component still cancels the dialog.
            send_key(monitor, "ctrl-esc")
            time.sleep(args.settle)
            edited = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-editor-edited.ppm"),
                args.timeout,
            )
            send_key(monitor, "ctrl-a")
            time.sleep(args.settle)
            selected = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-editor-selected.ppm"),
                args.timeout,
            )
            if changed_bytes(edited, selected) < 100:
                raise RuntimeError("editor selection was not visibly highlighted")
            send_key(monitor, "ctrl-c")
            send_key(monitor, "end")
            send_text(monitor, "_copy_")
            send_key(monitor, "ctrl-v")
            send_key(monitor, "f2")
            tee_stdout("[smoke:gui] new, Save As, and Open workflows\n")
            send_key(monitor, "ctrl-n")
            send_text(monitor, "newdocvalue")
            send_key(monitor, "ctrl-s")
            time.sleep(args.settle)
            picker_dialog = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-editor-save-as.ppm"),
                args.timeout,
            )
            if changed_bytes(selected, picker_dialog) < 1000:
                raise RuntimeError("Save As did not show the shared file picker")
            send_key(monitor, "tab")
            send_text(monitor, "tmp/gui_picker.txt")
            send_key(monitor, "ret")
            send_key(monitor, "ctrl-o")
            send_key(monitor, "down")
            send_key(monitor, "down")
            send_key(monitor, "ret")
            send_key(monitor, "esc")
            time.sleep(args.settle)
            desktop = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-desktop.ppm"),
                args.timeout,
            )

            tee_stdout("[smoke:gui] exercise Start and native apps\n")
            send_key(monitor, "ctrl-esc")
            time.sleep(args.settle)
            start_menu = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-start.ppm"),
                args.timeout,
            )
            if changed_bytes(desktop, start_menu) < 1000:
                raise RuntimeError("Start menu did not visibly open")
            send_key(monitor, "v")
            send_key(monitor, "ret")
            time.sleep(args.settle)
            viewer = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-viewer.ppm"),
                args.timeout,
            )
            if changed_bytes(desktop, viewer) < 10000:
                raise RuntimeError("Viewer did not visibly open from Start")
            send_key(monitor, "x")
            send_key(monitor, "ctrl-esc")
            send_key(monitor, "t")
            send_key(monitor, "ret")
            time.sleep(args.settle)
            tasks = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-tasks.ppm"),
                args.timeout,
            )
            if changed_bytes(desktop, tasks) < 10000:
                raise RuntimeError("Tasks did not visibly open from Start")
            send_key(monitor, "alt-f10")
            send_key(monitor, "alt-f10")
            send_key(monitor, "x")
            send_key(monitor, "ctrl-esc")
            send_key(monitor, "n")
            send_key(monitor, "ret")
            time.sleep(args.settle)
            network = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-network.ppm"),
                args.timeout,
            )
            if changed_bytes(desktop, network) < 10000:
                raise RuntimeError("Network did not visibly open from Start")
            send_key(monitor, "alt-f9")
            send_key(monitor, "alt-tab")
            send_key(monitor, "x")

            tee_stdout("[smoke:gui] open Files via keyboard\n")
            send_key(monitor, "f")
            time.sleep(args.settle)
            files = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-files.ppm"),
                args.timeout,
            )
            if changed_bytes(desktop, files) < 10000:
                raise RuntimeError("opening Files did not materially change the desktop")

            tee_stdout("[smoke:gui] create folder through shared modal\n")
            send_key(monitor, "ctrl-n")
            time.sleep(args.settle)
            files_dialog = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-files-new-folder.ppm"),
                args.timeout,
            )
            if changed_bytes(files, files_dialog) < 1000:
                raise RuntimeError("Files New Folder did not show the shared modal")
            for _ in range(10):
                send_key(monitor, "backspace")
            send_text(monitor, "gui_modal_dir")
            send_key(monitor, "ret")
            time.sleep(args.settle)

            tee_stdout("[smoke:gui] scroll Files list\n")
            send_key(monitor, "pgdn")
            time.sleep(args.settle)
            scrolled_files = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-files-scrolled.ppm"),
                args.timeout,
            )
            if changed_bytes(files, scrolled_files) < 500:
                raise RuntimeError("Files keyboard navigation did not scroll")

            tee_stdout("[smoke:gui] resize Files\n")
            move_mouse(monitor, 3, 25)
            time.sleep(0.1)
            set_mouse_buttons(monitor, 1)
            move_mouse(monitor, 80, 60)
            time.sleep(args.settle)
            set_mouse_buttons(monitor, 0)
            resized_files = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-files-resized.ppm"),
                args.timeout,
            )
            if changed_bytes(files, resized_files) < 10000:
                raise RuntimeError("resizing Files did not materially change the window")

            tee_stdout("[smoke:gui] close Files and open Shell\n")
            send_key(monitor, "x")
            send_key(monitor, "t")
            time.sleep(args.settle)
            send_text(monitor, "echo gui_smoke")
            send_key(monitor, "ret")
            time.sleep(args.settle)
            shell = capture_screen(
                monitor,
                os.path.join(args.screenshot_dir, "gui-shell.ppm"),
                args.timeout,
            )
            if changed_bytes(desktop, shell) < 10000:
                raise RuntimeError("opening Shell did not materially change the desktop")
            for x, y in ((100, 100), (850, 650)):
                if pixel_at(desktop, x, y) != pixel_at(shell, x, y):
                    raise RuntimeError("Shell startup damaged the desktop background")

            send_key(monitor, "esc")
            send_key(monitor, "q")
            offset = wait_for_prompt_or_error(log, offset, args.timeout)

            metrics = read_last_gui_metrics(args.serial)
            saved = metrics.get("saved_presentbuffer", 0)
            if saved < 2.5 * 1024 * 1024:
                raise RuntimeError(
                    f"present-buffer removal saved only {saved} bytes at 1024x768"
                )
            if metrics.get("startup_ram", 0) <= 0:
                raise RuntimeError("diagnostics did not report startup user memory")
            if metrics.get("dirty", 0) <= 0 or metrics.get("presented", 0) <= 0:
                raise RuntimeError("diagnostics did not report rendering activity")

            tee_stdout("[smoke:gui] verify editor persistence\n")
            send_text(monitor, "cat tmp/gui_edit.txt")
            send_key(monitor, "ret")
            offset = wait_for_marker_or_error(
                log,
                offset,
                "saved_dirty_seed_copy_saved_dirty_seed",
                args.timeout,
            )
            send_text(monitor, "cat tmp/gui_picker.txt")
            send_key(monitor, "ret")
            offset = wait_for_marker_or_error(
                log,
                offset,
                "newdocvalue",
                args.timeout,
            )

            tee_stdout("[smoke:gui] clean fixtures\n")
            for index in range(12):
                send_text(monitor, f"rmdir gui_scroll_{index}")
                send_key(monitor, "ret")
                offset = wait_for_prompt_or_error(log, offset, args.timeout)
            send_text(monitor, "rmdir gui_modal_dir")
            send_key(monitor, "ret")
            offset = wait_for_prompt_or_error(log, offset, args.timeout)
            for path in ("tmp/gui_edit.txt", "tmp/gui_picker.txt"):
                send_text(monitor, f"rm {path}")
                send_key(monitor, "ret")
                offset = wait_for_prompt_or_error(log, offset, args.timeout)

        tee_stdout("[smoke:gui] PASS\n")
        ok = True
        return 0
    finally:
        rc = shutdown_qemu(monitor, args.pidfile)
        if ok and rc != 0:
            raise SystemExit(rc)


def main():
    parser = argparse.ArgumentParser(description="SmallOS GUI launch smoke")
    parser.add_argument("--monitor", default="/tmp/smallos-monitor.sock")
    parser.add_argument("--serial", default="/tmp/smallos-serial.log")
    parser.add_argument("--pidfile", default="/tmp/smallos.pid")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle", type=float, default=0.5)
    parser.add_argument("--screenshot-dir", default="build/smoke")
    args = parser.parse_args()

    try:
        return run_gui_smoke(args)
    except Exception as exc:
        print(f"GUI smoke FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
