#ifndef CEDIT_SYNTAX_H
#define CEDIT_SYNTAX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Highlight classes, one per byte of a line. */
enum {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_KEYWORD,
    HL_TYPE,
    HL_STRING,
    HL_NUMBER,
    HL_OPERATOR,
    HL_PREPROC,
    HL_CLASS_COUNT
};

/*
 * A language definition. Most languages are described by data (keyword
 * lists, comment and string delimiters) and use the generic tokenizer;
 * languages with unusual structure (Markdown, Makefiles) supply their own
 * highlight function. Multiline constructs are carried between lines as
 * a small state value (0 = none).
 */
typedef struct Syntax Syntax;

const Syntax *syntax_detect(const char *path, const char *first_line,
                            size_t len);
const Syntax *syntax_by_name(const char *name);
const char *syntax_name(const Syntax *s);
/* Comma-separated list of supported language names. */
const char *syntax_names(void);

/* True if the language uses quote character q for strings (used for
 * auto-pairing). */
bool syntax_quote_pairs(const Syntax *s, char q);

/* Fills hl[0..len) and returns the state at the start of the next line. */
uint8_t syntax_highlight(const Syntax *s, const char *line, size_t len,
                         uint8_t state, uint8_t *hl);

#endif
