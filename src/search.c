#include "cedit/search.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cedit/editor.h"

/* ---- engine ---- */

int search_compile(Search *s, const char *pat, bool regex, char *err,
                   size_t errlen) {
    memset(s, 0, sizeof *s);
    s->pat = xstrdup(pat);
    s->patlen = strlen(pat);
    s->regex = regex;
    s->icase = true;
    for (const char *p = pat; *p; p++)
        if (isupper((unsigned char)*p)) s->icase = false;
    if (regex) {
        int flags = REG_EXTENDED | REG_NEWLINE | (s->icase ? REG_ICASE : 0);
        int rc = regcomp(&s->re, pat, flags);
        if (rc != 0) {
            regerror(rc, &s->re, err, errlen);
            free(s->pat);
            s->pat = NULL;
            return -1;
        }
        s->compiled = true;
    }
    return 0;
}

void search_free(Search *s) {
    if (s->compiled) regfree(&s->re);
    free(s->pat);
    str_free(&s->scratch);
    memset(s, 0, sizeof *s);
}

static bool literal_at(const Search *s, const char *p) {
    if (!s->icase) return memcmp(p, s->pat, s->patlen) == 0;
    for (size_t i = 0; i < s->patlen; i++)
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)s->pat[i]))
            return false;
    return true;
}

bool search_in_line(Search *s, const char *line, size_t len, size_t from,
                    size_t *ms, size_t *me) {
    if (from > len || !s->pat) return false;
    if (!s->regex) {
        if (s->patlen == 0 || s->patlen > len) return false;
        for (size_t i = from; i + s->patlen <= len; i++) {
            if (literal_at(s, line + i)) {
                *ms = i;
                *me = i + s->patlen;
                return true;
            }
        }
        return false;
    }
    str_clear(&s->scratch);
    str_append(&s->scratch, line, len); /* NUL-terminated copy */
    const char *str = str_cstr(&s->scratch);
    if (memchr(str, '\0', len)) len = strlen(str); /* regex stops at NUL */
    if (from > len) return false;
    if (regexec(&s->re, str + from, 10, s->groups, from ? REG_NOTBOL : 0) != 0)
        return false;
    for (int i = 0; i < 10; i++) {
        if (s->groups[i].rm_so >= 0) {
            s->groups[i].rm_so += (regoff_t)from;
            s->groups[i].rm_eo += (regoff_t)from;
        }
    }
    *ms = (size_t)s->groups[0].rm_so;
    *me = (size_t)s->groups[0].rm_eo;
    return true;
}

/* Last match in line starting strictly before limit. */
static bool last_in_line(Search *s, const char *line, size_t len,
                         size_t limit, size_t *ms, size_t *me) {
    bool found = false;
    size_t from = 0, a, z;
    while (search_in_line(s, line, len, from, &a, &z) && a < limit) {
        *ms = a;
        *me = z;
        found = true;
        from = a + 1;
    }
    return found;
}

bool search_find(Search *s, Buffer *b, size_t pos, bool forward, bool wrap,
                 size_t *ms, size_t *me) {
    Text *t = &b->text;
    size_t n = text_lines(t), start_line = text_line_of(t, pos);
    for (size_t k = 0; k <= n; k++) {
        size_t line;
        if (forward) {
            line = start_line + k;
            if (line >= n) {
                if (!wrap) return false;
                line -= n;
            }
        } else {
            if (k > start_line) {
                if (!wrap) return false;
                line = n - (k - start_line);
            } else {
                line = start_line - k;
            }
        }
        size_t ls = text_line_start(t, line), le = text_line_end(t, line);
        const char *p = text_view(t, ls, le - ls);
        size_t a, z;
        bool ok;
        if (forward) {
            size_t from = (k == 0) ? pos - ls : 0;
            ok = search_in_line(s, p, le - ls, from, &a, &z);
            if (ok && k == n && ls + a >= pos) ok = false; /* wrapped fully */
        } else {
            size_t limit = (k == 0) ? pos - ls : le - ls + 1;
            ok = last_in_line(s, p, le - ls, limit, &a, &z);
            if (ok && k == n && ls + a < pos) ok = false;
        }
        if (ok) {
            *ms = ls + a;
            *me = ls + z;
            return true;
        }
    }
    return false;
}

