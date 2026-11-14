/* File, buffer and miscellaneous editor commands. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cedit/commands.h"
#include "cedit/editor.h"
#include "cedit/lsp.h"
#include "cedit/script.h"
#include "cedit/utf8.h"

/* ---- completion ---- */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Complete text (whose last `keep` bytes are the completable part) from
 * candidates; text[0..len-keep) is kept as prefix. */
static void complete_list(Editor *e, Str *text, size_t keep,
                          char **cands, size_t n) {
    size_t base = text->len - keep;
    const char *want = str_cstr(text) + base;
    size_t wl = strlen(want);
    char **match = xcalloc(n ? n : 1, sizeof *match);
    size_t m = 0;
    for (size_t i = 0; i < n; i++)
        if (strncmp(cands[i], want, wl) == 0) match[m++] = cands[i];
    if (m == 0) {
        editor_message(e, "No match");
        free(match);
        return;
    }
    qsort(match, m, sizeof *match, cmp_str);
    size_t common = strlen(match[0]);
    for (size_t i = 1; i < m; i++) {
        size_t j = 0;
        while (j < common && match[i][j] == match[0][j]) j++;
        common = j;
    }
    Str out = {0};
    str_append(&out, str_cstr(text), base);
    str_append(&out, match[0], common);
    str_free(text);
    *text = out;
    if (m > 1) {
        Str list = {0};
        for (size_t i = 0; i < m && list.len < 300; i++) {
            const char *slash = strrchr(match[i], '/');
            const char *shown = slash && slash[1] ? slash + 1 : match[i];
            str_appendf(&list, "%s%s", i ? " " : "", shown);
        }
        if (m > 1 && list.len >= 300) str_appendz(&list, " ...");
        editor_message(e, "%s", str_cstr(&list));
        str_free(&list);
    }
    free(match);
}

static char *expand_home(const char *p) {
    if (p[0] == '~' && (p[1] == '/' || p[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home) {
            Str s = {0};
            str_appendz(&s, home);
            str_appendz(&s, p + 1);
            return s.data;
        }
    }
    return xstrdup(p);
}

static void complete_file(Editor *e, Str *text) {
    char *full = expand_home(str_cstr(text));
    char *slash = strrchr(full, '/');
    Str dir = {0};
    const char *prefix;
    if (slash) {
        str_append(&dir, full, (size_t)(slash - full) + 1);
        prefix = slash + 1;
    } else {
        prefix = full;
    }
    DIR *d = opendir(dir.len ? str_cstr(&dir) : ".");
    if (!d) {
        editor_message(e, "No match");
        free(full);
        str_free(&dir);
        return;
    }
    char **cands = NULL;
    size_t n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < 10000) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (de->d_name[0] == '.' && prefix[0] != '.') continue;
        if (strncmp(de->d_name, prefix, strlen(prefix)) != 0) continue;
        Str c = {0};
        str_appendz(&c, str_cstr(&dir));
        str_appendz(&c, de->d_name);
        struct stat st;
        if (stat(c.data, &st) == 0 && S_ISDIR(st.st_mode)) str_putc(&c, '/');
        cands = xreallocarray(cands, n + 1, sizeof *cands);
        cands[n++] = c.data;
    }
    closedir(d);
    str_clear(text);
    str_appendz(text, full);
    complete_list(e, text, text->len, cands, n);
    for (size_t i = 0; i < n; i++) free(cands[i]);
    free(cands);
    free(full);
    str_free(&dir);
}

static void complete_command(Editor *e, Str *text) {
    char **c = xcalloc(e->ncmds ? e->ncmds : 1, sizeof *c);
    for (size_t i = 0; i < e->ncmds; i++) c[i] = e->cmds[i].name;
    complete_list(e, text, text->len, c, e->ncmds);
    free(c);
}

static void complete_buffer(Editor *e, Str *text) {
    char **c = xcalloc(e->nbufs, sizeof *c);
    for (size_t i = 0; i < e->nbufs; i++) c[i] = e->bufs[i]->name;
    complete_list(e, text, text->len, c, e->nbufs);
    free(c);
}

static void complete_recent(Editor *e, Str *text) {
    complete_list(e, text, text->len, e->recent, e->nrecent);
}

static void complete_syntax(Editor *e, Str *text) {
    static char *names[] = {"C", "C++", "Rust", "Go", "Python", "Shell",
                            "JavaScript", "JSON", "TOML", "Markdown",
                            "Makefile", "Text"};
    complete_list(e, text, text->len, names, sizeof names / sizeof names[0]);
}

/* ---- helpers ---- */

static const char *safe(const char *s, char *buf, size_t n) {
    return sanitize_for_display(s, buf, n);
}

