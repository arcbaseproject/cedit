<p align="center">
  <img src="logo-text.png" alt="Cedit" width="520">
</p>

<p align="center">
  A small terminal text editor written in C.<br>
  Nano keys by default, Emacs keys when you want them.
</p>

<p align="center">
  <a href="#install">Install</a> ·
  <a href="#keys">Keys</a> ·
  <a href="#configuration">Configuration</a> ·
  <a href="#documentation">Docs</a> ·
  <a href="#performance">Performance</a>
</p>

<p align="center">
  <img src="docs/demo.gif" alt="Cedit editing a C file: search, auto-pairing, clangd completion and an error underline" width="760">
</p>

---

Cedit is an independent project with no connection to the GNU Project.
The name is a working title.

## What you get

- **Speed on big files.** You can type in the middle of a 55 MB,
  million-line file at 0.14 µs per keystroke. Cedit stores text in a gap
  buffer and redraws the rows that changed.
- **Saves you can trust.** Cedit writes a temporary file, syncs it to
  disk and renames it over the original. If a save fails, your file stays
  as it was. Cedit keeps permissions and symlinks, warns you when another
  program changes the file, and writes `FILE.save` if you close the
  terminal with unsaved work.
- **A safe screen.** Control characters in a file or filename show up as
  `^[` or `<9b>`. Cedit sends none of them to your terminal as escape
  codes.
- **Unicode.** Cursor movement and column counts follow what your
  terminal draws for CJK text, emoji and combining accents.
- **Language servers.** Install clangd and open a C file: you get
  underlined errors, a completion popup, go-to-definition and hover.
  Cedit also starts rust-analyzer, gopls, pylsp, bash-language-server,
  typescript-language-server, taplo and marksman when it finds them.
- **Syntax highlighting** for C, C++, Rust, Go, Python, Shell,
  JavaScript, JSON, TOML, Markdown and Makefiles.
- **Editing tools.** Undo and redo, selections, incremental and regex
  search, query-replace, auto-indent, bracket and quote pairing, several
  open buffers and a recent-files list.
- **Three keymaps.** Nano (default), Emacs or simple Ctrl+S/Ctrl+Q. You
  can rebind any key in a config file.
- **Lua plugins** if you build with `make LUA=1`.
- **One 230 KB binary** that needs libc and nothing else. It uses no CPU
  while you sit idle.

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/arcbaseproject/cedit/main/install.sh | sh
```

The script downloads the source, builds it, runs the unit tests and
installs `cedit` to `~/.local/bin` (or `/usr/local/bin` when you run it
as root). It also sets up a daily systemd timer, or a cron job if you
lack systemd, that compares your version with
[`VERSION`](VERSION) on GitHub and rebuilds when they differ. A new
version replaces yours after it builds and passes its tests.

| Option | Effect |
|--------|--------|
| `--prefix DIR` | install somewhere else |
| `--lua` | build with Lua scripting |
| `--no-auto-update` | skip the timer |
| `--uninstall` | remove cedit, `cedit-update` and the timer |

Pass options after `sh -s --`, for example
`curl -fsSL .../install.sh | sh -s -- --no-auto-update`. Run
`cedit-update --check-update` to update by hand; the log lives in
`~/.local/state/cedit/update.log`.

Auto-update runs whatever lands on the `main` branch of this repository.
Use `--no-auto-update` if you want to review changes before you upgrade.

### From source

You need a C17 compiler (GCC or Clang), GNU make and Linux or another
POSIX system. Development happens on Arch Linux.

```sh
make                 # builds ./cedit
./cedit notes.txt    # try it without installing
sudo make install    # /usr/local/bin/cedit plus the man page
sudo make uninstall
```

Packagers can run `make PREFIX=/usr DESTDIR="$pkgdir" install`.

| Target | Purpose |
|--------|---------|
| `make debug` | `-O0 -g` build in `build/debug` |
| `make asan` | AddressSanitizer + UBSan build |
| `make test` / `make test-asan` | unit tests, plain or sanitized |
| `python3 tests/pty_test.py` | end-to-end tests in a pseudo-terminal |
| `make fuzz` | libFuzzer targets (needs clang) |
| `make bench` | benchmarks |
| `make LUA=1` | build with Lua scripting |

## Usage

```
cedit [-e|--emacs] [-s|--simple] [-n|--no-config] [--no-plugins] [+LINE[:COL]] [FILE]...
```

Name a file that doesn't exist and Cedit creates it when you save. Press
`^G` inside the editor to see every key and command.

## Keys

`^X` means Ctrl+X. `M-X` means Alt+X, or Esc followed by X.

| Keys | Action |
|------|--------|
| `^O` or `^S` | save |
| `^X` | exit, asking about unsaved files |
| `^R` | open a file (TAB completes) |
| `^W` / `^Q` | search forward / backward |
| `M-W` / `M-Q` | next / previous match |
| `^\` | replace |
| `^K` / `^U` | cut the line or selection / paste |
| `M-6` | copy the line or selection |
| `^6` | start a selection (Shift+arrows work too) |
| `M-U` / `M-E` | undo / redo |
| `^_` | go to line |
| `^C` | show the cursor position |
| `M-\` / `M-/` | first / last line |
| `M-<` / `M->` | previous / next buffer |
| `^]` | complete with the language server |
| `M-.` / `M-,` | go to definition / come back |
| `M-H` | hover info |
| `^T` | run a command by name |
| `^G` | help |

Start with `cedit --emacs` for `C-x C-s`, `C-x C-c`, `C-s` and `M-x`, or
with `cedit --simple` for Ctrl+S, Ctrl+Q, Ctrl+Z and Ctrl+C/V. The
[key reference](docs/KEYBINDINGS.md) lists all three keymaps.

## Configuration

Cedit reads `~/.config/cedit/config`:

```
keymap nano
tab-width 4
expand-tabs yes
line-numbers relative
theme dark
face comment #6a9955 - italic
bind M-d kill-word
lsp Rust off
```

[docs/CONFIGURATION.md](docs/CONFIGURATION.md) covers every option, and
[examples/config](examples/config) gives you a starting file.

## Documentation

| Document | Covers |
|----------|--------|
| [KEYBINDINGS.md](docs/KEYBINDINGS.md) | nano, Emacs and simple keymaps |
| [CONFIGURATION.md](docs/CONFIGURATION.md) | options, themes, language servers |
| [PLUGINS.md](docs/PLUGINS.md) | the Lua API and its security model |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | design decisions and trade-offs |
| [CONTRIBUTING.md](docs/CONTRIBUTING.md) | build, test and code rules |
| [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | Alt keys, colors, `.save` files |

`man cedit` works after you install.

## Performance

`make bench` on an AMD Ryzen 5 5600G, with a generated C file of
1,000,001 lines (54.8 MB):

| Operation | Time |
|-----------|------|
| load the file | 50 ms |
| memory after load | 65 MB |
| type one character mid-file | 0.15 µs |
| backspace mid-file | 0.37 µs |
| redraw after a keystroke | 103 µs |
| search the whole file | 130 ms |
| save with fsync | 26 ms |
| first redraw at the end of the file | 730 ms |

The last row includes a one-time highlighting scan of the lines above;
Cedit caches the result. Through a pseudo-terminal, Cedit draws its first
frame in 1.6 ms with no file and 54 ms with the 55 MB file.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
