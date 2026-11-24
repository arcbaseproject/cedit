/* Command registry, default keymaps, and movement/editing commands. */
#include "cedit/commands.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "cedit/editor.h"
#include "cedit/lsp.h"
#include "cedit/script.h"
#include "cedit/utf8.h"

/* ---- registry ---- */

void command_register(Editor *e, const char *name, CommandFn fn,
                      const char *doc, unsigned flags, int script_ref) {
    for (size_t i = 0; i < e->ncmds; i++) {
        if (strcmp(e->cmds[i].name, name) == 0) {
            e->cmds[i].fn = fn;
            e->cmds[i].doc = doc;
            e->cmds[i].flags = flags;
            e->cmds[i].script_ref = script_ref;
            return;
        }
    }
    if (e->ncmds == e->cmdcap) {
        /* Pointers into cmds (this_cmd/last_cmd) must stay valid: grow
         * only during startup; scripts registering later reuse slack. */
        e->cmdcap = e->cmdcap ? e->cmdcap * 2 : 128;
        const Command *base = e->cmds;
        e->cmds = xreallocarray(e->cmds, e->cmdcap, sizeof *e->cmds);
        if (e->this_cmd) e->this_cmd = e->cmds + (e->this_cmd - base);
        if (e->last_cmd) e->last_cmd = e->cmds + (e->last_cmd - base);
    }
    e->cmds[e->ncmds++] = (Command){
        .name = xstrdup(name),
        .fn = fn,
        .doc = doc,
        .flags = flags,
        .script_ref = script_ref,
    };
}

const Command *command_find(const Editor *e, const char *name) {
    for (size_t i = 0; i < e->ncmds; i++)
        if (strcmp(e->cmds[i].name, name) == 0) return &e->cmds[i];
    return NULL;
}

bool command_run(Editor *e, const char *name) {
    const Command *c = command_find(e, name);
    if (!c) return false;
    editor_run_command(e, c, false);
    return true;
}

void commands_free(Editor *e) {
    for (size_t i = 0; i < e->ncmds; i++) free(e->cmds[i].name);
    free(e->cmds);
    e->cmds = NULL;
    e->ncmds = e->cmdcap = 0;
}

/* ---- helpers ---- */

#define BUF(e) editor_buf(e)

static void report_edit_error(Editor *e) {
    if (errno == EROFS)
        editor_error(e, "Buffer is read-only");
    else
        editor_error(e, "Edit failed: %s", strerror(errno));
}

bool insert_text(Editor *e, const char *s, size_t n) {
    Buffer *b = BUF(e);
    if (buffer_insert(b, b->cursor, s, n) != 0) {
        report_edit_error(e);
        return false;
    }
    return true;
}

static bool delete_range(Editor *e, size_t start, size_t end) {
    if (end <= start) return true;
    if (buffer_delete(BUF(e), start, end - start) != 0) {
        report_edit_error(e);
        return false;
    }
    return true;
}

/* Delete the active region (delete-selection behaviour). */
static bool delete_region_if_active(Editor *e) {
    Buffer *b = BUF(e);
    size_t s, t;
    if (!buffer_region(b, &s, &t)) {
        b->mark_active = false;
        return false;
    }
    delete_range(e, s, t);
    b->mark_active = false;
    b->shift_selection = false;
    return true;
}

static size_t line_start_of(Buffer *b, size_t pos) {
    return text_line_start(&b->text, text_line_of(&b->text, pos));
}

static size_t line_end_of(Buffer *b, size_t pos) {
    return text_line_end(&b->text, text_line_of(&b->text, pos));
}

static size_t indent_end(Buffer *b, size_t start) {
    size_t len = buffer_len(b);
    while (start < len) {
        unsigned char c = text_byte(&b->text, start);
        if (c != ' ' && c != '\t') break;
        start++;
    }
    return start;
}

static bool is_word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

static size_t word_forward(Buffer *b, size_t p) {
    size_t len = buffer_len(b);
    while (p < len && !is_word_byte(text_byte(&b->text, p))) p++;
    while (p < len && is_word_byte(text_byte(&b->text, p))) p++;
    return p;
}

static size_t word_backward(Buffer *b, size_t p) {
    while (p > 0 && !is_word_byte(text_byte(&b->text, p - 1))) p--;
    while (p > 0 && is_word_byte(text_byte(&b->text, p - 1))) p--;
    return p;
}

