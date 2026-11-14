#include "cedit/buffer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cedit/utf8.h"
#include "cedit/util.h"

Buffer *buffer_new(const char *name) {
    Buffer *b = xcalloc(1, sizeof *b);
    if (text_init(&b->text) != 0) {
        free(b);
        return NULL;
    }
    undo_init(&b->undo);
    b->name = xstrdup(name);
    b->tab_width = 8;
    return b;
}

void buffer_free(Buffer *b) {
    if (!b) return;
    text_free(&b->text);
    undo_free(&b->undo);
    free(b->path);
    free(b->special_name);
    free(b->name);
    free(b->hl_state);
    buffer_clear_diags(b);
    free(b);
}

size_t buffer_len(const Buffer *b) { return text_len(&b->text); }

void buffer_clear_diags(Buffer *b) {
    for (size_t i = 0; i < b->ndiags; i++) free(b->diags[i].msg);
    free(b->diags);
    b->diags = NULL;
    b->ndiags = 0;
}

static void invalidate_hl(Buffer *b, size_t pos) {
    b->version++;
    size_t line = text_line_of(&b->text, pos);
    if (b->hl_valid > line + 1) b->hl_valid = line + 1;
}

static void apply_insert(Buffer *b, size_t pos, size_t n) {
    if (b->cursor >= pos) b->cursor += n;
    if (b->mark > pos) b->mark += n;
}

static void apply_delete(Buffer *b, size_t pos, size_t n) {
    if (b->cursor >= pos + n)
        b->cursor -= n;
    else if (b->cursor > pos)
        b->cursor = pos;
    if (b->mark >= pos + n)
        b->mark -= n;
    else if (b->mark > pos)
        b->mark = pos;
}

int buffer_insert(Buffer *b, size_t pos, const char *s, size_t n) {
    if (b->readonly) {
        errno = EROFS;
        return -1;
    }
    if (n == 0) return 0;
    invalidate_hl(b, pos);
    size_t before = b->cursor;
    if (text_insert(&b->text, pos, s, n) != 0) return -1;
    if (undo_record(&b->undo, true, pos, s, n, before) != 0) {
        text_delete(&b->text, pos, n); /* cannot fail: range is valid */
        errno = ENOMEM;
        return -1;
    }
    apply_insert(b, pos, n);
    b->goal_valid = false;
    return 0;
}

int buffer_delete(Buffer *b, size_t pos, size_t n) {
    if (b->readonly) {
        errno = EROFS;
        return -1;
    }
    size_t len = buffer_len(b);
    if (pos > len || n > len - pos) {
        errno = EINVAL;
        return -1;
    }
    if (n == 0) return 0;
    char *copy = text_dup(&b->text, pos, n);
    if (!copy) return -1;
    if (undo_record(&b->undo, false, pos, copy, n, b->cursor) != 0) {
        free(copy);
        return -1;
    }
    free(copy);
    invalidate_hl(b, pos);
    text_delete(&b->text, pos, n);
    apply_delete(b, pos, n);
    b->goal_valid = false;
    return 0;
}

int buffer_replace_all(Buffer *b, const char *s, size_t n) {
    size_t cur = b->cursor;
    if (buffer_delete(b, 0, buffer_len(b)) != 0) return -1;
    if (buffer_insert(b, 0, s, n) != 0) return -1;
    b->cursor = cur < n ? cur : n;
    return 0;
}

bool buffer_undo(Buffer *b) {
    Undo *u = &b->undo;
    if (u->cur == 0 || b->readonly) return false;
    unsigned long g = u->ops[u->cur - 1].group;
    while (u->cur > 0 && u->ops[u->cur - 1].group == g) {
        UndoOp *op = &u->ops[--u->cur];
        invalidate_hl(b, op->pos);
        if (op->insert) {
            text_delete(&b->text, op->pos, op->len);
            apply_delete(b, op->pos, op->len);
        } else if (text_insert(&b->text, op->pos, op->data, op->len) != 0) {
            u->cur++; /* out of memory: stop here, history stays consistent */
            return false;
        } else {
            apply_insert(b, op->pos, op->len);
        }
        b->cursor = op->cursor;
    }
    if (b->cursor > buffer_len(b)) b->cursor = buffer_len(b);
    b->mark_active = false;
    b->goal_valid = false;
    u->group++; /* never merge new edits into redone/undone ops */
    return true;
}

