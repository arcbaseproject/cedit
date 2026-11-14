# Contributing

## Workflow

```sh
make debug                     # build/debug/cedit
make test && make test-asan    # unit tests, plain and sanitized
make asan && python3 tests/pty_test.py build/asan/cedit
make fuzz && build/fuzz/fuzz_text -max_total_time=60
make LUA=1 BUILD=debug         # if you touch src/script.c
```

A change is ready when all of the above pass with no compiler warnings
(the build uses `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2
-Wmissing-prototypes ...`).

## Rules

- C17 plus POSIX.1-2008 (`_XOPEN_SOURCE=700`). Avoid GNU extensions
  except `__builtin_*_overflow` and `__attribute__((format))`.
- No `strcpy`, `strcat`, `sprintf` or `gets`. Use `snprintf`, `Str`, or
  `str_copy`.
- Check every size computation that could overflow with
  `__builtin_add_overflow` / `__builtin_mul_overflow`.
- Document text, undo and file paths must handle allocation failure and
  leave state unchanged. UI code may use the `x*` allocators.
- Anything shown on the terminal goes through `glyph_next()` or
  `sanitize_for_display()`. Never print file contents, filenames or user
  input directly.
- Keep modules layered (see ARCHITECTURE.md). `text`, `undo`, `utf8` and
  `input` must not include editor headers.
- New behavior needs a test: a unit test in `tests/test_main.c`, or an
  end-to-end case in `tests/pty_test.py` for interactive flows.
- Commands: write a `static void cmd_x(Editor *e)`, register it in its
  file's `commands_register_*` table with a one-line doc string, and add
  default bindings in `commands.c` only if they are widely expected.
- Update `docs/` and the man page when user-visible behavior changes.

## Adding a language

Add an entry to `languages[]` in `src/syntax.c`. Most languages need only
data: `match` patterns (`.ext` or an exact basename), optional shebang
interpreters, keyword and type lists, comment delimiters, and flags such
as `F_DQ`, `F_TRIPLE` or `F_BACKTICK`. Supply a `fn` only if the generic
tokenizer cannot express the syntax. Add a case to `test_syntax`.