static char *current_dir_prefix(Buffer *b) {
    Str s = {0};
    if (b->path && strchr(b->path, '/')) {
        str_append(&s, b->path, (size_t)(strrchr(b->path, '/') - b->path) + 1);
    }
    return s.data ? s.data : xstrdup("");
}

static long find_buffer(Editor *e, const char *name) {
    for (size_t i = 0; i < e->nbufs; i++)
        if (strcmp(e->bufs[i]->name, name) == 0) return (long)i;
    return -1;
}

static bool save_to(Editor *e, Buffer *b, const char *path) {
    char err[512], s[256];
    bool in_place = false;
    if (buffer_save(b, path, &in_place, err, sizeof err) != 0) {
        editor_error(e, "%s", err);
        return false;
    }
    editor_message(e, "Wrote %s (%zu lines)%s", safe(path, s, sizeof s),
                   text_lines(&b->text),
                   in_place ? " [rewritten in place]" : "");
    recent_add(e, path);
    script_hook(e, "save", b);
    lsp_buffer_saved(e, b);
    return true;
}

static bool write_file_prompt(Editor *e, Buffer *b) {
    char *init = current_dir_prefix(b);
    char *in = editor_prompt(e, "Write file: ", init, NULL, NULL,
                             complete_file);
    free(init);
    if (!in) return false;
    char *path = expand_home(in);
    free(in);
    if (!*path) {
        free(path);
        return false;
    }
    struct stat st;
    if (stat(path, &st) == 0 && (!b->path || strcmp(b->path, path) != 0)) {
        char s[256], q[320];
        snprintf(q, sizeof q, "%s exists; overwrite? (y/n)",
                 safe(path, s, sizeof s));
        if (editor_ask(e, q, "yn") != 'y') {
            free(path);
            return false;
        }
    }
    bool ok = save_to(e, b, path);
    if (ok) {
        const char *base = strrchr(path, '/');
        free(b->name);
        b->name = xstrdup(base && base[1] ? base + 1 : path);
        editor_unique_name(e, b);
        const Syntax *syn = syntax_detect(path, NULL, 0);
        if (syn) buffer_set_syntax(b, syn);
    }
    free(path);
    return ok;
}

static bool save_buffer(Editor *e, Buffer *b) {
    if (b->readonly) {
        editor_error(e, "Buffer is read-only");
        return false;
    }
    if (!b->path) return write_file_prompt(e, b);
    if (buffer_disk_changed(b) &&
        editor_ask(e, "File changed on disk since it was read; overwrite? "
                      "(y/n)", "yn") != 'y')
        return false;
    return save_to(e, b, b->path);
}

/* ---- commands ---- */

static void cmd_save_buffer(Editor *e) {
    Buffer *b = editor_buf(e);
    if (b->path && !buffer_modified(b) && !buffer_disk_changed(b)) {
        editor_message(e, "(No changes need to be saved)");
        return;
    }
    save_buffer(e, b);
}

static void cmd_write_file(Editor *e) {
    Buffer *b = editor_buf(e);
    if (b->readonly) {
        editor_error(e, "Buffer is read-only");
        return;
    }
    write_file_prompt(e, b);
}

static void cmd_find_file(Editor *e) {
    char *init = current_dir_prefix(editor_buf(e));
    char *in = editor_prompt(e, "Find file: ", init, NULL, NULL,
                             complete_file);
    free(init);
    if (!in) return;
    char *path = expand_home(in);
    free(in);
    if (*path) editor_open(e, path);
    free(path);
}

static void cmd_recent_files(Editor *e) {
    if (e->nrecent == 0) {
        editor_error(e, "No recent files");
        return;
    }
    char *in = editor_prompt(e, "Recent file (TAB completes): ", NULL, NULL,
                             NULL, complete_recent);
    if (!in) return;
    const char *path = *in ? in : e->recent[0];
    char *p = xstrdup(path);
    free(in);
    editor_open(e, p);
    free(p);
}

static void cmd_kill_buffer(Editor *e) {
    char prompt[320], s[256];
    snprintf(prompt, sizeof prompt, "Kill buffer (default %s): ",
             safe(editor_buf(e)->name, s, sizeof s));
    char *in = editor_prompt(e, prompt, NULL, NULL, NULL, complete_buffer);
    if (!in) return;
    long idx = *in ? find_buffer(e, in) : (long)e->cur;
    free(in);
    if (idx < 0) {
        editor_error(e, "No such buffer");
        return;
    }
    Buffer *b = e->bufs[idx];
    if (buffer_modified(b) && !b->readonly &&
        editor_ask(e, "Buffer modified; kill anyway? (y/n)", "yn") != 'y')
        return;
    editor_close(e, (size_t)idx);
}

