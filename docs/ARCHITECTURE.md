# Architecture

```
include/cedit/   public headers, one per module
src/
  main.c         argument parsing, startup, locale
  editor.c       Editor state, main loop, key dispatch, minibuffer,
                 buffer list, emergency save, recent files
  commands.c     command registry, default keymaps, movement/editing
  filecmds.c     file, buffer and misc commands, completion
  search.c       search engine + isearch / query-replace commands
  keymap.c       keymap tree (prefix maps), key sequence parsing
  buffer.c       document model: text + undo + file + view state
  text.c         gap buffer with incremental line index
  undo.c         grouped linear undo history
  file.c         POSIX file I/O, atomic save, file stamps
  syntax.c       language table + highlighters
  render.c       frame composition, row diffing, themes, colors
  terminal.c     raw mode, alternate screen, signals, poll
  input.c        pure terminal input parser, key names
  utf8.c         UTF-8 codec, display glyphs, sanitizing
  config.c       config file parser, XDG paths
  lsp.c          language server client (process, JSON-RPC, features)
  json.c         bounded JSON parser / string quoting
  script.c       optional Lua bridge (stubs without Lua)
  util.c         checked allocation, growable strings
tests/           unit tests, pty end-to-end tests, fuzzers, benchmarks
```

Dependencies only point downward: `text`, `undo`, `utf8` and `input`
know nothing about the editor, which is what lets the fuzzers link them
alone. `buffer` combines text, undo and file. Commands operate on the
`Editor`. The renderer only reads state.

## Key decisions

### Gap buffer + lazy line index

The gap buffer gives O(1) inserts and deletes at the cursor, and its
memory use is the file size plus a small gap. A piece table makes undo
cheaper, but costs more for every read during rendering and searching.

Line starts live in a `size_t` array, so going from a line to an offset
is O(1) and from an offset to a line is O(log n). After an edit, every
later line start moves by the edit length. That shift is kept lazily: the
entries from `dfrom` onward are stale by `delta`, and an edit only
rewrites the entries between its line and the previous edit's line.
Typing on one line therefore costs O(1) with respect to the file size.
The benchmark went from 83 µs to 0.14 µs per character on a 1M-line file.
A newline still memmoves the tail of the index (8 bytes per line).

`text_view()` returns a contiguous pointer to a range, moving the gap out
of the way if needed. Rendering and searching use it instead of copying.

### Undo

Each op is stored with its bytes. Ops carry a group id, and the editor
starts a new group per command, so one undo reverts one command.
Consecutive typing or deleting shares a group, up to 20 keystrokes. Ops
in the same group merge when contiguous, capped at 1 KiB so prepending
stays cheap. Every content state gets a sequence number, and "modified"
means `top seq != saved seq`. That stays correct across undo, redo and
merges.

### Rendering

Every frame builds each screen row as a string and compares it with the
row sent last time. Only changed rows are written, inside a synchronized
update (`CSI ?2026`). The whole frame goes out in one `write`. A
keystroke typically redraws one or two rows. Frame time is about 100 µs
on an 80x50 screen.

All text goes through `glyph_next()`, which turns bytes into displayable
cells. Control characters become `^X`. C1 controls and invalid bytes
become `<9b>` / `<ff>`. Bidi override characters become `<U+202E>`.
Tabs expand to spaces. **No byte from a file, filename or message can
reach the terminal as a control sequence.** Cursor columns are computed
by the same function, so the display and cursor movement cannot drift
apart.

### Syntax highlighting

`Syntax` is a data-driven language description: keyword and type lists,
comment delimiters, string rules and flags. A generic tokenizer handles
most languages. Markdown and Makefiles plug in their own `highlight`
function. Multiline constructs (block comments, triple-quoted and raw
strings, code fences) carry state between lines. The buffer caches the
state at each line start and invalidates it from the edited line on. The
renderer highlights only the visible lines.

The trade-off: jumping far past the cached region scans the lines in
between once (about 730 ms for the last line of a 1M-line file). Editing
near the top of a huge file invalidates the cache below it.

### Input

`input_parse()` is a pure function from bytes to one key. It returns 0
for a sequence that is still incomplete, and the editor waits 25 ms
before treating a lone ESC as a key. It handles CSI and SS3 sequences
with modifiers, kitty's `CSI u`, UTF-8 and bracketed paste, and it bounds
the length of CSI sequences. It is fuzzed. Pasted text is inserted
literally, without bindings or auto-indent, and redraw is suppressed
until the input queue drains.

