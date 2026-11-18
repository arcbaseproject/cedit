#define _DEFAULT_SOURCE /* SIGWINCH */
#include "cedit/terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* Process-wide terminal state: signal handlers need to reach it, so it
 * is the one deliberate piece of global state in the editor. */
static struct termios orig_termios;
static volatile sig_atomic_t raw_active;
static volatile sig_atomic_t pending_signal;
static int sig_pipe[2] = {-1, -1};
static bool atexit_registered;

static const char enter_seq[] = "\x1b[?1049h\x1b[?2004h\x1b[H\x1b[2J";
/* mouse reporting is switched on by render.c, off here */
static const char leave_seq[] = "\x1b[?1006l\x1b[?1002l\x1b[?2004l\x1b[0m\x1b[?25h\x1b[?1049l";

static void write_all_raw(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

/* Async-signal-safe restore. */
static void restore_now(void) {
    if (!raw_active) return;
    write_all_raw(STDOUT_FILENO, leave_seq, sizeof leave_seq - 1);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    raw_active = 0;
}

void term_restore(void) { restore_now(); }

static void notify(char c) {
    int saved = errno;
    if (sig_pipe[1] >= 0 && write(sig_pipe[1], &c, 1) < 0) {
        /* pipe full: an event is already pending */
    }
    errno = saved;
}

static void on_winch(int sig) {
    (void)sig;
    notify('W');
}

static void on_terminate(int sig) {
    pending_signal = sig;
    notify('T');
}

static void on_fatal(int sig) {
    restore_now();
    signal(sig, SIG_DFL);
    raise(sig);
}

static int enable_raw(void) {
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    raw_active = 1;
    write_all_raw(STDOUT_FILENO, enter_seq, sizeof enter_seq - 1);
    return 0;
}

static void install(int sig, void (*fn)(int), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    sigaction(sig, &sa, NULL);
}

int term_init(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        errno = ENOTTY;
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &orig_termios) != 0) return -1;
    if (sig_pipe[0] < 0) {
        if (pipe(sig_pipe) != 0) return -1;
        for (int i = 0; i < 2; i++) {
            fcntl(sig_pipe[i], F_SETFL,
                  fcntl(sig_pipe[i], F_GETFL) | O_NONBLOCK);
            fcntl(sig_pipe[i], F_SETFD, FD_CLOEXEC);
        }
    }
    if (!atexit_registered) {
        atexit(term_restore);
        atexit_registered = true;
    }
    install(SIGWINCH, on_winch, SA_RESTART);
    install(SIGTERM, on_terminate, SA_RESTART);
    install(SIGHUP, on_terminate, SA_RESTART);
    install(SIGINT, on_terminate, SA_RESTART);
    install(SIGQUIT, on_terminate, SA_RESTART);
    install(SIGPIPE, SIG_IGN, 0);
    int fatal[] = {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL};
    for (size_t i = 0; i < sizeof fatal / sizeof fatal[0]; i++)
        install(fatal[i], on_fatal, SA_RESETHAND);
    return enable_raw();
}

int term_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 ||
        ws.ws_col == 0) {
        *rows = 24;
        *cols = 80;
        return -1;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

void term_write(const char *s, size_t n) { write_all_raw(STDOUT_FILENO, s, n); }

int term_wait(int timeout_ms) { return term_wait_fds(timeout_ms, NULL, 0); }

int term_wait_fds(int timeout_ms, struct pollfd *extra, int nextra) {
    struct pollfd fds[2 + 32] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = sig_pipe[0], .events = POLLIN},
    };
    if (nextra > 32) nextra = 32;
    for (int i = 0; i < nextra; i++) fds[2 + i] = extra[i];
    int r = poll(fds, (nfds_t)(2 + nextra), timeout_ms);
    for (int i = 0; i < nextra; i++) extra[i].revents = r > 0 ? fds[2 + i].revents : 0;
    if (r <= 0) return 0; /* timeout or EINTR: caller loops */
    int ev = 0;
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) ev |= TERM_EV_INPUT;
    if (sig_pipe[0] >= 0 && (fds[1].revents & POLLIN)) {
        char buf[64];
        ssize_t n;
        while ((n = read(sig_pipe[0], buf, sizeof buf)) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == 'W') ev |= TERM_EV_RESIZE;
                if (buf[i] == 'T') ev |= TERM_EV_TERMINATE;
            }
        }
    }
    if (pending_signal) ev |= TERM_EV_TERMINATE;
    return ev;
}

ssize_t term_read(unsigned char *buf, size_t n) {
    ssize_t r;
    do {
        r = read(STDIN_FILENO, buf, n);
    } while (r < 0 && errno == EINTR);
    return r;
}

int term_pending_signal(void) { return pending_signal; }

void term_suspend(void) {
    restore_now();
    install(SIGTSTP, SIG_DFL, 0);
    raise(SIGTSTP);
    /* resumed */
    tcgetattr(STDIN_FILENO, &orig_termios);
    enable_raw();
}

ColorMode term_color_mode(void) {
    const char *ct = getenv("COLORTERM");
    const char *t = getenv("TERM");
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit")))
        return COLOR_TRUE;
    if (t && (strstr(t, "256color") || strstr(t, "kitty") ||
              strstr(t, "alacritty") || strstr(t, "foot") ||
              strstr(t, "direct")))
        return t && strstr(t, "direct") ? COLOR_TRUE : COLOR_256;
    return COLOR_16;
}
