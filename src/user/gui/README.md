# SmallOS GUI

`/bin/gui.elf` still enters through `src/user/gui.c`, but the desktop code now lives
under this folder so the GUI can grow into separate modules.

- `app.c`: desktop, windows, icons, drawing, and event dispatch.
- `shell_window.c`: shell-window state, scrollback, prompt handling, and the
  shell backend. It prefers a PTY-backed `/bin/shell.elf` child process, keeps
  pipe-backed child launching as a compatibility fallback, and falls back to
  the embedded command runner if child launch fails.
- `gui.h`: user-program entrypoint shared by the tiny wrapper and the desktop.

## Files Window

The Files window is a live ext2 directory browser. Directory rows open in place,
including the synthesized `..` parent row for non-root paths. File rows use a
double-click launcher:

- `.elf` files run directly as foreground programs.
- `.bmp` files run through `/bin/bmpview.elf`.
- Text-like files (`.txt`, `.c`, `.h`, `.md`, `.ini`, `.log`, `.html`) run
  through `/bin/edit.elf`.

The desktop owns the framebuffer while it is active, so launching a full-screen
program temporarily releases the display, waits for that child to exit, then
reacquires the display and redraws the desktop.

## Shell Window

The GUI treats each shell window as a window-owned terminal session:

1. Create a shell window with its own scrollback, input line, cursor state, and
   child process metadata.
2. Allocate a PTY pair and size it to the visible terminal grid.
3. Feed key events from the focused window into the backend.
4. Drain backend output into the window scrollback during the GUI event loop.
5. Close the window by closing the backend and reaping the child.

The normal path forks a child, duplicates the PTY slave onto fd `0`, `1`, and
`2`, then execs `/bin/shell.elf`. The parent keeps the PTY master nonblocking
so the desktop can poll shell output without freezing pointer or window input.
The user shell owns command dispatch, history, completion, pipelines, and
external program launch; the kernel shell remains a fallback/debug monitor.

The terminal renderer stores fixed-width rows, not C strings, so spaces and
short redraws from line editing remain visible. It handles the CSI/ESC controls
used by the shell editor and simple full-screen tools: carriage return,
newline, cursor up/down/left/right, absolute row/column moves, erase in line,
erase in display, save/restore cursor, reset, and no-op acceptance for SGR and
private mode toggles. UTF-8 box-drawing output from tools such as `tree` is
mapped into ASCII cells for the bitmap text renderer.

The `backend` field in `gui_shell_window_t` records the active connection:
`GUI_SHELL_BACKEND_PTY_CHILD` is the default, `GUI_SHELL_BACKEND_PIPE_CHILD`
keeps older child-shell plumbing available, and `GUI_SHELL_BACKEND_EMBEDDED`
is the recovery path when no child shell can be launched.
