#include "cedit/editor.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cedit/lsp.h"
#include "cedit/script.h"
#include "cedit/utf8.h"

/* Only for the out-of-memory handler, which has no other way in. */
static Editor *oom_editor;

static void oom_handler(void) {
    if (!oom_editor) return;
    term_restore();
    editor_emergency_save(oom_editor);
}

Editor *editor_new(bool headless) {
    Editor *e = xcalloc(1, sizeof *e);
    e->headless = headless;
    e->rows = 24;
    e->cols = 80;
    config_defaults(&e->cfg);
    theme_builtin(e->styles, "dark");
    e->color = term_color_mode();
    commands_init(e);
    editor_reset_keymap(e);
    oom_editor = e;
    util_set_oom_handler(oom_handler);
    return e;
}

void editor_free(Editor *e) {
    if (!e) return;
    lsp_shutdown(e);
    script_free(e);
    for (size_t i = 0; i < e->nbufs; i++) buffer_free(e->bufs[i]);
    free(e->bufs);
    keymap_free(e->keymap);
    commands_free(e);
    str_free(&e->kill);
    str_free(&e->mb_text);
    if (e->hl_search) {
        search_free(e->hl_search);
        free(e->hl_search);
    }
    free(e->last_search);
    render_free(e);
    for (size_t i = 0; i < e->nrecent; i++) free(e->recent[i]);
    free(e->recent);
    if (oom_editor == e) oom_editor = NULL;
    free(e);
}

Buffer *editor_buf(Editor *e) { return e->bufs[e->cur]; }

void editor_reset_keymap(Editor *e) {
    keymap_free(e->keymap);
    e->keymap = keymap_new();
    if (e->cfg.keymap == KEYMAP_SIMPLE)
        keymap_load_simple(e->keymap);
    else if (e->cfg.keymap == KEYMAP_EMACS)
        keymap_load_emacs(e->keymap);
    else
        keymap_load_nano(e->keymap);
    e->prefix = NULL;
}

void editor_init_buffer(Editor *e, Buffer *b) {
    b->tab_width = e->cfg.tab_width;
    b->expand_tabs = e->cfg.expand_tabs;
}

__attribute__((format(printf, 3, 0)))
static void vmessage(Editor *e, bool error, const char *fmt, va_list ap) {
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    e->msg_error = error;
}

void editor_message(Editor *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vmessage(e, false, fmt, ap);
    va_end(ap);
}

void editor_error(Editor *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vmessage(e, true, fmt, ap);
    va_end(ap);
}

/* ---- termination ---- */

static void write_str(int fd, const char *s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static bool write_buffer_fd(int fd, Buffer *b) {
    const char *seg[2];
    size_t len[2];
    text_segments(&b->text, &seg[0], &len[0], &seg[1], &len[1]);
    for (int i = 0; i < 2; i++) {
        const char *p = seg[i];
        size_t n = len[i];
        while (n) {
            ssize_t w = write(fd, p, n);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return false;
            p += w;
            n -= (size_t)w;
        }
    }
    return fsync(fd) == 0 || errno == EINVAL;
}

void editor_emergency_save(Editor *e) {
    for (size_t i = 0; i < e->nbufs; i++) {
        Buffer *b = e->bufs[i];
        if (!buffer_modified(b) || b->readonly) continue;
        char base[PATH_MAX];
        if (b->path) {
            snprintf(base, sizeof base, "%s", b->path);
        } else {
            const char *home = getenv("HOME");
            snprintf(base, sizeof base, "%s/cedit-buffer-%zu",
                     home && *home ? home : "/tmp", i);
        }
        char path[PATH_MAX + 32];
        int fd = -1;
        for (int n = 0; n < 100 && fd < 0; n++) {
            int w = n == 0 ? snprintf(path, sizeof path, "%s.save", base)
                           : snprintf(path, sizeof path, "%s.save.%d", base, n);
            if (w < 0 || (size_t)w >= sizeof path) break; /* never truncate */
            fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0 && errno != EEXIST) break;
        }
        char safe[PATH_MAX];
        sanitize_for_display(path, safe, sizeof safe);
        if (fd < 0) {
            write_str(STDERR_FILENO, "cedit: could not write emergency copy ");
            write_str(STDERR_FILENO, safe);
            write_str(STDERR_FILENO, "\n");
            continue;
        }
        bool ok = write_buffer_fd(fd, b);
        if (close(fd) != 0) ok = false;
        write_str(STDERR_FILENO, ok ? "cedit: unsaved changes written to "
                                    : "cedit: emergency copy incomplete: ");
        write_str(STDERR_FILENO, safe);
        write_str(STDERR_FILENO, "\n");
    }
}

