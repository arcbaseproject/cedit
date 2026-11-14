#ifndef CEDIT_UTIL_H
#define CEDIT_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Allocation policy: the document text, undo history and file I/O paths
 * use plain malloc/realloc and report ENOMEM to the user without losing
 * data. Small incidental UI allocations use these x* wrappers, which on
 * failure run the registered emergency handler (restores the terminal and
 * writes emergency copies of modified buffers) and then abort.
 */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
void *xreallocarray(void *p, size_t n, size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void util_set_oom_handler(void (*fn)(void));

/* Growable byte string, always NUL-terminated when data != NULL. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Str;

void str_append(Str *s, const char *p, size_t n);
void str_appendz(Str *s, const char *p);
void str_putc(Str *s, char c);
void str_appendf(Str *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void str_clear(Str *s);
void str_free(Str *s);
const char *str_cstr(const Str *s);

/* Copies at most size-1 bytes and always NUL-terminates (size > 0). */
size_t str_copy(char *dst, const char *src, size_t size);

#endif