void search_expand(const Search *s, const char *line, const char *repl,
                   Str *out) {
    if (!s->regex) {
        str_appendz(out, repl);
        return;
    }
    for (const char *p = repl; *p; p++) {
        if (*p == '\\' && p[1] >= '0' && p[1] <= '9') {
            const regmatch_t *g = &s->groups[p[1] - '0'];
            if (g->rm_so >= 0)
                str_append(out, line + g->rm_so, (size_t)(g->rm_eo - g->rm_so));
            p++;
        } else if (*p == '\\' && p[1] == '\\') {
            str_putc(out, '\\');
            p++;
        } else if (*p == '\\' && p[1] == 'n') {
            str_putc(out, '\n');
            p++;
        } else if (*p == '\\' && p[1] == 't') {
            str_putc(out, '\t');
            p++;
        } else {
            str_putc(out, *p);
        }
    }
}

/* ---- incremental search ---- */

typedef struct {
    bool forward;
    bool regex;
    bool failing;
    size_t origin;
    size_t ms, me;      /* current match */
    bool have;
    Search search;
    bool compiled;
    char err[128];
} ISearch;

static void set_highlight(Editor *e, ISearch *is) {
    if (e->hl_search) {
        search_free(e->hl_search);
        free(e->hl_search);
        e->hl_search = NULL;
    }
    e->hl_have_cur = false;
    if (!is->compiled) return;
    e->hl_search = xmalloc(sizeof *e->hl_search);
    char err[128];
    if (search_compile(e->hl_search, is->search.pat, is->regex, err,
                       sizeof err) != 0) {
        free(e->hl_search);
        e->hl_search = NULL;
        return;
    }
    e->hl_have_cur = is->have;
    e->hl_cur_start = is->ms;
    e->hl_cur_end = is->me;
}

static void isearch_step(Editor *e, ISearch *is, size_t from, bool forward) {
    Buffer *b = editor_buf(e);
    size_t ms, me;
    is->forward = forward;
    if (search_find(&is->search, b, from, forward, true, &ms, &me)) {
        is->have = true;
        is->failing = false;
        is->ms = ms;
        is->me = me;
        b->cursor = forward ? me : ms;
    } else {
        is->failing = true;
    }
}

/* +1 / -1 if k is bound to a forward / backward search command. */
static int search_key(Editor *e, Key k) {
    const char *cmd = NULL;
    const Keymap *sub;
    if (keymap_lookup(e->keymap, k, &cmd, &sub) != KM_COMMAND) return 0;
    if (!strcmp(cmd, "isearch-forward") || !strcmp(cmd, "search-next") ||
        !strcmp(cmd, "isearch-forward-regexp"))
        return 1;
    if (!strcmp(cmd, "isearch-backward") || !strcmp(cmd, "search-previous") ||
        !strcmp(cmd, "isearch-backward-regexp"))
        return -1;
    return 0;
}

static bool isearch_hook(Editor *e, const char *text, Key k, void *ud) {
    ISearch *is = ud;
    Buffer *b = editor_buf(e);
    const char *label = is->regex ? "regexp " : "";
    if (k == KEY_NONE) { /* text changed: search again from the origin */
        if (is->compiled) search_free(&is->search);
        is->compiled = false;
        is->have = false;
        is->failing = false;
        e->msg[0] = '\0';
        if (*text) {
            if (search_compile(&is->search, text, is->regex, is->err,
                               sizeof is->err) == 0) {
                is->compiled = true;
                isearch_step(e, is, is->origin, is->forward);
            } else {
                editor_message(e, "incomplete: %s", is->err);
            }
        }
        if (!is->have) b->cursor = is->origin;
        set_highlight(e, is);
    } else if (k == 19 || k == 18 || search_key(e, k) != 0) {
        /* C-s / C-r, or any key bound to a search command */
        bool fwd = k == 19 || (k != 18 && search_key(e, k) > 0);
        if (!*text && e->last_search) {
            str_clear(&e->mb_text);
            str_appendz(&e->mb_text, e->last_search);
            e->mb_cursor = e->mb_text.len;
            isearch_hook(e, e->last_search, KEY_NONE, ud);
        } else if (is->compiled) {
            size_t from;
            if (fwd)
                from = is->have ? (is->me > is->ms ? is->me : is->me + 1)
                                : b->cursor;
            else
                from = is->have ? is->ms : b->cursor;
            if (from > buffer_len(b)) from = 0;
            isearch_step(e, is, from, fwd);
            set_highlight(e, is);
        }
    } else {
        return false;
    }
    snprintf(e->mb_prompt, sizeof e->mb_prompt, "%s%sI-search%s: ",
             is->failing ? "Failing " : "", label,
             is->forward ? "" : " backward");
    return k != KEY_NONE;
}

