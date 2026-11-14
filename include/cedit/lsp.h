#ifndef CEDIT_LSP_H
#define CEDIT_LSP_H

#include <stdbool.h>
#include <stddef.h>

struct Editor;
struct Buffer;
struct pollfd;

/*
 * Language Server Protocol client: diagnostics, completion, go to
 * definition and hover.
 *
 * Servers are ordinary executables found in PATH (or configured with
 * "lsp LANG COMMAND ARGS..."), started directly with execvp (no shell)
 * when the first file of their language is opened, one per language.
 * Their stderr goes to /dev/null so they cannot write to the terminal.
 * Note that some servers execute project code (e.g. rust-analyzer runs
 * build scripts); "lsp off" disables all servers.
 *
 * Documents are synced in full, 300 ms after the last keystroke.
 */

void lsp_register_commands(struct Editor *e);
/* Config directives: "lsp on|off", "lsp LANG off", "lsp LANG CMD...". */
int lsp_config(struct Editor *e, int ntok, char **tok, char *err,
               size_t errlen);

void lsp_buffer_opened(struct Editor *e, struct Buffer *b);
void lsp_buffer_saved(struct Editor *e, struct Buffer *b);
void lsp_buffer_closed(struct Editor *e, struct Buffer *b);

/* Event loop integration. */
int lsp_pollfds(struct Editor *e, struct pollfd *fds, int max);
int lsp_timeout_ms(struct Editor *e);   /* -1: nothing pending */
/* Handle I/O on the fds from lsp_pollfds; send due document changes.
 * Returns true if the screen should be redrawn. */
bool lsp_process(struct Editor *e, struct pollfd *fds, int n);

/* Short status for the status bar ("" if no server). */
const char *lsp_status(struct Editor *e, struct Buffer *b);
void lsp_shutdown(struct Editor *e);

#endif
