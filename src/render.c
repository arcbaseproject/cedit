#include "cedit/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cedit/editor.h"
#include "cedit/lsp.h"
#include "cedit/utf8.h"

/* ---- themes ---- */

static const char *const slot_names[S_COUNT] = {
    "normal", "comment", "keyword", "type",      "string",  "number",
    "operator", "preproc", "linenr", "curlinenr", "status", "selection",
    "match",  "curmatch", "special", "error",    "filler", "diag-error",
    "diag-warning", "popup", "popup-selected",
};

int style_slot(const char *name) {
    for (int i = 0; i < S_COUNT; i++)
        if (strcmp(slot_names[i], name) == 0) return i;
    return -1;
}

const char *theme_names(void) { return "dark, light, mono"; }

#define ST(f, b, a) ((Style){(f), (b), (a)})

bool theme_builtin(Style s[S_COUNT], const char *name) {
    for (int i = 0; i < S_COUNT; i++) s[i] = ST(-1, -1, 0);
    if (strcmp(name, "dark") == 0) {
        s[HL_COMMENT] = ST(0x7f848e, -1, A_ITALIC);
        s[HL_KEYWORD] = ST(0xc678dd, -1, A_BOLD);
        s[HL_TYPE] = ST(0xe5c07b, -1, 0);
        s[HL_STRING] = ST(0x98c379, -1, 0);
        s[HL_NUMBER] = ST(0xd19a66, -1, 0);
        s[HL_OPERATOR] = ST(0x56b6c2, -1, 0);
        s[HL_PREPROC] = ST(0xe06c75, -1, 0);
        s[S_LINENR] = ST(0x5c6370, -1, 0);
        s[S_CURLINENR] = ST(0xabb2bf, -1, A_BOLD);
        s[S_STATUS] = ST(0x282c34, 0x61afef, 0);
        s[S_SELECTION] = ST(-1, 0x3e4451, 0);
        s[S_MATCH] = ST(0x000000, 0xe5c07b, 0);
        s[S_CURMATCH] = ST(0x000000, 0xff8800, A_BOLD);
        s[S_SPECIAL] = ST(0xff5555, -1, A_BOLD);
        s[S_ERROR] = ST(0xff5555, -1, A_BOLD);
        s[S_FILLER] = ST(0x5c6370, -1, 0);
        s[S_DIAG_ERROR] = ST(0xff5555, -1, A_UNDERLINE);
        s[S_DIAG_WARN] = ST(0xe5c07b, -1, A_UNDERLINE);
        s[S_POPUP] = ST(0xabb2bf, 0x2c313a, 0);
        s[S_POPUP_SEL] = ST(0x282c34, 0x61afef, A_BOLD);
    } else if (strcmp(name, "light") == 0) {
        s[HL_COMMENT] = ST(0x8e908c, -1, A_ITALIC);
        s[HL_KEYWORD] = ST(0x8959a8, -1, A_BOLD);
        s[HL_TYPE] = ST(0xc18401, -1, 0);
        s[HL_STRING] = ST(0x50a14f, -1, 0);
        s[HL_NUMBER] = ST(0x986801, -1, 0);
        s[HL_OPERATOR] = ST(0x0184bc, -1, 0);
        s[HL_PREPROC] = ST(0xe45649, -1, 0);
        s[S_LINENR] = ST(0x9d9d9f, -1, 0);
        s[S_CURLINENR] = ST(0x383a42, -1, A_BOLD);
        s[S_STATUS] = ST(0xfafafa, 0x4078f2, 0);
        s[S_SELECTION] = ST(-1, 0xd0d8ff, 0);
        s[S_MATCH] = ST(0x000000, 0xffe066, 0);
        s[S_CURMATCH] = ST(0x000000, 0xff9933, A_BOLD);
        s[S_SPECIAL] = ST(0xe45649, -1, A_BOLD);
        s[S_ERROR] = ST(0xe45649, -1, A_BOLD);
        s[S_FILLER] = ST(0x9d9d9f, -1, 0);
        s[S_DIAG_ERROR] = ST(0xe45649, -1, A_UNDERLINE);
        s[S_DIAG_WARN] = ST(0xc18401, -1, A_UNDERLINE);
        s[S_POPUP] = ST(0x383a42, 0xe5e5e6, 0);
        s[S_POPUP_SEL] = ST(0xfafafa, 0x4078f2, A_BOLD);
    } else if (strcmp(name, "mono") == 0) {
        s[HL_COMMENT].attr = A_ITALIC;
        s[HL_KEYWORD].attr = A_BOLD;
        s[S_CURLINENR].attr = A_BOLD;
        s[S_STATUS].attr = A_REVERSE;
        s[S_SELECTION].attr = A_REVERSE;
        s[S_MATCH].attr = A_UNDERLINE;
        s[S_CURMATCH].attr = A_REVERSE | A_BOLD;
        s[S_SPECIAL].attr = A_REVERSE;
        s[S_ERROR].attr = A_BOLD;
        s[S_DIAG_ERROR].attr = A_UNDERLINE | A_BOLD;
        s[S_DIAG_WARN].attr = A_UNDERLINE;
        s[S_POPUP].attr = A_REVERSE;
        s[S_POPUP_SEL].attr = A_BOLD;
    } else {
        return false;
    }
    return true;
}

