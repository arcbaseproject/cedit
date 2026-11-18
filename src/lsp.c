#include "cedit/lsp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cedit/editor.h"
#include "cedit/json.h"
#include "cedit/utf8.h"

#define MAX_MSG (64u << 20)     /* refuse absurd messages from servers */
#define SYNC_DELAY 0.3          /* seconds after the last key */
#define REQUEST_TIMEOUT 5.0
#define MAX_ITEMS 2000

typedef struct {
    char *lang;
    char **argv;    /* NULL-terminated; NULL when off */
} Conf;

typedef struct {
    char *lang;
    char *name;
    pid_t pid;
    int to_fd, from_fd;
    Str rbuf, wbuf, pending;
    bool ready, dead, utf8;
    long next_id, init_id;
    bool cap_completion, cap_definition, cap_hover;
} Server;

typedef struct {
    Buffer *b;
    Server *s;
    char *uri;
    unsigned long sent;
} Doc;

typedef struct {
    char *path;
    size_t pos;
} Jump;

typedef struct {
    bool disabled;
    Conf *conf;
    size_t nconf;
    Server **servers;
    size_t nservers;
    Doc *docs;
    size_t ndocs;
    char **warned;
    size_t nwarned;
    Server *wait_server;
    long wait_id;
    Json *wait_msg;
    bool wait_done;
    Jump jumps[64];
    int njumps;
    char status[64];
} Lsp;

static Lsp *state(Editor *e) {
    if (!e->lsp) e->lsp = xcalloc(1, sizeof(Lsp));
    return e->lsp;
}

static const struct {
    const char *lang, *id;
    const char *argv[4];
} defaults[] = {
    {"C", "c", {"clangd", NULL}},
    {"C++", "cpp", {"clangd", NULL}},
    {"Rust", "rust", {"rust-analyzer", NULL}},
    {"Go", "go", {"gopls", NULL}},
    {"Python", "python", {"pylsp", NULL}},
    {"Python", "python", {"pyright-langserver", "--stdio", NULL}},
    {"Python", "python", {"basedpyright-langserver", "--stdio", NULL}},
    {"Shell", "shellscript", {"bash-language-server", "start", NULL}},
    {"JavaScript", "javascript", {"typescript-language-server", "--stdio", NULL}},
    {"JSON", "json", {"vscode-json-language-server", "--stdio", NULL}},
    {"TOML", "toml", {"taplo", "lsp", "stdio", NULL}},
    {"Markdown", "markdown", {"marksman", "server", NULL}},
};
#define NDEFAULTS (sizeof defaults / sizeof defaults[0])

static const char *language_id(const char *lang) {
    for (size_t i = 0; i < NDEFAULTS; i++)
        if (!strcmp(defaults[i].lang, lang)) return defaults[i].id;
    return "plaintext";
}

static bool in_path(const char *name) {
    if (strchr(name, '/')) return access(name, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        char full[PATH_MAX];
        if (n && snprintf(full, sizeof full, "%.*s/%s", (int)n, path, name) <
                     (int)sizeof full &&
            access(full, X_OK) == 0)
            return true;
        path += n + (end ? 1 : 0);
    }
    return false;
}

/* ---- URIs and positions ---- */

static char *path_to_uri(const char *path) {
    char *real = realpath(path, NULL);
    const char *p = real ? real : path;
    Str s = {0};
    str_appendz(&s, "file://");
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || strchr("-._~/", c))
            str_putc(&s, (char)c);
        else
            str_appendf(&s, "%%%02X", c);
    }
    free(real);
    return s.data;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *uri_to_path(const char *uri) {
    if (!uri || strncmp(uri, "file://", 7) != 0) return NULL;
    Str s = {0};
    for (const char *p = uri + 7; *p; p++) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            char c = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            if (c == '\0') {
                str_free(&s);
                return NULL;
            }
            str_putc(&s, c);
            p += 2;
        } else {
            str_putc(&s, *p);
        }
    }
    return s.data;
}

/* Byte column -> LSP character offset (UTF-8 bytes or UTF-16 units). */
static size_t to_lsp_col(const Server *s, Buffer *b, size_t line, size_t col) {
    if (s->utf8) return col;
    size_t start = text_line_start(&b->text, line);
    size_t end = text_line_end(&b->text, line);
    if (col > end - start) col = end - start;
    const char *p = text_view(&b->text, start, col);
    size_t i = 0, units = 0;
    while (i < col) {
        uint32_t cp;
        i += utf8_decode(p + i, col - i, &cp, NULL);
        units += cp >= 0x10000 ? 2 : 1;
    }
    return units;
}

/* LSP position -> byte offset in the buffer (clamped). */
static size_t from_lsp_pos(const Server *s, Buffer *b, const Json *pos) {
    double l = json_num(json_get(pos, "line"), 0);
    double c = json_num(json_get(pos, "character"), 0);
    size_t n = text_lines(&b->text);
    size_t line = l < 0 ? 0 : (l >= (double)n ? n - 1 : (size_t)l);
    size_t want = c < 0 ? 0 : (c > 1e9 ? (size_t)1e9 : (size_t)c);
    size_t start = text_line_start(&b->text, line);
    size_t len = text_line_end(&b->text, line) - start;
    const char *p = text_view(&b->text, start, len);
    size_t i = 0, units = 0;
    while (i < len && units < want) {
        uint32_t cp;
        size_t k = utf8_decode(p + i, len - i, &cp, NULL);
        units += s->utf8 ? k : (cp >= 0x10000 ? 2 : 1);
        i += k;
    }
    return start + i;
}

/* ---- transport ---- */

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void server_kill(Server *s) {
    if (s->to_fd >= 0) close(s->to_fd);
    if (s->from_fd >= 0) close(s->from_fd);
    s->to_fd = s->from_fd = -1;
    if (s->pid > 0) {
        if (waitpid(s->pid, NULL, WNOHANG) == 0) {
            kill(s->pid, SIGTERM);
            for (int i = 0; i < 20 && waitpid(s->pid, NULL, WNOHANG) == 0; i++)
                sleep_ms(10);
            if (waitpid(s->pid, NULL, WNOHANG) == 0) {
                kill(s->pid, SIGKILL);
                waitpid(s->pid, NULL, 0);
            }
        }
        s->pid = -1;
    }
}

