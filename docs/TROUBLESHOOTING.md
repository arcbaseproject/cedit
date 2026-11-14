# Troubleshooting

**Alt/Meta keys do nothing.** Your terminal must send Esc+key for Alt.
xterm needs `XTerm*metaSendsEscape: true`. On macOS Terminal and iTerm2,
enable "Use Option as Meta". Pressing Esc and then the key always works.

**Ctrl+S freezes the terminal in another program.** That is XON/XOFF
flow control (Ctrl+Q resumes it). Cedit turns flow control off while it
runs, so Ctrl+S reaches the editor.

**Backspace inserts `^H` or deletes forward.** Both `DEL` (0x7f) and
`C-h` (0x08) are bound to `delete-backward-char`. If your terminal sends
something else, check its "Backspace sends" setting.

**Colors look wrong.** Set `colors 256` or `colors 16` in the config, or
export `COLORTERM=truecolor` if your terminal supports 24-bit color. The
`mono` theme uses only bold, italic and reverse.

**Wide characters or emoji misalign.** Display widths come from the C
library's `wcwidth()` for the active locale. Cedit switches to a UTF-8
`LC_CTYPE` if one is available (`C.UTF-8` or `en_US.UTF-8`). Emoji
sequences joined with ZWJ, and characters newer than your libc's Unicode
tables, may be measured differently from how your terminal draws them.

**"File changed on disk".** Another program modified the file. Use
`M-x revert-buffer` to load the disk version. Saving asks before
overwriting it.

**"[rewritten in place]" after saving.** The file has several hard
links, belongs to another user, or sits in a directory you cannot write
to. Cedit then overwrites the file directly instead of doing an atomic
rename, so links and ownership survive. Ordinary saves are atomic.

**I found `notes.txt.save`.** Cedit was killed (SIGTERM or SIGHUP, for
example a closed terminal) or ran out of memory with unsaved changes.
The `.save` file holds the buffer contents (LF line endings). Compare it
with the original and delete it.

**A leftover `.notes.txt.cedit-XXXXXX` file.** The editor died in the
middle of a save (for example from SIGKILL or a power cut). The original
file is untouched. The temporary file is safe to delete.

**The editor seems stuck.** `C-g` cancels prefixes, prompts and
searches. A runaway Lua command is stopped after 5 seconds. As a last
resort, `kill <pid>` from another terminal triggers the emergency save.

**Terminal left in a weird state after a crash.** Run `reset`. Cedit
restores the terminal on normal exit, on termination signals and on
fatal signals, but cannot after `SIGKILL`.

**Debug builds.** Run `make asan` and reproduce the problem with
`build/asan/cedit`. Sanitizer reports go to stderr; redirect them with
`2>asan.log`.