static void isearch(Editor *e, bool forward, bool regex) {
    Buffer *b = editor_buf(e);
    ISearch is = {.forward = forward, .regex = regex, .origin = b->cursor};
    char prompt[64];
    snprintf(prompt, sizeof prompt, "%sI-search%s: ", regex ? "regexp " : "",
             forward ? "" : " backward");
    char *text = editor_prompt(e, prompt, NULL, isearch_hook, &is, NULL);
    if (text) {
        if (*text) {
            free(e->last_search);
            e->last_search = xstrdup(text);
            e->last_search_regex = regex;
        }
        if (is.have) {
            b->mark = is.origin; /* like Emacs: mark where the search began */
            b->mark_active = false;
        }
        if (is.failing) editor_error(e, "Not found");
    } else {
        b->cursor = is.origin;
    }
    free(text);
    if (is.compiled) search_free(&is.search);
    if (e->hl_search) {
        search_free(e->hl_search);
        free(e->hl_search);
        e->hl_search = NULL;
    }
    e->hl_have_cur = false;
}

static void cmd_isearch_forward(Editor *e) { isearch(e, true, false); }
static void cmd_isearch_backward(Editor *e) { isearch(e, false, false); }
static void cmd_isearch_forward_regexp(Editor *e) { isearch(e, true, true); }
static void cmd_isearch_backward_regexp(Editor *e) { isearch(e, false, true); }

static void search_repeat(Editor *e, bool forward) {
    Buffer *b = editor_buf(e);
    if (!e->last_search) {
        editor_error(e, "No previous search");
        return;
    }
    Search s;
    char err[128];
    if (search_compile(&s, e->last_search, e->last_search_regex, err,
                       sizeof err) != 0) {
        editor_error(e, "%s", err);
        return;
    }
    size_t ms, me;
    size_t from = forward ? b->cursor : b->cursor;
    if (search_find(&s, b, from, forward, true, &ms, &me)) {
        if (forward && ms == b->cursor && me == ms) /* empty match here */
            search_find(&s, b, from + 1, true, true, &ms, &me);
        b->cursor = forward ? me : ms;
    } else {
        editor_error(e, "Not found");
    }
    search_free(&s);
}

static void cmd_search_next(Editor *e) { search_repeat(e, true); }
static void cmd_search_previous(Editor *e) { search_repeat(e, false); }

/* ---- query replace ---- */