static void cmd_switch_to_buffer(Editor *e) {
    size_t other = (e->cur + 1) % e->nbufs;
    char prompt[320], s[256];
    snprintf(prompt, sizeof prompt, "Switch to buffer (default %s): ",
             safe(e->bufs[other]->name, s, sizeof s));
    char *in = editor_prompt(e, prompt, NULL, NULL, NULL, complete_buffer);
    if (!in) return;
    if (!*in) {
        editor_switch(e, other);
    } else {
        long idx = find_buffer(e, in);
        if (idx < 0) {
            Buffer *b = buffer_new(in);
            if (b) {
                editor_init_buffer(e, b);
                editor_add_buffer(e, b);
                idx = (long)e->nbufs - 1;
            }
        }
        if (idx >= 0) editor_switch(e, (size_t)idx);
    }
    free(in);
}

static void cmd_next_buffer(Editor *e) {
    editor_switch(e, (e->cur + 1) % e->nbufs);
}

static void cmd_previous_buffer(Editor *e) {
    editor_switch(e, (e->cur + e->nbufs - 1) % e->nbufs);
}

static void cmd_list_buffers(Editor *e) {
    Str s = {0};
    str_appendf(&s, " M  %-24s %8s  %s\n", "Buffer", "Lines", "File");
    for (size_t i = 0; i < e->nbufs; i++) {
        Buffer *b = e->bufs[i];
        if (strcmp(b->name, "*Buffers*") == 0) continue;
        str_appendf(&s, " %c  %-24s %8zu  %s\n",
                    buffer_modified(b) && !b->readonly ? '*' : ' ', b->name,
                    text_lines(&b->text), b->path ? b->path : "");
    }
    str_appendz(&s, "\nC-x b switches buffers, C-x k kills one.\n");
    editor_show_text(e, "*Buffers*", str_cstr(&s));
    str_free(&s);
}

static void cmd_revert_buffer(Editor *e) {
    Buffer *b = editor_buf(e);
    if (!b->path) {
        editor_error(e, "Buffer is not visiting a file");
        return;
    }
    if (buffer_modified(b) &&
        editor_ask(e, "Discard changes and reload from disk? (y/n)", "yn") !=
            'y')
        return;
    size_t cursor = b->cursor, top = b->top;
    char *path = xstrdup(b->path), err[512];
    bool is_new;
    if (buffer_load(b, path, &is_new, err, sizeof err) != 0) {
        editor_error(e, "%s", err);
    } else {
        b->cursor = cursor > buffer_len(b) ? buffer_len(b) : cursor;
        b->top = top;
        editor_unique_name(e, b);
        editor_message(e, "Reverted");
    }
    free(path);
}

/* Offer to save each modified buffer. Returns false if cancelled. */
static bool save_some(Editor *e) {
    bool all = false;
    for (size_t i = 0; i < e->nbufs; i++) {
        Buffer *b = e->bufs[i];
        if (!buffer_modified(b) || b->readonly) continue;
        int a = 'y';
        if (!all) {
            editor_switch(e, i);
            char q[400], s[256];
            snprintf(q, sizeof q, "Save %s? (y)es (n)o (!)all (q)uit",
                     safe(b->path ? b->path : b->name, s, sizeof s));
            a = editor_ask(e, q, "yn!q");
        }
        if (a == 0 || a == 'q') return false;
        if (a == 'n') continue;
        if (a == '!') all = true;
        editor_switch(e, i);
        if (!save_buffer(e, b)) return false;
    }
    return true;
}

static void cmd_save_some_buffers(Editor *e) {
    if (save_some(e)) editor_message(e, "Done saving");
}

static void cmd_save_buffers_kill_editor(Editor *e) {
    /* answering "no" for a buffer already means "discard it" */
    if (save_some(e)) e->quit = true;
}

static void cmd_suspend(Editor *e) {
    if (e->headless) return;
    term_suspend();
    term_size(&e->rows, &e->cols);
    render_invalidate(e);
}

static void cmd_execute_extended_command(Editor *e) {
    char *in = editor_prompt(e, "M-x ", NULL, NULL, NULL, complete_command);
    if (!in) return;
    const Command *c = command_find(e, in);
    if (!c)
        editor_error(e, "No such command: %s", in);
    else
        editor_run_command(e, c, false);
    free(in);
}

static void cmd_describe_bindings(Editor *e) {
    Str s = {0};
    str_appendf(&s, "Key bindings (%s keymap)\n\n",
                e->cfg.keymap == KEYMAP_SIMPLE  ? "simple"
                : e->cfg.keymap == KEYMAP_EMACS ? "emacs" : "nano");
    keymap_describe(e->keymap, &s);
    str_appendz(&s, "\nPrintable keys insert themselves. Shift+movement "
                    "selects.\n\nCommands (M-x NAME):\n\n");
    for (size_t i = 0; i < e->ncmds; i++)
        str_appendf(&s, "%-28s %s\n", e->cmds[i].name,
                    e->cmds[i].doc ? e->cmds[i].doc : "");
    editor_show_text(e, "*Help*", str_cstr(&s));
    str_free(&s);
}

