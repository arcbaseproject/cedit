# Lua plugin API

Scripting is optional. Build it with `make LUA=1`. `cedit --version`
reports whether it is enabled.

## Security model: read this first

**Lua scripts are not sandboxed.** They run inside the editor with your
full user privileges, including the `io` and `os` libraries. Treat a
plugin exactly like any program you install.

These boundaries are enforced:

- Scripts are loaded only from your config directory:
  `~/.config/cedit/init.lua`, then `~/.config/cedit/plugins/*.lua` in
  alphabetical order.
- Nothing is ever loaded from the current directory, the edited file, or
  the project, and file contents are never executed.
- A script is refused unless it is a regular file owned by you and not
  writable by group or others.
- `--no-plugins` or `plugins no` skips the plugins directory, and `-n`
  skips scripting entirely.
- A watchdog aborts any single script call that runs longer than 5
  seconds, so a runaway loop cannot freeze the editor for good.
- Errors are caught and shown in the message line. They never crash the
  editor.

## API: the `cedit` table

| Function | Description |
|----------|-------------|
| `cedit.command(name, fn)` | Register a command, callable through `M-x name` and bindable. |
| `cedit.bind(keys, name)` | Bind a key sequence, for example `"C-c t"`. |
| `cedit.on(event, fn)` | Hook; `fn(buffer_name)`. Events: `open`, `save`, `modify`, `buffer-switch`. |
| `cedit.run(name)` | Run a command by name; returns `false` if it is unknown. |
| `cedit.set(line)` | Apply a configuration line, for example `"tab-width 2"`; raises an error if invalid. |
| `cedit.message(text)` | Show a message. |
| `cedit.insert(text)` | Insert at the cursor (undoable). |
| `cedit.get_line([n])` | Text of line `n` (1-based; default: current line), or `nil`. |
| `cedit.line_count()` | Number of lines. |
| `cedit.cursor()` | `line, col`, both 1-based; `col` is a byte column. |
| `cedit.set_cursor(line [, col])` | Move the cursor (clamped). |
| `cedit.selection()` | Selected text, or `nil`. |
| `cedit.filename()` | Path of the current buffer, or `nil`. |
| `cedit.buffer_name()` | Display name of the current buffer. |

`modify` fires after each command that changed the buffer.

`M-:` (`lua-eval`) evaluates a chunk interactively; the value of an
expression is shown in the message line.

## Example

See [examples/init.lua](../examples/init.lua):

```lua
cedit.command("insert-date", function()
  cedit.insert(os.date("%Y-%m-%d"))
end)
cedit.bind("C-c d", "insert-date")

cedit.on("save", function(name)
  cedit.message("saved " .. name)
end)
```

## Hooks in C

The core calls `script_hook(editor, event, buffer)` at the same points.
A build without Lua compiles these calls to no-ops (`src/script.c`), so
the core never depends on the scripting module.