static void kill_save(Editor *e, size_t start, size_t end, bool prepend) {
    Buffer *b = BUF(e);
    char *s = text_dup(&b->text, start, end - start);
    if (!s) {
        editor_error(e, "Out of memory");
        return;
    }
    if (!e->last_kill) str_clear(&e->kill);
    if (prepend && e->kill.len) {
        Str t = {0};
        str_appendz(&t, s);
        str_append(&t, e->kill.data, e->kill.len);
        str_free(&e->kill);
        e->kill = t;
    } else {
        str_append(&e->kill, s, end - start);
    }
    free(s);
    e->this_kill = true;
}

static void indent_unit(Buffer *b, Str *out) {
    if (b->expand_tabs) {
        for (int i = 0; i < b->tab_width; i++) str_putc(out, ' ');
    } else {
        str_putc(out, '\t');
    }
}

/* ---- movement ---- */

static void cmd_forward_char(Editor *e) {
    Buffer *b = BUF(e);
    b->cursor = buffer_next_char(b, b->cursor);
}

static void cmd_backward_char(Editor *e) {
    Buffer *b = BUF(e);
    b->cursor = buffer_prev_char(b, b->cursor);
}

static void move_lines(Editor *e, long delta) {
    Buffer *b = BUF(e);
    size_t line = buffer_cur_line(b), n = text_lines(&b->text);
    if (!b->goal_valid) {
        b->goal_col = buffer_col_of(b, b->cursor);
        b->goal_valid = true;
    }
    size_t target;
    if (delta < 0)
        target = (size_t)-delta > line ? 0 : line - (size_t)-delta;
    else
        target = (size_t)delta >= n - line ? n - 1 : line + (size_t)delta;
    b->cursor = buffer_pos_at_col(b, target, b->goal_col);
}

static void cmd_next_line(Editor *e) { move_lines(e, 1); }
static void cmd_previous_line(Editor *e) { move_lines(e, -1); }

static void cmd_page_down(Editor *e) {
    Buffer *b = BUF(e);
    long page = render_text_rows(e) - 2;
    if (page < 1) page = 1;
    move_lines(e, page);
    b->top += (size_t)page;
    size_t n = text_lines(&b->text);
    if (b->top >= n) b->top = n - 1;
}

static void cmd_page_up(Editor *e) {
    Buffer *b = BUF(e);
    long page = render_text_rows(e) - 2;
    if (page < 1) page = 1;
    move_lines(e, -page);
    b->top = b->top > (size_t)page ? b->top - (size_t)page : 0;
}

static void cmd_beginning_of_line(Editor *e) {
    Buffer *b = BUF(e);
    size_t start = line_start_of(b, b->cursor);
    size_t ind = indent_end(b, start);
    if (ind > line_end_of(b, b->cursor)) ind = line_end_of(b, b->cursor);
    b->cursor = b->cursor == ind ? start : ind;
}

static void cmd_end_of_line(Editor *e) {
    Buffer *b = BUF(e);
    size_t start = line_start_of(b, b->cursor);
    size_t end = line_end_of(b, b->cursor);
    size_t last = end; /* end of the last non-blank character */
    while (last > start) {
        unsigned char c = text_byte(&b->text, last - 1);
        if (c != ' ' && c != '\t' && c != '\r') break;
        last--;
    }
    b->cursor = (b->cursor == end && last != end) ? last : end;
}

static void cmd_forward_word(Editor *e) {
    Buffer *b = BUF(e);
    b->cursor = word_forward(b, b->cursor);
}

static void cmd_backward_word(Editor *e) {
    Buffer *b = BUF(e);
    b->cursor = word_backward(b, b->cursor);
}

static void cmd_beginning_of_buffer(Editor *e) { BUF(e)->cursor = 0; }

static void cmd_end_of_buffer(Editor *e) {
    Buffer *b = BUF(e);
    b->cursor = buffer_len(b);
}

static void cmd_recenter(Editor *e) {
    Buffer *b = BUF(e);
    size_t line = buffer_cur_line(b), half = (size_t)render_text_rows(e) / 2;
    b->top = line > half ? line - half : 0;
    render_invalidate(e);
}

