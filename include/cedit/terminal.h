#ifndef CEDIT_TERMINAL_H
#define CEDIT_TERMINAL_H

#include <stddef.h>
#include <sys/types.h>

typedef enum { COLOR_16, COLOR_256, COLOR_TRUE } ColorMode;

enum {
    TERM_EV_INPUT = 1,
    TERM_EV_RESIZE = 2,
    TERM_EV_TERMINATE = 4,
};

/* Raw mode, alternate screen, bracketed paste, signal handlers.
 * Returns 0 or -1 with errno set. */
int term_init(void);
/* Idempotent; also registered with atexit. */
void term_restore(void);
int term_size(int *rows, int *cols);
/* Writes everything, retrying on EINTR/short writes. */
void term_write(const char *s, size_t n);
/* Blocks until input, a resize or a termination signal (timeout_ms < 0
 * waits forever). Returns a mask of TERM_EV_*, 0 on timeout. */
int term_wait(int timeout_ms);
/* Same, also polling extra descriptors (their revents are filled in). */
struct pollfd;
int term_wait_fds(int timeout_ms, struct pollfd *extra, int nextra);
ssize_t term_read(unsigned char *buf, size_t n);
/* Signal number of a pending SIGTERM/SIGHUP/..., or 0. */
int term_pending_signal(void);
/* Restore the terminal, stop the process (job control), re-init. */
void term_suspend(void);
ColorMode term_color_mode(void);

#endif