static void editor_terminate(Editor *e, int sig) {
    term_restore();
    editor_emergency_save(e);
    exit(128 + (sig > 0 ? sig : 1));
}

/* ---- input ---- */

static bool fill_input(Editor *e) {
    if (e->inlen == sizeof e->inbuf) return true;
    ssize_t r = term_read(e->inbuf + e->inlen, sizeof e->inbuf - e->inlen);
    if (r == 0) editor_terminate(e, SIGHUP); /* terminal went away */
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        editor_terminate(e, SIGHUP);
    }
    e->inlen += (size_t)r;
    e->last_input = editor_now();
    return true;
}

static void handle_events(Editor *e, int ev) {
    if (ev & TERM_EV_TERMINATE) editor_terminate(e, term_pending_signal());
}

Key editor_read_key(Editor *e) {
    if (e->headless) return KEY_NONE;
    for (;;) {
        if (e->inlen) {
            Key k;
            size_t n = input_parse(e->inbuf, e->inlen, false, &k);
            if (n == 0) {
                /* incomplete escape/UTF-8 sequence: wait briefly */
                int ev = term_wait(25);
                handle_events(e, ev);
                if (ev & TERM_EV_INPUT) {
                    fill_input(e);
                    continue;
                }
                n = input_parse(e->inbuf, e->inlen, true, &k);
            }
            memmove(e->inbuf, e->inbuf + n, e->inlen - n);
            e->inlen -= n;
            if (k != KEY_NONE) return k;
            continue;
        }
        struct pollfd lfds[16];
        int nl = lsp_pollfds(e, lfds, 16);
        int ev = term_wait_fds(lsp_timeout_ms(e), lfds, nl);
        handle_events(e, ev);
        if (lsp_process(e, lfds, nl)) return KEY_RESIZE; /* redraw */
        if (ev & TERM_EV_RESIZE) {
            term_size(&e->rows, &e->cols);
            render_invalidate(e);
            return KEY_RESIZE;
        }
        if (ev & TERM_EV_INPUT) fill_input(e);
    }
}

/* ---- dispatch ---- */

void editor_run_command(Editor *e, const Command *c, bool shifted) {
    Buffer *b = editor_buf(e);
    unsigned long seq = b->undo.next_seq;
    e->this_cmd = c;
    e->this_kill = false;

    if ((c->flags & CMD_COALESCE) && e->last_cmd == c &&
        e->insert_streak < 20) {
        e->insert_streak++;
    } else {
        undo_boundary(&b->undo);
        e->insert_streak = 0;
    }
    if (c->flags & CMD_MOTION) {
        if (shifted) {
            if (!b->mark_active || !b->shift_selection) {
                b->mark = b->cursor;
                b->mark_active = true;
                b->shift_selection = true;
            }
        } else if (b->shift_selection) {
            b->mark_active = false;
            b->shift_selection = false;
        }
    }
    bool keep_goal = (c->flags & CMD_KEEP_GOAL) != 0;
    if (c->fn)
        c->fn(e);
    else
        script_run_command(e, c->script_ref);

    /* the command may have switched buffers */
    Buffer *nb = editor_buf(e);
    if (!keep_goal) nb->goal_valid = false;
    e->last_kill = e->this_kill;
    e->last_cmd = e->this_cmd; /* c may be stale if a script grew cmds */
    if (nb == b && b->undo.next_seq != seq) script_hook(e, "modify", b);
}

static bool self_insertable(Key k) {
    return k >= 32 && k != KEY_DEL && k <= 0x10FFFF && !(k & KM_MASK);
}

