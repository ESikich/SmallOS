# SmallOS GUI

`/bin/gui` still enters through `src/user/gui.c`, but the desktop code now lives
under this folder so the GUI can grow into separate modules.

- `app.c`: desktop composition, window chrome, Files/Shell/Editor hosting, and
  the main event loop.
- `builtin_apps.c`: System, Config, and About presentation.
- `canvas.c`: clipped pixel, rectangle, and line drawing primitives.
- `cursor.c`: software pointer shape and bounds.
- `damage.c`: bounded dirty-region collection and full-repaint fallback.
- `file_picker.c`: reusable Open/Save As state, directory navigation, focus,
  filename entry, and pointer/keyboard routing.
- `layout.c`: inset, vertical-stack, and equal-row layout geometry.
- `region.c`: rectangle intersection, clipping, union, and dirty-region merging.
- `widgets.c`: buttons, checkboxes, scrollbars, labels, text fields, and the
  reusable single-line input model.
- `window.c`: bounded window z-order and focus stack.
- `shell_window.c`: shell-window state, scrollback, prompt handling, and the
  shell backend. It prefers a PTY-backed `/bin/shell` child process, keeps
  pipe-backed child launching as a compatibility fallback, and falls back to
  the embedded command runner if child launch fails.
- `gui.h`: user-program entrypoint shared by the tiny wrapper and the desktop.

## Files Window

The Files window is a live ext2 directory browser. Directory rows open in place,
including the synthesized `..` parent row for non-root paths. File rows use a
double-click launcher:

- Extensionless files in program directories run directly as foreground programs.
- `.bmp` files run through `/bin/bmpview`.
- Text-like files (`.txt`, `.c`, `.h`, `.md`, `.ini`, `.log`, `.html`) open in
  an Editor window without releasing the desktop display.

The desktop owns the framebuffer while it is active, so launching a full-screen
program temporarily releases the display, waits for that child to exit, then
reacquires the display and redraws the desktop.

## Application Events And Controls

Window applications receive local keyboard, pointer, wheel, resize, tick, and
close-request events through their callback table. Results explicitly report
handled, redraw, close, or keep-open behavior. The desktop only records window
damage for redraw results, and close requests give applications a chance to
protect state before the window is destroyed.

Buttons, checkboxes, scrollbars, labels, and text fields share drawing and
hit-testing helpers. Scrollbar tracks support page clicks and their thumbs keep
pointer capture while dragged. Windows retain focused and pressed widget IDs so
controls can render hover, pressed, keyboard-focus, disabled, and active states
without global control state.

## Editor Window

Run `gui <text-path>` or double-click a text-like Files row to open a windowed
editor. It supports character insertion, Enter, Tab, Backspace, Delete, arrow
keys, Home/End, Page Up/Down, mouse caret placement, wheel scrolling, resize,
F2 or Ctrl+S save, horizontal scrolling, and a blinking caret. Its toolbar and
Ctrl+N/Ctrl+O/Ctrl+Shift+S shortcuts provide New, Open, and Save As. Untitled
documents route ordinary Save through the shared file picker. Drag across text
or use Shift with navigation keys to select it. Ctrl+A selects all, and
Ctrl+C/Ctrl+X/Ctrl+V use a clipboard shared by GUI editor windows. Closing a
dirty document offers Save, Discard, and Cancel; save failures keep the window
open. The same guard runs before New or Open replaces a dirty document.

The line and file model is shared with `/bin/edit`, which remains the terminal
editor and keeps its batch-command interface.

## Rendering, Damage, And Pacing

The desktop composes windows into a full-screen software surface, but presents
only the dirty rectangles recorded by input, application, and timer events.
Dirty regions are clipped to the display and nearby regions are merged when
the merged copy is reasonably compact. A logical event that records no pixel
damage is a no-op; it must not be promoted to a full-screen repaint.

The mouse pointer is an immediate software overlay rather than part of the
composed desktop surface. Pointer motion restores the old cursor rectangle and
draws the new one without waiting for the paced window frame. Hover changes
invalidate only controls whose appearance changes, such as desktop icons and
Files rows. In particular, crossing an undecorated window boundary records no
scene damage. Dirty rectangles that touch the cursor are split around its
footprint, then the cursor rectangle is presented once from the newly composed
scene, so application redraws cannot briefly expose the pixels beneath it.

## Config And Pacing

The Config window currently owns desktop-local settings. It can toggle the GUI
perf readout, which is off by default and persisted as `perf_visible` in
`/etc/gui.conf` when the mounted filesystem is writable.

The GUI keeps cursor presentation immediate, while active window-drag visuals
are paced to 60 FPS. The system timer is 300 Hz, so that cadence lands on an
exact five-tick frame interval. Shell PTY output is polled on the same 60 FPS
cadence so quiet shell windows do not keep the desktop awake or compete with
pointer motion.

Window application types register lifecycle, drawing, input, resize, timer,
and guarded-close callbacks plus default and minimum dimensions. Files, Shell,
and Editor allocate their larger private state only while open. Windows can be
resized from their bottom-right grip; PTY dimensions follow the visible Shell
grid.

Desktop shortcuts work when a text-entry window is not focused: `F` opens
Files, `T` opens Shell, `S` opens System, `C` opens Config, `A` opens About,
and `X` closes the focused window. `Q` or Escape exits the desktop. Escape
closes or requests closure of a focused Shell or Editor window first.

## Shell Window

The GUI treats each shell window as a window-owned terminal session:

1. Create a shell window with its own scrollback, input line, cursor state, and
   child process metadata.
2. Allocate a PTY pair and size it to the visible terminal grid.
3. Feed key events from the focused window into the backend.
4. Drain backend output into the window scrollback during the GUI event loop.
5. Close the window by closing the backend and reaping the child.

The normal path forks a child, duplicates the PTY slave onto fd `0`, `1`, and
`2`, then execs `/bin/shell`. The parent keeps the PTY master nonblocking
so the desktop can poll shell output without freezing pointer or window input.
The user shell owns command dispatch, history, completion, pipelines, and
external program launch. The GUI fallback shell keeps matching history and
completion semantics for the older non-PTY backends.

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
