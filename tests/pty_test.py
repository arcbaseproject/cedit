#!/usr/bin/env python3
"""End-to-end tests: drive the real binary through a pseudo-terminal.

Usage: tests/pty_test.py [path/to/cedit]
HOME and XDG dirs point at a temporary directory, so the user's
configuration and history are never touched.
"""
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./cedit")
TMP = tempfile.mkdtemp(prefix="cedit-pty-")
ENV = dict(os.environ, HOME=TMP, XDG_CONFIG_HOME=TMP + "/cfg",
           XDG_STATE_HOME=TMP + "/state", TERM="xterm-256color",
           LANG="C.UTF-8", LC_ALL="C.UTF-8")
failures = 0


class Session:
    def __init__(self, *args, rows=24, cols=80):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(TMP)
            if "--simple" not in args and "--nano" not in args:
                args = ("--emacs", *args)  # most cases drive Emacs keys
            args = tuple(a for a in args if a != "--nano")
            os.execve(BIN, [BIN, *args], ENV)
        self.resize(rows, cols)
        self.out = b""
        self.read(0.3)

    def resize(self, rows, cols):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))

    def read(self, t=0.15):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    self.out += os.read(self.fd, 65536)
                except OSError:
                    break
        return self.out

    def send(self, data, t=0.15):
        if isinstance(data, str):
            data = data.encode()
        os.write(self.fd, data)
        self.read(t)

    def wait(self, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.read(0.05)
                if b"Sanitizer" in self.out or b"runtime error" in self.out:
                    check("sanitizer clean", False, self.out[-400:])
                return os.waitstatus_to_exitcode(status)
            self.read(0.05)
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)
        return None


def check(name, cond, detail=""):
    global failures
    print(f"{name:40} {'ok' if cond else 'FAIL'} {detail if not cond else ''}")
    if not cond:
        failures += 1


def path(name):
    return os.path.join(TMP, name)


def read(name):
    with open(path(name), "rb") as f:
        return f.read()


C = lambda ch: bytes([ord(ch) & 0x1f])  # noqa: E731

# 1. new file, type, save, quit
s = Session("new.txt")
s.send("hello world\n")
s.send(C("x") + C("s"))
s.send(C("x") + C("c"))
code = s.wait()
check("save new file and quit", code == 0 and read("new.txt") == b"hello world\n",
      f"code={code} data={read('new.txt') if os.path.exists(path('new.txt')) else None}")
check("terminal restored (alt screen left)", b"\x1b[?1049l" in s.out)

# 2. CRLF preserved
with open(path("dos.txt"), "wb") as f:
    f.write(b"one\r\ntwo\r\n")
s = Session("dos.txt")
s.send("\x1b>")             # M-> end of buffer
s.send("three\n")
s.send(C("x") + C("s"))
s.send(C("x") + C("c"))
s.wait()
check("CRLF line endings preserved", read("dos.txt") == b"one\r\ntwo\r\nthree\r\n",
      repr(read("dos.txt")))

# 3. quitting with unsaved changes asks; original untouched
with open(path("keep.txt"), "wb") as f:
    f.write(b"keep me\n")
s = Session("keep.txt")
s.send("junk")
s.send(C("x") + C("c"))
check("quit prompts to save", b"Save" in s.out)
s.send("n")                 # "no" means discard and exit
code = s.wait()
check("discard + exit leaves file alone", code == 0 and read("keep.txt") == b"keep me\n")

# 4. SIGTERM writes an emergency copy
s = Session("term.txt")
s.send("unsaved work")
os.kill(s.pid, signal.SIGTERM)
code = s.wait()
check("SIGTERM exits", code is not None and code != 0, f"code={code}")
check("SIGTERM emergency copy",
      os.path.exists(path("term.txt.save")) and read("term.txt.save") == b"unsaved work")

# 5. resize does not crash; incremental search; goto-line
with open(path("code.c"), "w") as f:
    f.write("int main(void) {\n" + "".join(f"    int v{i} = {i};\n" for i in range(200)) + "}\n")
s = Session("code.c")
s.resize(10, 40)
os.kill(s.pid, signal.SIGWINCH)
s.read(0.2)
s.resize(30, 100)
os.kill(s.pid, signal.SIGWINCH)
s.read(0.2)
s.send(C("s") + b"v150", 0.3)
s.send("\r")
s.send("X")
s.send("\x1bgg")            # M-g g
s.send("1\r")
s.send("Y")
s.send(C("x") + C("s"))
s.send(C("x") + C("c"))
code = s.wait()
data = read("code.c").decode()
check("resize survives", code == 0, f"code={code}")
check("isearch lands after match", "v150X" in data)
check("goto-line works", data.startswith("Yint main"))

# 6. simple keymap
s = Session("--simple", "simple.txt")
s.send("abc")
s.send(C("s"))
s.send(C("q"))
code = s.wait()
check("simple keymap Ctrl+S / Ctrl+Q", code == 0 and read("simple.txt") == b"abc")

# 7. M-x command with completion, undo
s = Session("mx.txt")
s.send("abc")
s.send("\x1bx" + "und", 0.2)
s.send("\t")                # completes to "undo"
s.send("\r")
s.send(C("x") + C("s"))
s.send(C("x") + C("c"))
code = s.wait()
# undo restored the pristine (nonexistent) state, so nothing was saved
check("M-x completion + undo", code == 0 and not os.path.exists(path("mx.txt")))