void editor_handle_key(Editor *e, Key k) {
    if (k == KEY_NONE || k == KEY_RESIZE) return;
    Buffer *b = editor_buf(e);
    if (k == KEY_PASTE_START) {
        e->pasting = true;
        undo_boundary(&b->undo);
        return;
    }
    if (k == KEY_PASTE_END) {
        e->pasting = false;
        undo_boundary(&b->undo);
        return;
    }
    if (e->pasting) { /* literal insertion, no auto-indent or bindings */
        char enc[4];
        size_t n = 0;
        if (k == KEY_RET || k == '\n') {
            enc[0] = '\n';
            n = 1;
        } else if (k == KEY_TAB || self_insertable(k)) {
            n = utf8_encode((uint32_t)k, enc);
        }
        if (n) {
            size_t s, t;
            if (buffer_region(b, &s, &t)) {
                buffer_delete(b, s, t - s);
                b->mark_active = false;
            }
            insert_text(e, enc, n);
        }
        return;
    }

    if (!e->prefix) e->msg[0] = '\0';
    e->last_key = k;
    const Keymap *map = e->prefix ? e->prefix : e->keymap;
    const char *cmdname = NULL;
    const Keymap *sub = NULL;
    bool shifted = false;
    int r = keymap_lookup(map, k, &cmdname, &sub);
    if (r == KM_UNBOUND && (k & KM_SHIFT) && key_base(k) > KEY_SPECIAL) {
        r = keymap_lookup(map, k & ~KM_SHIFT, &cmdname, &sub);
        shifted = r == KM_COMMAND;
    }
    char name[48];
    key_name(k, name, sizeof name);

    if (r == KM_PREFIX) {
        size_t used = e->prefix ? strlen(e->prefix_desc) : 0;
        snprintf(e->prefix_desc + used, sizeof e->prefix_desc - used, "%s%s",
                 used ? " " : "", name);
        e->prefix = sub;
        editor_message(e, "%s-", e->prefix_desc);
        return;
    }
    bool had_prefix = e->prefix != NULL;
    char desc[64];
    snprintf(desc, sizeof desc, "%s", e->prefix_desc);
    e->prefix = NULL;
    e->prefix_desc[0] = '\0';
    e->msg[0] = '\0';

    if (r == KM_COMMAND) {
        const Command *c = command_find(e, cmdname);
        if (!c) {
            editor_error(e, "Unknown command: %s", cmdname);
            return;
        }
        editor_run_command(e, c, shifted);
        return;
    }
    if (had_prefix && k == 7) {
        editor_message(e, "Quit");
        return;
    }
    if (!had_prefix && self_insertable(k)) {
        editor_run_command(e, command_find(e, "self-insert"), false);
        return;
    }
    editor_error(e, "%s%s%s is undefined", desc, *desc ? " " : "", name);
}

/* ---- minibuffer ---- */

static void mb_insert(Editor *e, const char *s, size_t n) {
    Str t = {0};
    str_append(&t, e->mb_text.data ? e->mb_text.data : "", e->mb_cursor);
    str_append(&t, s, n);
    str_appendz(&t, str_cstr(&e->mb_text) + e->mb_cursor);
    str_free(&e->mb_text);
    e->mb_text = t;
    e->mb_cursor += n;
}

static size_t mb_prev(const Editor *e, size_t p) {
    while (p > 0) {
        p--;
        if (!utf8_is_cont((unsigned char)e->mb_text.data[p])) break;
    }
    return p;
}

static size_t mb_next(const Editor *e, size_t p) {
    if (p >= e->mb_text.len) return e->mb_text.len;
    p++;
    while (p < e->mb_text.len &&
           utf8_is_cont((unsigned char)e->mb_text.data[p]))
        p++;
    return p;
}

static void mb_erase(Editor *e, size_t a, size_t b) {
    if (b <= a) return;
    memmove(e->mb_text.data + a, e->mb_text.data + b, e->mb_text.len - b + 1);
    e->mb_text.len -= b - a;
    if (e->mb_cursor > b)
        e->mb_cursor -= b - a;
    else if (e->mb_cursor > a)
        e->mb_cursor = a;
}