static void cmd_goto_matching_bracket(Editor *e) {
    Buffer *b = BUF(e);
    size_t len = buffer_len(b), p = b->cursor;
    static const char pairs[] = "()[]{}";
    const char *at = NULL;
    if (p < len) at = strchr(pairs, text_byte(&b->text, p));
    if ((!at || !*at) && p > 0) { /* also accept the char before point */
        at = strchr(pairs, text_byte(&b->text, p - 1));
        if (at && *at) p--;
    }
    if (!at || !*at) {
        editor_error(e, "No bracket at point");
        return;
    }
    int idx = (int)(at - pairs);
    char open = pairs[idx & ~1], close = pairs[idx | 1];
    bool fwd = (idx & 1) == 0;
    long depth = 0;
    size_t i = p;
    for (;;) {
        unsigned char c = text_byte(&b->text, i);
        if (c == (unsigned char)open) depth += fwd ? 1 : -1;
        else if (c == (unsigned char)close) depth += fwd ? -1 : 1;
        if (depth == 0) {
            b->cursor = i;
            return;
        }
        if (fwd) {
            if (++i >= len) break;
        } else {
            if (i-- == 0) break;
        }
    }
    editor_error(e, "Unbalanced bracket");
}

static void cmd_goto_line(Editor *e) {
    Buffer *b = BUF(e);
    char *s = editor_prompt(e, "Goto line[:col]: ", NULL, NULL, NULL, NULL);
    if (!s) return;
    char *end;
    errno = 0;
    unsigned long line = strtoul(s, &end, 10), col = 1;
    if (errno || end == s || line == 0) {
        editor_error(e, "Invalid line number");
        free(s);
        return;
    }
    if (*end == ':' || *end == ',' || *end == ' ') {
        col = strtoul(end + 1, NULL, 10);
        if (col == 0) col = 1;
    }
    free(s);
    size_t n = text_lines(&b->text);
    size_t l = line > n ? n - 1 : line - 1;
    b->cursor = buffer_pos_at_col(b, l, col - 1);
    b->mark_active = false;
}

/* ---- editing ---- */

/* Closing partner if k opens a pair in this buffer's language, else 0. */
static char pair_closer(Buffer *b, Key k) {
    switch (k) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '"': return '"';
    case '\'': case '`':
        return syntax_quote_pairs(b->syntax, (char)k) ? (char)k : 0;
    default: return 0;
    }
}

/* Auto-pairing. Returns true if it handled the key. */
static bool auto_pair(Editor *e, Key k) {
    Buffer *b = BUF(e);
    size_t len = buffer_len(b), s, t;
    unsigned char next = b->cursor < len ? text_byte(&b->text, b->cursor) : 0;
    unsigned char prev = b->cursor ? text_byte(&b->text, b->cursor - 1) : 0;
    char close = pair_closer(b, k);
    bool quote = k == '"' || k == '\'' || k == '`';

    if (close && buffer_region(b, &s, &t)) { /* wrap the selection */
        char open = (char)k;
        buffer_insert(b, t, &close, 1);
        buffer_insert(b, s, &open, 1);
        b->cursor = t + 1;
        b->mark_active = false;
        return true;
    }
    if (next == (unsigned char)k && (k == ')' || k == ']' || k == '}' ||
                                     (quote && close))) {
        b->cursor++; /* type over the closer */
        return true;
    }
    if (!close) return false;
    bool next_ok = next == 0 || next == '\n' || next == ' ' || next == '\t' ||
                   strchr(")]},;:", next);
    if (quote && (is_word_byte(prev) || prev == (unsigned char)k)) return false;
    if (!next_ok) return false;
    char pair[2] = {(char)k, close};
    size_t at = b->cursor;
    if (insert_text(e, pair, 2)) b->cursor = at + 1;
    return true;
}

void cmd_self_insert(Editor *e) {
    Buffer *b = BUF(e);
    Key k = e->last_key;
    char enc[4];
    size_t n = utf8_encode((uint32_t)k, enc);
    if (n == 0) return;
    if (e->cfg.auto_pair && auto_pair(e, k)) return;
    delete_region_if_active(e);
    /* electric dedent: closing brace on an otherwise blank line */
    if (k == '}' && e->cfg.auto_indent) {
        size_t start = line_start_of(b, b->cursor);
        if (indent_end(b, start) == b->cursor && b->cursor > start) {
            size_t col = buffer_col_of(b, b->cursor);
            size_t w = (size_t)b->tab_width;
            size_t target = col >= w ? (col - 1) / w * w : 0;
            size_t p = buffer_pos_at_col(b, text_line_of(&b->text, start),
                                         target);
            delete_range(e, p, b->cursor);
        }
    }
    insert_text(e, enc, n);
}

