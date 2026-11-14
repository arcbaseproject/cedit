#ifndef CEDIT_COMMANDS_H
#define CEDIT_COMMANDS_H

#include <stdbool.h>

#include "cedit/keymap.h"

struct Editor;
typedef void (*CommandFn)(struct Editor *e);

enum {
    CMD_MOTION = 1,     /* shift+key extends the selection */
    CMD_KILL = 2,       /* consecutive kills append to the kill buffer */
    CMD_COALESCE = 4,   /* repeats share one undo group (typing) */
    CMD_KEEP_GOAL = 8,  /* vertical motion: keep the goal column */
};

typedef struct {
    char *name;
    CommandFn fn;       /* NULL for scripted commands */
    const char *doc;
    unsigned flags;
    int script_ref;     /* Lua registry ref, or -1 */
} Command;

void commands_init(struct Editor *e);
void commands_free(struct Editor *e);
/* Replaces an existing command of the same name. */
void command_register(struct Editor *e, const char *name, CommandFn fn,
                      const char *doc, unsigned flags, int script_ref);
const Command *command_find(const struct Editor *e, const char *name);
/* Run by name as if invoked by a key. Returns false if unknown. */
bool command_run(struct Editor *e, const char *name);

/* Default key bindings. */
void keymap_load_emacs(Keymap *m);
void keymap_load_simple(Keymap *m);
void keymap_load_nano(Keymap *m);

/* Registration of command groups (one per source file). */
void commands_register_edit(struct Editor *e);
void commands_register_file(struct Editor *e);
void commands_register_search(struct Editor *e);

/* Shared helpers used by several command groups. */
void cmd_self_insert(struct Editor *e);
bool insert_text(struct Editor *e, const char *s, size_t n);

#endif