char *editor_prompt(Editor *e, const char *prompt, const char *initial,
                    PromptHook hook, void *ud, Completer comp) {
    if (e->headless) return NULL;
    bool was_active = e->mb_active;
    e->mb_active = true;
    snprintf(e->mb_prompt, sizeof e->mb_prompt, "%s", prompt);
    str_clear(&e->mb_text);
    str_appendz(&e->mb_text, initial ? initial : "");
    e->mb_cursor = e->mb_text.len;
    e->msg[0] = '\0';
    char *result = NULL;
    bool pasting = false;
    if (hook) hook(e, str_cstr(&e->mb_text), KEY_NONE, ud);

    for (;;) {
        render_frame(e);
        Key k = editor_read_key(e);
        if (k == KEY_NONE || k == KEY_RESIZE) continue;
        if (k == KEY_PASTE_START || k == KEY_PASTE_END) {
            pasting = k == KEY_PASTE_START;
            continue;
        }
        if (!pasting && hook && hook(e, str_cstr(&e->mb_text), k, ud))
            continue;
        bool changed = false;
        if (pasting) {
            if (self_insertable(k)) {
                char enc[4];
                size_t n = utf8_encode((uint32_t)k, enc);
                mb_insert(e, enc, n);
                changed = true;
            }
        } else if (k == 7 || k == 3 || k == KEY_ESC) { /* C-g, nano's ^C */
            break;
        } else if (k == KEY_RET) {
            result = xstrdup(str_cstr(&e->mb_text));
            break;
        } else if (k == KEY_DEL || k == 8) {
            mb_erase(e, mb_prev(e, e->mb_cursor), e->mb_cursor);
            changed = true;
        } else if (k == KEY_DELETE || k == 4) {
            mb_erase(e, e->mb_cursor, mb_next(e, e->mb_cursor));
            changed = true;
        } else if (k == KEY_LEFT || k == 2) {
            e->mb_cursor = mb_prev(e, e->mb_cursor);
        } else if (k == KEY_RIGHT || k == 6) {
            e->mb_cursor = mb_next(e, e->mb_cursor);
        } else if (k == KEY_HOME || k == 1) {
            e->mb_cursor = 0;
        } else if (k == KEY_END || k == 5) {
            e->mb_cursor = e->mb_text.len;
        } else if (k == 11) {
            mb_erase(e, e->mb_cursor, e->mb_text.len);
            changed = true;
        } else if (k == 21) {
            mb_erase(e, 0, e->mb_cursor);
            changed = true;
        } else if (k == 25 && e->kill.len) {
            const char *nl = memchr(e->kill.data, '\n', e->kill.len);
            mb_insert(e, e->kill.data,
                      nl ? (size_t)(nl - e->kill.data) : e->kill.len);
            changed = true;
        } else if (k == KEY_TAB && comp) {
            e->msg[0] = '\0';
            comp(e, &e->mb_text);
            e->mb_cursor = e->mb_text.len;
            changed = true;
        } else if (self_insertable(k)) {
            char enc[4];
            size_t n = utf8_encode((uint32_t)k, enc);
            mb_insert(e, enc, n);
            changed = true;
        }
        if (changed && hook) hook(e, str_cstr(&e->mb_text), KEY_NONE, ud);
    }
    e->mb_active = was_active;
    if (!result) editor_message(e, "Quit");
    return result;
}

int editor_ask(Editor *e, const char *question, const char *choices) {
    if (e->headless) return 0;
    bool was_active = e->mb_active;
    e->mb_active = true;
    snprintf(e->mb_prompt, sizeof e->mb_prompt, "%s", question);
    str_clear(&e->mb_text);
    e->mb_cursor = 0;
    int answer = 0;
    for (;;) {
        render_frame(e);
        Key k = editor_read_key(e);
        if (k == KEY_NONE || k == KEY_RESIZE || k == KEY_PASTE_START ||
            k == KEY_PASTE_END)
            continue;
        if (k == 7 || k == 3 || k == KEY_ESC) break;
        if (k > 0 && k < 128 && strchr(choices, (int)k)) {
            answer = (int)k;
            break;
        }
        if (k >= 'A' && k <= 'Z' && strchr(choices, (int)k + 32)) {
            answer = (int)k + 32;
            break;
        }
    }
    e->mb_active = was_active;
    e->msg[0] = '\0';
    return answer;
}

/* ---- buffers ---- */

void editor_unique_name(Editor *e, Buffer *b) {
    char *base = xstrdup(b->name);
    for (int n = 2;; n++) {
        bool clash = false;
        for (size_t i = 0; i < e->nbufs; i++)
            if (e->bufs[i] != b && strcmp(e->bufs[i]->name, b->name) == 0)
                clash = true;
        if (!clash) break;
        Str s = {0};
        str_appendf(&s, "%s<%d>", base, n);
        free(b->name);
        b->name = s.data;
    }
    free(base);
}

