#include "cedit/text.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MIN_GAP 4096

int text_init(Text *t) {
    memset(t, 0, sizeof *t);
    t->buf = malloc(MIN_GAP);
    t->lines = malloc(16 * sizeof *t->lines);
    if (!t->buf || !t->lines) {
        free(t->buf);
        free(t->lines);
        memset(t, 0, sizeof *t);
        errno = ENOMEM;
        return -1;
    }
    t->cap = MIN_GAP;
    t->gs = 0;
    t->ge = MIN_GAP;
    t->lines[0] = 0;
    t->nlines = 1;
    t->lcap = 16;
    t->dfrom = 1;
    t->delta = 0;
    return 0;
}

void text_free(Text *t) {
    free(t->buf);
    free(t->lines);
    memset(t, 0, sizeof *t);
}

size_t text_len(const Text *t) { return t->cap - (t->ge - t->gs); }

unsigned char text_byte(const Text *t, size_t pos) {
    return (unsigned char)(pos < t->gs ? t->buf[pos]
                                       : t->buf[pos + (t->ge - t->gs)]);
}

static size_t count_newlines(const char *s, size_t n) {
    size_t c = 0;
    const char *end = s + n;
    while (s < end && (s = memchr(s, '\n', (size_t)(end - s))) != NULL) {
        c++;
        s++;
    }
    return c;
}

static int lines_reserve(Text *t, size_t need) {
    if (need <= t->lcap) return 0;
    size_t cap = t->lcap ? t->lcap : 16;
    while (cap < need) {
        if (cap > SIZE_MAX / 2 / sizeof(size_t)) {
            errno = ENOMEM;
            return -1;
        }
        cap *= 2;
    }
    size_t *nl = realloc(t->lines, cap * sizeof *nl);
    if (!nl) {
        errno = ENOMEM;
        return -1;
    }
    t->lines = nl;
    t->lcap = cap;
    return 0;
}

static size_t *index_lines(const char *data, size_t len, size_t *count) {
    size_t nl = count_newlines(data, len) + 1;
    if (nl > SIZE_MAX / sizeof(size_t)) return NULL;
    size_t *lines = malloc(nl * sizeof *lines);
    if (!lines) return NULL;
    size_t li = 0;
    lines[li++] = 0;
    for (const char *p = data, *end = data + len;
         p < end && (p = memchr(p, '\n', (size_t)(end - p))) != NULL; p++)
        lines[li++] = (size_t)(p - data) + 1;
    *count = nl;
    return lines;
}

static void install(Text *t, char *buf, size_t cap, size_t len,
                    size_t *lines, size_t nl) {
    free(t->buf);
    free(t->lines);
    t->buf = buf;
    t->cap = cap;
    t->gs = len;
    t->ge = cap;
    t->lines = lines;
    t->nlines = nl;
    t->lcap = nl;
    t->dfrom = nl;
    t->delta = 0;
}

int text_set(Text *t, const char *data, size_t len) {
    size_t cap, nl;
    if (__builtin_add_overflow(len, (size_t)MIN_GAP, &cap)) {
        errno = ENOMEM;
        return -1;
    }
    char *buf = malloc(cap);
    size_t *lines = buf ? index_lines(data, len, &nl) : NULL;
    if (!buf || !lines) {
        free(buf);
        errno = ENOMEM;
        return -1;
    }
    if (len) memcpy(buf, data, len);
    install(t, buf, cap, len, lines, nl);
    return 0;
}

int text_take(Text *t, char *data, size_t len) {
    size_t cap, nl;
    if (__builtin_add_overflow(len, (size_t)MIN_GAP, &cap)) {
        errno = ENOMEM;
        return -1;
    }
    size_t *lines = index_lines(data, len, &nl);
    if (!lines) {
        errno = ENOMEM;
        return -1;
    }
    char *buf = realloc(data, cap);
    if (!buf) {
        free(lines);
        errno = ENOMEM;
        return -1;
    }
    install(t, buf, cap, len, lines, nl);
    return 0;
}

/* Make dfrom == at by applying the pending delta to the entries between. */
static void settle(Text *t, size_t at) {
    if (t->delta == 0) {
        t->dfrom = at;
        return;
    }
    if (at < t->dfrom) {
        /* entries [at, dfrom) are exact; they join the pending range */
        for (size_t i = at; i < t->dfrom; i++) t->lines[i] -= t->delta;
    } else {
        for (size_t i = t->dfrom; i < at && i < t->nlines; i++)
            t->lines[i] += t->delta;
    }
    t->dfrom = at;
}

static size_t line_at(const Text *t, size_t i) {
    return i >= t->dfrom ? t->lines[i] + t->delta : t->lines[i];
}

static void move_gap(Text *t, size_t pos) {
    size_t gap = t->ge - t->gs;
    if (pos < t->gs) {
        size_t n = t->gs - pos;
        memmove(t->buf + t->ge - n, t->buf + pos, n);
        t->gs = pos;
        t->ge = pos + gap;
    } else if (pos > t->gs) {
        size_t n = pos - t->gs;
        memmove(t->buf + t->gs, t->buf + t->ge, n);
        t->gs += n;
        t->ge += n;
    }
}

