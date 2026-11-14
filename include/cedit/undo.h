#ifndef CEDIT_UNDO_H
#define CEDIT_UNDO_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Linear undo history. ops[0, cur) can be undone, ops[cur, n) redone.
 * Ops sharing a group id are undone/redone together; the editor starts a
 * new group per command (consecutive typing shares one group).
 */
typedef struct {
    bool insert;
    size_t pos;
    char *data;
    size_t len;
    size_t cursor;          /* cursor before the op */
    unsigned long group;
    unsigned long seq;      /* unique per content state, for "modified" */
} UndoOp;

typedef struct {
    UndoOp *ops;
    size_t n, cur, cap;
    unsigned long group;
    unsigned long next_seq;
    unsigned long saved_seq;
} Undo;

void undo_init(Undo *u);
void undo_free(Undo *u);
void undo_clear(Undo *u);        /* drop history, mark clean */
void undo_boundary(Undo *u);
/* Returns 0 or -1 on ENOMEM (history unchanged). */
int undo_record(Undo *u, bool insert, size_t pos, const char *data,
                size_t len, size_t cursor);
void undo_mark_saved(Undo *u);
bool undo_modified(const Undo *u);

#endif