bool buffer_redo(Buffer *b) {
    Undo *u = &b->undo;
    if (u->cur == u->n || b->readonly) return false;
    unsigned long g = u->ops[u->cur].group;
    while (u->cur < u->n && u->ops[u->cur].group == g) {
        UndoOp *op = &u->ops[u->cur];
        invalidate_hl(b, op->pos);
        if (op->insert) {
            if (text_insert(&b->text, op->pos, op->data, op->len) != 0)
                return false;
            apply_insert(b, op->pos, op->len);
            b->cursor = op->pos + op->len;
        } else {
            text_delete(&b->text, op->pos, op->len);
            apply_delete(b, op->pos, op->len);
            b->cursor = op->pos;
        }
        u->cur++;
    }
    b->mark_active = false;
    b->goal_valid = false;
    u->group++;
    return true;
}

bool buffer_modified(const Buffer *b) { return undo_modified(&b->undo); }

size_t buffer_cur_line(const Buffer *b) {
    return text_line_of(&b->text, b->cursor);
}

size_t buffer_col_of(Buffer *b, size_t pos) {
    size_t line = text_line_of(&b->text, pos);
    size_t start = text_line_start(&b->text, line);
    const char *p = text_view(&b->text, start, pos - start);
    return display_width(p, pos - start, b->tab_width);
}

size_t buffer_pos_at_col(Buffer *b, size_t line, size_t col) {
    size_t start = text_line_start(&b->text, line);
    size_t end = text_line_end(&b->text, line);
    const char *p = text_view(&b->text, start, end - start);
    size_t len = end - start, i = 0, c = 0;
    Glyph g;
    while (i < len) {
        glyph_next(p + i, len - i, c, b->tab_width, &g);
        if (c + (size_t)g.width > col) break;
        c += (size_t)g.width;
        i += g.bytes;
        if (c == col) {
            /* include trailing combining marks of the reached glyph */
            while (i < len) {
                glyph_next(p + i, len - i, c, b->tab_width, &g);
                if (g.width != 0) break;
                i += g.bytes;
            }
            break;
        }
    }
    return start + i;
}

static bool combining_at(Buffer *b, size_t pos) {
    size_t len = buffer_len(b);
    if (pos >= len || text_byte(&b->text, pos) < 0x80) return false;
    size_t n = len - pos < 4 ? len - pos : 4;
    const char *p = text_view(&b->text, pos, n);
    uint32_t cp;
    bool valid;
    utf8_decode(p, n, &cp, &valid);
    return valid && cp_is_combining(cp);
}

size_t buffer_next_char(Buffer *b, size_t pos) {
    size_t len = buffer_len(b);
    if (pos >= len) return len;
    size_t n = len - pos < 4 ? len - pos : 4;
    const char *p = text_view(&b->text, pos, n);
    uint32_t cp;
    pos += utf8_decode(p, n, &cp, NULL);
    while (pos < len && combining_at(b, pos)) {
        n = len - pos < 4 ? len - pos : 4;
        p = text_view(&b->text, pos, n);
        pos += utf8_decode(p, n, &cp, NULL);
    }
    return pos;
}

static size_t prev_cp(Buffer *b, size_t pos) {
    size_t start = pos;
    for (int i = 0; i < 4 && start > 0; i++) {
        start--;
        if (!utf8_is_cont(text_byte(&b->text, start))) break;
    }
    const char *p = text_view(&b->text, start, pos - start);
    uint32_t cp;
    if (utf8_decode(p, pos - start, &cp, NULL) == pos - start) return start;
    return pos - 1;
}

