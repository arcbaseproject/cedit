#include "cedit/config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cedit/editor.h"
#include "cedit/lsp.h"

void config_defaults(Config *c) {
    c->tab_width = 4;
    c->expand_tabs = true;
    c->auto_indent = true;
    c->auto_pair = true;
    c->line_numbers = LINENR_ABSOLUTE;
    c->keymap = KEYMAP_NANO;
    c->colors = -1;
    c->load_plugins = true;
}

static int parse_bool(const char *s, bool *out) {
    if (!strcmp(s, "yes") || !strcmp(s, "on") || !strcmp(s, "true") ||
        !strcmp(s, "1")) {
        *out = true;
        return 0;
    }
    if (!strcmp(s, "no") || !strcmp(s, "off") || !strcmp(s, "false") ||
        !strcmp(s, "0")) {
        *out = false;
        return 0;
    }
    return -1;
}

static int parse_color(const char *s, int32_t *out) {
    if (!strcmp(s, "-") || !strcmp(s, "default")) {
        *out = -1;
        return 0;
    }
    if (s[0] != '#' || strlen(s) != 7) return -1;
    char *end;
    long v = strtol(s + 1, &end, 16);
    if (*end) return -1;
    *out = (int32_t)v;
    return 0;
}

#define MAXTOK 16

int config_apply_line(Editor *e, const char *line, char *err, size_t errlen) {
    char buf[1024];
    char *tok[MAXTOK];
    int n = 0;
    if (strlen(line) >= sizeof buf) {
        snprintf(err, errlen, "line too long");
        return -1;
    }
    snprintf(buf, sizeof buf, "%s", line);
    for (char *p = buf; *p && n < MAXTOK;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p || (*p == '#' && n == 0)) break; /* comment lines */
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    if (n == 0) return 0;
    const char *k = tok[0];
    bool bv;

    if (!strcmp(k, "bind") || !strcmp(k, "unbind")) {
        bool bind = k[0] == 'b';
        if (n < (bind ? 3 : 2)) {
            snprintf(err, errlen, "usage: %s", bind ? "bind KEYS... COMMAND"
                                                    : "unbind KEYS...");
            return -1;
        }
        char seq[256] = "";
        int last = bind ? n - 1 : n;
        for (int i = 1; i < last; i++) {
            size_t used = strlen(seq);
            snprintf(seq + used, sizeof seq - used, "%s%s", i > 1 ? " " : "",
                     tok[i]);
        }
        return keymap_bind(e->keymap, seq, bind ? tok[n - 1] : NULL, err,
                           errlen);
    }
    if (!strcmp(k, "lsp")) return lsp_config(e, n, tok, err, errlen);
    if (n != 2 && strcmp(k, "face") != 0) {
        snprintf(err, errlen, "usage: %s VALUE", k);
        return -1;
    }
    const char *v = tok[1];
    if (!strcmp(k, "tab-width")) {
        char *end;
        long w = strtol(v, &end, 10);
        if (*end || w < 1 || w > 16) {
            snprintf(err, errlen, "tab-width must be 1-16");
            return -1;
        }
        e->cfg.tab_width = (int)w;
        for (size_t i = 0; i < e->nbufs; i++) e->bufs[i]->tab_width = (int)w;
        render_invalidate(e);
    } else if (!strcmp(k, "expand-tabs")) {
        if (parse_bool(v, &bv)) goto badbool;
        e->cfg.expand_tabs = bv;
        for (size_t i = 0; i < e->nbufs; i++) e->bufs[i]->expand_tabs = bv;
    } else if (!strcmp(k, "auto-indent")) {
        if (parse_bool(v, &bv)) goto badbool;
        e->cfg.auto_indent = bv;
    } else if (!strcmp(k, "auto-pair")) {
        if (parse_bool(v, &bv)) goto badbool;
        e->cfg.auto_pair = bv;
    } else if (!strcmp(k, "plugins")) {
        if (parse_bool(v, &bv)) goto badbool;
        e->cfg.load_plugins = bv;
    } else if (!strcmp(k, "line-numbers")) {
        if (!strcmp(v, "off") || !strcmp(v, "no"))
            e->cfg.line_numbers = LINENR_OFF;
        else if (!strcmp(v, "on") || !strcmp(v, "absolute") || !strcmp(v, "yes"))
            e->cfg.line_numbers = LINENR_ABSOLUTE;
        else if (!strcmp(v, "relative"))
            e->cfg.line_numbers = LINENR_RELATIVE;
        else {
            snprintf(err, errlen, "line-numbers: off, absolute or relative");
            return -1;
        }
    } else if (!strcmp(k, "keymap")) {
        if (!strcmp(v, "nano")) e->cfg.keymap = KEYMAP_NANO;
        else if (!strcmp(v, "emacs")) e->cfg.keymap = KEYMAP_EMACS;
        else if (!strcmp(v, "simple")) e->cfg.keymap = KEYMAP_SIMPLE;
        else {
            snprintf(err, errlen, "keymap: nano, emacs or simple");
            return -1;
        }
        editor_reset_keymap(e);
    } else if (!strcmp(k, "theme")) {
        if (!theme_builtin(e->styles, v)) {
            snprintf(err, errlen, "unknown theme (%s)", theme_names());
            return -1;
        }
        render_invalidate(e);
    } else if (!strcmp(k, "colors")) {
        if (!strcmp(v, "auto")) e->cfg.colors = -1;
        else if (!strcmp(v, "16")) e->cfg.colors = COLOR_16;
        else if (!strcmp(v, "256")) e->cfg.colors = COLOR_256;
        else if (!strcmp(v, "truecolor")) e->cfg.colors = COLOR_TRUE;
        else {
            snprintf(err, errlen, "colors: auto, 16, 256 or truecolor");
            return -1;
        }
        render_invalidate(e);
    } else if (!strcmp(k, "face")) {
        if (n < 3) {
            snprintf(err, errlen, "usage: face SLOT FG [BG] [bold] [italic] "
                                  "[underline] [reverse]");
            return -1;
        }
        int slot = style_slot(tok[1]);
        if (slot < 0) {
            snprintf(err, errlen, "unknown face '%s'", tok[1]);
            return -1;
        }
        Style st = {-1, -1, 0};
        if (parse_color(tok[2], &st.fg)) goto badcolor;
        int i = 3;
        if (i < n && (tok[i][0] == '#' || tok[i][0] == '-' ||
                      !strcmp(tok[i], "default"))) {
            if (parse_color(tok[i], &st.bg)) goto badcolor;
            i++;
        }
        for (; i < n; i++) {
            if (!strcmp(tok[i], "bold")) st.attr |= A_BOLD;
            else if (!strcmp(tok[i], "italic")) st.attr |= A_ITALIC;
            else if (!strcmp(tok[i], "underline")) st.attr |= A_UNDERLINE;
            else if (!strcmp(tok[i], "reverse")) st.attr |= A_REVERSE;
            else {
                snprintf(err, errlen, "unknown attribute '%s'", tok[i]);
                return -1;
            }
        }
        e->styles[slot] = st;
        render_invalidate(e);
    } else {
        snprintf(err, errlen, "unknown option '%s'", k);
        return -1;
    }
    return 0;

badbool:
    snprintf(err, errlen, "%s: expected yes or no", k);
    return -1;
badcolor:
    snprintf(err, errlen, "colors are #rrggbb or -");
    return -1;
}

