#include "cedit/keymap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Key key;
    char *cmd;
    Keymap *sub;
} Binding;

struct Keymap {
    Binding *b;
    size_t n, cap;
};

Keymap *keymap_new(void) { return xcalloc(1, sizeof(Keymap)); }

void keymap_free(Keymap *m) {
    if (!m) return;
    for (size_t i = 0; i < m->n; i++) {
        free(m->b[i].cmd);
        keymap_free(m->b[i].sub);
    }
    free(m->b);
    free(m);
}

static Binding *find(const Keymap *m, Key k) {
    for (size_t i = 0; i < m->n; i++)
        if (m->b[i].key == k) return &m->b[i];
    return NULL;
}

static Binding *add(Keymap *m, Key k) {
    Binding *b = find(m, k);
    if (b) return b;
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->b = xreallocarray(m->b, m->cap, sizeof *m->b);
    }
    b = &m->b[m->n++];
    memset(b, 0, sizeof *b);
    b->key = k;
    return b;
}

static void unbind_at(Keymap *m, Binding *b) {
    free(b->cmd);
    keymap_free(b->sub);
    *b = m->b[--m->n];
}

int keymap_bind(Keymap *root, const char *seq, const char *cmd, char *err,
                size_t errlen) {
    Key keys[8];
    size_t nk = 0;
    const char *p = seq;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (nk == sizeof keys / sizeof keys[0]) {
            snprintf(err, errlen, "key sequence too long");
            return -1;
        }
        if (!key_parse(s, (size_t)(p - s), &keys[nk])) {
            snprintf(err, errlen, "bad key '%.*s'", (int)(p - s > 32 ? 32 : p - s), s);
            return -1;
        }
        nk++;
    }
    if (nk == 0) {
        snprintf(err, errlen, "empty key sequence");
        return -1;
    }
    Keymap *m = root;
    for (size_t i = 0; i + 1 < nk; i++) {
        Binding *b = find(m, keys[i]);
        if (!b && !cmd) return 0;
        if (!b) b = add(m, keys[i]);
        if (!b->sub) {
            free(b->cmd);
            b->cmd = NULL;
            b->sub = keymap_new();
        }
        m = b->sub;
    }
    Binding *b = find(m, keys[nk - 1]);
    if (!cmd) {
        if (b) unbind_at(m, b);
        return 0;
    }
    b = add(m, keys[nk - 1]);
    keymap_free(b->sub);
    b->sub = NULL;
    free(b->cmd);
    b->cmd = xstrdup(cmd);
    return 0;
}

int keymap_lookup(const Keymap *m, Key k, const char **cmd,
                  const Keymap **prefix) {
    Binding *b = find(m, k);
    if (!b) return KM_UNBOUND;
    if (b->sub) {
        *prefix = b->sub;
        return KM_PREFIX;
    }
    *cmd = b->cmd;
    return KM_COMMAND;
}

static int cmp_lines(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void collect(const Keymap *m, const char *prefix, char ***lines,
                    size_t *n, size_t *cap) {
    for (size_t i = 0; i < m->n; i++) {
        char name[48], keys[256];
        key_name(m->b[i].key, name, sizeof name);
        snprintf(keys, sizeof keys, "%s%s%s", prefix, *prefix ? " " : "",
                 name);
        if (m->b[i].sub) {
            collect(m->b[i].sub, keys, lines, n, cap);
            continue;
        }
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *lines = xreallocarray(*lines, *cap, sizeof **lines);
        }
        Str s = {0};
        str_appendf(&s, "%-20s %s\n", keys, m->b[i].cmd);
        (*lines)[(*n)++] = s.data;
    }
}

void keymap_describe(const Keymap *m, Str *out) {
    char **lines = NULL;
    size_t n = 0, cap = 0;
    collect(m, "", &lines, &n, &cap);
    qsort(lines, n, sizeof *lines, cmp_lines);
    for (size_t i = 0; i < n; i++) {
        str_appendz(out, lines[i]);
        free(lines[i]);
    }
    free(lines);
}

static bool where(const Keymap *m, const char *cmd, char *buf, size_t size,
                  size_t used) {
    for (size_t i = 0; i < m->n; i++) {
        char name[48];
        key_name(m->b[i].key, name, sizeof name);
        int w = snprintf(buf + used, size - used, "%s%s", used ? " " : "",
                         name);
        if (w < 0 || (size_t)w >= size - used) continue;
        if (m->b[i].sub) {
            if (where(m->b[i].sub, cmd, buf, size, used + (size_t)w))
                return true;
        } else if (strcmp(m->b[i].cmd, cmd) == 0) {
            return true;
        }
        buf[used] = '\0';
    }
    return false;
}

bool keymap_where_is(const Keymap *m, const char *cmd, char *buf,
                     size_t size) {
    if (size == 0) return false;
    buf[0] = '\0';
    return where(m, cmd, buf, size, 0);
}
