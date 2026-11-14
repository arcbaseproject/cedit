#ifndef CEDIT_KEYMAP_H
#define CEDIT_KEYMAP_H

#include <stddef.h>

#include "cedit/input.h"
#include "cedit/util.h"

/* A keymap maps keys to command names or to nested prefix keymaps
 * ("C-x" -> map containing "C-s" -> "save-buffer"). Commands are stored
 * by name and resolved at dispatch, so bindings may precede the
 * definition of (e.g. scripted) commands. */
typedef struct Keymap Keymap;

enum { KM_UNBOUND = 0, KM_COMMAND, KM_PREFIX };

Keymap *keymap_new(void);
void keymap_free(Keymap *m);
/* seq is space-separated keys, e.g. "C-x C-s". cmd NULL unbinds.
 * Returns 0, or -1 with a message in err. */
int keymap_bind(Keymap *root, const char *seq, const char *cmd, char *err,
                size_t errlen);
int keymap_lookup(const Keymap *m, Key k, const char **cmd,
                  const Keymap **prefix);
/* Appends "keys<TAB>command" lines for every binding. */
void keymap_describe(const Keymap *m, Str *out);
/* First key sequence bound to cmd, or false. */
bool keymap_where_is(const Keymap *m, const char *cmd, char *buf,
                     size_t size);

#endif