size_t buffer_prev_char(Buffer *b, size_t pos) {
    if (pos == 0) return 0;
    if (pos > buffer_len(b)) return buffer_len(b);
    pos = prev_cp(b, pos);
    while (pos > 0 && combining_at(b, pos)) pos = prev_cp(b, pos);
    return pos;
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

int buffer_load(Buffer *b, const char *path, bool *is_new, char *err,
                size_t errlen) {
    char *data = NULL;
    size_t len = 0;
    FileStamp stamp = {0};
    *is_new = false;
    int e = file_read_all(path, &data, &len, &stamp, err, errlen);
    if (e == ENOENT) {
        *is_new = true;
        data = NULL;
        len = 0;
        stamp.valid = false;
    } else if (e) {
        return -1;
    }

    size_t nl = 0, crlf = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            nl++;
            if (i > 0 && data[i - 1] == '\r') crlf++;
        }
    }
    bool is_crlf = nl > 0 && crlf == nl;
    if (is_crlf) { /* strip the CR of every CRLF; exact round trip */
        size_t o = 0;
        for (size_t i = 0; i < len; i++) {
            if (data[i] == '\r' && i + 1 < len && data[i + 1] == '\n')
                continue;
            data[o++] = data[i];
        }
        len = o;
    }

    size_t first = len;
    const char *nlp = data ? memchr(data, '\n', len) : NULL;
    if (nlp) first = (size_t)(nlp - data);
    char shebang[128];
    size_t sl = first < sizeof shebang ? first : sizeof shebang - 1;
    if (sl) memcpy(shebang, data, sl);
    shebang[sl] = '\0';
    int rc = data ? text_take(&b->text, data, len) : text_set(&b->text, "", 0);
    if (rc != 0) {
        free(data);
        snprintf(err, errlen, "Out of memory loading file");
        return -1;
    }
    b->syntax = syntax_detect(path, shebang, sl);

    undo_clear(&b->undo);
    free(b->path);
    b->path = xstrdup(path);
    free(b->name);
    b->name = xstrdup(base_name(path));
    b->crlf = is_crlf;
    b->stamp = stamp;
    b->disk_changed_warned = false;
    b->cursor = b->mark = 0;
    b->mark_active = false;
    b->top = b->left = 0;
    b->hl_valid = 0;
    b->goal_valid = false;
    b->version++;
    return 0;
}

int buffer_save(Buffer *b, const char *path, bool *in_place, char *err,
                size_t errlen) {
    const char *a, *c;
    size_t alen, clen;
    FileStamp stamp = {0};
    text_segments(&b->text, &a, &alen, &c, &clen);
    if (file_write(path, a, alen, c, clen, b->crlf, &stamp, in_place, err,
                   errlen) != 0)
        return -1;
    if (!b->path || strcmp(b->path, path) != 0) {
        char *np = xstrdup(path);
        free(b->path);
        b->path = np;
    }
    b->stamp = stamp;
    b->disk_changed_warned = false;
    undo_mark_saved(&b->undo);
    return 0;
}

bool buffer_disk_changed(const Buffer *b) {
    if (!b->path) return false;
    FileStamp now;
    file_stamp(b->path, &now);
    if (!b->stamp.valid) return now.valid; /* new file created elsewhere */
    return !file_stamp_equal(&b->stamp, &now);
}

uint8_t buffer_hl_state(Buffer *b, size_t line) {
    if (!b->syntax) return 0;
    if (line >= b->hl_cap) {
        size_t cap = b->hl_cap ? b->hl_cap : 256;
        while (cap <= line) cap *= 2;
        b->hl_state = xrealloc(b->hl_state, cap);
        b->hl_cap = cap;
    }
    if (b->hl_valid == 0) {
        b->hl_state[0] = 0;
        b->hl_valid = 1;
    }
    while (b->hl_valid <= line) {
        size_t l = b->hl_valid - 1;
        size_t s = text_line_start(&b->text, l);
        size_t e = text_line_end(&b->text, l);
        const char *p = text_view(&b->text, s, e - s);
        b->hl_state[b->hl_valid++] =
            syntax_highlight(b->syntax, p, e - s, b->hl_state[l], NULL);
    }
    return b->hl_state[line];
}

void buffer_set_syntax(Buffer *b, const Syntax *s) {
    b->syntax = s;
    b->hl_valid = 0;
}

bool buffer_region(const Buffer *b, size_t *start, size_t *end) {
    if (!b->mark_active || b->mark == b->cursor) return false;
    size_t len = buffer_len(b);
    size_t m = b->mark > len ? len : b->mark;
    *start = m < b->cursor ? m : b->cursor;
    *end = m < b->cursor ? b->cursor : m;
    return true;
}
