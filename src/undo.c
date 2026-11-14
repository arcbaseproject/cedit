#include "cedit/undo.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void undo_init(Undo *u) {
    memset(u, 0, sizeof *u);
    u->next_seq = 1;
}

static void drop_from(Undo *u, size_t from) {
    for (size_t i = from; i < u->n; i++) free(u->ops[i].data);
    u->n = from;
    if (u->cur > from) u->cur = from;
}

void undo_free(Undo *u) {
    drop_from(u, 0);
    free(u->ops);
    memset(u, 0, sizeof *u);
}

void undo_clear(Undo *u) {
    drop_from(u, 0);
    u->saved_seq = 0;
}

void undo_boundary(Undo *u) { u->group++; }

static unsigned long top_seq(const Undo *u) {
    return u->cur ? u->ops[u->cur - 1].seq : 0;
}

void undo_mark_saved(Undo *u) { u->saved_seq = top_seq(u); }

bool undo_modified(const Undo *u) { return top_seq(u) != u->saved_seq; }

/* Try to extend the last op of the current group instead of adding one. */
static int try_merge(Undo *u, bool insert, size_t pos, const char *data,
                     size_t len) {
    if (u->cur == 0 || u->cur != u->n) return 1;
    UndoOp *op = &u->ops[u->cur - 1];
    if (op->group != u->group || op->insert != insert) return 1;
    if (op->len >= 1024) return 1; /* bounded cost: prepending is O(len) */
    bool append, prepend = false;
    if (insert) {
        append = pos == op->pos + op->len;
    } else {
        append = pos == op->pos;              /* forward delete */
        prepend = pos + len == op->pos;       /* backspace */
    }
    if (!append && !prepend) return 1;
    char *nd = realloc(op->data, op->len + len);
    if (!nd) return -1;
    if (append) {
        memcpy(nd + op->len, data, len);
    } else {
        memmove(nd + len, nd, op->len);
        memcpy(nd, data, len);
        op->pos = pos;
    }
    op->data = nd;
    op->len += len;
    op->seq = u->next_seq++;
    return 0;
}

int undo_record(Undo *u, bool insert, size_t pos, const char *data,
                size_t len, size_t cursor) {
    if (len == 0) return 0;
    drop_from(u, u->cur);
    int m = try_merge(u, insert, pos, data, len);
    if (m <= 0) {
        if (m < 0) errno = ENOMEM;
        return m;
    }
    if (u->n == u->cap) {
        size_t cap = u->cap ? u->cap * 2 : 64;
        UndoOp *ops = realloc(u->ops, cap * sizeof *ops);
        if (!ops) {
            errno = ENOMEM;
            return -1;
        }
        u->ops = ops;
        u->cap = cap;
    }
    char *copy = malloc(len);
    if (!copy) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(copy, data, len);
    u->ops[u->n++] = (UndoOp){
        .insert = insert,
        .pos = pos,
        .data = copy,
        .len = len,
        .cursor = cursor,
        .group = u->group,
        .seq = u->next_seq++,
    };
    u->cur = u->n;
    return 0;
}