static void query_replace(Editor *e, bool regex) {
    Buffer *b = editor_buf(e);
    if (b->readonly) {
        editor_error(e, "Buffer is read-only");
        return;
    }
    char *pat = editor_prompt(e, regex ? "Query replace regexp: "
                                       : "Query replace: ",
                              NULL, NULL, NULL, NULL);
    if (!pat) return;
    if (!*pat) {
        free(pat);
        return;
    }
    Str prompt = {0};
    str_appendf(&prompt, "Replace %s with: ", pat);
    char *repl = editor_prompt(e, str_cstr(&prompt), NULL, NULL, NULL, NULL);
    str_free(&prompt);
    if (!repl) {
        free(pat);
        return;
    }
    Search s;
    char err[128];
    if (search_compile(&s, pat, regex, err, sizeof err) != 0) {
        editor_error(e, "Bad regexp: %s", err);
        free(pat);
        free(repl);
        return;
    }
    free(e->last_search);
    e->last_search = xstrdup(pat);
    e->last_search_regex = regex;

    e->hl_search = xmalloc(sizeof *e->hl_search);
    search_compile(e->hl_search, pat, regex, err, sizeof err);

    size_t pos = b->cursor, count = 0;
    bool all = false;
    size_t region_s = 0, region_e = SIZE_MAX;
    if (buffer_region(b, &region_s, &region_e)) { /* limit to selection */
        pos = region_s;
        b->mark_active = false;
    }
    Str rep = {0};
    for (;;) {
        size_t ms, me;
        if (!search_find(&s, b, pos, true, false, &ms, &me) ||
            me > region_e)
            break;
        e->hl_have_cur = true;
        e->hl_cur_start = ms;
        e->hl_cur_end = me;
        b->cursor = me;
        int a = 'y';
        if (!all)
            a = editor_ask(e, "Replace? (y)es (n)o (!)all (q)uit", "yn!q.");
        if (a == 0 || a == 'q') break;
        if (a == 'n') {
            pos = me > ms ? me : buffer_next_char(b, ms);
            if (pos == ms) break;
            continue;
        }
        if (a == '!') all = true;
        /* recompute the line copy the groups refer to */
        size_t line = text_line_of(&b->text, ms);
        size_t ls = text_line_start(&b->text, line);
        size_t le = text_line_end(&b->text, line);
        char *lc = text_dup(&b->text, ls, le - ls);
        if (!lc) {
            editor_error(e, "Out of memory");
            break;
        }
        size_t a2, z2;
        search_in_line(&s, lc, le - ls, ms - ls, &a2, &z2);
        str_clear(&rep);
        search_expand(&s, lc, repl, &rep);
        free(lc);
        if (buffer_delete(b, ms, me - ms) != 0 ||
            buffer_insert(b, ms, str_cstr(&rep), rep.len) != 0) {
            editor_error(e, "Replace failed");
            break;
        }
        count++;
        if (region_e != SIZE_MAX) region_e = region_e - (me - ms) + rep.len;
        pos = ms + rep.len;
        if (me == ms) { /* empty match: step over one char */
            if (pos >= buffer_len(b)) break;
            pos = buffer_next_char(b, pos);
        }
        b->cursor = pos;
        if (a == '.') break;
    }
    str_free(&rep);
    search_free(&s);
    search_free(e->hl_search);
    free(e->hl_search);
    e->hl_search = NULL;
    e->hl_have_cur = false;
    editor_message(e, "Replaced %zu occurrence%s", count, count == 1 ? "" : "s");
    free(pat);
    free(repl);
}

static void cmd_query_replace(Editor *e) { query_replace(e, false); }
static void cmd_query_replace_regexp(Editor *e) { query_replace(e, true); }

void commands_register_search(Editor *e) {
    command_register(e, "isearch-forward", cmd_isearch_forward,
                     "Incremental search forward", 0, -1);
    command_register(e, "isearch-backward", cmd_isearch_backward,
                     "Incremental search backward", 0, -1);
    command_register(e, "isearch-forward-regexp", cmd_isearch_forward_regexp,
                     "Incremental regular expression search forward", 0, -1);
    command_register(e, "isearch-backward-regexp", cmd_isearch_backward_regexp,
                     "Incremental regular expression search backward", 0, -1);
    command_register(e, "search-next", cmd_search_next,
                     "Repeat the last search forward", CMD_MOTION, -1);
    command_register(e, "search-previous", cmd_search_previous,
                     "Repeat the last search backward", CMD_MOTION, -1);
    command_register(e, "query-replace", cmd_query_replace,
                     "Replace text interactively (in region if active)", 0, -1);
    command_register(e, "query-replace-regexp", cmd_query_replace_regexp,
                     "Replace regexp matches interactively; \\1-\\9 in the "
                     "replacement", 0, -1);
}
