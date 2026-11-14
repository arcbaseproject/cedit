#ifndef CEDIT_SEARCH_H
#define CEDIT_SEARCH_H

#include <regex.h>
#include <stdbool.h>
#include <stddef.h>

#include "cedit/buffer.h"
#include "cedit/util.h"

/*
 * A compiled search pattern: literal or POSIX extended regex, with
 * smart case (case-insensitive unless the pattern contains an
 * uppercase letter). Matching is line by line, so a match never spans
 * a newline. Regex matching stops at NUL bytes inside a line.
 */
typedef struct {
    char *pat;
    size_t patlen;
    bool regex;
    bool icase;
    bool compiled;
    regex_t re;
    regmatch_t groups[10];  /* submatches of the last regex match */
    Str scratch;
} Search;

int search_compile(Search *s, const char *pat, bool regex, char *err,
                   size_t errlen);
void search_free(Search *s);

/* First match in line[0..len) starting at or after from. */
bool search_in_line(Search *s, const char *line, size_t len, size_t from,
                    size_t *ms, size_t *me);

/* Next match at or after pos (forward) or starting before pos
 * (backward), optionally wrapping around the buffer. */
bool search_find(Search *s, Buffer *b, size_t pos, bool forward, bool wrap,
                 size_t *ms, size_t *me);

/* Expand \0-\9 and \\ in a regex replacement using the last match of
 * line. For literal searches the replacement is copied as is. */
void search_expand(const Search *s, const char *line, const char *repl,
                   Str *out);

#endif