static void cmd_set_option(Editor *e) {
    char *in = editor_prompt(e, "Set option (e.g. tab-width 4): ", NULL,
                             NULL, NULL, NULL);
    if (!in) return;
    char err[256];
    if (config_apply_line(e, in, err, sizeof err) != 0)
        editor_error(e, "%s", err);
    else
        editor_message(e, "OK: %s", in);
    free(in);
}

static void cmd_set_syntax(Editor *e) {
    char *in = editor_prompt(e, "Language: ", NULL, NULL, NULL,
                             complete_syntax);
    if (!in) return;
    const Syntax *s = syntax_by_name(in);
    if (!s && strcmp(in, "Text") != 0 && *in)
        editor_error(e, "Unknown language (%s)", syntax_names());
    else
        buffer_set_syntax(editor_buf(e), s);
    free(in);
}

static void cmd_toggle_line_numbers(Editor *e) {
    e->cfg.line_numbers = (e->cfg.line_numbers + 1) % 3;
    static const char *names[] = {"off", "absolute", "relative"};
    editor_message(e, "Line numbers: %s", names[e->cfg.line_numbers]);
}

static void cmd_toggle_line_endings(Editor *e) {
    Buffer *b = editor_buf(e);
    b->crlf = !b->crlf;
    b->undo.saved_seq = (unsigned long)-1; /* force "modified" */
    editor_message(e, "Line endings: %s (save to apply)", b->crlf ? "CRLF" : "LF");
}

static void cmd_cursor_position(Editor *e) {
    Buffer *b = editor_buf(e);
    size_t line = buffer_cur_line(b), n = text_lines(&b->text);
    size_t len = buffer_len(b);
    size_t ls = text_line_start(&b->text, line);
    size_t ll = text_line_end(&b->text, line) - ls;
    editor_message(e, "line %zu/%zu (%zu%%), col %zu/%zu, char %zu/%zu (%zu%%)",
                   line + 1, n, (line + 1) * 100 / n, b->cursor - ls + 1, ll + 1,
                   b->cursor + 1, len + 1, len ? b->cursor * 100 / len : 100);
}

static void cmd_lua_eval(Editor *e) {
    if (!script_available()) {
        editor_error(e, "Built without Lua support (make LUA=1)");
        return;
    }
    char *in = editor_prompt(e, "Lua: ", NULL, NULL, NULL, NULL);
    if (!in) return;
    script_eval(e, in);
    free(in);
}

void commands_register_file(Editor *e) {
    static const struct {
        const char *name;
        CommandFn fn;
        const char *doc;
    } t[] = {
        {"save-buffer", cmd_save_buffer, "Save the current buffer"},
        {"write-file", cmd_write_file, "Save the buffer under a new name"},
        {"find-file", cmd_find_file, "Open a file (TAB completes)"},
        {"recent-files", cmd_recent_files, "Open a recently used file"},
        {"kill-buffer", cmd_kill_buffer, "Close a buffer"},
        {"switch-to-buffer", cmd_switch_to_buffer, "Switch to another buffer"},
        {"next-buffer", cmd_next_buffer, "Switch to the next buffer"},
        {"previous-buffer", cmd_previous_buffer, "Switch to the previous buffer"},
        {"list-buffers", cmd_list_buffers, "Show the buffer list"},
        {"revert-buffer", cmd_revert_buffer, "Reload the file from disk"},
        {"save-some-buffers", cmd_save_some_buffers, "Offer to save each modified buffer"},
        {"save-buffers-kill-editor", cmd_save_buffers_kill_editor, "Offer to save, then quit"},
        {"suspend", cmd_suspend, "Suspend to the shell (fg resumes)"},
        {"execute-extended-command", cmd_execute_extended_command, "Run a command by name"},
        {"describe-bindings", cmd_describe_bindings, "List key bindings and commands"},
        {"set-option", cmd_set_option, "Apply a configuration line"},
        {"set-syntax", cmd_set_syntax, "Choose the highlighting language"},
        {"toggle-line-numbers", cmd_toggle_line_numbers, "Cycle line numbers off/absolute/relative"},
        {"toggle-line-endings", cmd_toggle_line_endings, "Switch the buffer between LF and CRLF"},
        {"cursor-position", cmd_cursor_position, "Report line, column and character position"},
        {"lua-eval", cmd_lua_eval, "Evaluate a Lua expression (Lua builds only)"},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        command_register(e, t[i].name, t[i].fn, t[i].doc, 0, -1);
}