static void cmd_newline(Editor *e) {
    Buffer *b = BUF(e);
    delete_region_if_active(e);
    if (e->pasting || !e->cfg.auto_indent) {
        insert_text(e, "\n", 1);
        return;
    }
    size_t start = line_start_of(b, b->cursor);
    size_t ind = indent_end(b, start);
    if (ind > b->cursor) ind = b->cursor;
    Str s = {0};
    str_putc(&s, '\n');
    char *prefix = text_dup(&b->text, start, ind - start);
    if (prefix) str_append(&s, prefix, ind - start);
    free(prefix);

    /* the char before point (ignoring blanks) opens a block? */
    size_t p = b->cursor;
    while (p > start && (text_byte(&b->text, p - 1) == ' ' ||
                         text_byte(&b->text, p - 1) == '\t'))
        p--;
    unsigned char before = p > start ? text_byte(&b->text, p - 1) : 0;
    bool python = b->syntax && strcmp(syntax_name(b->syntax), "Python") == 0;
    bool opens = before == '{' || before == '(' || before == '[' ||
                 (before == ':' && python);
    size_t base_len = s.len;
    if (opens) indent_unit(b, &s);
    size_t cursor_off = s.len;
    unsigned char after = b->cursor < buffer_len(b)
                              ? text_byte(&b->text, b->cursor) : 0;
    if (opens && ((before == '{' && after == '}') ||
                  (before == '(' && after == ')') ||
                  (before == '[' && after == ']')))
    {
        char *base = xstrndup(s.data, base_len); /* s.data may move */
        str_append(&s, base, base_len); /* closer on its own line */
        free(base);
    }

    /* leaving a line that holds only auto-indentation: clear it */
    if (ind == b->cursor && ind > start &&
        ind == line_end_of(b, b->cursor)) {
        delete_range(e, start, ind);
    }
    size_t at = b->cursor;
    if (insert_text(e, s.data, s.len)) b->cursor = at + cursor_off;
    str_free(&s);
}

static void cmd_open_line(Editor *e) {
    Buffer *b = BUF(e);
    size_t at = b->cursor;
    if (insert_text(e, "\n", 1)) b->cursor = at;
}

/* Apply fn-style indentation change to every line touched by the region
 * (or the current line). */
static void shift_lines(Editor *e, bool indent) {
    Buffer *b = BUF(e);
    size_t s, t;
    bool region = buffer_region(b, &s, &t);
    if (!region) s = t = b->cursor;
    size_t l1 = text_line_of(&b->text, s);
    size_t l2 = text_line_of(&b->text, t);
    if (region && l2 > l1 && t == text_line_start(&b->text, l2)) l2--;
    Str unit = {0};
    indent_unit(b, &unit);
    for (size_t l = l1; l <= l2; l++) {
        size_t start = text_line_start(&b->text, l);
        size_t end = text_line_end(&b->text, l);
        if (indent) {
            if (end > start || !region)
                buffer_insert(b, start, unit.data, unit.len);
            continue;
        }
        size_t n = 0, col = 0;
        while (start + n < end && col < (size_t)b->tab_width) {
            unsigned char c = text_byte(&b->text, start + n);
            if (c == '\t') { n++; break; }
            if (c != ' ') break;
            n++;
            col++;
        }
        if (n) buffer_delete(b, start, n);
    }
    str_free(&unit);
    if (region) b->mark_active = true; /* keep the selection for repeats */
}

static void cmd_indent(Editor *e) {
    Buffer *b = BUF(e);
    size_t s, t;
    if (buffer_region(b, &s, &t)) {
        shift_lines(e, true);
        return;
    }
    if (b->expand_tabs) {
        size_t col = buffer_col_of(b, b->cursor);
        size_t n = (size_t)b->tab_width - col % (size_t)b->tab_width;
        char sp[16];
        memset(sp, ' ', n);
        insert_text(e, sp, n);
    } else {
        insert_text(e, "\t", 1);
    }
}

static void cmd_dedent(Editor *e) { shift_lines(e, false); }