static void server_died(Editor *e, Server *s) {
    if (s->dead) return;
    s->dead = true;
    server_kill(s);
    char name[64];
    sanitize_for_display(s->name, name, sizeof name);
    editor_error(e, "Language server %s stopped (M-x lsp-restart)", name);
}

static void flush(Editor *e, Server *s) {
    size_t off = 0;
    while (off < s->wbuf.len && !s->dead) {
        ssize_t w = write(s->to_fd, s->wbuf.data + off, s->wbuf.len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            server_died(e, s);
            return;
        }
        off += (size_t)w;
    }
    if (off) {
        memmove(s->wbuf.data, s->wbuf.data + off, s->wbuf.len - off);
        s->wbuf.len -= off;
        s->wbuf.data[s->wbuf.len] = '\0';
    }
}

static void frame(Str *q, const Str *body) {
    str_appendf(q, "Content-Length: %zu\r\n\r\n", body->len);
    str_append(q, str_cstr(body), body->len);
}

/* Queue a message; before initialization only `initialize` goes out. */
static void send_msg(Editor *e, Server *s, const Str *body, bool now) {
    if (s->dead) return;
    if (s->ready || now) {
        frame(&s->wbuf, body);
        flush(e, s);
    } else {
        frame(&s->pending, body);
    }
}

static void notify(Editor *e, Server *s, const char *method, const char *params) {
    Str m = {0};
    str_appendf(&m, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
                method, params);
    send_msg(e, s, &m, false);
    str_free(&m);
}

static long request(Editor *e, Server *s, const char *method,
                    const char *params, bool now) {
    long id = ++s->next_id;
    Str m = {0};
    str_appendf(&m, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":\"%s\",\"params\":%s}",
                id, method, params);
    send_msg(e, s, &m, now);
    str_free(&m);
    return id;
}

static void respond(Editor *e, Server *s, const Json *id, const char *result) {
    Str m = {0};
    str_appendz(&m, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id->type == JSON_STR)
        json_quote(&m, id->str, id->len);
    else
        str_appendf(&m, "%.0f", id->num);
    str_appendf(&m, ",\"result\":%s}", result);
    send_msg(e, s, &m, true);
    str_free(&m);
}

/* ---- documents ---- */

static Doc *doc_of(Lsp *L, const Buffer *b) {
    for (size_t i = 0; i < L->ndocs; i++)
        if (L->docs[i].b == b) return &L->docs[i];
    return NULL;
}

static void text_json(Buffer *b, Str *out) {
    size_t len = buffer_len(b);
    json_quote(out, text_view(&b->text, 0, len), len);
}

static void send_open(Editor *e, Doc *d) {
    Str p = {0};
    str_appendz(&p, "{\"textDocument\":{\"uri\":");
    json_quote(&p, d->uri, strlen(d->uri));
    str_appendf(&p, ",\"languageId\":\"%s\",\"version\":%lu,\"text\":",
                language_id(d->s->lang), d->b->version);
    text_json(d->b, &p);
    str_appendz(&p, "}}");
    notify(e, d->s, "textDocument/didOpen", p.data);
    str_free(&p);
    d->sent = d->b->version;
}

static void send_change(Editor *e, Doc *d) {
    if (d->sent == d->b->version || d->s->dead) return;
    Str p = {0};
    str_appendz(&p, "{\"textDocument\":{\"uri\":");
    json_quote(&p, d->uri, strlen(d->uri));
    str_appendf(&p, ",\"version\":%lu},\"contentChanges\":[{\"text\":",
                d->b->version);
    text_json(d->b, &p);
    str_appendz(&p, "}]}");
    notify(e, d->s, "textDocument/didChange", p.data);
    str_free(&p);
    d->sent = d->b->version;
}

static void uri_params(Str *p, const Doc *d) {
    str_appendz(p, "{\"textDocument\":{\"uri\":");
    json_quote(p, d->uri, strlen(d->uri));
    str_appendz(p, "}}");
}

static void pos_params(Str *p, Doc *d, size_t pos) {
    Buffer *b = d->b;
    size_t line = text_line_of(&b->text, pos);
    size_t col = pos - text_line_start(&b->text, line);
    str_appendz(p, "{\"textDocument\":{\"uri\":");
    json_quote(p, d->uri, strlen(d->uri));
    str_appendf(p, "},\"position\":{\"line\":%zu,\"character\":%zu}}", line,
                to_lsp_col(d->s, b, line, col));
}

/* ---- incoming messages ---- */

