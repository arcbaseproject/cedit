#include "cedit/json.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "cedit/utf8.h"

#define MAX_DEPTH 64

typedef struct {
    const char *s;
    size_t len, i;
} P;

static void ws(P *p) {
    while (p->i < p->len && strchr(" \t\r\n", p->s[p->i]) && p->s[p->i]) p->i++;
}

static void release(Json *j) {
    if (j->type == JSON_STR) free(j->str);
    if (j->type == JSON_ARR || j->type == JSON_OBJ) {
        for (size_t i = 0; i < j->n; i++) {
            release(&j->items[i]);
            if (j->keys) free(j->keys[i]);
        }
        free(j->items);
        free(j->keys);
    }
}

void json_free(Json *j) {
    if (!j) return;
    release(j);
    free(j);
}

static int hex4(P *p, unsigned *out) {
    if (p->len - p->i < 4) return -1;
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p->s[p->i++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static int parse_string(P *p, char **out, size_t *outlen) {
    Str s = {0};
    p->i++; /* opening quote */
    while (p->i < p->len) {
        char c = p->s[p->i++];
        if (c == '"') {
            *out = s.data ? s.data : xstrdup("");
            *outlen = s.len;
            return 0;
        }
        if ((unsigned char)c < 0x20) break;
        if (c != '\\') {
            str_putc(&s, c);
            continue;
        }
        if (p->i >= p->len) break;
        c = p->s[p->i++];
        if (c != 'u') {
            char d;
            switch (c) {
            case '"': case '\\': case '/': d = c; break;
            case 'b': d = '\b'; break;
            case 'f': d = '\f'; break;
            case 'n': d = '\n'; break;
            case 'r': d = '\r'; break;
            case 't': d = '\t'; break;
            default: goto fail;
            }
            str_putc(&s, d);
            continue;
        }
        unsigned cp, lo;
        if (hex4(p, &cp)) goto fail;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (p->len - p->i >= 6 && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                p->i += 2;
                if (hex4(p, &lo) || lo < 0xDC00 || lo > 0xDFFF) goto fail;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
                cp = UTF8_REPLACEMENT;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = UTF8_REPLACEMENT;
        }
        char enc[4];
        size_t n = utf8_encode(cp, enc);
        str_append(&s, enc, n);
    }
fail:
    str_free(&s);
    return -1;
}

static int parse_value(P *p, Json *j, int depth);

static int parse_container(P *p, Json *j, int depth, bool obj) {
    j->type = obj ? JSON_OBJ : JSON_ARR;
    p->i++;
    size_t cap = 0;
    ws(p);
    if (p->i < p->len && p->s[p->i] == (obj ? '}' : ']')) {
        p->i++;
        return 0;
    }
    for (;;) {
        if (j->n == cap) {
            cap = cap ? cap * 2 : 4;
            j->items = xreallocarray(j->items, cap, sizeof *j->items);
            if (obj) j->keys = xreallocarray(j->keys, cap, sizeof *j->keys);
        }
        ws(p);
        if (obj) {
            size_t kl;
            if (p->i >= p->len || p->s[p->i] != '"' ||
                parse_string(p, &j->keys[j->n], &kl))
                return -1;
            ws(p);
            if (p->i >= p->len || p->s[p->i++] != ':') {
                free(j->keys[j->n]);
                return -1;
            }
        }
        memset(&j->items[j->n], 0, sizeof *j->items);
        int rc = parse_value(p, &j->items[j->n], depth + 1);
        j->n++; /* count it so release() frees partial values */
        if (rc) return -1;
        ws(p);
        if (p->i >= p->len) return -1;
        char c = p->s[p->i++];
        if (c == ',') continue;
        if (c == (obj ? '}' : ']')) return 0;
        return -1;
    }
}

static int parse_value(P *p, Json *j, int depth) {
    if (depth > MAX_DEPTH) return -1;
    ws(p);
    if (p->i >= p->len) return -1;
    char c = p->s[p->i];
    if (c == '{' || c == '[') return parse_container(p, j, depth, c == '{');
    if (c == '"') {
        j->type = JSON_STR;
        return parse_string(p, &j->str, &j->len);
    }
    static const struct { const char *w; JsonType t; bool b; } lit[] = {
        {"true", JSON_BOOL, true}, {"false", JSON_BOOL, false}, {"null", JSON_NULL, false}};
    for (size_t k = 0; k < 3; k++) {
        size_t n = strlen(lit[k].w);
        if (p->len - p->i >= n && memcmp(p->s + p->i, lit[k].w, n) == 0) {
            j->type = lit[k].t;
            j->b = lit[k].b;
            p->i += n;
            return 0;
        }
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[64];
        size_t n = 0;
        while (p->i < p->len && n < sizeof buf - 1 &&
               strchr("0123456789+-.eE", p->s[p->i]) && p->s[p->i])
            buf[n++] = p->s[p->i++];
        buf[n] = '\0';
        char *end;
        j->type = JSON_NUM;
        j->num = strtod(buf, &end);
        return *end ? -1 : 0;
    }
    return -1;
}

Json *json_parse(const char *s, size_t len) {
    P p = {s, len, 0};
    Json *j = xcalloc(1, sizeof *j);
    if (parse_value(&p, j, 0) != 0) {
        json_free(j);
        return NULL;
    }
    ws(&p);
    if (p.i != len) {
        json_free(j);
        return NULL;
    }
    return j;
}

const Json *json_get(const Json *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJ) return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (strcmp(obj->keys[i], key) == 0) return &obj->items[i];
    return NULL;
}

const Json *json_at(const Json *arr, size_t i) {
    if (!arr || arr->type != JSON_ARR || i >= arr->n) return NULL;
    return &arr->items[i];
}

const char *json_str(const Json *j) {
    return j && j->type == JSON_STR ? j->str : NULL;
}

double json_num(const Json *j, double def) {
    return j && j->type == JSON_NUM ? j->num : def;
}

const Json *json_path(const Json *j, ...) {
    va_list ap;
    va_start(ap, j);
    const char *k;
    while (j && (k = va_arg(ap, const char *)) != NULL) j = json_get(j, k);
    va_end(ap);
    return j;
}

void json_quote(Str *out, const char *s, size_t n) {
    str_putc(out, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': str_appendz(out, "\\\""); break;
        case '\\': str_appendz(out, "\\\\"); break;
        case '\n': str_appendz(out, "\\n"); break;
        case '\r': str_appendz(out, "\\r"); break;
        case '\t': str_appendz(out, "\\t"); break;
        default:
            if (c < 0x20)
                str_appendf(out, "\\u%04x", c);
            else
                str_putc(out, (char)c);
        }
    }
    str_putc(out, '"');
}