static void cmd_delete_backward_char(Editor *e) {
    Buffer *b = BUF(e);
    if (delete_region_if_active(e) || b->cursor == 0) return;
    if (e->cfg.auto_pair && b->cursor < buffer_len(b)) { /* empty pair */
        char close = pair_closer(b, text_byte(&b->text, b->cursor - 1));
        if (close && text_byte(&b->text, b->cursor) == (unsigned char)close) {
            delete_range(e, b->cursor - 1, b->cursor + 1);
            return;
        }
    }
    size_t start = line_start_of(b, b->cursor);
    /* inside leading spaces with expand-tabs: delete back to a tab stop */
    if (b->expand_tabs && indent_end(b, start) >= b->cursor &&
        b->cursor > start && text_byte(&b->text, b->cursor - 1) == ' ') {
        size_t col = buffer_col_of(b, b->cursor);
        size_t w = (size_t)b->tab_width;
        size_t target = (col - 1) / w * w;
        size_t p = b->cursor;
        while (p > start && col > target &&
               text_byte(&b->text, p - 1) == ' ') {
            p--;
            col--;
        }
        delete_range(e, p, b->cursor);
        return;
    }
    delete_range(e, buffer_prev_char(b, b->cursor), b->cursor);
}

static void cmd_delete_char(Editor *e) {
    Buffer *b = BUF(e);
    if (delete_region_if_active(e)) return;
    delete_range(e, b->cursor, buffer_next_char(b, b->cursor));
}

static void cmd_kill_line(Editor *e) {
    Buffer *b = BUF(e);
    size_t end = line_end_of(b, b->cursor);
    if (end == b->cursor) end = end < buffer_len(b) ? end + 1 : end;
    if (end == b->cursor) {
        editor_error(e, "End of buffer");
        return;
    }
    kill_save(e, b->cursor, end, false);
    delete_range(e, b->cursor, end);
}

static bool region_or_line(Buffer *b, size_t *s, size_t *t) {
    if (buffer_region(b, s, t)) return true;
    size_t l = buffer_cur_line(b);
    *s = text_line_start(&b->text, l);
    *t = l + 1 < text_lines(&b->text) ? text_line_start(&b->text, l + 1)
                                      : buffer_len(b);
    return false;
}

static void cmd_kill_region(Editor *e) {
    Buffer *b = BUF(e);
    size_t s, t;
    region_or_line(b, &s, &t);
    if (s == t) return;
    kill_save(e, s, t, false);
    delete_range(e, s, t);
    b->mark_active = false;
}

static void cmd_copy_region(Editor *e) {
    Buffer *b = BUF(e);
    size_t s, t;
    bool region = region_or_line(b, &s, &t);
    if (s == t) return;
    e->last_kill = false; /* copying always starts a fresh kill */
    kill_save(e, s, t, false);
    e->this_kill = false;
    b->mark_active = false;
    editor_message(e, region ? "Copied region" : "Copied line");
}

static void cmd_yank(Editor *e) {
    Buffer *b = BUF(e);
    if (!e->kill.len) {
        editor_error(e, "Kill buffer is empty");
        return;
    }
    delete_region_if_active(e);
    size_t at = b->cursor;
    if (insert_text(e, e->kill.data, e->kill.len)) {
        b->mark = at;
        b->mark_active = false;
    }
}

static void cmd_kill_word(Editor *e) {
    Buffer *b = BUF(e);
    size_t end = word_forward(b, b->cursor);
    if (end == b->cursor) return;
    kill_save(e, b->cursor, end, false);
    delete_range(e, b->cursor, end);
}

static void cmd_backward_kill_word(Editor *e) {
    Buffer *b = BUF(e);
    size_t start = word_backward(b, b->cursor);
    if (start == b->cursor) return;
    kill_save(e, start, b->cursor, true);
    delete_range(e, start, b->cursor);
}

static void cmd_set_mark(Editor *e) {
    Buffer *b = BUF(e);
    if (b->mark_active && b->mark == b->cursor) {
        b->mark_active = false;
        editor_message(e, "Mark deactivated");
        return;
    }
    b->mark = b->cursor;
    b->mark_active = true;
    b->shift_selection = false;
    editor_message(e, "Mark set");
}

static void cmd_exchange_point_and_mark(Editor *e) {
    Buffer *b = BUF(e);
    size_t m = b->mark > buffer_len(b) ? buffer_len(b) : b->mark;
    b->mark = b->cursor;
    b->cursor = m;
    b->mark_active = true;
}

static void cmd_mark_whole_buffer(Editor *e) {
    Buffer *b = BUF(e);
    b->mark = 0;
    b->cursor = buffer_len(b);
    b->mark_active = true;
    b->shift_selection = false;
}