/* ---- color output ---- */

static int to_256(int32_t rgb) {
    int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    if (r == g && g == b) { /* grayscale ramp */
        if (r < 8) return 16;
        if (r > 248) return 231;
        return 232 + (r - 8) * 24 / 240;
    }
    int ri = (r * 5 + 127) / 255, gi = (g * 5 + 127) / 255,
        bi = (b * 5 + 127) / 255;
    return 16 + 36 * ri + 6 * gi + bi;
}

static int to_16(int32_t rgb) {
    static const int32_t pal[16] = {
        0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd,
        0x00cdcd, 0xe5e5e5, 0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
        0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff};
    int best = 0;
    long bd = -1;
    for (int i = 0; i < 16; i++) {
        long dr = ((rgb >> 16) & 0xff) - ((pal[i] >> 16) & 0xff);
        long dg = ((rgb >> 8) & 0xff) - ((pal[i] >> 8) & 0xff);
        long db = (rgb & 0xff) - (pal[i] & 0xff);
        long d = dr * dr + dg * dg + db * db;
        if (bd < 0 || d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static void color_sgr(Str *s, ColorMode mode, int32_t rgb, bool bg) {
    if (rgb < 0) return;
    if (mode == COLOR_TRUE)
        str_appendf(s, ";%d;2;%d;%d;%d", bg ? 48 : 38, (int)(rgb >> 16) & 0xff,
                    (int)(rgb >> 8) & 0xff, (int)rgb & 0xff);
    else if (mode == COLOR_256)
        str_appendf(s, ";%d;5;%d", bg ? 48 : 38, to_256(rgb));
    else {
        int c = to_16(rgb);
        str_appendf(s, ";%d", (c < 8 ? (bg ? 40 : 30) : (bg ? 100 : 90)) +
                                  (c & 7));
    }
}

static void style_sgr(const Editor *e, int slot, Str *s) {
    Style st = e->styles[slot];
    ColorMode mode = e->color;
    if (e->cfg.colors >= 0) mode = (ColorMode)e->cfg.colors;
    /* with 16 colors, background tints are unreliable: use reverse */
    if (mode == COLOR_16 && st.bg >= 0 &&
        (slot == S_SELECTION || slot == S_STATUS || slot == S_MATCH ||
         slot == S_CURMATCH || slot == S_POPUP || slot == S_POPUP_SEL)) {
        st.fg = -1;
        st.bg = -1;
        st.attr |= A_REVERSE;
    }
    str_appendz(s, "\x1b[0");
    if (st.attr & A_BOLD) str_appendz(s, ";1");
    if (st.attr & A_ITALIC) str_appendz(s, ";3");
    if (st.attr & A_UNDERLINE) str_appendz(s, ";4");
    if (st.attr & A_REVERSE) str_appendz(s, ";7");
    color_sgr(s, mode, st.fg, false);
    color_sgr(s, mode, st.bg, true);
    str_putc(s, 'm');
}

/* ---- layout ---- */

int render_text_rows(const Editor *e) {
    return e->rows > 3 ? e->rows - 2 : 1;
}

static int gutter_width(const Editor *e, const Buffer *b) {
    if (e->cfg.line_numbers == LINENR_OFF) return 0;
    size_t n = text_lines(&b->text);
    int d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    if (d < 3) d = 3;
    return d + 1;
}

static int text_width(const Editor *e, const Buffer *b) {
    int w = e->cols - gutter_width(e, b);
    return w < 1 ? 1 : w;
}

void render_scroll(Editor *e) {
    Buffer *b = editor_buf(e);
    size_t rows = (size_t)render_text_rows(e);
    size_t line = buffer_cur_line(b);
    size_t n = text_lines(&b->text);
    if (b->top >= n) b->top = n - 1;
    if (line < b->top) b->top = line;
    if (line >= b->top + rows) b->top = line - rows + 1;

    size_t w = (size_t)text_width(e, b);
    size_t col = buffer_col_of(b, b->cursor);
    if (col < b->left) b->left = col;
    if (col >= b->left + w) b->left = col - w + 1;
}

void render_invalidate(Editor *e) {
    for (int i = 0; i < e->rows_prev_n; i++) str_free(&e->rows_prev[i]);
    free(e->rows_prev);
    e->rows_prev = NULL;
    e->rows_prev_n = 0;
}

void render_free(Editor *e) {
    render_invalidate(e);
    str_free(&e->frame);
    free(e->hl_scratch);
    e->hl_scratch = NULL;
}

static uint8_t *hl_buf(Editor *e, size_t len) {
    if (len + 1 > e->hl_scratch_cap) {
        size_t cap = e->hl_scratch_cap ? e->hl_scratch_cap : 256;
        while (cap < len + 1) cap *= 2;
        e->hl_scratch = xrealloc(e->hl_scratch, cap);
        e->hl_scratch_cap = cap;
    }
    return e->hl_scratch;
}

static void render_line(Editor *e, Buffer *b, size_t line, size_t cur_line,
                        Str *row) {
    int gw = gutter_width(e, b);
    size_t tw = (size_t)text_width(e, b);
    if (gw) {
        size_t num = line + 1;
        if (e->cfg.line_numbers == LINENR_RELATIVE && line != cur_line)
            num = line > cur_line ? line - cur_line : cur_line - line;
        int worst = 0; /* most severe diagnostic on the line */
        for (size_t i = 0; i < b->ndiags; i++)
            if (b->diags[i].line == line &&
                (!worst || b->diags[i].severity < worst))
                worst = b->diags[i].severity;
        style_sgr(e, worst == 1   ? S_ERROR
                     : worst == 2 ? S_DIAG_WARN
                     : line == cur_line ? S_CURLINENR : S_LINENR, row);
        str_appendf(row, "%*zu ", gw - 1, num);
    }
    size_t s = text_line_start(&b->text, line);
    size_t end = text_line_end(&b->text, line);
    size_t len = end - s;
    uint8_t state = buffer_hl_state(b, line); /* may move the gap */
    uint8_t *hl = hl_buf(e, len);
    const char *p = text_view(&b->text, s, len);
    syntax_highlight(b->syntax, p, len, state, hl);

    for (size_t i = 0; i < b->ndiags; i++) {
        const Diagnostic *d = &b->diags[i];
        if (d->line != line || d->severity > 2 || len == 0) continue;
        size_t a = d->start < len ? d->start : len - 1;
        size_t z = d->end < len ? d->end : len;
        if (z <= a) z = a + 1; /* zero-width range: mark one cell */
        memset(hl + a, d->severity == 1 ? S_DIAG_ERROR : S_DIAG_WARN, z - a);
    }
    if (e->hl_search) {
        size_t from = 0, ms, me;
        while (from <= len &&
               search_in_line(e->hl_search, p, len, from, &ms, &me)) {
            bool cur = e->hl_have_cur && s + ms == e->hl_cur_start &&
                       s + me == e->hl_cur_end;
            memset(hl + ms, cur ? S_CURMATCH : S_MATCH, me - ms);
            from = me > ms ? me : ms + 1;
        }
    }
    size_t rs, re;
    bool region = buffer_region(b, &rs, &re);
    if (region && rs < end && re > s) {
        size_t a = rs > s ? rs - s : 0, z = re < end ? re - s : len;
        memset(hl + a, S_SELECTION, z - a);
    }

    int cur_style = -1;
    size_t col = 0, i = 0;
    Glyph g;
    while (i < len) {
        glyph_next(p + i, len - i, col, b->tab_width, &g);
        int slot = hl[i];
        if (g.special && slot < S_LINENR) slot = S_SPECIAL;
        size_t w = (size_t)g.width;
        if (col + w <= b->left && !(w == 0 && col == b->left)) {
            col += w;
            i += g.bytes;
            continue;
        }
        if (col < b->left) { /* glyph cut by the left edge */
            if (slot != cur_style) style_sgr(e, cur_style = slot, row);
            for (size_t k = b->left; k < col + w; k++) str_putc(row, ' ');
            col += w;
            i += g.bytes;
            continue;
        }
        size_t vis = col - b->left;
        bool last = i + g.bytes >= len;
        if (vis + w > tw - 1 && !(last && vis + w <= tw)) {
            if (slot != cur_style) style_sgr(e, cur_style = slot, row);
            for (; vis < tw - 1; vis++) str_putc(row, ' ');
            style_sgr(e, cur_style = S_FILLER, row);
            str_putc(row, '>');
            col = SIZE_MAX;
            break;
        }
        if (slot != cur_style) style_sgr(e, cur_style = slot, row);
        str_append(row, g.out, g.outlen);
        col += w;
        i += g.bytes;
    }
    /* show a selected line break as one highlighted cell */
    if (col != SIZE_MAX && region && rs <= end && re > end &&
        col >= b->left && col - b->left < tw) {
        style_sgr(e, S_SELECTION, row);
        str_putc(row, ' ');
    }
    style_sgr(e, HL_NORMAL, row);
    str_appendz(row, "\x1b[K");
}

static void render_status(Editor *e, Buffer *b, Str *row) {
    size_t n = text_lines(&b->text), rows = (size_t)render_text_rows(e);
    size_t line = buffer_cur_line(b);
    size_t col = buffer_col_of(b, b->cursor);
    char pos[32];
    if (b->top == 0 && n <= rows)
        snprintf(pos, sizeof pos, "All");
    else if (b->top == 0)
        snprintf(pos, sizeof pos, "Top");
    else if (b->top + rows >= n)
        snprintf(pos, sizeof pos, "Bot");
    else
        snprintf(pos, sizeof pos, "%zu%%", b->top * 100 / (n - rows));

    char name[256], left[320], right[320];
    sanitize_for_display(b->name, name, sizeof name);
    snprintf(left, sizeof left, " %s%s%s%s", name,
             buffer_modified(b) ? " [+]" : "", b->readonly ? " [RO]" : "",
             b->disk_changed_warned ? " [changed on disk]" : "");
    char hscroll[32] = "";
    if (b->left) snprintf(hscroll, sizeof hscroll, "<+%zu  ", b->left);
    size_t nerr = 0, nwarn = 0;
    for (size_t i = 0; i < b->ndiags; i++) {
        nerr += b->diags[i].severity == 1;
        nwarn += b->diags[i].severity == 2;
    }
    char diag[64] = "", srv[80] = "";
    if (nerr || nwarn) snprintf(diag, sizeof diag, "E%zu W%zu  ", nerr, nwarn);
    const char *ls = lsp_status(e, b);
    if (*ls) snprintf(srv, sizeof srv, "%s  ", ls);
    snprintf(right, sizeof right, "%s%s%sLn %zu, Col %zu  %s  %s  %s ", diag,
             srv, hscroll, line + 1, col + 1, syntax_name(b->syntax),
             b->crlf ? "CRLF" : "LF",
             pos);

    size_t cols = (size_t)e->cols;
    size_t rw = strlen(right);
    style_sgr(e, S_STATUS, row);
    if (rw >= cols) {
        str_append(row, right, cols);
    } else {
        size_t room = cols - rw, used = 0, i = 0, llen = strlen(left);
        Glyph g;
        while (i < llen) {
            glyph_next(left + i, llen - i, used, 1, &g);
            if (used + (size_t)g.width > room) break;
            str_append(row, g.out, g.outlen);
            used += (size_t)g.width;
            i += g.bytes;
        }
        for (; used < room; used++) str_putc(row, ' ');
        str_appendz(row, right);
    }
    style_sgr(e, HL_NORMAL, row);
    str_appendz(row, "\x1b[K");
}

/* Message line; returns the cursor column when the minibuffer is active. */
static size_t render_message(Editor *e, Str *row) {
    size_t cols = (size_t)e->cols, cursor_col = 0;
    char prompt[256];
    if (e->mb_active) {
        sanitize_for_display(e->mb_prompt, prompt, sizeof prompt);
        size_t pw = display_width(prompt, strlen(prompt), 1);
        const char *t = str_cstr(&e->mb_text);
        size_t tlen = e->mb_text.len;
        /* scroll so the cursor fits */
        size_t start = 0;
        while (start < e->mb_cursor &&
               pw + display_width(t + start, e->mb_cursor - start, 1) + 1 >
                   cols) {
            start++;
            while (start < tlen && utf8_is_cont((unsigned char)t[start]))
                start++;
        }
        style_sgr(e, HL_NORMAL, row);
        str_appendz(row, prompt);
        size_t used = pw, i = start;
        Glyph g;
        while (i < tlen) {
            glyph_next(t + i, tlen - i, used, 1, &g);
            if (used + (size_t)g.width >= cols) break;
            if (g.special) style_sgr(e, S_SPECIAL, row);
            str_append(row, g.out, g.outlen);
            if (g.special) style_sgr(e, HL_NORMAL, row);
            if (i < e->mb_cursor) cursor_col = used + (size_t)g.width;
            used += (size_t)g.width;
            i += g.bytes;
        }
        if (e->mb_cursor == start) cursor_col = pw;
        if (e->msg[0]) { /* completion candidates etc. */
            char msg[512];
            sanitize_for_display(e->msg, msg, sizeof msg);
            size_t mw = display_width(msg, strlen(msg), 1);
            if (used + 3 + mw < cols) {
                style_sgr(e, S_FILLER, row);
                str_appendf(row, "  [%s]", msg);
            }
        }
    } else {
        char msg[512];
        int slot = e->msg_error ? S_ERROR : HL_NORMAL;
        if (e->msg[0]) {
            sanitize_for_display(e->msg, msg, sizeof msg);
        } else { /* show the diagnostic under the cursor line */
            Buffer *b = editor_buf(e);
            size_t line = buffer_cur_line(b);
            const Diagnostic *d = NULL;
            for (size_t i = 0; i < b->ndiags; i++)
                if (b->diags[i].line == line &&
                    (!d || b->diags[i].severity < d->severity))
                    d = &b->diags[i];
            char raw[512];
            snprintf(raw, sizeof raw, "%s%s",
                     !d ? "" : d->severity == 1 ? "error: "
                             : d->severity == 2 ? "warning: " : "note: ",
                     d ? d->msg : "");
            for (char *c = raw; *c; c++) if (*c == '\n') *c = ' ';
            sanitize_for_display(raw, msg, sizeof msg);
            slot = d && d->severity == 1 ? S_ERROR : S_FILLER;
        }
        style_sgr(e, slot, row);
        size_t used = 0, i = 0, len = strlen(msg);
        Glyph g;
        while (i < len) {
            glyph_next(msg + i, len - i, used, 1, &g);
            if (used + (size_t)g.width > cols) break;
            str_append(row, g.out, g.outlen);
            used += (size_t)g.width;
            i += g.bytes;
        }
    }
    style_sgr(e, HL_NORMAL, row);
    str_appendz(row, "\x1b[K");
    return cursor_col;
}

void render_frame(Editor *e) {
    Buffer *b = editor_buf(e);
    render_scroll(e);
    int rows = e->rows;
    bool full = e->rows_prev_n != rows;
    if (full) {
        render_invalidate(e);
        e->rows_prev = xcalloc((size_t)rows, sizeof *e->rows_prev);
        e->rows_prev_n = rows;
    }
    Str *out = &e->frame;
    str_clear(out);
    str_appendz(out, "\x1b[?2026h\x1b[?25l");
    if (full) str_appendz(out, "\x1b[0m\x1b[2J");

    int trows = render_text_rows(e);
    size_t n = text_lines(&b->text), cur_line = buffer_cur_line(b);
    Str row = {0};

    /* completion popup geometry */
    int pop_r0 = -1, pop_h = 0, pop_c = 0, pop_w = 0;
    if (e->popup_active && e->popup_n) {
        size_t al = text_line_of(&b->text, e->popup_anchor);
        int ar = (int)(al - b->top);
        size_t acol = buffer_col_of(b, e->popup_anchor);
        int ac = gutter_width(e, b) + (acol >= b->left ? (int)(acol - b->left) : 0);
        pop_h = (int)e->popup_n < 10 ? (int)e->popup_n : 10;
        for (size_t i = 0; i < e->popup_n; i++) {
            int w = (int)display_width(e->popup[i], strlen(e->popup[i]), 1) + 2;
            if (w > pop_w) pop_w = w;
        }
        bool text_box = e->popup_sel == SIZE_MAX; /* hover text */
        if (pop_w > (text_box ? 100 : 60)) pop_w = text_box ? 100 : 60;
        if (pop_w > e->cols) pop_w = e->cols;
        if (text_box && pop_h < (int)e->popup_n && (int)e->popup_n <= trows - 1)
            pop_h = (int)e->popup_n;
        pop_c = ac + pop_w > e->cols ? e->cols - pop_w : ac;
        if (ar + 1 + pop_h <= trows) pop_r0 = ar + 1;
        else if (ar - pop_h >= 0) pop_r0 = ar - pop_h;
        else { pop_r0 = 0; pop_h = pop_h < trows ? pop_h : trows; }
        if (!text_box && e->popup_sel < e->popup_top) e->popup_top = e->popup_sel;
        if (!text_box && e->popup_sel >= e->popup_top + (size_t)pop_h)
            e->popup_top = e->popup_sel - (size_t)pop_h + 1;
    }
    for (int r = 0; r < rows; r++) {
        str_clear(&row);
        if (r < trows) {
            size_t line = b->top + (size_t)r;
            if (line < n) {
                render_line(e, b, line, cur_line, &row);
            } else {
                style_sgr(e, S_FILLER, &row);
                str_putc(&row, '~');
                style_sgr(e, HL_NORMAL, &row);
                str_appendz(&row, "\x1b[K");
            }
            if (pop_r0 >= 0 && r >= pop_r0 && r < pop_r0 + pop_h) {
                size_t idx = e->popup_top + (size_t)(r - pop_r0);
                const char *item = idx < e->popup_n ? e->popup[idx] : "";
                style_sgr(e, idx == e->popup_sel ? S_POPUP_SEL : S_POPUP, &row);
                str_appendf(&row, "\x1b[%d;%dH ", r + 1, pop_c + 1);
                int used = 1;
                size_t i = 0, len = strlen(item);
                Glyph g;
                while (i < len) {
                    glyph_next(item + i, len - i, (size_t)used, 1, &g);
                    if (used + g.width > pop_w - 1) break;
                    str_append(&row, g.out, g.outlen);
                    used += g.width;
                    i += g.bytes;
                }
                for (; used < pop_w; used++) str_putc(&row, ' ');
                style_sgr(e, HL_NORMAL, &row);
            }
        } else if (r == rows - 2) {
            render_status(e, b, &row);
        } else if (r == rows - 1) {
            render_message(e, &row);
        } else {
            continue;
        }
        Str *prev = &e->rows_prev[r];
        if (!full && prev->len == row.len &&
            memcmp(str_cstr(prev), str_cstr(&row), row.len) == 0)
            continue;
        str_appendf(out, "\x1b[%d;1H", r + 1);
        str_append(out, str_cstr(&row), row.len);
        str_clear(prev);
        str_append(prev, str_cstr(&row), row.len);
    }
    str_free(&row);

    size_t crow, ccol;
    if (e->mb_active) {
        Str tmp = {0};
        ccol = render_message(e, &tmp);
        str_free(&tmp);
        crow = (size_t)rows - 1;
    } else {
        crow = buffer_cur_line(b) - b->top;
        ccol = (size_t)gutter_width(e, b) + buffer_col_of(b, b->cursor) - b->left;
    }
    if (ccol >= (size_t)e->cols) ccol = (size_t)e->cols - 1;
    str_appendf(out, "\x1b[%zu;%zuH\x1b[?25h\x1b[?2026l", crow + 1, ccol + 1);
    if (!e->headless) term_write(out->data, out->len);
}