void editor_add_buffer(Editor *e, Buffer *b) {
    e->bufs = xreallocarray(e->bufs, e->nbufs + 1, sizeof *e->bufs);
    e->bufs[e->nbufs++] = b;
    editor_unique_name(e, b);
}

void editor_switch(Editor *e, size_t idx) {
    if (idx >= e->nbufs || idx == e->cur) return;
    Buffer *old = editor_buf(e);
    old->mark_active = old->mark_active && !old->shift_selection;
    e->cur = idx;
    e->prefix = NULL;
    render_invalidate(e);
    script_hook(e, "buffer-switch", editor_buf(e));
}

void editor_close(Editor *e, size_t idx) {
    if (idx >= e->nbufs) return;
    lsp_buffer_closed(e, e->bufs[idx]);
    buffer_free(e->bufs[idx]);
    memmove(e->bufs + idx, e->bufs + idx + 1,
            (e->nbufs - idx - 1) * sizeof *e->bufs);
    e->nbufs--;
    if (e->nbufs == 0) {
        Buffer *b = buffer_new("*scratch*");
        if (!b) {
            term_restore();
            fputs("cedit: out of memory\n", stderr);
            exit(1);
        }
        editor_init_buffer(e, b);
        editor_add_buffer(e, b);
    }
    if (e->cur > idx || e->cur >= e->nbufs) e->cur = e->cur ? e->cur - 1 : 0;
    render_invalidate(e);
}

bool editor_any_modified(const Editor *e) {
    for (size_t i = 0; i < e->nbufs; i++)
        if (buffer_modified(e->bufs[i]) && !e->bufs[i]->readonly) return true;
    return false;
}

static bool same_file(const char *a, const char *b) {
    if (strcmp(a, b) == 0) return true;
    char *ra = realpath(a, NULL), *rb = realpath(b, NULL);
    bool same = ra && rb && strcmp(ra, rb) == 0;
    free(ra);
    free(rb);
    return same;
}

/* Follow the file's existing indentation style. */
static void detect_indent(Editor *e, Buffer *b) {
    if (b->syntax && strcmp(syntax_name(b->syntax), "Makefile") == 0) {
        b->expand_tabs = false;
        return;
    }
    size_t tabs = 0, spaces = 0, n = text_lines(&b->text);
    for (size_t l = 0; l < n && l < 2000; l++) {
        size_t s = text_line_start(&b->text, l);
        if (s >= buffer_len(b)) break;
        unsigned char c = text_byte(&b->text, s);
        if (c == '\t') tabs++;
        else if (c == ' ' && s + 1 < buffer_len(b) &&
                 text_byte(&b->text, s + 1) == ' ')
            spaces++;
    }
    if (tabs > spaces) b->expand_tabs = false;
    else if (spaces > tabs) b->expand_tabs = true;
    else b->expand_tabs = e->cfg.expand_tabs;
}

Buffer *editor_open(Editor *e, const char *path) {
    for (size_t i = 0; i < e->nbufs; i++) {
        if (e->bufs[i]->path && same_file(e->bufs[i]->path, path)) {
            editor_switch(e, i);
            return e->bufs[i];
        }
    }
    Buffer *b = buffer_new(path);
    if (!b) {
        editor_error(e, "Out of memory");
        return NULL;
    }
    editor_init_buffer(e, b);
    char err[512];
    bool is_new;
    if (buffer_load(b, path, &is_new, err, sizeof err) != 0) {
        editor_error(e, "%s", err);
        buffer_free(b);
        return NULL;
    }
    detect_indent(e, b);
    /* replace an untouched initial scratch buffer */
    size_t replace = SIZE_MAX;
    if (e->nbufs == 1 && !e->bufs[0]->path && buffer_len(e->bufs[0]) == 0 &&
        !buffer_modified(e->bufs[0]))
        replace = 0;
    editor_add_buffer(e, b);
    size_t idx = e->nbufs - 1;
    e->cur = idx;
    if (replace != SIZE_MAX) {
        editor_close(e, replace);
        e->cur = e->nbufs - 1;
    }
    render_invalidate(e);
    char safe[256];
    sanitize_for_display(path, safe, sizeof safe);
    if (is_new)
        editor_message(e, "(New file) %s", safe);
    else if (b->crlf)
        editor_message(e, "Read %s (CRLF line endings)", safe);
    recent_add(e, path);
    script_hook(e, "open", b);
    lsp_buffer_opened(e, b);
    return b;
}