static void cmd_select_line(Editor *e) {
    Buffer *b = BUF(e);
    size_t l = buffer_cur_line(b), n = text_lines(&b->text);
    if (!b->mark_active) {
        b->mark = text_line_start(&b->text, l);
        b->mark_active = true;
        b->shift_selection = false;
    }
    /* after a previous select-line the cursor sits at the next line's
     * start, so repeating extends by one line */
    b->cursor = l + 1 < n ? text_line_start(&b->text, l + 1) : buffer_len(b);
}

static void cmd_undo(Editor *e) {
    if (!buffer_undo(BUF(e))) editor_error(e, "No further undo information");
}

static void cmd_redo(Editor *e) {
    if (!buffer_redo(BUF(e))) editor_error(e, "No further redo information");
}

static void cmd_keyboard_quit(Editor *e) {
    Buffer *b = BUF(e);
    b->mark_active = false;
    b->shift_selection = false;
    e->hl_have_cur = false;
    if (e->hl_search) {
        search_free(e->hl_search);
        free(e->hl_search);
        e->hl_search = NULL;
    }
    editor_message(e, "Quit");
}

static void cmd_toggle_cursor_shape(Editor *e) {
    e->bar_cursor = !e->bar_cursor;
}

static void cmd_ignore(Editor *e) { (void)e; }

