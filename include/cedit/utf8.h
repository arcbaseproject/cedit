#ifndef CEDIT_UTF8_H
#define CEDIT_UTF8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UTF8_REPLACEMENT 0xFFFDu

/* Decodes one code point. Always consumes >= 1 byte when len > 0.
 * Invalid, overlong, surrogate or truncated sequences yield
 * UTF8_REPLACEMENT with *valid = false and consume 1 byte. */
size_t utf8_decode(const char *s, size_t len, uint32_t *cp, bool *valid);

/* Encodes cp into out (4 bytes). Returns length, 0 if cp is invalid. */
size_t utf8_encode(uint32_t cp, char out[4]);

static inline bool utf8_is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

/*
 * One displayable unit of a line. Every byte sequence maps to a glyph
 * whose output contains no terminal control bytes, so file contents can
 * never inject escape sequences.
 */
typedef struct {
    size_t bytes;     /* bytes consumed from the source */
    int width;        /* terminal columns */
    char out[16];     /* bytes to send to the terminal (not NUL-terminated) */
    size_t outlen;
    bool special;     /* control char / invalid byte, drawn highlighted */
} Glyph;

/* Decode the glyph at s[0..len) located at display column col. */
void glyph_next(const char *s, size_t len, size_t col, int tab_width,
                Glyph *g);

/* Display width of s[0..len) starting at column 0. */
size_t display_width(const char *s, size_t len, int tab_width);

/* Copies src into dst with every unsafe byte replaced (for filenames
 * and messages). Returns dst. */
char *sanitize_for_display(const char *src, char *dst, size_t size);

/* True for code points that are zero-width and attach to the previous
 * glyph (combining marks etc.). */
bool cp_is_combining(uint32_t cp);

#endif
