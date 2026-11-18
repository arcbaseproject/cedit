#ifndef CEDIT_TREE_H
#define CEDIT_TREE_H

#include <stdbool.h>

#include "cedit/input.h"

struct Editor;

/*
 * File tree sidebar on the left of the text area, rooted at the working
 * directory. The "file-tree" command shows it and moves focus into it;
 * while focused, the keys below drive the tree and any other key returns
 * focus to the text and runs as usual. Dotfiles are hidden, as in
 * find-file completion.
 *
 *   up/C-p, down/C-n  move        RET    open file, fold/unfold folder
 *   right             unfold      left   fold, or jump to parent folder
 *   ESC, C-g, TAB     back to text   q, file-tree key   hide the tree
 */

typedef struct {
    char *path;   /* relative to the working directory */
    int depth;
    bool dir, open;
} TreeEntry;

void tree_register_commands(struct Editor *e);
void tree_free(struct Editor *e);
/* (Re)list the entries of dir as the top level. */
void tree_load(struct Editor *e, const char *dir);
/* Columns the sidebar takes, separator included; 0 when hidden. */
int tree_width(const struct Editor *e);
/* Keep the selection inside rows visible rows. */
void tree_scroll(struct Editor *e, int rows);
/* Handle one key while the tree has focus. Returns false if the tree
 * does not use the key (the caller then gives focus back). */
bool tree_key(struct Editor *e, Key k);

#endif
