# Renders docs/demo.gif. Run from the repository root after `make`:
#   vhs docs/demo/demo.tape
# Needs clangd for the completion and diagnostics part.

Output docs/demo.gif

Set Shell "bash"
Set FontFamily "JetBrainsMono Nerd Font Mono"
Set FontSize 17
Set Width 1080
Set Height 640
Set Padding 18
Set Theme "OneDark"
Set TypingSpeed 55ms
Set CursorBlink false

Env COLORTERM "truecolor"
Env XDG_CONFIG_HOME "/tmp/cedit-demo/cfg"
Env XDG_STATE_HOME "/tmp/cedit-demo/state"

Hide
Type "rm -rf /tmp/cedit-demo && mkdir -p /tmp/cedit-demo/bin && cp cedit /tmp/cedit-demo/bin/ && cp docs/demo/wordcount.c /tmp/cedit-demo/ && cd /tmp/cedit-demo && export PATH=/tmp/cedit-demo/bin:$PATH PS1='$ ' && clear"
Enter
Show

Type "cedit wordcount.c"
Sleep 400ms
Enter
Sleep 2.5s

# search with ^W
Ctrl+W
Sleep 300ms
Type "strchr"
Sleep 800ms
Enter
Sleep 1s

# go to line 29 and add a line (auto-pairing fills in ) and ")
Alt+G
Sleep 300ms
Type "29"
Enter
Sleep 500ms
Ctrl+E
Enter
Type 'printf("lines: %d\n'
Sleep 300ms
Type '"'
Type ", count_"
Sleep 500ms

# ^] asks clangd for completions
Ctrl+]
Sleep 2s
Type "l"
Sleep 900ms
Enter
Sleep 600ms
Type "(text)"
Sleep 200ms
Type ")"
Type ";"
Sleep 1.5s

# a typo: clangd underlines it and the status bar counts it
Enter
Type "retrun 0;"
Sleep 3s

# ^K cuts the bad line; clangd clears the error
Ctrl+K
Sleep 2.5s

# ^O saves, ^X exits
Ctrl+O
Sleep 1.5s
Ctrl+X
Sleep 800ms
