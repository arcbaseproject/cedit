#ifndef CEDIT_BUFFER_H
#define CEDIT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cedit/file.h"
#include "cedit/syntax.h"
#include "cedit/text.h"
#include "cedit/undo.h"

/* A diagnostic from a language server, in byte columns of one line. */
typedef struct {
    size_t line, start, end;    /* end exclusive */
    int severity;               /* 1 error, 2 warning, 3 info, 4 hint */
    char *msg;
} Diagnostic;

/* A document: text, history, file association and per-buffer view. */
typedef struct Buffer {
    Text text;
    Undo undo;
    char *path;             /* NULL if not associated with a file */
    char *name;             /* display name, unique among buffers */
    bool crlf;              /* every line ended in CRLF on load */
    bool readonly;
    char *special_name;     /* identity of generated help/list buffers */
    FileStamp stamp;        /* disk state at last load/save */
    bool disk_changed_warned;

    size_t cursor;          /* byte offset */
    size_t mark;
    bool mark_active;
    bool shift_selection;   /* mark was set by a shifted movement key */
    size_t goal_col;        /* display column kept across vertical moves */
    bool goal_valid;
    size_t top;             /* first visible line */
    size_t left;            /* first visible display column */

    const Syntax *syntax;
    uint8_t *hl_state;      /* syntax state at start of each line */
    size_t hl_cap;
    size_t hl_valid;        /* hl_state[0..hl_valid) are valid */

    int tab_width;
    bool expand_tabs;

    unsigned long version;  /* bumped on every content change */
    Diagnostic *diags;
    size_t ndiags;
} Buffer;

void buffer_clear_diags(Buffer *b);

Buffer *buffer_new(const char *name);
void buffer_free(Buffer *b);

/* Edits record undo, adjust cursor/mark, invalidate highlighting.
 * Return 0 or -1 (errno ENOMEM/EINVAL/EROFS), buffer unchanged. */
int buffer_insert(Buffer *b, size_t pos, const char *s, size_t n);
int buffer_delete(Buffer *b, size_t pos, size_t n);
int buffer_replace_all(Buffer *b, const char *s, size_t n);

bool buffer_undo(Buffer *b);
bool buffer_redo(Buffer *b);
bool buffer_modified(const Buffer *b);

size_t buffer_len(const Buffer *b);
size_t buffer_cur_line(const Buffer *b);
/* Display column of byte offset pos within its line. */
size_t buffer_col_of(Buffer *b, size_t pos);
/* Byte offset in line for display column col (clamped to line end). */
size_t buffer_pos_at_col(Buffer *b, size_t line, size_t col);
/* Code point boundaries, skipping combining marks. */
size_t buffer_next_char(Buffer *b, size_t pos);
size_t buffer_prev_char(Buffer *b, size_t pos);

/* Load path into b, replacing its contents. A missing file yields an
 * empty buffer bound to path (returns 0, *is_new = true). */
int buffer_load(Buffer *b, const char *path, bool *is_new, char *err,
                size_t errlen);
int buffer_save(Buffer *b, const char *path, bool *in_place, char *err,
                size_t errlen);
/* True if the file on disk differs from the last load/save. */
bool buffer_disk_changed(const Buffer *b);

/* Syntax state at the start of line, computing lazily. */
uint8_t buffer_hl_state(Buffer *b, size_t line);
void buffer_set_syntax(Buffer *b, const Syntax *s);

/* Selection [*start, *end) if mark active and non-empty. */
bool buffer_region(const Buffer *b, size_t *start, size_t *end);

#endif