### Commands and keymaps

Commands are named functions in a registry (`Command`). Keymaps are trees
mapping a key to a command name or to a nested prefix map. Bindings hold
command names, so a binding may come before its command exists (useful
for Lua). Flags on each command drive behavior that would otherwise need
special cases: shift-selection on motions, kill appending, undo
coalescing, and goal-column keeping.

### The minibuffer

Prompts run a modal loop (`editor_prompt`) with an optional hook, used by
incremental search, and an optional completer. This keeps commands
written as straight-line code ("ask for a file name, then open it")
instead of callback chains.

### Language servers

`lsp.c` starts one server per language, using `fork` + `execvp` with no
shell. The child gets `setsid`, stderr goes to `/dev/null`, and the pipes
are non-blocking. Server pipes join the main `poll()`, next to the
terminal and the signal pipe, so the editor never blocks on a server.
Outgoing messages are buffered and flushed when the pipe is writable.
Incoming messages are framed by `Content-Length`; anything over 64 MB, or
malformed, stops that server. The JSON parser bounds nesting depth and is
fuzzed.

Documents are synced in full, 300 ms after the last keystroke (the poll
timeout drives it). Full sync is simpler and robust across undo, revert
and replace-all. Its cost grows with file size, which only matters for
very large files.

Requests that need an answer (completion, definition, hover) wait
synchronously for up to 5 s. While waiting, other server traffic is still
processed and `C-g` cancels (`$/cancelRequest`). The client offers UTF-8
positions and converts from UTF-16 when the server insists. Server-to-
client requests (`workspace/configuration` etc.) get null answers so
servers never stall.

Diagnostics are stored per buffer as byte ranges. The renderer underlines
them, colors the line number, counts them in the status bar and shows
the cursor line's message. They are not shifted by local edits; the
server republishes them after the next sync.

### Files and data safety

- **Load:** `open(O_NONBLOCK)` + `fstat`. Only regular files are
  accepted, so FIFOs and devices cannot hang the editor. CRLF mode is
  used only when *every* newline is CRLF, which round-trips exactly.
  Mixed files are kept byte for byte, with `\r` shown as `^M`.
- **Save:** write to `.NAME.cedit-XXXXXX` in the same directory, `fsync`,
  copy the mode (and owner when running as root), `rename`, then `fsync`
  the directory. Any failure leaves the original untouched. Symlinks are
  resolved, so the link survives. Files with several hard links, files
  owned by someone else, and files in unwritable directories are
  rewritten in place instead, to keep links and ownership. That path is
  reported, and a failure there may damage the file. The buffer stays
  intact either way.
- **External changes:** a (dev, inode, size, mtime) stamp is checked
  every 2 s while editing and before each save.
- **Termination:** SIGTERM, SIGHUP, SIGINT, SIGQUIT and terminal hang-up
  restore the terminal, write `FILE.save` (opened with `O_EXCL`, mode
  0600) for each modified buffer, and exit. This happens in the main
  loop, not inside the signal handler. Fatal signals (SIGSEGV etc.) only
  restore the terminal, because the heap may be corrupt.
- **Out of memory:** edits that fail to allocate are refused with a
  message, and the buffer stays consistent. Small UI allocations that
  fail trigger the emergency save and then `abort()`. Emergency saving
  allocates nothing.

### Global state

There are two deliberate globals: the terminal/signal state in
`terminal.c` (signal handlers must reach it), and a pointer used only by
the out-of-memory handler. Everything else lives in `Editor`.

## Testing

- `tests/test_main.c`: unit tests covering the gap buffer against a
  reference model (20,000 random ops), UTF-8, input parsing, undo, file
  round trips (CRLF, permissions, symlinks, hard links, failed saves),
  search, highlighting, keymaps, config, and editor-level command
  behavior. One test checks that escape sequences in a document never
  reach the terminal.
- `tests/pty_test.py`: drives the real binary in a pseudo-terminal
  (saving, quitting, prompts, SIGTERM emergency save, resize, search,
  simple keymap, completion, paste, read-only failures). It reports any
  sanitizer output.
- `tests/fuzz_input.c`, `tests/fuzz_text.c`: libFuzzer targets.
- `make test-asan`: the unit tests under ASan, UBSan and LeakSanitizer.