void editor_show_text(Editor *e, const char *name, const char *text) {
    long idx = -1;
    for (size_t i = 0; i < e->nbufs; i++)
        if (e->bufs[i]->special_name &&
            strcmp(e->bufs[i]->special_name, name) == 0) idx = (long)i;
    Buffer *b;
    if (idx < 0) {
        b = buffer_new(name);
        if (!b) {
            editor_error(e, "Out of memory");
            return;
        }
        b->special_name = xstrdup(name);
        editor_init_buffer(e, b);
        editor_add_buffer(e, b);
        idx = (long)e->nbufs - 1;
    }
    b = e->bufs[idx];
    if (text_set(&b->text, text, strlen(text)) != 0) {
        editor_error(e, "Out of memory");
        return;
    }
    undo_clear(&b->undo);
    b->readonly = true;
    b->cursor = b->mark = b->top = b->left = 0;
    b->mark_active = false;
    b->hl_valid = 0;
    editor_switch(e, (size_t)idx);
}

double editor_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- recent files ---- */

#define RECENT_MAX 100

void recent_load(Editor *e) {
    char *path = state_path("recent");
    if (!path) return;
    char *data;
    size_t len;
    char err[256];
    if (file_read_all(path, &data, &len, NULL, err, sizeof err) == 0) {
        size_t i = 0;
        while (i < len && e->nrecent < RECENT_MAX) {
            const char *nl = memchr(data + i, '\n', len - i);
            size_t n = nl ? (size_t)(nl - (data + i)) : len - i;
            if (n && data[i] == '/') {
                e->recent = xreallocarray(e->recent, e->nrecent + 1,
                                          sizeof *e->recent);
                e->recent[e->nrecent++] = xstrndup(data + i, n);
            }
            i += n + 1;
        }
        free(data);
    }
    free(path);
}

void recent_add(Editor *e, const char *p) {
    char *real = realpath(p, NULL);
    if (!real || strchr(real, '\n')) {
        free(real);
        return;
    }
    for (size_t i = 0; i < e->nrecent; i++) {
        if (strcmp(e->recent[i], real) == 0) {
            free(e->recent[i]);
            memmove(e->recent + i, e->recent + i + 1,
                    (e->nrecent - i - 1) * sizeof *e->recent);
            e->nrecent--;
            break;
        }
    }
    if (e->nrecent == RECENT_MAX) free(e->recent[--e->nrecent]);
    e->recent = xreallocarray(e->recent, e->nrecent + 1, sizeof *e->recent);
    memmove(e->recent + 1, e->recent, e->nrecent * sizeof *e->recent);
    e->recent[0] = real;
    e->nrecent++;
    if (e->headless) return;

    char *path = state_path("recent");
    if (!path) return;
    Str s = {0};
    for (size_t i = 0; i < e->nrecent; i++) {
        str_appendz(&s, e->recent[i]);
        str_putc(&s, '\n');
    }
    FileStamp st;
    bool in_place;
    char err[256];
    file_write(path, str_cstr(&s), s.len, "", 0, false, &st, &in_place, err,
               sizeof err); /* best effort: history is not critical */
    str_free(&s);
    free(path);
}

/* ---- main loop ---- */

static void check_disk(Editor *e) {
    time_t now = time(NULL);
    if (now - e->last_disk_check < 2) return;
    e->last_disk_check = now;
    Buffer *b = editor_buf(e);
    if (b->disk_changed_warned || !b->path) return;
    if (buffer_disk_changed(b)) {
        b->disk_changed_warned = true;
        editor_error(e, "File changed on disk; M-x revert-buffer reloads it");
    }
}

void editor_run(Editor *e) {
    while (!e->quit) {
        if (e->inlen == 0) render_frame(e); /* coalesce pastes/key bursts */
        Key k = editor_read_key(e);
        editor_handle_key(e, k);
        check_disk(e);
    }
}