void commands_register_edit(Editor *e) {
    static const struct {
        const char *name;
        CommandFn fn;
        unsigned flags;
        const char *doc;
    } t[] = {
        {"forward-char", cmd_forward_char, CMD_MOTION, "Move one character right"},
        {"backward-char", cmd_backward_char, CMD_MOTION, "Move one character left"},
        {"next-line", cmd_next_line, CMD_MOTION | CMD_KEEP_GOAL, "Move down one line"},
        {"previous-line", cmd_previous_line, CMD_MOTION | CMD_KEEP_GOAL, "Move up one line"},
        {"page-down", cmd_page_down, CMD_MOTION | CMD_KEEP_GOAL, "Scroll one screen down"},
        {"page-up", cmd_page_up, CMD_MOTION | CMD_KEEP_GOAL, "Scroll one screen up"},
        {"beginning-of-line", cmd_beginning_of_line, CMD_MOTION, "Smart home: indentation, then column 0"},
        {"end-of-line", cmd_end_of_line, CMD_MOTION, "Smart end: line end, then last non-blank"},
        {"forward-word", cmd_forward_word, CMD_MOTION, "Move to the end of the next word"},
        {"backward-word", cmd_backward_word, CMD_MOTION, "Move to the start of the previous word"},
        {"beginning-of-buffer", cmd_beginning_of_buffer, CMD_MOTION, "Move to the start of the buffer"},
        {"end-of-buffer", cmd_end_of_buffer, CMD_MOTION, "Move to the end of the buffer"},
        {"recenter", cmd_recenter, 0, "Center the cursor line and redraw"},
        {"goto-matching-bracket", cmd_goto_matching_bracket, CMD_MOTION, "Jump to the matching bracket"},
        {"goto-line", cmd_goto_line, 0, "Go to line[:column]"},
        {"newline", cmd_newline, 0, "Insert a newline with auto-indentation"},
        {"open-line", cmd_open_line, 0, "Insert a newline after the cursor"},
        {"indent", cmd_indent, 0, "Insert indentation, or indent the region"},
        {"dedent", cmd_dedent, 0, "Remove one indentation level from line or region"},
        {"delete-backward-char", cmd_delete_backward_char, CMD_COALESCE, "Delete the previous character or the region"},
        {"delete-char", cmd_delete_char, CMD_COALESCE, "Delete the next character or the region"},
        {"kill-line", cmd_kill_line, CMD_KILL, "Kill to end of line"},
        {"kill-region", cmd_kill_region, CMD_KILL, "Cut the region (or the current line)"},
        {"copy-region", cmd_copy_region, 0, "Copy the region (or the current line)"},
        {"yank", cmd_yank, 0, "Paste the kill buffer"},
        {"kill-word", cmd_kill_word, CMD_KILL, "Kill to the end of the word"},
        {"backward-kill-word", cmd_backward_kill_word, CMD_KILL, "Kill to the start of the word"},
        {"set-mark", cmd_set_mark, 0, "Set the mark (start a selection)"},
        {"exchange-point-and-mark", cmd_exchange_point_and_mark, 0, "Swap cursor and mark"},
        {"mark-whole-buffer", cmd_mark_whole_buffer, 0, "Select the whole buffer"},
        {"select-line", cmd_select_line, 0, "Select the current line; repeat to extend"},
        {"undo", cmd_undo, 0, "Undo the last change"},
        {"redo", cmd_redo, 0, "Redo the last undone change"},
        {"keyboard-quit", cmd_keyboard_quit, 0, "Cancel selection, prefix or search highlight"},
        {"toggle-cursor-shape", cmd_toggle_cursor_shape, 0, "Switch between block and bar cursors"},
        {"ignore", cmd_ignore, 0, "Do nothing (for unbinding keys)"},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        command_register(e, t[i].name, t[i].fn, t[i].doc, t[i].flags, -1);
    command_register(e, "self-insert", cmd_self_insert,
                     "Insert the typed character", CMD_COALESCE, -1);
}

void commands_init(Editor *e) {
    commands_register_edit(e);
    commands_register_file(e);
    tree_register_commands(e);
    commands_register_search(e);
    lsp_register_commands(e);
}

/* ---- default keymaps ---- */

static void bind_all(Keymap *m, const char *const (*t)[2], size_t n) {
    char err[128];
    for (size_t i = 0; i < n; i++) keymap_bind(m, t[i][0], t[i][1], err, sizeof err);
}

static const char *const common_keys[][2] = {
    {"<right>", "forward-char"},       {"<left>", "backward-char"},
    {"<down>", "next-line"},           {"<up>", "previous-line"},
    {"<home>", "beginning-of-line"},   {"<end>", "end-of-line"},
    {"<next>", "page-down"},           {"<prior>", "page-up"},
    {"C-<right>", "forward-word"},     {"C-<left>", "backward-word"},
    {"M-<right>", "forward-word"},     {"M-<left>", "backward-word"},
    {"C-<home>", "beginning-of-buffer"}, {"C-<end>", "end-of-buffer"},
    {"RET", "newline"},                {"TAB", "indent"},
    {"<backtab>", "dedent"},           {"DEL", "delete-backward-char"},
    {"C-h", "delete-backward-char"},   {"<delete>", "delete-char"},
    {"<insert>", "toggle-cursor-shape"}, {"M-i", "toggle-cursor-shape"},
    {"M-x", "execute-extended-command"},
    {"M-/", "lsp-complete"},           {"M-TAB", "lsp-complete"},
    {"M-.", "lsp-definition"},         {"M-,", "lsp-back"},
    {"M-t", "file-tree"},
};

static const char *const emacs_keys[][2] = {
    {"C-f", "forward-char"},        {"C-b", "backward-char"},
    {"C-n", "next-line"},           {"C-p", "previous-line"},
    {"C-a", "beginning-of-line"},   {"C-e", "end-of-line"},
    {"M-f", "forward-word"},        {"M-b", "backward-word"},
    {"M-<", "beginning-of-buffer"}, {"M->", "end-of-buffer"},
    {"C-v", "page-down"},           {"M-v", "page-up"},
    {"C-l", "recenter"},            {"M-g g", "goto-line"},
    {"M-g M-g", "goto-line"},       {"M-g b", "goto-matching-bracket"},
    {"C-j", "newline"},             {"C-o", "open-line"},
    {"C-d", "delete-char"},         {"C-k", "kill-line"},
    {"C-w", "kill-region"},         {"M-w", "copy-region"},
    {"C-y", "yank"},                {"M-d", "kill-word"},
    {"M-DEL", "backward-kill-word"}, {"C-SPC", "set-mark"},
    {"C-x C-x", "exchange-point-and-mark"},
    {"C-x h", "mark-whole-buffer"}, {"C-x l", "select-line"},
    {"C-_", "undo"},                {"C-x u", "undo"},
    {"M-_", "redo"},                {"C-M-_", "redo"},
    {"C-g", "keyboard-quit"},       {"C-x C-s", "save-buffer"},
    {"C-x s", "save-some-buffers"},
    {"C-x C-w", "write-file"},      {"C-x C-f", "find-file"},
    {"C-x C-r", "recent-files"},    {"C-x k", "kill-buffer"},
    {"C-x b", "switch-to-buffer"},  {"C-x C-b", "list-buffers"},
    {"C-x <right>", "next-buffer"}, {"C-x <left>", "previous-buffer"},
    {"C-x C-c", "save-buffers-kill-editor"},
    {"C-z", "suspend"},             {"M-:", "lua-eval"},
    {"C-s", "isearch-forward"},     {"C-r", "isearch-backward"},
    {"C-M-s", "isearch-forward-regexp"},
    {"C-M-r", "isearch-backward-regexp"},
    {"M-%", "query-replace"},       {"C-M-%", "query-replace-regexp"},
    {"C-c ?", "describe-bindings"},
    {"C-c h", "lsp-hover"},
    {"M-g n", "lsp-next-diagnostic"}, {"M-g p", "lsp-previous-diagnostic"},
};

static const char *const simple_keys[][2] = {
    {"C-s", "save-buffer"},         {"C-q", "save-buffers-kill-editor"},
    {"C-o", "find-file"},           {"C-w", "kill-buffer"},
    {"C-z", "undo"},                {"C-y", "redo"},
    {"C-x", "kill-region"},         {"C-c", "copy-region"},
    {"C-v", "yank"},                {"C-a", "mark-whole-buffer"},
    {"C-l", "select-line"},         {"C-k", "kill-line"},
    {"C-f", "isearch-forward"},     {"C-r", "query-replace"},
    {"C-g", "goto-line"},           {"C-b", "switch-to-buffer"},
    {"C-p", "execute-extended-command"},
    {"C-]", "goto-matching-bracket"},
    {"C-n", "search-next"},         {"ESC", "keyboard-quit"},
    {"M-<right>", "next-buffer"},   {"M-<left>", "previous-buffer"},
    {"C-SPC", "lsp-complete"},      {"C-d", "delete-char"},
    {"M-h", "lsp-hover"},
    {"M-n", "lsp-next-diagnostic"}, {"M-p", "lsp-previous-diagnostic"},
};

/* Modelled on GNU nano's default bindings. */
static const char *const nano_keys[][2] = {
    {"C-g", "describe-bindings"},     {"C-x", "save-buffers-kill-editor"},
    {"C-o", "save-buffer"},           {"C-s", "save-buffer"},
    {"C-r", "find-file"},             {"C-w", "isearch-forward"},
    {"C-q", "isearch-backward"},      {"M-w", "search-next"},
    {"M-q", "search-previous"},       {"C-\\", "query-replace"},
    {"M-r", "query-replace"},         {"C-k", "kill-region"},
    {"C-u", "yank"},                  {"M-6", "copy-region"},
    {"M-^", "copy-region"},           {"C-^", "set-mark"},
    {"M-a", "set-mark"},              {"C-t", "execute-extended-command"},
    {"C-c", "cursor-position"},       {"C-_", "goto-line"},
    {"M-g", "goto-line"},             {"M-u", "undo"},
    {"M-e", "redo"},                  {"C-a", "beginning-of-line"},
    {"C-e", "end-of-line"},           {"C-p", "previous-line"},
    {"C-n", "next-line"},             {"C-f", "forward-char"},
    {"C-b", "backward-char"},         {"C-SPC", "forward-word"},
    {"M-SPC", "backward-word"},       {"C-y", "page-up"},
    {"C-v", "page-down"},             {"M-\\", "beginning-of-buffer"},
    {"M-/", "end-of-buffer"},         {"M-]", "goto-matching-bracket"},
    {"C-d", "delete-char"},           {"M-}", "indent"},
    {"M-{", "dedent"},                {"M-#", "toggle-line-numbers"},
    {"M-<", "previous-buffer"},       {"M->", "next-buffer"},
    {"C-l", "recenter"},              {"C-z", "suspend"},
    {"ESC", "keyboard-quit"},         {"C-]", "lsp-complete"},
    {"M-h", "lsp-hover"},             {"M-n", "lsp-next-diagnostic"},
    {"M-p", "lsp-previous-diagnostic"},
};

void keymap_load_nano(Keymap *m) {
    bind_all(m, common_keys, sizeof common_keys / sizeof common_keys[0]);
    /* nano's M-/ is "last line"; completion lives on ^] instead */
    bind_all(m, nano_keys, sizeof nano_keys / sizeof nano_keys[0]);
}

void keymap_load_emacs(Keymap *m) {
    bind_all(m, common_keys, sizeof common_keys / sizeof common_keys[0]);
    bind_all(m, emacs_keys, sizeof emacs_keys / sizeof emacs_keys[0]);
}

void keymap_load_simple(Keymap *m) {
    bind_all(m, common_keys, sizeof common_keys / sizeof common_keys[0]);
    /* M-<left>/<right> switch buffers here instead of moving by word */
    bind_all(m, simple_keys, sizeof simple_keys / sizeof simple_keys[0]);
}
