#ifndef CEDIT_SCRIPT_H
#define CEDIT_SCRIPT_H

#include <stdbool.h>

struct Editor;
struct Buffer;

/*
 * Optional Lua scripting (built with `make LUA=1`). Scripts run with the
 * full privileges of the user: they are NOT sandboxed. Only files from
 * the user's own config directory are loaded, and only if they are
 * owned by the user and not writable by group or others.
 */
bool script_available(void);
/* Load init.lua and plugins/<*>.lua. Errors are reported as messages. */
void script_init(struct Editor *e);
void script_free(struct Editor *e);
/* Events: "open", "save", "modify", "buffer-switch". */
void script_hook(struct Editor *e, const char *event, struct Buffer *b);
void script_run_command(struct Editor *e, int ref);
/* Evaluate a Lua chunk typed by the user (M-x lua-eval). */
void script_eval(struct Editor *e, const char *code);

#endif
