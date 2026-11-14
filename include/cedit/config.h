#ifndef CEDIT_CONFIG_H
#define CEDIT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "cedit/util.h"

struct Editor;

enum { LINENR_OFF = 0, LINENR_ABSOLUTE, LINENR_RELATIVE };
enum { KEYMAP_NANO = 0, KEYMAP_EMACS, KEYMAP_SIMPLE };

typedef struct {
    int tab_width;          /* 1..16 */
    bool expand_tabs;
    bool auto_indent;
    bool auto_pair;         /* insert closing brackets/quotes */
    int line_numbers;       /* LINENR_* */
    int keymap;             /* KEYMAP_* */
    int colors;             /* -1 auto, else ColorMode */
    bool load_plugins;
} Config;

void config_defaults(Config *c);

/*
 * Applies one configuration line ("tab-width 4", "bind C-c d cmd",
 * "face keyword #ff8800 - bold", ...). Blank lines and '#' comments are
 * accepted. Returns 0, or -1 with a message in err.
 */
int config_apply_line(struct Editor *e, const char *line, char *err,
                      size_t errlen);
/* Loads a config file. A missing file is not an error. Problems are
 * appended to errors (one per line). Returns the number of errors. */
int config_load_file(struct Editor *e, const char *path, Str *errors);

/* $XDG_CONFIG_HOME/cedit/<name> (malloc'd) or NULL without a home. */
char *config_path(const char *name);
/* $XDG_STATE_HOME/cedit/<name>, creating the directory. */
char *state_path(const char *name);

#endif