# 8. bracketed paste is literal; filename escapes are neutralised
evil = "e\x1b]0;x\x07.txt"
s = Session(evil)
s.send(b"\x1b[200~if x:\n    y\n\x1b[201~")
s.send(C("x") + C("s"))
s.send(C("x") + C("c"))
code = s.wait()
check("paste inserted literally", read(evil) == b"if x:\n    y\n", repr(read(evil)))
check("no OSC from filename reaches terminal", b"\x1b]0;x" not in s.out)

# 9. read-only file: save fails clearly, nothing lost
with open(path("ro.txt"), "w") as f:
    f.write("ro\n")
os.chmod(path("ro.txt"), 0o444)
os.chmod(TMP, 0o555)
s = Session("ro.txt")
s.send("x")
s.send(C("x") + C("s"), 0.3)
check("save error reported", b"Permission denied" in s.out or b"Cannot" in s.out)
os.chmod(TMP, 0o755)
s.send(C("x") + C("c"))
s.send("n")
s.wait()
check("read-only original intact", read("ro.txt") == b"ro\n")

# 9b. nano keymap (default)
s = Session("--nano")
check("nano hint shown", b"^O save" in s.out)
s.send(C("x"))
s.wait()
s = Session("--nano", "nano.txt")
s.send("alpha\rbeta\r")                 # Enter sends CR
s.send(C("w") + b"alp\r")              # ^W search
s.send("X")
s.send("\x1b\\")                      # M-\ first line
s.send(C("k"))                          # ^K cut line
s.send("\x1b/")                         # M-/ last line
s.send(C("u"))                          # ^U paste
s.send(C("o"), 0.3)                     # ^O save
s.send(C("x"))                          # ^X exit
code = s.wait()
check("nano keys: search, cut, paste, save, exit",
      code == 0 and read("nano.txt") == b"beta\nalpXha\n", repr(read("nano.txt")))
s = Session("--nano", "nano2.txt")
s.send("dirty")
s.send(C("x"))
check("nano ^X asks to save", b"Save" in s.out)
s.send(C("c"))                          # ^C cancels
s.send("more")
s.send(C("x"))
s.send("y")
code = s.wait()
check("nano ^X then y saves and exits", code == 0 and read("nano2.txt") == b"dirtymore")

# 9c. mouse: drag-select, right-click menu cut and paste
s = Session("ctx.txt")
s.send("hello world")
s.send(C("a"))
s.send("\x1b[<0;5;1M\x1b[<32;10;1M\x1b[<0;10;1m")  # drag over "hello"
s.send("\x1b[<2;7;1M\x1b[<2;7;1m")                 # right-click inside
check("context menu shown", b"Cut" in s.out and b"Paste" in s.out)
s.send("\r")                                        # Cut (first item)
s.send("\x1b[<2;40;1M\x1b[<2;40;1m")               # right-click line end
s.send("\r")                                        # Paste (first item)
s.send(C("x") + C("s"), 0.3)
s.send(C("x") + C("c"))
code = s.wait()
check("context menu cut + paste", code == 0 and read("ctx.txt") == b" worldhello",
      repr(read("ctx.txt")))

# 10. language server (fake server; UTF-16 positions)
FAKE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_lsp.py")
os.makedirs(TMP + "/cfg/cedit", exist_ok=True)
with open(TMP + "/cfg/cedit/config", "w") as f:
    f.write(f"lsp Python python3 {FAKE}\n")
with open(path("prog.py"), "w") as f:
    f.write("def target():\n    pass\n# \u00e9\U0001F600 BAD\n")
s = Session("prog.py", rows=24, cols=100)
s.read(0.8)
check("lsp diagnostics in status bar", b"E1 W0" in s.out)
check("lsp server name shown", b"python3" in s.out)
s.send("\x1b>")                                 # end of buffer
s.send("ap")
s.send("\x1b/", 0.6)                             # M-/ completion
check("completion popup lists items", b"apple" in s.out and b"apricot" in s.out)
s.send("\x1b[B")                                 # down
s.send("\r")
s.send("\x1bgg")
s.send("3\r")
s.send("\x1bgn", 0.3)                            # next diagnostic
check("next-diagnostic shows message", b"BAD is not allowed" in s.out)
s.send("X")                                       # lands right before BAD
s.send(C("c") + b"h", 0.5)                        # hover
check("hover shows text", b"hover info" in s.out)
s.send("\x1b.", 0.5)                             # definition
s.send("Z")
s.send(C("x") + C("s"), 0.3)
s.send(C("x") + C("c"))
code = s.wait()
data = read("prog.py").decode()
check("completion inserted choice", "apricot" in data, repr(data))
check("diagnostic position UTF-16 -> bytes", "\U0001F600 XBAD" in data, repr(data))
check("definition jumped", data.startswith("def Ztarget"), repr(data))
check("editor exits with server running", code == 0, f"code={code}")
os.remove(TMP + "/cfg/cedit/config")

os.system(f"chmod -R u+w '{TMP}'; rm -rf '{TMP}'")
print(f"{failures} failure(s)")
sys.exit(1 if failures else 0)