static void handle_init(Editor *e, Server *s, const Json *m) {
    const Json *caps = json_path(m, "result", "capabilities", NULL);
    if (!caps) {
        const char *err = json_str(json_path(m, "error", "message", NULL));
        editor_error(e, "%s: initialize failed: %s", s->name, err ? err : "?");
        server_died(e, s);
        return;
    }
    const char *enc = json_str(json_get(caps, "positionEncoding"));
    s->utf8 = enc && !strcmp(enc, "utf-8");
    s->cap_completion = json_get(caps, "completionProvider") != NULL;
    const Json *d = json_get(caps, "definitionProvider");
    s->cap_definition = d && !(d->type == JSON_BOOL && !d->b);
    const Json *h = json_get(caps, "hoverProvider");
    s->cap_hover = h && !(h->type == JSON_BOOL && !h->b);
    Str n = {0};
    str_appendz(&n, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
    send_msg(e, s, &n, true);
    str_free(&n);
    s->ready = true;
    str_append(&s->wbuf, str_cstr(&s->pending), s->pending.len);
    str_free(&s->pending);
    flush(e, s);
}

static void handle_diagnostics(Editor *e, Server *s, const Json *params) {
    const char *uri = json_str(json_get(params, "uri"));
    Lsp *L = state(e);
    Doc *d = NULL;
    for (size_t i = 0; uri && i < L->ndocs; i++)
        if (L->docs[i].s == s && !strcmp(L->docs[i].uri, uri)) d = &L->docs[i];
    if (!d) return;
    Buffer *b = d->b;
    buffer_clear_diags(b);
    const Json *list = json_get(params, "diagnostics");
    if (!list || list->type != JSON_ARR) return;
    size_t n = list->n < 10000 ? list->n : 10000;
    b->diags = xcalloc(n ? n : 1, sizeof *b->diags);
    for (size_t i = 0; i < n; i++) {
        const Json *dg = &list->items[i];
        const Json *start = json_path(dg, "range", "start", NULL);
        const Json *end = json_path(dg, "range", "end", NULL);
        const char *msg = json_str(json_get(dg, "message"));
        if (!start || !end || !msg) continue;
        size_t a = from_lsp_pos(s, b, start), z = from_lsp_pos(s, b, end);
        Diagnostic *out = &b->diags[b->ndiags++];
        out->line = text_line_of(&b->text, a);
        size_t ls = text_line_start(&b->text, out->line);
        size_t le = text_line_end(&b->text, out->line);
        out->start = a - ls;
        out->end = (z > le ? le : z) - ls;
        if (out->end < out->start) out->end = out->start;
        out->severity = (int)json_num(json_get(dg, "severity"), 1);
        out->msg = xstrdup(msg);
    }
}

/* Takes ownership of m. Returns true if the screen should be redrawn. */
static bool dispatch(Editor *e, Server *s, Json *m) {
    Lsp *L = state(e);
    const Json *id = json_get(m, "id");
    const char *method = json_str(json_get(m, "method"));
    bool redraw = false;
    if (method && id) { /* request from the server */
        if (!strcmp(method, "workspace/configuration")) {
            const Json *items = json_path(m, "params", "items", NULL);
            Str r = {0};
            str_putc(&r, '[');
            for (size_t i = 0; items && i < items->n && i < 1000; i++)
                str_appendz(&r, i ? ",null" : "null");
            str_putc(&r, ']');
            respond(e, s, id, r.data);
            str_free(&r);
        } else {
            respond(e, s, id, "null");
        }
    } else if (method) {
        if (!strcmp(method, "textDocument/publishDiagnostics")) {
            handle_diagnostics(e, s, json_get(m, "params"));
            redraw = true;
        } else if (!strcmp(method, "window/showMessage")) {
            const char *msg = json_str(json_path(m, "params", "message", NULL));
            double type = json_num(json_path(m, "params", "type", NULL), 3);
            if (msg && type <= 2) {
                editor_error(e, "%s: %s", s->name, msg);
                redraw = true;
            }
        }
    } else if (id && id->type == JSON_NUM) {
        long n = (long)id->num;
        if (n == s->init_id) {
            handle_init(e, s, m);
            redraw = true;
        } else if (L->wait_server == s && n == L->wait_id) {
            L->wait_msg = m;
            L->wait_done = true;
            return false;
        }
    }
    json_free(m);
    return redraw;
}

static const char *find_hdr_end(const char *p, size_t n) {
    for (size_t i = 0; i + 4 <= n; i++)
        if (!memcmp(p + i, "\r\n\r\n", 4)) return p + i;
    return NULL;
}

static bool read_server(Editor *e, Server *s) {
    bool redraw = false;
    for (;;) {
        char buf[65536];
        ssize_t r = read(s->from_fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (r <= 0) {
            server_died(e, s);
            return true;
        }
        str_append(&s->rbuf, buf, (size_t)r);
        if (s->rbuf.len > MAX_MSG + 4096) {
            server_died(e, s);
            return true;
        }
    }
    for (;;) {
        const char *base = str_cstr(&s->rbuf);
        const char *he = find_hdr_end(base, s->rbuf.len);
        if (!he) break;
        size_t hlen = (size_t)(he - base), clen = SIZE_MAX;
        for (const char *line = base; line < he;) {
            const char *eol = line;
            while (eol < he && *eol != '\r') eol++;
            if ((size_t)(eol - line) > 15 && !strncasecmp(line, "Content-Length:", 15))
                clen = strtoul(line + 15, NULL, 10);
            line = eol + 2;
        }
        if (clen == SIZE_MAX || clen > MAX_MSG) {
            server_died(e, s);
            return true;
        }
        if (s->rbuf.len < hlen + 4 + clen) break;
        Json *m = json_parse(base + hlen + 4, clen);
        size_t used = hlen + 4 + clen;
        memmove(s->rbuf.data, s->rbuf.data + used, s->rbuf.len - used);
        s->rbuf.len -= used;
        s->rbuf.data[s->rbuf.len] = '\0';
        if (m) redraw |= dispatch(e, s, m);
        if (s->dead) break;
    }
    return redraw;
}

/* ---- servers ---- */

static Server *spawn_argv(Editor *e, const char *lang, char **argv,
                          const char *root, int in[2], int out[2]);

static char *find_root(const char *path) {
    static const char *const markers[] = {
        ".git", "compile_commands.json", ".clangd", "Cargo.toml", "go.mod",
        "pyproject.toml", "setup.py", "package.json", NULL};
    char *real = realpath(path, NULL);
    Str dir = {0};
    str_appendz(&dir, real ? real : path);
    free(real);
    char *slash = strrchr(dir.data, '/');
    if (slash) *slash = '\0';
    if (!dir.data[0]) str_appendz(&dir, "/");
    Str probe = {0};
    for (char *d = xstrdup(dir.data);;) {
        for (int i = 0; markers[i]; i++) {
            str_clear(&probe);
            str_appendf(&probe, "%s/%s", d, markers[i]);
            if (access(probe.data, F_OK) == 0) {
                str_free(&probe);
                str_free(&dir);
                return d;
            }
        }
        char *s = strrchr(d, '/');
        if (!s || s == d) {
            free(d);
            break;
        }
        *s = '\0';
    }
    str_free(&probe);
    return dir.data; /* no marker: the file's directory */
}

static Server *spawn(Editor *e, const char *lang, const char *const *cargv,
                     const char *root) {
    int in[2], out[2];
    size_t argc = 0;
    while (cargv[argc]) argc++;
    char **argv = xcalloc(argc + 1, sizeof *argv); /* execvp wants char ** */
    for (size_t i = 0; i < argc; i++) argv[i] = xstrdup(cargv[i]);
    Server *s = spawn_argv(e, lang, argv, root, in, out);
    for (size_t i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return s;
}

static Server *spawn_argv(Editor *e, const char *lang, char **argv,
                          const char *root, int in[2], int out[2]) {
    if (pipe(in) != 0) return NULL;
    if (pipe(out) != 0) {
        close(in[0]);
        close(in[1]);
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        return NULL;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        if (null >= 0) dup2(null, STDERR_FILENO);
        for (int fd = 3; fd < 256; fd++) close(fd);
        setsid(); /* never touch our terminal */
        signal(SIGPIPE, SIG_DFL);
        if (chdir(root) != 0) { /* run from wherever we are */ }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    Server *s = xcalloc(1, sizeof *s);
    s->lang = xstrdup(lang);
    const char *base = strrchr(argv[0], '/');
    s->name = xstrdup(base ? base + 1 : argv[0]);
    s->pid = pid;
    s->to_fd = in[1];
    s->from_fd = out[0];
    for (int fd = 0; fd < 2; fd++) {
        int f = fd ? s->to_fd : s->from_fd;
        fcntl(f, F_SETFL, fcntl(f, F_GETFL) | O_NONBLOCK);
        fcntl(f, F_SETFD, FD_CLOEXEC);
    }
    char *root_uri = path_to_uri(root);
    const char *rb = strrchr(root, '/');
    Str p = {0};
    str_appendf(&p, "{\"processId\":%ld,\"clientInfo\":{\"name\":\"cedit\"},"
                    "\"rootUri\":", (long)getpid());
    json_quote(&p, root_uri, strlen(root_uri));
    str_appendz(&p, ",\"workspaceFolders\":[{\"uri\":");
    json_quote(&p, root_uri, strlen(root_uri));
    str_appendz(&p, ",\"name\":");
    rb = rb && rb[1] ? rb + 1 : root;
    json_quote(&p, rb, strlen(rb));
    str_appendz(&p,
        "}],\"capabilities\":{"
        "\"general\":{\"positionEncodings\":[\"utf-8\",\"utf-16\"]},"
        "\"textDocument\":{"
        "\"synchronization\":{\"didSave\":true},"
        "\"completion\":{\"completionItem\":{\"snippetSupport\":false}},"
        "\"hover\":{\"contentFormat\":[\"plaintext\",\"markdown\"]},"
        "\"definition\":{},\"publishDiagnostics\":{}},"
        "\"window\":{\"workDoneProgress\":false}}}");
    s->init_id = request(e, s, "initialize", p.data, true);
    str_free(&p);
    free(root_uri);
    return s;
}

static Server *server_for(Editor *e, Buffer *b) {
    Lsp *L = state(e);
    const char *lang = syntax_name(b->syntax);
    for (size_t i = 0; i < L->nservers; i++)
        if (!strcmp(L->servers[i]->lang, lang) && !L->servers[i]->dead)
            return L->servers[i];
    for (size_t i = 0; i < L->nservers; i++)
        if (!strcmp(L->servers[i]->lang, lang)) return NULL; /* died: no auto-restart */

    const char *const *argv = NULL;
    for (size_t i = 0; i < L->nconf; i++) {
        if (!strcmp(L->conf[i].lang, lang)) {
            if (!L->conf[i].argv) return NULL; /* "lsp LANG off" */
            argv = (const char *const *)L->conf[i].argv;
        }
    }
    Str tried = {0};
    for (size_t i = 0; !argv && i < NDEFAULTS; i++) {
        if (strcmp(defaults[i].lang, lang)) continue;
        if (in_path(defaults[i].argv[0]))
            argv = defaults[i].argv;
        else
            str_appendf(&tried, "%s%s", tried.len ? ", " : "", defaults[i].argv[0]);
    }
    if (!argv) {
        bool warned = false;
        for (size_t i = 0; i < L->nwarned; i++)
            if (!strcmp(L->warned[i], lang)) warned = true;
        if (!warned && tried.len) {
            editor_message(e, "No language server for %s (install %s)", lang, tried.data);
            L->warned = xreallocarray(L->warned, L->nwarned + 1, sizeof *L->warned);
            L->warned[L->nwarned++] = xstrdup(lang);
        }
        str_free(&tried);
        return NULL;
    }
    str_free(&tried);
    char *root = find_root(b->path);
    Server *s = spawn(e, lang, argv, root);
    free(root);
    if (!s) {
        editor_error(e, "Cannot start %s: %s", argv[0], strerror(errno));
        return NULL;
    }
    L->servers = xreallocarray(L->servers, L->nservers + 1, sizeof *L->servers);
    L->servers[L->nservers++] = s;
    return s;
}

void lsp_buffer_opened(Editor *e, Buffer *b) {
    Lsp *L = state(e);
    if (L->disabled || !b->path || !b->syntax || doc_of(L, b)) return;
    Server *s = server_for(e, b);
    if (!s) return;
    L->docs = xreallocarray(L->docs, L->ndocs + 1, sizeof *L->docs);
    Doc *d = &L->docs[L->ndocs++];
    d->b = b;
    d->s = s;
    d->uri = path_to_uri(b->path);
    send_open(e, d);
}

void lsp_buffer_saved(Editor *e, Buffer *b) {
    Doc *d = doc_of(state(e), b);
    if (!d || d->s->dead) return;
    send_change(e, d);
    Str p = {0};
    uri_params(&p, d);
    notify(e, d->s, "textDocument/didSave", p.data);
    str_free(&p);
}

void lsp_buffer_closed(Editor *e, Buffer *b) {
    Lsp *L = state(e);
    Doc *d = doc_of(L, b);
    if (!d) return;
    if (!d->s->dead) {
        Str p = {0};
        uri_params(&p, d);
        notify(e, d->s, "textDocument/didClose", p.data);
        str_free(&p);
    }
    free(d->uri);
    *d = L->docs[--L->ndocs];
}

/* ---- event loop ---- */

int lsp_pollfds(Editor *e, struct pollfd *fds, int max) {
    Lsp *L = e->lsp;
    int n = 0;
    for (size_t i = 0; L && i < L->nservers && n + 2 <= max; i++) {
        Server *s = L->servers[i];
        if (s->dead) continue;
        fds[n++] = (struct pollfd){.fd = s->from_fd, .events = POLLIN};
        if (s->wbuf.len) fds[n++] = (struct pollfd){.fd = s->to_fd, .events = POLLOUT};
    }
    return n;
}

static bool sync_pending(Lsp *L) {
    for (size_t i = 0; i < L->ndocs; i++)
        if (L->docs[i].sent != L->docs[i].b->version && !L->docs[i].s->dead)
            return true;
    return false;
}

int lsp_timeout_ms(Editor *e) {
    Lsp *L = e->lsp;
    if (!L || !sync_pending(L)) return -1;
    double left = SYNC_DELAY - (editor_now() - e->last_input);
    return left <= 0 ? 0 : (int)(left * 1000) + 1;
}

bool lsp_process(Editor *e, struct pollfd *fds, int n) {
    Lsp *L = e->lsp;
    if (!L) return false;
    bool redraw = false;
    for (int i = 0; i < n; i++) {
        if (!fds[i].revents) continue;
        for (size_t k = 0; k < L->nservers; k++) {
            Server *s = L->servers[k];
            if (s->dead) continue;
            if (fds[i].fd == s->from_fd) redraw |= read_server(e, s);
            else if (fds[i].fd == s->to_fd) flush(e, s);
        }
    }
    if (sync_pending(L) && editor_now() - e->last_input >= SYNC_DELAY)
        for (size_t i = 0; i < L->ndocs; i++) send_change(e, &L->docs[i]);
    return redraw;
}

/* Send a request and wait for its response. C-g cancels. Returns the
 * whole response message (caller frees) or NULL. */
static Json *request_wait(Editor *e, Doc *d, const char *method,
                          const char *params) {
    Lsp *L = state(e);
    Server *s = d->s;
    send_change(e, d);
    long id = request(e, s, method, params, false);
    L->wait_server = s;
    L->wait_id = id;
    L->wait_done = false;
    L->wait_msg = NULL;
    double start = editor_now();
    bool shown = false, cancelled = false;
    while (!L->wait_done && !s->dead && !cancelled) {
        double el = editor_now() - start;
        if (el > REQUEST_TIMEOUT) break;
        if (!shown && el > 0.15 && !e->headless) {
            editor_message(e, "Waiting for %s... (C-g cancels)", s->name);
            render_frame(e);
            shown = true;
        }
        struct pollfd fds[3] = {
            {.fd = s->from_fd, .events = POLLIN},
            {.fd = s->to_fd, .events = s->wbuf.len ? POLLOUT : 0},
            {.fd = e->headless ? -1 : STDIN_FILENO, .events = POLLIN},
        };
        if (poll(fds, 3, 50) <= 0) continue;
        if (fds[1].revents) flush(e, s);
        if (fds[0].revents) read_server(e, s);
        if (fds[2].revents & POLLIN && e->inlen < sizeof e->inbuf) {
            ssize_t r = term_read(e->inbuf + e->inlen, sizeof e->inbuf - e->inlen);
            if (r > 0) e->inlen += (size_t)r;
            unsigned char *g = memchr(e->inbuf, 7, e->inlen);
            if (g) {
                memmove(g, g + 1, e->inlen - (size_t)(g - e->inbuf) - 1);
                e->inlen--;
                cancelled = true;
            }
        }
    }
    L->wait_server = NULL;
    if (!L->wait_done) {
        if (!s->dead) {
            char p[64];
            snprintf(p, sizeof p, "{\"id\":%ld}", id);
            notify(e, s, "$/cancelRequest", p);
        }
        editor_error(e, cancelled ? "Cancelled" : "%s did not answer", s->name);
        return NULL;
    }
    if (shown) e->msg[0] = '\0';
    Json *m = L->wait_msg;
    L->wait_msg = NULL;
    const char *err = json_str(json_path(m, "error", "message", NULL));
    if (err) {
        editor_error(e, "%s: %s", s->name, err);
        json_free(m);
        return NULL;
    }
    return m;
}

static Doc *current_doc(Editor *e) {
    Doc *d = doc_of(state(e), editor_buf(e));
    if (!d)
        editor_error(e, "No language server for this buffer");
    else if (d->s->dead)
        editor_error(e, "%s is not running (M-x lsp-restart)", d->s->name), d = NULL;
    else if (!d->s->ready)
        editor_error(e, "%s is still starting", d->s->name), d = NULL;
    return d;
}

/* ---- completion ---- */

typedef struct {
    char *label, *insert, *filter, *sort, *detail;
    bool has_edit;
    size_t edit_start;
} Item;

static int cmp_items(const void *a, const void *b) {
    return strcmp(((const Item *)a)->sort, ((const Item *)b)->sort);
}

static bool is_ident_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

static const char *str_or(const Json *j, const char *def) {
    const char *s = json_str(j);
    return s ? s : def;
}

static void free_popup(Editor *e) {
    for (size_t i = 0; i < e->popup_n; i++) free(e->popup[i]);
    free(e->popup);
    e->popup = NULL;
    e->popup_n = 0;
    e->popup_active = false;
}

static void accept_item(Editor *e, const Item *it, size_t ws) {
    Buffer *b = editor_buf(e);
    size_t start = it->has_edit && it->edit_start <= b->cursor ? it->edit_start : ws;
    if (buffer_delete(b, start, b->cursor - start) == 0) {
        b->cursor = start;
        insert_text(e, it->insert, strlen(it->insert));
    }
}

static void cmd_lsp_complete(Editor *e) {
    Doc *d = current_doc(e);
    if (!d) return;
    if (!d->s->cap_completion) {
        editor_error(e, "%s does not offer completion", d->s->name);
        return;
    }
    Buffer *b = d->b;
    size_t ws = b->cursor;
    size_t ls = text_line_start(&b->text, buffer_cur_line(b));
    while (ws > ls && is_ident_byte(text_byte(&b->text, ws - 1))) ws--;
    Str p = {0};
    pos_params(&p, d, b->cursor);
    Json *m = request_wait(e, d, "textDocument/completion", p.data);
    str_free(&p);
    if (!m) return;
    const Json *res = json_get(m, "result");
    const Json *arr = res && res->type == JSON_ARR ? res : json_get(res, "items");
    size_t n = arr && arr->type == JSON_ARR ? arr->n : 0;
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    Item *items = xcalloc(n ? n : 1, sizeof *items);
    size_t cur_line = buffer_cur_line(b), ni = 0;
    for (size_t i = 0; i < n; i++) {
        const Json *it = &arr->items[i];
        const char *label = json_str(json_get(it, "label"));
        if (!label) continue;
        Item *o = &items[ni++];
        const Json *te = json_get(it, "textEdit");
        const Json *range = json_get(te, "range") ? json_get(te, "range")
                                                  : json_get(te, "insert");
        const char *fallback = str_or(json_get(it, "insertText"), label);
        o->insert = xstrdup(str_or(json_get(te, "newText"), fallback));
        o->label = xstrdup(label);
        o->filter = xstrdup(str_or(json_get(it, "filterText"), label));
        o->sort = xstrdup(str_or(json_get(it, "sortText"), label));
        o->detail = xstrdup(str_or(json_get(it, "detail"), ""));
        const Json *st = json_get(range, "start");
        if (st && json_num(json_get(st, "line"), -1) == (double)cur_line) {
            o->has_edit = true;
            o->edit_start = from_lsp_pos(d->s, b, st);
        }
    }
    json_free(m);
    qsort(items, ni, sizeof *items, cmp_items);

    size_t *vis = xcalloc(ni ? ni : 1, sizeof *vis);
    bool first = true;
    Key pending = KEY_NONE;
    for (;;) {
        char *prefix = text_dup(&b->text, ws, b->cursor - ws);
        size_t pl = prefix ? strlen(prefix) : 0, nv = 0;
        for (size_t i = 0; i < ni; i++) {
            const char *f = items[i].filter;
            while (*f == ' ') f++;
            if (prefix && strncasecmp(f, prefix, pl) == 0) vis[nv++] = i;
        }
        free(prefix);
        if (nv == 0) {
            if (first) editor_message(e, "No completions");
            break;
        }
        if (nv == 1 && first && pl) {
            accept_item(e, &items[vis[0]], ws);
            break;
        }
        first = false;
        free_popup(e);
        e->popup = xcalloc(nv, sizeof *e->popup);
        for (size_t i = 0; i < nv; i++) {
            Str line = {0};
            const char *l = items[vis[i]].label;
            while (*l == ' ') l++;
            str_appendz(&line, l);
            if (*items[vis[i]].detail) str_appendf(&line, "  %s", items[vis[i]].detail);
            char safe[256];
            sanitize_for_display(line.data, safe, sizeof safe);
            e->popup[i] = xstrdup(safe);
            str_free(&line);
        }
        e->popup_n = nv;
        if (e->popup_sel >= nv) e->popup_sel = nv - 1;
        e->popup_anchor = ws;
        e->popup_active = true;
        if (e->headless) break;
        render_frame(e);
        Key k = editor_read_key(e);
        if (k == KEY_RESIZE || k == KEY_NONE) continue;
        if (k == KEY_DOWN || k == 14) {
            e->popup_sel = (e->popup_sel + 1) % nv;
        } else if (k == KEY_UP || k == 16) {
            e->popup_sel = (e->popup_sel + nv - 1) % nv;
        } else if (k == KEY_RET || k == KEY_TAB) {
            accept_item(e, &items[vis[e->popup_sel]], ws);
            break;
        } else if (k == 7 || k == KEY_ESC) {
            break;
        } else if (k == KEY_MOUSE && e->popup_r0 >= 0 &&
                   e->mouse.y >= e->popup_r0 &&
                   e->mouse.y < e->popup_r0 + e->popup_h &&
                   e->mouse.x >= e->popup_c &&
                   e->mouse.x < e->popup_c + e->popup_w) {
            const Mouse *mo = &e->mouse;
            size_t idx = e->popup_top + (size_t)(mo->y - e->popup_r0);
            if (mo->button == 64) {
                e->popup_sel = (e->popup_sel + nv - 1) % nv;
            } else if (mo->button == 65) {
                e->popup_sel = (e->popup_sel + 1) % nv;
            } else if (mo->button == 0 && !mo->release && !mo->drag &&
                       idx < nv) {
                accept_item(e, &items[vis[idx]], ws);
                break;
            }
        } else if (k == KEY_DEL || k == 8) {
            if (b->cursor <= ws) break;
            buffer_delete(b, buffer_prev_char(b, b->cursor),
                          b->cursor - buffer_prev_char(b, b->cursor));
            e->popup_sel = 0;
        } else if (k > 32 && k < 0x110000 && !(k & KM_MASK) &&
                   (k >= 0x80 || is_ident_byte((unsigned char)k))) {
            char enc[4];
            size_t len = utf8_encode((uint32_t)k, enc);
            insert_text(e, enc, len);
            e->popup_sel = 0;
        } else {
            pending = k; /* close and handle the key normally */
            break;
        }
    }
    free_popup(e);
    e->popup_sel = 0;
    free(vis);
    for (size_t i = 0; i < ni; i++) {
        free(items[i].label);
        free(items[i].insert);
        free(items[i].filter);
        free(items[i].sort);
        free(items[i].detail);
    }
    free(items);
    if (pending != KEY_NONE) editor_handle_key(e, pending);
}

/* ---- definition, hover, diagnostics ---- */

static void push_jump(Lsp *L, Buffer *b) {
    if (!b->path) return;
    if (L->njumps == 64) {
        free(L->jumps[0].path);
        memmove(L->jumps, L->jumps + 1, 63 * sizeof *L->jumps);
        L->njumps--;
    }
    L->jumps[L->njumps++] = (Jump){xstrdup(b->path), b->cursor};
}

static void cmd_lsp_definition(Editor *e) {
    Doc *d = current_doc(e);
    if (!d) return;
    if (!d->s->cap_definition) {
        editor_error(e, "%s does not offer go-to-definition", d->s->name);
        return;
    }
    Server *s = d->s;
    Str p = {0};
    pos_params(&p, d, d->b->cursor);
    Json *m = request_wait(e, d, "textDocument/definition", p.data);
    str_free(&p);
    if (!m) return;
    const Json *res = json_get(m, "result");
    const Json *loc = res && res->type == JSON_ARR ? json_at(res, 0) : res;
    const char *uri = json_str(json_get(loc, "uri"));
    const Json *range = json_get(loc, "range");
    if (!uri) {
        uri = json_str(json_get(loc, "targetUri"));
        range = json_get(loc, "targetSelectionRange");
    }
    char *path = uri_to_path(uri);
    if (!path || !range) {
        editor_error(e, "No definition found");
        free(path);
        json_free(m);
        return;
    }
    push_jump(state(e), d->b);
    Buffer *t = editor_open(e, path);
    if (t) {
        t->cursor = from_lsp_pos(s, t, json_get(range, "start"));
        t->mark_active = false;
    }
    free(path);
    json_free(m);
}

static void cmd_lsp_back(Editor *e) {
    Lsp *L = state(e);
    if (L->njumps == 0) {
        editor_error(e, "No previous location");
        return;
    }
    Jump j = L->jumps[--L->njumps];
    Buffer *b = editor_open(e, j.path);
    if (b) b->cursor = j.pos > buffer_len(b) ? buffer_len(b) : j.pos;
    free(j.path);
}

static void hover_text(const Json *c, Str *out) {
    if (!c) return;
    if (c->type == JSON_STR) {
        str_appendz(out, c->str);
    } else if (c->type == JSON_OBJ) {
        str_appendz(out, str_or(json_get(c, "value"), ""));
    } else if (c->type == JSON_ARR) {
        for (size_t i = 0; i < c->n; i++) {
            if (out->len) str_appendz(out, "\n\n");
            hover_text(&c->items[i], out);
        }
    }
}

static void cmd_lsp_hover(Editor *e) {
    Doc *d = current_doc(e);
    if (!d) return;
    if (!d->s->cap_hover) {
        editor_error(e, "%s does not offer hover", d->s->name);
        return;
    }
    Str p = {0};
    pos_params(&p, d, d->b->cursor);
    Json *m = request_wait(e, d, "textDocument/hover", p.data);
    str_free(&p);
    if (!m) return;
    Str t = {0};
    hover_text(json_path(m, "result", "contents", NULL), &t);
    json_free(m);
    while (t.len && strchr(" \n\t", t.data[t.len - 1])) t.data[--t.len] = '\0';
    if (!t.len) {
        editor_message(e, "No information");
    } else if (!strchr(t.data, '\n') && t.len < (size_t)e->cols) {
        editor_message(e, "%s", t.data);
    } else if (!e->headless) { /* show in the popup until the next key */
        free_popup(e);
        size_t cap = 0;
        for (char *line = t.data; line && e->popup_n < 20;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            char safe[256];
            sanitize_for_display(line, safe, sizeof safe);
            if (e->popup_n == cap) {
                cap = cap ? cap * 2 : 8;
                e->popup = xreallocarray(e->popup, cap, sizeof *e->popup);
            }
            e->popup[e->popup_n++] = xstrdup(safe);
            line = nl ? nl + 1 : NULL;
        }
        e->popup_sel = SIZE_MAX; /* no selection: plain text box */
        e->popup_top = 0;
        e->popup_anchor = editor_buf(e)->cursor;
        e->popup_active = true;
        Key k;
        do {
            render_frame(e);
            k = editor_read_key(e);
        } while (k == KEY_RESIZE || k == KEY_NONE);
        free_popup(e);
        e->popup_sel = 0;
        if (k != 7 && k != KEY_ESC) editor_handle_key(e, k);
    }
    str_free(&t);
}

static void goto_diag(Editor *e, bool forward) {
    Buffer *b = editor_buf(e);
    if (!b->ndiags) {
        editor_message(e, "No diagnostics");
        return;
    }
    size_t line = buffer_cur_line(b);
    size_t col = b->cursor - text_line_start(&b->text, line);
    const Diagnostic *best = NULL, *wrap = NULL;
    for (size_t i = 0; i < b->ndiags; i++) {
        const Diagnostic *g = &b->diags[i];
        bool after = g->line > line || (g->line == line && g->start > col);
        bool before = g->line < line || (g->line == line && g->start < col);
        if (forward) {
            if (after && (!best || g->line < best->line ||
                          (g->line == best->line && g->start < best->start)))
                best = g;
            if (!wrap || g->line < wrap->line ||
                (g->line == wrap->line && g->start < wrap->start))
                wrap = g;
        } else {
            if (before && (!best || g->line > best->line ||
                           (g->line == best->line && g->start > best->start)))
                best = g;
            if (!wrap || g->line > wrap->line ||
                (g->line == wrap->line && g->start > wrap->start))
                wrap = g;
        }
    }
    if (!best) best = wrap;
    size_t ls = text_line_start(&b->text, best->line);
    size_t le = text_line_end(&b->text, best->line);
    b->cursor = ls + best->start > le ? le : ls + best->start;
    if (best->severity == 1)
        editor_error(e, "%s", best->msg);
    else
        editor_message(e, "%s", best->msg);
}

static void cmd_next_diag(Editor *e) { goto_diag(e, true); }
static void cmd_prev_diag(Editor *e) { goto_diag(e, false); }

static void shutdown_server(Editor *e, Server *s) {
    if (!s->dead && s->ready) {
        request(e, s, "shutdown", "null", true);
        notify(e, s, "exit", "null");
        for (int i = 0; i < 20 && s->wbuf.len && !s->dead; i++) {
            struct pollfd p = {.fd = s->to_fd, .events = POLLOUT};
            if (poll(&p, 1, 10) > 0) flush(e, s);
        }
        for (int i = 0; i < 30 && waitpid(s->pid, NULL, WNOHANG) == 0; i++)
            sleep_ms(10);
    }
    s->dead = true;
    server_kill(s);
}

static void free_server(Server *s) {
    str_free(&s->rbuf);
    str_free(&s->wbuf);
    str_free(&s->pending);
    free(s->lang);
    free(s->name);
    free(s);
}

static void cmd_lsp_restart(Editor *e) {
    Lsp *L = state(e);
    Buffer *cur = editor_buf(e);
    if (!cur->syntax || !cur->path) {
        editor_error(e, "No language for this buffer");
        return;
    }
    const char *lang = syntax_name(cur->syntax);
    for (size_t i = 0; i < L->nservers;) {
        Server *s = L->servers[i];
        if (strcmp(s->lang, lang) != 0) {
            i++;
            continue;
        }
        for (size_t k = 0; k < L->ndocs;) {
            if (L->docs[k].s == s) {
                free(L->docs[k].uri);
                L->docs[k] = L->docs[--L->ndocs];
            } else {
                k++;
            }
        }
        shutdown_server(e, s);
        free_server(s);
        L->servers[i] = L->servers[--L->nservers];
    }
    for (size_t i = 0; i < L->nwarned; i++) free(L->warned[i]);
    L->nwarned = 0;
    for (size_t i = 0; i < e->nbufs; i++)
        if (e->bufs[i]->syntax == cur->syntax) {
            buffer_clear_diags(e->bufs[i]);
            lsp_buffer_opened(e, e->bufs[i]);
        }
    if (doc_of(L, cur)) editor_message(e, "Restarted %s", doc_of(L, cur)->s->name);
}

static void cmd_lsp_status(Editor *e) {
    Lsp *L = state(e);
    Str s = {0};
    if (L->disabled) str_appendz(&s, "LSP disabled (lsp on)");
    for (size_t i = 0; i < L->nservers; i++) {
        Server *v = L->servers[i];
        size_t docs = 0;
        for (size_t k = 0; k < L->ndocs; k++) docs += L->docs[k].s == v;
        str_appendf(&s, "%s%s: %s (%s, %zu file%s)", s.len ? "; " : "", v->lang,
                    v->name, v->dead ? "stopped" : v->ready ? "running" : "starting",
                    docs, docs == 1 ? "" : "s");
    }
    editor_message(e, "%s", s.len ? s.data : "No language servers running");
    str_free(&s);
}

void lsp_register_commands(Editor *e) {
    command_register(e, "lsp-complete", cmd_lsp_complete,
                     "Complete the symbol at point (language server)", 0, -1);
    command_register(e, "lsp-definition", cmd_lsp_definition,
                     "Go to the definition of the symbol at point", 0, -1);
    command_register(e, "lsp-back", cmd_lsp_back,
                     "Return to where lsp-definition was invoked", 0, -1);
    command_register(e, "lsp-hover", cmd_lsp_hover,
                     "Show type/documentation of the symbol at point", 0, -1);
    command_register(e, "lsp-next-diagnostic", cmd_next_diag,
                     "Jump to the next error or warning", CMD_MOTION, -1);
    command_register(e, "lsp-previous-diagnostic", cmd_prev_diag,
                     "Jump to the previous error or warning", CMD_MOTION, -1);
    command_register(e, "lsp-restart", cmd_lsp_restart,
                     "Restart the language server for this buffer", 0, -1);
    command_register(e, "lsp-status", cmd_lsp_status,
                     "Show running language servers", 0, -1);
}

int lsp_config(Editor *e, int ntok, char **tok, char *err, size_t errlen) {
    Lsp *L = state(e);
    if (ntok == 2 && (!strcmp(tok[1], "on") || !strcmp(tok[1], "off"))) {
        L->disabled = !strcmp(tok[1], "off");
        return 0;
    }
    if (ntok < 3) {
        snprintf(err, errlen, "usage: lsp on|off, lsp LANG off, lsp LANG COMMAND [ARGS]");
        return -1;
    }
    const Syntax *syn = syntax_by_name(tok[1]);
    if (!syn) {
        snprintf(err, errlen, "unknown language '%s'", tok[1]);
        return -1;
    }
    Conf c = {xstrdup(syntax_name(syn)), NULL};
    if (!(ntok == 3 && !strcmp(tok[2], "off"))) {
        c.argv = xcalloc((size_t)ntok - 1, sizeof *c.argv);
        for (int i = 2; i < ntok; i++) c.argv[i - 2] = xstrdup(tok[i]);
    }
    for (size_t i = 0; i < L->nconf; i++) { /* replace an earlier entry */
        if (!strcmp(L->conf[i].lang, c.lang)) {
            for (char **a = L->conf[i].argv; a && *a; a++) free(*a);
            free(L->conf[i].argv);
            free(L->conf[i].lang);
            L->conf[i] = c;
            return 0;
        }
    }
    L->conf = xreallocarray(L->conf, L->nconf + 1, sizeof *L->conf);
    L->conf[L->nconf++] = c;
    return 0;
}

const char *lsp_status(Editor *e, Buffer *b) {
    Lsp *L = e->lsp;
    Doc *d = L ? doc_of(L, b) : NULL;
    if (!d) return "";
    snprintf(L->status, sizeof L->status, "%s%s", d->s->name,
             d->s->dead ? " (stopped)" : d->s->ready ? "" : " (starting)");
    return L->status;
}

void lsp_shutdown(Editor *e) {
    Lsp *L = e->lsp;
    if (!L) return;
    for (size_t i = 0; i < L->nservers; i++) {
        shutdown_server(e, L->servers[i]);
        free_server(L->servers[i]);
    }
    free(L->servers);
    for (size_t i = 0; i < L->ndocs; i++) free(L->docs[i].uri);
    free(L->docs);
    for (size_t i = 0; i < L->nconf; i++) {
        for (char **a = L->conf[i].argv; a && *a; a++) free(*a);
        free(L->conf[i].argv);
        free(L->conf[i].lang);
    }
    free(L->conf);
    for (size_t i = 0; i < L->nwarned; i++) free(L->warned[i]);
    free(L->warned);
    for (int i = 0; i < L->njumps; i++) free(L->jumps[i].path);
    json_free(L->wait_msg);
    free(L);
    e->lsp = NULL;
}