static int ensure_gap(Text *t, size_t n) {
    size_t gap = t->ge - t->gs;
    if (gap >= n) return 0;
    size_t len = text_len(t), need, cap;
    if (__builtin_add_overflow(len, n, &need) ||
        __builtin_add_overflow(need, (size_t)MIN_GAP, &need)) {
        errno = ENOMEM;
        return -1;
    }
    cap = t->cap > SIZE_MAX / 2 ? need : t->cap * 2;
    if (cap < need) cap = need;
    char *nb = realloc(t->buf, cap);
    if (!nb) {
        errno = ENOMEM;
        return -1;
    }
    size_t tail = t->cap - t->ge;
    memmove(nb + cap - tail, nb + t->ge, tail);
    t->buf = nb;
    t->ge = cap - tail;
    t->cap = cap;
    return 0;
}

size_t text_line_of(const Text *t, size_t pos) {
    size_t lo = 0, hi = t->nlines; /* find last i with lines[i] <= pos */
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (line_at(t, mid) <= pos)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

int text_insert(Text *t, size_t pos, const char *s, size_t n) {
    size_t len = text_len(t);
    if (pos > len) {
        errno = EINVAL;
        return -1;
    }
    if (n == 0) return 0;
    size_t k = count_newlines(s, n);
    size_t newcount;
    if (__builtin_add_overflow(t->nlines, k, &newcount) ||
        lines_reserve(t, newcount) != 0 || ensure_gap(t, n) != 0) {
        errno = ENOMEM;
        return -1;
    }
    move_gap(t, pos);
    memcpy(t->buf + t->gs, s, n);
    t->gs += n;

    size_t l = text_line_of(t, pos);
    settle(t, l + 1);
    t->delta += n;
    if (k) {
        memmove(t->lines + l + 1 + k, t->lines + l + 1,
                (t->nlines - (l + 1)) * sizeof(size_t));
        size_t idx = l + 1;
        for (size_t j = 0; j < n; j++)
            if (s[j] == '\n') t->lines[idx++] = pos + j + 1;
        t->dfrom = l + 1 + k; /* new entries are exact */
    }
    t->nlines = newcount;
    return 0;
}

int text_delete(Text *t, size_t pos, size_t n) {
    size_t len = text_len(t);
    if (pos > len || n > len - pos) {
        errno = EINVAL;
        return -1;
    }
    if (n == 0) return 0;
    size_t l1 = text_line_of(t, pos);
    size_t l2 = text_line_of(t, pos + n);
    move_gap(t, pos);
    t->ge += n;

    settle(t, l2 + 1);
    t->delta -= n;
    if (l2 > l1) {
        memmove(t->lines + l1 + 1, t->lines + l2 + 1,
                (t->nlines - (l2 + 1)) * sizeof(size_t));
        t->nlines -= l2 - l1;
        t->dfrom = l1 + 1;
    }
    return 0;
}

size_t text_copy(const Text *t, size_t pos, size_t n, char *out) {
    size_t len = text_len(t);
    if (pos >= len) return 0;
    if (n > len - pos) n = len - pos;
    size_t done = 0;
    if (pos < t->gs) {
        size_t a = t->gs - pos < n ? t->gs - pos : n;
        memcpy(out, t->buf + pos, a);
        done = a;
    }
    if (done < n) {
        size_t p = pos + done + (t->ge - t->gs);
        memcpy(out + done, t->buf + p, n - done);
    }
    return n;
}

char *text_dup(const Text *t, size_t pos, size_t n) {
    size_t len = text_len(t);
    if (pos > len) pos = len;
    if (n > len - pos) n = len - pos;
    char *p = malloc(n + 1);
    if (!p) {
        errno = ENOMEM;
        return NULL;
    }
    text_copy(t, pos, n, p);
    p[n] = '\0';
    return p;
}

const char *text_view(Text *t, size_t pos, size_t n) {
    size_t len = text_len(t);
    if (pos > len) pos = len;
    if (n > len - pos) n = len - pos;
    if (pos < t->gs && pos + n > t->gs) move_gap(t, pos + n);
    return pos < t->gs ? t->buf + pos : t->buf + pos + (t->ge - t->gs);
}

size_t text_lines(const Text *t) { return t->nlines; }

size_t text_line_start(const Text *t, size_t line) {
    if (line >= t->nlines) return text_len(t);
    return line_at(t, line);
}

size_t text_line_end(const Text *t, size_t line) {
    if (line + 1 >= t->nlines) return text_len(t);
    return line_at(t, line + 1) - 1;
}

void text_segments(const Text *t, const char **a, size_t *alen,
                   const char **b, size_t *blen) {
    *a = t->buf;
    *alen = t->gs;
    *b = t->buf + t->ge;
    *blen = t->cap - t->ge;
}
