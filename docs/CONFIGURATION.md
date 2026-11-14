# Configuration

Cedit reads `$XDG_CONFIG_HOME/cedit/config` (normally
`~/.config/cedit/config`) at startup. `cedit -n` skips it. Each line is
one directive. Lines whose first non-blank character is `#` are comments.
If a line has an error, the editor reports the first one in the message
line and keeps going.

`M-x set-option` applies any of these directives while the editor is
running, for example `tab-width 2`.

## Options

| Directive | Values | Default |
|-----------|--------|---------|
| `keymap` | `nano`, `emacs`, `simple` | `nano` |
| `tab-width` | 1-16 | 4 |
| `expand-tabs` | `yes`/`no` | `yes` |
| `auto-indent` | `yes`/`no` | `yes` |
| `auto-pair` | `yes`/`no` (closing brackets/quotes) | `yes` |
| `line-numbers` | `off`, `absolute`, `relative` | `absolute` |
| `theme` | `dark`, `light`, `mono` | `dark` |
| `colors` | `auto`, `16`, `256`, `truecolor` | `auto` |
| `plugins` | `yes`/`no` (Lua plugin directory) | `yes` |

`keymap` resets all bindings to that keymap's defaults, so put it before
any `bind` lines.

`expand-tabs` sets the default for new buffers. When a file is opened,
its existing indentation wins: mostly-tab files keep tabs, and Makefiles
always use tabs.

`colors auto` picks a color mode from the environment: `COLORTERM=truecolor`
or `24bit` gives 24-bit color, a `TERM` containing `256color` gives 256
colors, and anything else gives 16 colors. In 16-color mode, selections
and the status bar use reverse video, so they stay readable on any
palette.

## Language servers (LSP)

When you open a file, cedit starts the language server for its language
if one is installed in `PATH`. There is one server per language, rooted
at the nearest directory containing `.git`, `compile_commands.json`,
`Cargo.toml`, `go.mod`, `pyproject.toml`, `package.json` and similar
markers.

| Language | Default server (first one found) |
|----------|----------------------------------|
| C, C++ | `clangd` |
| Rust | `rust-analyzer` |
| Go | `gopls` |
| Python | `pylsp`, `pyright-langserver --stdio`, `basedpyright-langserver --stdio` |
| Shell | `bash-language-server start` |
| JavaScript | `typescript-language-server --stdio` |
| JSON | `vscode-json-language-server --stdio` |
| TOML | `taplo lsp stdio` |
| Markdown | `marksman server` |

```
lsp off                          # disable all language servers
lsp on
lsp Rust off                     # disable one language
lsp Python pyright-langserver --stdio   # choose the command
```

The command is run directly with `execvp`, without a shell. File
contents never choose what runs. Server stderr is discarded.

**Security note:** some servers execute code from the project you open.
rust-analyzer, for example, runs `build.rs` scripts and procedural
macros, and others may run project tooling. Use `lsp off` (or
`lsp LANG off`) before opening untrusted code.

Features: diagnostics (underlined, counted in the status bar, the
cursor-line message shown at the bottom), completion (`M-/`), go to
definition (`M-.`, back with `M-,`) and hover (`C-c h`).
`M-x lsp-status` lists the servers. `M-x lsp-restart` restarts a crashed
server; a server that exits is not restarted automatically. Documents
are sent in full 300 ms after you stop typing.

## Key bindings

```
bind KEY... COMMAND
unbind KEY...
```

Keys use Emacs notation: `C-x`, `M-x`, `C-M-s`, `S-<up>`, `C-<left>`,
`<home>`, `<end>`, `<prior>`, `<next>`, `<delete>`, `<backtab>`, `RET`,
`TAB`, `SPC`, `ESC`, `DEL`. Separate several keys with spaces to make a
prefix sequence:

```
bind C-c d kill-line
bind C-c C-c save-buffer
bind M-n search-next
bind M-p search-previous
unbind C-z
```

`M-x describe-bindings` lists every command with a short description.
Binding a key to a command name that does not exist yet is allowed; Lua
plugins can define the command later.

## Faces (colors)

```
face SLOT FG [BG] [bold] [italic] [underline] [reverse]
```

`FG` and `BG` are `#rrggbb`, or `-` for the terminal default.

Slots: `normal`, `comment`, `keyword`, `type`, `string`, `number`,
`operator`, `preproc`, `linenr`, `curlinenr`, `status`, `selection`,
`match`, `curmatch`, `special` (control characters and invalid bytes),
`error`, `filler`, `diag-error`, `diag-warning`, `popup`,
`popup-selected`.

```
theme dark
face keyword #ff79c6 - bold
face status #000000 #50fa7b
face selection - #44475a
```

## Files cedit writes

| File | Purpose |
|------|---------|
| `$XDG_STATE_HOME/cedit/recent` | recent-file history (100 entries, directory mode 0700) |
| `FILE.save`, `FILE.save.N` | emergency copies written on SIGTERM/SIGHUP or out-of-memory (mode 0600) |
| `.FILE.cedit-XXXXXX` | temporary file during an atomic save; renamed over FILE or removed |

## Example

See [examples/config](../examples/config).
