#ifndef CEDIT_TEXT_H
#define CEDIT_TEXT_H

#include <stddef.h>

/*
 * Gap buffer holding the raw bytes of a document, plus an index of line
 * start offsets kept up to date incrementally on every edit.
 *
 * Positions are byte offsets into the logical text (gap excluded).
 * Line i spans [line_start(i), line_end(i)); line_end points at the '\n'
 * or at the end of the text. There is always at least one line.
 *
 * Line starts after an edit shift by the edit length. That shift is kept
 * lazily: entries from index dfrom on are stale by +delta (modular
 * arithmetic), and only the entries between consecutive edit locations
 * get rewritten. Typing on one line therefore costs O(1) in the number
 * of lines; inserting or removing a line break memmoves the index tail.
 */
typedef struct {
    char *buf;
    size_t cap;
    size_t gs, ge;      /* gap is buf[gs, ge) */
    size_t *lines;      /* lines[i] = start offset of line i */
    size_t nlines;
    size_t lcap;
    size_t dfrom;       /* lines[i] for i >= dfrom need + delta */
    size_t delta;
} Text;

int text_init(Text *t);
void text_free(Text *t);

/* Replace the whole content. Returns 0 or -1 (ENOMEM); on failure the
 * old content is untouched. */
int text_set(Text *t, const char *data, size_t len);
/* Like text_set but adopts the malloc'd data (no copy). On failure the
 * caller still owns data. */
int text_take(Text *t, char *data, size_t len);

size_t text_len(const Text *t);
unsigned char text_byte(const Text *t, size_t pos);

/* Return 0 on success, -1 with errno = ENOMEM/EINVAL on failure (text
 * unchanged). */
int text_insert(Text *t, size_t pos, const char *s, size_t n);
int text_delete(Text *t, size_t pos, size_t n);

/* Copy [pos, pos+n) clipped to the text into out; returns bytes copied. */
size_t text_copy(const Text *t, size_t pos, size_t n, char *out);
/* malloc'd NUL-terminated copy, NULL on ENOMEM. */
char *text_dup(const Text *t, size_t pos, size_t n);

/* Pointer to a contiguous view of [pos, pos+n). May move the gap, which
 * invalidates previously returned pointers. */
const char *text_view(Text *t, size_t pos, size_t n);

size_t text_lines(const Text *t);
size_t text_line_start(const Text *t, size_t line);
size_t text_line_end(const Text *t, size_t line);
size_t text_line_of(const Text *t, size_t pos);

/* The two contiguous halves of the text (for writing to disk). */
void text_segments(const Text *t, const char **a, size_t *alen,
                   const char **b, size_t *blen);

#endif
