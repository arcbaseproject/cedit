#include "cedit/util.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void (*oom_handler)(void);

void util_set_oom_handler(void (*fn)(void)) { oom_handler = fn; }

static void out_of_memory(void) {
    static const char msg[] = "cedit: out of memory\n";
    if (oom_handler) {
        void (*fn)(void) = oom_handler;
        oom_handler = NULL; /* never recurse */
        fn();
    }
    if (write(STDERR_FILENO, msg, sizeof msg - 1) < 0) {
        /* nothing else we can do */
    }
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) out_of_memory();
    return q;
}

void *xreallocarray(void *p, size_t n, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(n, size, &total)) out_of_memory();
    return xrealloc(p, total);
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

static void str_reserve(Str *s, size_t extra) {
    size_t need;
    if (__builtin_add_overflow(s->len, extra, &need) ||
        __builtin_add_overflow(need, 1, &need))
        out_of_memory();
    if (need <= s->cap) return;
    size_t cap = s->cap ? s->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    s->data = xrealloc(s->data, cap);
    s->cap = cap;
}

void str_append(Str *s, const char *p, size_t n) {
    str_reserve(s, n);
    if (n) memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = '\0';
}

void str_appendz(Str *s, const char *p) { str_append(s, p, strlen(p)); }

void str_putc(Str *s, char c) { str_append(s, &c, 1); }

void str_appendf(Str *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char small[256];
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof small) {
        str_append(s, small, (size_t)n);
        return;
    }
    str_reserve(s, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(s->data + s->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    s->len += (size_t)n;
}

void str_clear(Str *s) {
    s->len = 0;
    if (s->data) s->data[0] = '\0';
}

void str_free(Str *s) {
    free(s->data);
    s->data = NULL;
    s->len = s->cap = 0;
}

const char *str_cstr(const Str *s) { return s->data ? s->data : ""; }

size_t str_copy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size == 0) return n;
    size_t c = n < size - 1 ? n : size - 1;
    memcpy(dst, src, c);
    dst[c] = '\0';
    return n;
}
