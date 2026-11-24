# Keyboard reference

`C-` (or `^`) is Ctrl, `M-` is Alt (or press Esc first), `S-` is Shift.
Three keymaps exist: **nano** (default), **emacs** and **simple**. Choose
one with `keymap NAME` in the config, or with `--emacs` / `--simple`.
Inside the editor, `^G` (nano), `C-c ?` (emacs) or
`M-x describe-bindings` shows the bindings that are actually active,
including your own.

Any movement key with Shift held starts or extends a selection. Typing,
`DEL` or `C-d` while a selection is active replaces it
(delete-selection behaviour).

## Nano keymap (default)

Modelled on GNU nano. `^X` means Ctrl+X, `M-X` means Alt+X (or Esc, X).

| Key | Action | Command |
|-----|--------|---------|
| `^G` | help (list bindings) | `describe-bindings` |
| `^X` | exit (asks to save each modified buffer) | `save-buffers-kill-editor` |
| `^O`, `^S` | save | `save-buffer` |
| `^R` | open a file in a new buffer | `find-file` |
| `^W` / `^Q` | search forward / backward | `isearch-forward` / `isearch-backward` |
| `M-W` / `M-Q` | next / previous match | `search-next` / `search-previous` |
| `^\`, `M-R` | replace | `query-replace` |
| `^K` | cut line (or selection); repeat to append | `kill-region` |
| `^U` | paste | `yank` |
| `M-6`, `M-^` | copy line (or selection) | `copy-region` |
| `^6`, `M-A` | set mark (start selection) | `set-mark` |
| `M-U` / `M-E` | undo / redo | `undo` / `redo` |
| `^C` | show cursor position | `cursor-position` |
| `^_`, `^/`, `M-G` | go to line[:col] | `goto-line` |
| `^T` | run a command by name | `execute-extended-command` |
| `^A` / `^E` | line start (smart) / end | |
| `^P` `^N` `^F` `^B` | up / down / right / left | |
| `^Space` / `M-Space` | next / previous word | |
| `^Y` / `^V` | page up / down | |
| `M-\` / `M-/` | first / last line | |
| `M-]` | matching bracket | `goto-matching-bracket` |
| `^D` | delete character | |
| `M-}` / `M-{` | indent / unindent | |
| `M-#` | toggle line numbers | |
| `M-<` / `M->` | previous / next buffer | |
| `^L` | redraw / center | `recenter` |
| `^Z` | suspend | |
| `^]` | complete (language server) | `lsp-complete` |
| `M-.` / `M-,` | go to definition / back | |
| `M-T` | file tree sidebar | `file-tree` |
| `M-H` | hover info | `lsp-hover` |
| `M-N` / `M-P` | next / previous diagnostic | |
| `Esc` | cancel selection or search highlight | `keyboard-quit` |

In prompts, `^C` cancels, as in nano (`C-g` and `Esc` work too). In a
search prompt, `^W`/`M-W` jump to the next match and `^Q`/`M-Q` to the
previous one.

Differences from nano: `^R` opens the file in a new buffer instead of
inserting it; there is no justify (`^J`), spell check or bottom shortcut
bar; and answering "no" at the exit prompt discards that buffer's
changes, the same as in nano.

## Shared by all keymaps

| Key | Command |
|-----|---------|
| arrows | `forward-char`, `backward-char`, `next-line`, `previous-line` |
| `C-<left>` / `C-<right>` | `backward-word` / `forward-word` |
| `<home>` / `<end>` | smart `beginning-of-line` / `end-of-line` |
| `C-<home>` / `C-<end>` | `beginning-of-buffer` / `end-of-buffer` |
| `<prior>` / `<next>` (PgUp/PgDn) | `page-up` / `page-down` |
| `RET` | `newline` (auto-indent) |
| `TAB` / `S-TAB` | `indent` / `dedent` (region-aware) |
| `DEL`, `C-h` | `delete-backward-char` |
| `<delete>` | `delete-char` |
| `<insert>` or `M-i` | `toggle-cursor-shape` (block or vertical bar) |
| `M-x` | `execute-extended-command` |
| `M-/`, `M-TAB` | `lsp-complete` |
| `M-.` / `M-,` | `lsp-definition` / `lsp-back` |
| `M-t` | `file-tree` (show and focus the sidebar; again: hide) |

### File tree

`M-t` opens a sidebar listing the working directory and moves focus into
it. Dotfiles are hidden. While it has focus:

| Key | Action |
|-----|--------|
| up / down, `C-p` / `C-n` | move |
| `RET` | open the file, or fold/unfold the folder |
| right / left | unfold / fold, or jump to the parent folder |
| `Esc`, `C-g`, `TAB` | back to the text (the tree stays) |
| `q`, `M-t` | hide the tree |

Any other key returns focus to the text and runs as usual.

## Mouse

| Action | Effect |
|--------|--------|
| click in the text | move the cursor there |
| drag | select (typing replaces the selection, like Shift+arrows) |
| wheel | scroll three lines; the cursor stays on screen |
| click in the file tree | open the file, or fold/unfold the folder |
| click / wheel on the completion popup | accept an item / move the selection |
| right-click | menu: Cut and Copy (on a selection), Paste, Select all, Undo |

In the right-click menu, click an item or use up/down and `RET`; `Esc`
or a click elsewhere closes it. Right-clicking outside the selection
moves the cursor there first.

While cedit has the mouse, your terminal's own select, copy and
middle-click paste need Shift held. `mouse no` in the config turns
mouse handling off.

## Emacs keymap (`--emacs` or `keymap emacs`)

| Key | Command |
|-----|---------|
| `C-f` `C-b` `C-n` `C-p` | char/line movement |
| `C-a` / `C-e` | smart line start / end |
| `M-f` / `M-b` | word movement |
| `M-<` / `M->` | buffer start / end |
| `C-v` / `M-v` | page down / up |
| `C-l` | `recenter` |
| `M-g g`, `M-g M-g` | `goto-line` (accepts `LINE:COL`) |
| `M-g b` | `goto-matching-bracket` |
| `C-j` | `newline` |
| `C-o` | `open-line` |
| `C-d` | `delete-char` |
| `C-k` | `kill-line` (repeated kills append) |
| `M-d` / `M-DEL` | `kill-word` / `backward-kill-word` |
| `C-w` | `kill-region` (current line without a selection) |
| `M-w` | `copy-region` (current line without a selection) |
| `C-y` | `yank` |
| `C-SPC` | `set-mark` |
| `C-x C-x` | `exchange-point-and-mark` |
| `C-x h` | `mark-whole-buffer` |
| `C-x l` | `select-line` (repeat to extend) |
| `C-_`, `C-/`, `C-x u` | `undo` |
| `M-_`, `C-M-_` | `redo` |
| `C-g` | `keyboard-quit` |
| `C-s` / `C-r` | `isearch-forward` / `isearch-backward` |
| `C-M-s` / `C-M-r` | regex incremental search |
| `M-%` | `query-replace` |
| `C-M-%` | `query-replace-regexp` |
| `C-x C-f` | `find-file` |
| `C-x C-s` | `save-buffer` |
| `C-x s` | `save-some-buffers` |
| `C-x C-w` | `write-file` |
| `C-x C-r` | `recent-files` |
| `C-x b` | `switch-to-buffer` |
| `C-x k` | `kill-buffer` |
| `C-x C-b` | `list-buffers` |
| `C-x <right>` / `C-x <left>` | `next-buffer` / `previous-buffer` |
| `C-x C-c` | `save-buffers-kill-editor` |
| `C-z` | `suspend` (resume with `fg`) |
| `M-:` | `lua-eval` (Lua builds) |
| `C-c ?` | `describe-bindings` |
| `C-c h` | `lsp-hover` |
| `M-g n` / `M-g p` | `lsp-next-diagnostic` / `lsp-previous-diagnostic` |

## Simple keymap (`--simple` or `keymap simple`)

| Key | Command |
|-----|---------|
| `C-s` | save |
| `C-q` | quit (offers to save) |
| `C-o` | open file |
| `C-w` | close buffer |
| `C-z` / `C-y` | undo / redo |
| `C-x` / `C-c` / `C-v` | cut / copy / paste |
| `C-a` | select all |
| `C-l` | select line |
| `C-k` | kill line |
| `C-d` | delete char |
| `C-f` | find (press again for the next match) |
| `C-n` | find next |
| `C-r` | replace |
| `C-g` | go to line |
| `C-b` | switch buffer |
| `C-p` | command palette (`M-x`) |
| `C-]` | matching bracket |
| `C-SPC` | complete (language server) |
| `M-h` | hover info |
| `M-n` / `M-p` | next / previous diagnostic |
| `M-<left>` / `M-<right>` | previous / next buffer |
| `ESC` | cancel selection or search highlight |

## Auto-pairing

Typing `(`, `[` or `{` inserts the closing bracket too, when the next
character is a space, the end of the line or another closer. Double quotes pair the same
way in every buffer. Single quotes and backticks pair in languages that
use them for strings. Quotes do not pair right after a word character
(`don't`). Typing a closer right before an
identical one steps over it. Backspace inside an empty pair deletes both
characters. With a selection, an opener wraps the selection. Turn it off
with `auto-pair no`.

## Completion popup

`M-/` (simple keymap: `C-SPC`) asks the language server for completions.
A single match is inserted immediately. Otherwise a list appears:
`↑`/`↓` or `C-p`/`C-n` select, `RET`/`TAB` insert, `C-g`/`ESC` close.
Typing more characters narrows the list and `DEL` widens it. Any other
key closes the list and then runs normally. Hover info (`C-c h`) shows
in the same box until the next key.

## In the minibuffer

| Key | Action |
|-----|--------|
| `RET` | accept |
| `C-g`, `ESC` | cancel |
| `TAB` | complete (files, commands, buffers, recent files, languages) |
| `C-a` `C-e` `C-b` `C-f`, arrows | move |
| `DEL` `C-d` `C-k` `C-u` | delete back / forward / to end / to start |
| `C-y` | insert the first line of the kill buffer |

During incremental search, `C-s` / `C-r` jump to the next / previous
match. With an empty search, `C-s` reuses the last search. `RET` keeps
the cursor at the match; `C-g` returns to where you started.

In query-replace: `y` replace, `n` skip, `!` replace all remaining,
`.` replace this one and stop, `q` or `C-g` stop.