int config_load_file(Editor *e, const char *path, Str *errors) {
    char *data;
    size_t len;
    char err[256];
    int rc = file_read_all(path, &data, &len, NULL, err, sizeof err);
    if (rc == ENOENT) return 0;
    if (rc) {
        str_appendf(errors, "%s\n", err);
        return 1;
    }
    int nerr = 0, lineno = 0;
    size_t i = 0;
    while (i < len) {
        const char *nl = memchr(data + i, '\n', len - i);
        size_t n = nl ? (size_t)(nl - (data + i)) : len - i;
        lineno++;
        char *line = xstrndup(data + i, n);
        if (strlen(line) != n) { /* embedded NUL */
            str_appendf(errors, "%s:%d: invalid byte\n", path, lineno);
            nerr++;
        } else if (config_apply_line(e, line, err, sizeof err) != 0) {
            str_appendf(errors, "%s:%d: %s\n", path, lineno, err);
            nerr++;
        }
        free(line);
        i += n + 1;
    }
    free(data);
    return nerr;
}

static char *xdg_path(const char *env, const char *fallback, const char *name,
                      bool create) {
    const char *base = getenv(env);
    Str dir = {0};
    if (base && base[0] == '/') {
        str_appendz(&dir, base);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return NULL;
        str_appendf(&dir, "%s/%s", home, fallback);
    }
    if (create) { /* mkdir -p, private */
        for (size_t i = 1; i <= dir.len; i++) {
            if (i == dir.len || dir.data[i] == '/') {
                char c = dir.data[i];
                dir.data[i] = '\0';
                mkdir(dir.data, 0700);
                dir.data[i] = c;
            }
        }
        str_appendz(&dir, "/cedit");
        mkdir(dir.data, 0700);
    } else {
        str_appendz(&dir, "/cedit");
    }
    str_appendf(&dir, "/%s", name);
    return dir.data;
}

char *config_path(const char *name) {
    return xdg_path("XDG_CONFIG_HOME", ".config", name, false);
}

char *state_path(const char *name) {
    return xdg_path("XDG_STATE_HOME", ".local/state", name, true);
}
