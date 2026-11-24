/* Unit tests. Built against the editor objects (everything but main.c). */
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cedit/editor.h"
#include "cedit/file.h"
#include "cedit/input.h"
#include "cedit/json.h"
#include "cedit/search.h"
#include "cedit/syntax.h"
#include "cedit/text.h"
#include "cedit/utf8.h"
#include "test.h"

static char tmpdir[256];

static char *text_all(Text *t) { return text_dup(t, 0, text_len(t)); }

static void check_lines(Text *t) {
    char *s = text_all(t);
    size_t len = text_len(t), line = 0;
    bool ok = text_line_start(t, 0) == 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            line++;
            ok = ok && line < text_lines(t) && text_line_start(t, line) == i + 1;
            ok = ok && text_line_end(t, line - 1) == i;
        }
    }
    ok = ok && text_lines(t) == line + 1;
    CHECK(ok);
    free(s);
}

static unsigned rng_state = 12345;
static unsigned rnd(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7fff;
}

static void test_text_random(void) {
    Text t;
    CHECK(text_init(&t) == 0);
    char *ref = calloc(1, 1);
    size_t rlen = 0;
    static const char alphabet[] = "ab\n\xc3\xa9x\n";
    for (int iter = 0; iter < 20000; iter++) {
        if (rnd() % 3 || rlen == 0) {
            size_t pos = rlen ? rnd() % (rlen + 1) : 0;
            char ins[16];
            size_t n = 1 + rnd() % 8;
            for (size_t i = 0; i < n; i++) ins[i] = alphabet[rnd() % 7];
            CHECK(text_insert(&t, pos, ins, n) == 0);
            ref = realloc(ref, rlen + n + 1);
            memmove(ref + pos + n, ref + pos, rlen - pos + 1);
            memcpy(ref + pos, ins, n);
            rlen += n;
        } else {
            size_t pos = rnd() % rlen;
            size_t n = 1 + rnd() % 10;
            if (n > rlen - pos) n = rlen - pos;
            CHECK(text_delete(&t, pos, n) == 0);
            memmove(ref + pos, ref + pos + n, rlen - pos - n + 1);
            rlen -= n;
        }
        if (iter % 997 == 0) {
            char *s = text_all(&t);
            CHECK(text_len(&t) == rlen && memcmp(s, ref, rlen) == 0);
            free(s);
            check_lines(&t);
        }
    }
    char *s = text_all(&t);
    CHECK(text_len(&t) == rlen && memcmp(s, ref, rlen) == 0);
    free(s);
    check_lines(&t);
    CHECK(text_insert(&t, rlen + 1, "x", 1) == -1 && errno == EINVAL);
    CHECK(text_delete(&t, rlen, 1) == -1);
    CHECK(text_set(&t, "one\ntwo\n", 8) == 0);
    CHECK(text_lines(&t) == 3);
    CHECK(text_line_of(&t, 4) == 1 && text_line_of(&t, 8) == 2);
    const char *v = text_view(&t, 4, 3);
    CHECK(memcmp(v, "two", 3) == 0);
    free(ref);
    text_free(&t);
}

static void test_utf8(void) {
    uint32_t cp;
    bool valid;
    CHECK(utf8_decode("\xc3\xa9", 2, &cp, &valid) == 2 && cp == 0xE9 && valid);
    CHECK(utf8_decode("\xe2\x82\xac", 3, &cp, &valid) == 3 && cp == 0x20AC);
    CHECK(utf8_decode("\xf0\x9f\x98\x80", 4, &cp, &valid) == 4 && cp == 0x1F600);
    CHECK(utf8_decode("\xc0\xaf", 2, &cp, &valid) == 1 && !valid); /* overlong */
    CHECK(utf8_decode("\xed\xa0\x80", 3, &cp, &valid) == 1 && !valid); /* surrogate */
    CHECK(utf8_decode("\xe2\x82", 2, &cp, &valid) == 1 && !valid);
    char out[4];
    CHECK(utf8_encode(0x1F600, out) == 4 && memcmp(out, "\xf0\x9f\x98\x80", 4) == 0);
    CHECK(utf8_encode(0xD800, out) == 0);

    Glyph g;
    glyph_next("\x1b[31m", 5, 0, 4, &g);
    CHECK(g.special && g.outlen == 2 && memcmp(g.out, "^[", 2) == 0);
    glyph_next("\xc2\x9b", 2, 0, 4, &g); /* C1 CSI */
    CHECK(g.special && memchr(g.out, 0x9b, g.outlen) == NULL);
    glyph_next("\xe2\x80\xae", 3, 0, 4, &g); /* RTL override */
    CHECK(g.special && g.bytes == 3);
    glyph_next("\t", 1, 3, 4, &g);
    CHECK(g.width == 1);
    glyph_next("\xe4\xb8\xad", 3, 0, 4, &g); /* CJK */
    CHECK(g.width == 2);
    glyph_next("\xcc\x81", 2, 0, 4, &g); /* combining acute */
    CHECK(g.width == 0);
    glyph_next("\xff", 1, 0, 4, &g);
    CHECK(g.special && g.width == 4);
    CHECK(display_width("a\tb", 3, 8) == 9);

    char buf[64];
    sanitize_for_display("evil\x1b]0;title\x07name", buf, sizeof buf);
    CHECK(strchr(buf, 0x1b) == NULL && strchr(buf, 0x07) == NULL);
}

static Key parse1(const char *s, bool final, size_t *used) {
    Key k;
    *used = input_parse((const unsigned char *)s, strlen(s), final, &k);
    return k;
}

static void test_input(void) {
    size_t n;
    CHECK(parse1("a", false, &n) == 'a' && n == 1);
    CHECK(parse1("\x1b[A", false, &n) == KEY_UP && n == 3);
    CHECK(parse1("\x1b[1;5C", false, &n) == (KEY_RIGHT | KM_CTRL) && n == 6);
    CHECK(parse1("\x1b[1;2D", false, &n) == (KEY_LEFT | KM_SHIFT));
    CHECK(parse1("\x1b[4h", false, &n) == KEY_INSERT && n == 4);
    CHECK(parse1("\x1b[3~", false, &n) == KEY_DELETE && n == 4);
    CHECK(parse1("\x1b[5~", false, &n) == KEY_PGUP);
    CHECK(parse1("\x1bOH", false, &n) == KEY_HOME);
    CHECK(parse1("\x1b[Z", false, &n) == KEY_BACKTAB);
    CHECK(parse1("\x1bx", false, &n) == ('x' | KM_ALT) && n == 2);
    CHECK(parse1("\x1b", false, &n) == KEY_NONE && n == 0);
    CHECK(parse1("\x1b", true, &n) == KEY_ESC && n == 1);
    CHECK(parse1("\x1b[", false, &n) == KEY_NONE && n == 0);
    CHECK(parse1("\x1b[200~", false, &n) == KEY_PASTE_START);
    CHECK(parse1("\x1b[201~", false, &n) == KEY_PASTE_END);
    CHECK(parse1("\xc3\xa9", false, &n) == 0xE9 && n == 2);
    CHECK(parse1("\xc3", false, &n) == KEY_NONE && n == 0);
    CHECK(parse1("\xc3", true, &n) == 0xFFFD && n == 1);
    CHECK(parse1("\x1b[99;99;99;99;99;99X", false, &n) == KEY_NONE && n > 0);
    CHECK(parse1("\x1b[97;5u", false, &n) == 1); /* kitty C-a */
    /* overlong CSI is consumed, never overflows */
    char big[200];
    memset(big, '1', sizeof big);
    big[0] = 0x1b;
    big[1] = '[';
    big[199] = '\0';
    CHECK(parse1(big, false, &n) == KEY_NONE && n > 0 && n < 200);

    Key k;
    CHECK(key_parse("C-x", 3, &k) && k == 24);
    CHECK(key_parse("M-x", 3, &k) && k == ('x' | KM_ALT));
    CHECK(key_parse("C-M-s", 5, &k) && k == (19 | KM_ALT));
    CHECK(key_parse("C-<left>", 8, &k) && k == (KEY_LEFT | KM_CTRL));
    CHECK(key_parse("C-SPC", 5, &k) && k == 0);
    CHECK(key_parse("C-/", 3, &k) && k == 31);
    CHECK(!key_parse("<bogus>", 7, &k));
    char name[32];
    key_name(24, name, sizeof name);
    CHECK_STR(name, "C-x");
    key_name(KEY_UP | KM_SHIFT, name, sizeof name);
    CHECK_STR(name, "S-<up>");
}

static void test_undo(void) {
    Buffer *b = buffer_new("t");
    CHECK(!buffer_modified(b));
    undo_boundary(&b->undo);
    buffer_insert(b, 0, "hello", 5);
    buffer_insert(b, 5, " world", 6); /* same group: merged */
    CHECK(buffer_modified(b));
    undo_boundary(&b->undo);
    buffer_delete(b, 0, 6);
    char *s = text_all(&b->text);
    CHECK_STR(s, "world");
    free(s);
    CHECK(buffer_undo(b));
    s = text_all(&b->text);
    CHECK_STR(s, "hello world");
    free(s);
    CHECK(buffer_undo(b));
    CHECK(buffer_len(b) == 0 && !buffer_modified(b));
    CHECK(!buffer_undo(b));
    CHECK(buffer_redo(b) && buffer_redo(b));
    s = text_all(&b->text);
    CHECK_STR(s, "world");
    free(s);
    undo_mark_saved(&b->undo);
    CHECK(!buffer_modified(b));
    undo_boundary(&b->undo);
    buffer_insert(b, 0, "x", 1);
    CHECK(buffer_modified(b));
    buffer_undo(b);
    CHECK(!buffer_modified(b));
    /* cursor/mark adjustment */
    b->cursor = 3;
    b->mark = 1;
    buffer_insert(b, 0, "ab", 2);
    CHECK(b->cursor == 5 && b->mark == 3);
    buffer_delete(b, 0, 4);
    CHECK(b->cursor == 1 && b->mark == 0);
    b->readonly = true;
    CHECK(buffer_insert(b, 0, "x", 1) == -1 && errno == EROFS);
    buffer_free(b);
}

static void test_buffer_motion(void) {
    Buffer *b = buffer_new("t");
    const char *s = "e\xcc\x81x\xe4\xb8\xad|"; /* e+combining, x, CJK, | */
    buffer_insert(b, 0, s, strlen(s));
    CHECK(buffer_next_char(b, 0) == 3);
    CHECK(buffer_prev_char(b, 3) == 0);
    CHECK(buffer_next_char(b, 4) == 7);
    CHECK(buffer_col_of(b, 7) == 4); /* e(1) x(1) CJK(2) */
    CHECK(buffer_pos_at_col(b, 0, 3) == 4); /* inside CJK: stays before it */
    buffer_free(b);
}

static char *tmp_path(const char *name) {
    static char buf[512];
    snprintf(buf, sizeof buf, "%s/%s", tmpdir, name);
    return buf;
}

static void write_raw(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    fputs(data, f);
    fclose(f);
}

static char *read_raw(const char *path) {
    char *d;
    size_t n;
    char err[256];
    if (file_read_all(path, &d, &n, NULL, err, sizeof err) != 0) return NULL;
    d = realloc(d, n + 1);
    d[n] = '\0';
    return d;
}

static void test_file(void) {
    char err[512];
    bool is_new, in_place;
    char *p = xstrdup(tmp_path("crlf.txt"));
    write_raw(p, "a\r\nb\r\n");
    Buffer *b = buffer_new("x");
    CHECK(buffer_load(b, p, &is_new, err, sizeof err) == 0 && !is_new);
    CHECK(b->crlf && buffer_len(b) == 4);
    buffer_insert(b, 4, "c\n", 2);
    CHECK(buffer_save(b, p, &in_place, err, sizeof err) == 0 && !in_place);
    char *d = read_raw(p);
    CHECK_STR(d, "a\r\nb\r\nc\r\n");
    free(d);
    buffer_free(b);

    /* mixed endings are preserved byte for byte */
    write_raw(p, "a\r\nb\n");
    b = buffer_new("x");
    CHECK(buffer_load(b, p, &is_new, err, sizeof err) == 0 && !b->crlf);
    CHECK(buffer_save(b, p, &in_place, err, sizeof err) == 0);
    d = read_raw(p);
    CHECK_STR(d, "a\r\nb\n");
    free(d);

    /* permissions preserved by atomic save */
    chmod(p, 0640);
    CHECK(buffer_save(b, p, &in_place, err, sizeof err) == 0);
    struct stat st;
    stat(p, &st);
    CHECK((st.st_mode & 0777) == 0640);

    /* symlinks: the link stays, the target is written */
    char *slink = xstrdup(tmp_path("link.txt"));
    unlink(slink);
    CHECK(symlink(p, slink) == 0);
    buffer_insert(b, 0, "Z", 1);
    CHECK(buffer_save(b, slink, &in_place, err, sizeof err) == 0);
    struct stat lst;
    CHECK(lstat(slink, &lst) == 0 && S_ISLNK(lst.st_mode));
    d = read_raw(p);
    CHECK(d && d[0] == 'Z');
    free(d);

    /* A missing symlink target must not cause the link to be replaced. */
    char *missing = xstrdup(tmp_path("absent-target"));
    char *dangling = xstrdup(tmp_path("dangling"));
    CHECK(symlink(missing, dangling) == 0);
    CHECK(buffer_save(b, dangling, &in_place, err, sizeof err) != 0);
    CHECK(strstr(err, "dangling symlink") != NULL);
    CHECK(lstat(dangling, &lst) == 0 && S_ISLNK(lst.st_mode));
    CHECK(access(missing, F_OK) != 0);
    free(missing);
    free(dangling);

    /* Temporary names must fit even when the destination name is long. */
    char long_name[251];
    memset(long_name, 'a', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    char *long_path = xstrdup(tmp_path(long_name));
    CHECK(buffer_save(b, long_path, &in_place, err, sizeof err) == 0);
    write_raw(long_path, "old");
    CHECK(buffer_save(b, long_path, &in_place, err, sizeof err) == 0);
    d = read_raw(long_path);
    CHECK_STR(d, "Za\r\nb\n");
    free(d);
    free(long_path);

    /* hard links are kept by rewriting in place */
    char *hard = xstrdup(tmp_path("hard.txt"));
    unlink(hard);
    CHECK(link(p, hard) == 0);
    CHECK(buffer_save(b, p, &in_place, err, sizeof err) == 0 && in_place);
    struct stat a1, a2;
    stat(p, &a1);
    stat(hard, &a2);
    CHECK(a1.st_ino == a2.st_ino);

    /* failed save leaves the original untouched */
    write_raw(p, "original\n");
    unlink(hard);
    CHECK(buffer_save(b, tmp_path("nodir/file.txt"), &in_place, err, sizeof err) != 0);
    CHECK(strstr(err, "nodir") != NULL);
    d = read_raw(p);
    CHECK_STR(d, "original\n");
    free(d);

    /* directories and missing files */
    CHECK(buffer_load(b, tmpdir, &is_new, err, sizeof err) != 0);
    CHECK(buffer_load(b, tmp_path("missing.txt"), &is_new, err, sizeof err) == 0 && is_new);
    CHECK(buffer_len(b) == 0 && b->path != NULL);

    /* external modification detection */
    write_raw(p, "v1\n");
    CHECK(buffer_load(b, p, &is_new, err, sizeof err) == 0);
    CHECK(!buffer_disk_changed(b));
    write_raw(p, "version two\n");
    CHECK(buffer_disk_changed(b));

    buffer_free(b);
    free(p);
    free(slink);
    free(hard);
}

static void test_search(void) {
    Buffer *b = buffer_new("s");
    const char *s = "Hello world\nhello again\nfoo123 bar\n";
    buffer_insert(b, 0, s, strlen(s));
    Search q;
    char err[128];
    size_t ms, me;
    CHECK(search_compile(&q, "hello", false, err, sizeof err) == 0 && q.icase);
    CHECK(search_find(&q, b, 0, true, false, &ms, &me) && ms == 0 && me == 5);
    CHECK(search_find(&q, b, 1, true, false, &ms, &me) && ms == 12);
    CHECK(!search_find(&q, b, 13, true, false, &ms, &me));
    CHECK(search_find(&q, b, 13, true, true, &ms, &me) && ms == 0); /* wrap */
    CHECK(search_find(&q, b, 12, false, false, &ms, &me) && ms == 0);
    search_free(&q);
    CHECK(search_compile(&q, "Hello", false, err, sizeof err) == 0 && !q.icase);
    CHECK(search_find(&q, b, 1, true, true, &ms, &me) && ms == 0);
    search_free(&q);
    CHECK(search_compile(&q, "([a-z]+)([0-9]+)", true, err, sizeof err) == 0);
    CHECK(search_find(&q, b, 0, true, false, &ms, &me) && ms == 24 && me == 30);
    Str out = {0};
    const char *line = "foo123 bar";
    size_t a, z;
    CHECK(search_in_line(&q, line, strlen(line), 0, &a, &z));
    search_expand(&q, line, "\\2-\\1", &out);
    CHECK_STR(str_cstr(&out), "123-foo");
    str_free(&out);
    search_free(&q);
    CHECK(search_compile(&q, "([", true, err, sizeof err) != 0);
    CHECK(search_compile(&q, "^h", true, err, sizeof err) == 0);
    CHECK(search_find(&q, b, 1, true, false, &ms, &me) && ms == 12);
    search_free(&q);
    buffer_free(b);
}

static void hl(const char *lang, const char *line, uint8_t state_in,
               uint8_t *out, uint8_t *state_out) {
    const Syntax *s = syntax_by_name(lang);
    *state_out = syntax_highlight(s, line, strlen(line), state_in, out);
}

static void test_syntax(void) {
    uint8_t h[128], st;
    CHECK_STR(syntax_name(syntax_detect("main.goose", NULL, 0)), "Goose");
    hl("Goose", "var n: u8 = 42; // count", 0, h, &st);
    CHECK(h[0] == HL_KEYWORD && h[7] == HL_TYPE && h[12] == HL_NUMBER &&
          h[16] == HL_COMMENT && st == 0);
    hl("Goose", "/* outer /* inner", 0, h, &st);
    hl("Goose", "// */ still */ fn", st, h, &st);
    CHECK(h[6] == HL_COMMENT && h[15] == HL_KEYWORD && st == 0);
    hl("Goose", "\"\"\"", 0, h, &st);
    hl("Goose", "raw // text", st, h, &st);
    CHECK(h[8] == HL_STRING && st != 0);
    hl("Goose", "\"\"\"; 'x'", st, h, &st);
    CHECK(h[2] == HL_STRING && h[3] == HL_OPERATOR && h[6] == HL_STRING && st == 0);
    hl("C", "int x = 42; // hi", 0, h, &st);
    CHECK(h[0] == HL_TYPE && h[8] == HL_NUMBER && h[12] == HL_COMMENT && st == 0);
    hl("C", "return \"a\\\"b\";", 0, h, &st);
    CHECK(h[0] == HL_KEYWORD && h[7] == HL_STRING && h[11] == HL_STRING && h[13] == HL_OPERATOR);
    hl("C", "a /* open", 0, h, &st);
    CHECK(h[2] == HL_COMMENT && st != 0);
    uint8_t st2;
    hl("C", "still */ int", st, h, &st2);
    CHECK(h[0] == HL_COMMENT && h[9] == HL_TYPE && st2 == 0);
    hl("C", "#include <stdio.h>", 0, h, &st);
    CHECK(h[0] == HL_PREPROC && h[10] == HL_STRING);
    hl("Python", "s = \"\"\"doc", 0, h, &st);
    CHECK(h[4] == HL_STRING && st != 0);
    hl("Python", "end\"\"\" # c", st, h, &st2);
    CHECK(h[0] == HL_STRING && h[7] == HL_COMMENT && st2 == 0);
    hl("Rust", "fn f<'a>(x: &'a str) -> char { 'c' }", 0, h, &st);
    CHECK(h[0] == HL_KEYWORD && h[5] != HL_STRING && h[31] == HL_STRING);
    hl("Shell", "echo $# a#b # real", 0, h, &st);
    CHECK(h[5] == HL_TYPE && h[9] != HL_COMMENT && h[13] == HL_COMMENT);
    hl("Markdown", "```c", 0, h, &st);
    CHECK(st != 0);
    hl("Markdown", "int x;", st, h, &st2);
    CHECK(h[0] == HL_STRING);
    hl("Markdown", "# Title", 0, h, &st);
    CHECK(h[2] == HL_KEYWORD);
    hl("Makefile", "all: $(TARGET)", 0, h, &st);
    CHECK(h[0] == HL_KEYWORD && h[3] == HL_OPERATOR && h[5] == HL_TYPE);
    hl("Makefile", "CC ?= cc # c", 0, h, &st);
    CHECK(h[0] == HL_PREPROC && h[3] == HL_OPERATOR && h[10] == HL_COMMENT);
    hl("TOML", "[server]", 0, h, &st);
    CHECK(h[1] == HL_TYPE);
    hl("JSON", "{\"a\": true}", 0, h, &st);
    CHECK(h[1] == HL_STRING && h[6] == HL_KEYWORD);
    hl("Go", "s := `raw", 0, h, &st);
    CHECK(st != 0);
    hl("JavaScript", "const x = `t`;", 0, h, &st);
    CHECK(h[0] == HL_KEYWORD && h[10] == HL_STRING && st == 0);
    CHECK(syntax_detect("/x/main.c", NULL, 0) == syntax_by_name("C"));
    CHECK(syntax_detect("Makefile", NULL, 0) == syntax_by_name("Makefile"));
    CHECK(syntax_detect("script", "#!/usr/bin/env python3", 22) == syntax_by_name("Python"));
    CHECK(syntax_detect("run", "#!/bin/bash", 11) == syntax_by_name("Shell"));
    CHECK(syntax_detect("x.unknown", NULL, 0) == NULL);
}

static void test_json(void) {
    const char *src = "{\"a\": [1, 2.5e1, true, null], \"s\": \"x\\u00e9\\ud83d\\ude00\\n\", "
                      "\"o\": {\"k\": -3}}";
    Json *j = json_parse(src, strlen(src));
    CHECK(j != NULL);
    CHECK(json_num(json_at(json_get(j, "a"), 1), 0) == 25.0);
    CHECK(json_at(json_get(j, "a"), 2)->b);
    CHECK_STR(json_str(json_get(j, "s")), "x\xc3\xa9\xf0\x9f\x98\x80\n");
    CHECK(json_num(json_path(j, "o", "k", NULL), 0) == -3);
    CHECK(json_get(j, "missing") == NULL && json_at(json_get(j, "a"), 9) == NULL);
    json_free(j);
    const char *bad[] = {"", "{", "[1,]", "{\"a\"}", "\"\\x\"", "tru", "[1] x", "\"\x01\""};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(json_parse(bad[i], strlen(bad[i])) == NULL);
    char deep[300];
    memset(deep, '[', sizeof deep);
    CHECK(json_parse(deep, sizeof deep) == NULL); /* depth bound */
    Str q = {0};
    json_quote(&q, "a\"b\\\n\x01", 6);
    CHECK_STR(str_cstr(&q), "\"a\\\"b\\\\\\n\\u0001\"");
    str_free(&q);
}

static void test_keymap_config(void) {
    Editor *e = editor_new(true);
    const char *c0 = NULL;
    const Keymap *s0 = NULL;
    CHECK(keymap_lookup(e->keymap, 15, &c0, &s0) == KM_COMMAND); /* nano default */
    CHECK_STR(c0, "save-buffer");
    CHECK(keymap_lookup(e->keymap, 24, &c0, &s0) == KM_COMMAND);
    CHECK_STR(c0, "save-buffers-kill-editor");
    e->cfg.keymap = KEYMAP_EMACS;
    editor_reset_keymap(e);
    const char *cmd = NULL;
    const Keymap *sub = NULL;
    CHECK(keymap_lookup(e->keymap, 24, &cmd, &sub) == KM_PREFIX);
    CHECK(keymap_lookup(sub, 19, &cmd, &sub) == KM_COMMAND);
    CHECK_STR(cmd, "save-buffer");
    char err[256];
    CHECK(config_apply_line(e, "bind C-c d kill-line", err, sizeof err) == 0);
    CHECK(keymap_lookup(e->keymap, 3, &cmd, &sub) == KM_PREFIX);
    CHECK(keymap_lookup(sub, 'd', &cmd, &sub) == KM_COMMAND);
    CHECK(config_apply_line(e, "tab-width 2", err, sizeof err) == 0 && e->cfg.tab_width == 2);
    CHECK(config_apply_line(e, "tab-width 99", err, sizeof err) != 0);
    CHECK(config_apply_line(e, "face keyword #ff0000 - bold", err, sizeof err) == 0);
    CHECK(e->styles[HL_KEYWORD].fg == 0xff0000 && e->styles[HL_KEYWORD].attr == A_BOLD);
    CHECK(config_apply_line(e, "  # comment", err, sizeof err) == 0);
    CHECK(config_apply_line(e, "nonsense 1", err, sizeof err) != 0);
    CHECK(config_apply_line(e, "keymap simple", err, sizeof err) == 0);
    CHECK(keymap_lookup(e->keymap, 17, &cmd, &sub) == KM_COMMAND);
    CHECK_STR(cmd, "save-buffers-kill-editor");
    char where[64];
    CHECK(keymap_where_is(e->keymap, "save-buffer", where, sizeof where));
    CHECK_STR(where, "C-s");
    Str errs = {0};
    CHECK(config_load_file(e, "examples/config", &errs) == 0);
    if (errs.len) fprintf(stderr, "%s", errs.data);
    str_free(&errs);
    editor_free(e);
}

static void type(Editor *e, const char *s) {
    while (*s) {
        uint32_t cp;
        size_t n = utf8_decode(s, strlen(s), &cp, NULL);
        editor_handle_key(e, *s == '\n' ? KEY_RET : (Key)cp);
        s += n;
    }
}

static char *contents(Editor *e) {
    return text_all(&editor_buf(e)->text);
}

static void test_editor(void) {
    Editor *e = editor_new(true);
    e->cfg.keymap = KEYMAP_EMACS; /* these tests drive Emacs keys */
    editor_reset_keymap(e);
    Buffer *b = buffer_new("*scratch*");
    editor_init_buffer(e, b);
    editor_add_buffer(e, b);
    char *s;

    /* auto-pairing */
    {
        type(e, "\"");
        s = contents(e);
        CHECK_STR(s, "\"\"");
        CHECK(b->cursor == 1);
        free(s);
        editor_handle_key(e, KEY_DEL);
        CHECK(buffer_len(b) == 0);

        Buffer *t = buffer_new("t.py");
        t->syntax = syntax_by_name("Python");
        editor_init_buffer(e, t);
        editor_add_buffer(e, t);
        editor_switch(e, e->nbufs - 1);
        type(e, "f(x");
        char *c = contents(e);
        CHECK_STR(c, "f(x)");
        free(c);
        type(e, ")");                 /* types over the closer */
        CHECK(t->cursor == 4);
        type(e, " = [");
        editor_handle_key(e, KEY_DEL); /* deletes the empty pair */
        c = contents(e);
        CHECK_STR(c, "f(x) = ");
        free(c);
        type(e, "\"a");               /* quotes pair in Python */
        c = contents(e);
        CHECK_STR(c, "f(x) = \"a\"");
        free(c);
        type(e, "\" don't");          /* no pairing after a word char */
        c = contents(e);
        CHECK_STR(c, "f(x) = \"a\" don't");
        free(c);
        buffer_replace_all(t, "word", 4); /* wrap selection */
        t->mark = 0;
        t->mark_active = true;
        t->cursor = 4;
        type(e, "(");
        c = contents(e);
        CHECK_STR(c, "(word)");
        free(c);
        buffer_replace_all(t, "x", 1);   /* no pair before a word */
        t->cursor = 0;
        type(e, "(");
        c = contents(e);
        CHECK_STR(c, "(x");
        free(c);
        editor_close(e, e->nbufs - 1);
        e->cur = 0;
    }
    e->cfg.auto_pair = false; /* the rest types closers by hand */

    type(e, "int f() {\n");
    s = contents(e);
    CHECK_STR(s, "int f() {\n    ");
    free(s);
    type(e, "x;\n}");
    s = contents(e);
    CHECK_STR(s, "int f() {\n    x;\n}");
    free(s);

    /* RET between brackets puts the closer on its own line */
    {
        Buffer *t = buffer_new("t");
        editor_init_buffer(e, t);
        editor_add_buffer(e, t);
        editor_switch(e, e->nbufs - 1);
        type(e, "f(){}");
        command_run(e, "backward-char");
        editor_handle_key(e, KEY_RET);
        char *c = contents(e);
        CHECK_STR(c, "f(){\n    \n}");
        free(c);
        editor_close(e, e->nbufs - 1);
        e->cur = 0;
    }

    /* undo removes the typed run, redo restores */
    editor_handle_key(e, 31); /* C-_ */
    s = contents(e); /* undoes "}" and its electric dedent */
    CHECK_STR(s, "int f() {\n    x;\n    ");
    free(s);
    editor_handle_key(e, '_' | KM_ALT);
    s = contents(e);
    CHECK_STR(s, "int f() {\n    x;\n}");
    free(s);

    /* kill-line + yank */
    editor_handle_key(e, '<' | KM_ALT);
    editor_handle_key(e, 11); /* C-k */
    editor_handle_key(e, 11);
    s = contents(e);
    CHECK_STR(s, "    x;\n}");
    free(s);
    CHECK_STR(str_cstr(&e->kill), "int f() {\n");
    editor_handle_key(e, '>' | KM_ALT);
    editor_handle_key(e, KEY_RET);
    editor_handle_key(e, 25); /* C-y */
    s = contents(e);
    CHECK_STR(s, "    x;\n}\nint f() {\n");
    free(s);

    /* shift selection + typing replaces it */
    editor_handle_key(e, 'h' | KM_ALT | 0); /* unbound: undefined message */
    editor_handle_key(e, '<' | KM_ALT);
    editor_handle_key(e, KEY_RIGHT | KM_SHIFT);
    editor_handle_key(e, KEY_RIGHT | KM_SHIFT);
    CHECK(b->mark_active && b->mark == 0 && b->cursor == 2);
    type(e, "Z");
    s = contents(e);
    CHECK(strncmp(s, "Z  x;", 5) == 0);
    free(s);
    CHECK(!b->mark_active);

    /* smart home */
    editor_handle_key(e, 'n' & 0x1f); /* C-n */
    editor_handle_key(e, 5);          /* C-e */
    editor_handle_key(e, 1);          /* C-a */
    CHECK(b->cursor == text_line_start(&b->text, 1));

    /* paste is literal: no auto-indent */
    editor_handle_key(e, '>' | KM_ALT);
    editor_handle_key(e, KEY_PASTE_START);
    type(e, "{\nq");
    editor_handle_key(e, KEY_PASTE_END);
    s = contents(e);
    CHECK(strstr(s, "{\nq") != NULL);
    free(s);

    /* escape sequences in the document never reach the terminal */
    editor_handle_key(e, KEY_PASTE_START);
    type(e, "\n");
    const char evil[] = "\x1b]0;pwned\x07\x1b[2J";
    for (const char *p = evil; *p; p++) editor_handle_key(e, (Key)(unsigned char)*p);
    editor_handle_key(e, KEY_PASTE_END);
    /* paste filters control chars; insert raw bytes directly as a file would */
    buffer_insert(b, 0, evil, strlen(evil));
    e->rows = 10;
    e->cols = 40;
    render_frame(e);
    CHECK(strstr(str_cstr(&e->frame), "\x1b]0;") == NULL);
    CHECK(strstr(str_cstr(&e->frame), "\x07") == NULL);
    CHECK(strstr(str_cstr(&e->frame), "^[") != NULL);
    CHECK(strstr(str_cstr(&e->frame), "]0;pwned") != NULL);

    /* matching bracket */
    buffer_replace_all(b, "a(b[c]d)e", 9);
    b->cursor = 1;
    command_run(e, "goto-matching-bracket");
    CHECK(b->cursor == 7);
    command_run(e, "goto-matching-bracket");
    CHECK(b->cursor == 1);

    /* indent / dedent region */
    buffer_replace_all(b, "a\nb\n", 4);
    b->mark = 0;
    b->mark_active = true;
    b->cursor = 4;
    command_run(e, "indent");
    s = contents(e);
    CHECK_STR(s, "    a\n    b\n");
    free(s);
    command_run(e, "dedent");
    s = contents(e);
    CHECK_STR(s, "a\nb\n");
    free(s);

    /* word motion and kill-word */
    buffer_replace_all(b, "foo bar_baz qux", 15);
    b->cursor = 0;
    command_run(e, "forward-word");
    CHECK(b->cursor == 3);
    command_run(e, "forward-word");
    CHECK(b->cursor == 11);
    command_run(e, "backward-kill-word");
    s = contents(e);
    CHECK_STR(s, "foo  qux");
    free(s);

    /* goal column across short lines */
    buffer_replace_all(b, "abcdef\nab\nabcdef", 16);
    b->cursor = 5;
    command_run(e, "next-line");
    CHECK(b->cursor == 9);
    command_run(e, "next-line");
    CHECK(b->cursor == 15);

    /* multiple buffers */
    Buffer *b2 = buffer_new("*scratch*");
    editor_init_buffer(e, b2);
    editor_add_buffer(e, b2);
    CHECK(strcmp(b2->name, "*scratch*<2>") == 0);
    command_run(e, "next-buffer");
    CHECK(editor_buf(e) == b2);
    command_run(e, "next-buffer");
    CHECK(editor_buf(e) == b);
    editor_close(e, 1);
    CHECK(e->nbufs == 1);

    /* opening a file replaces nothing modified; reopening switches */
    write_raw(tmp_path("open.c"), "int main(void) { return 0; }\n");
    Buffer *o = editor_open(e, tmp_path("open.c"));
    CHECK(o && o->syntax == syntax_by_name("C") && e->nbufs == 2);
    CHECK(editor_open(e, tmp_path("open.c")) == o && e->nbufs == 2);

    editor_free(e);
}

static void test_special_buffers(void) {
    Editor *e = editor_new(true);
    const char *names[] = {"*Help*", "*Buffers*"};
    const char *commands[] = {"describe-bindings", "list-buffers"};
    for (size_t i = 0; i < 2; i++) {
        Buffer *b = buffer_new(names[i]);
        editor_add_buffer(e, b);
        CHECK(buffer_insert(b, 0, "unsaved", 7) == 0);
        command_run(e, commands[i]);
        Buffer *generated = editor_buf(e);
        CHECK(generated != b && generated->readonly);
        CHECK(!b->readonly && buffer_modified(b));
        char *data = text_all(&b->text);
        CHECK_STR(data, "unsaved");
        free(data);
        size_t count = e->nbufs;
        command_run(e, commands[i]);
        CHECK(e->nbufs == count && editor_buf(e) == generated);
        CHECK(buffer_undo(b) && buffer_len(b) == 0);
    }
    editor_free(e);
}

static void test_nano_keys(void) {
    Editor *e = editor_new(true);
    Buffer *b = buffer_new("n");
    editor_init_buffer(e, b);
    editor_add_buffer(e, b);
    type(e, "one\ntwo\nthree");
    editor_handle_key(e, '\\' | KM_ALT); /* M-\ first line */
    CHECK(b->cursor == 0);
    editor_handle_key(e, 11);             /* ^K cuts the whole line */
    editor_handle_key(e, 11);             /* consecutive cuts append */
    char *s = contents(e);
    CHECK_STR(s, "three");
    free(s);
    CHECK_STR(str_cstr(&e->kill), "one\ntwo\n");
    editor_handle_key(e, '/' | KM_ALT);   /* M-/ last line */
    editor_handle_key(e, 21);             /* ^U pastes */
    s = contents(e);
    CHECK_STR(s, "threeone\ntwo\n");
    free(s);
    editor_handle_key(e, 'u' | KM_ALT);   /* M-U undo */
    s = contents(e);
    CHECK_STR(s, "three");
    free(s);
    editor_handle_key(e, 'e' | KM_ALT);   /* M-E redo */
    editor_handle_key(e, 3);              /* ^C position */
    CHECK(strstr(e->msg, "line 3/3") != NULL);
    editor_free(e);
}

static void test_tree(void) {
    char root[300];
    snprintf(root, sizeof root, "%s", tmp_path("tree"));
    CHECK(mkdir(root, 0700) == 0);
    CHECK(mkdir(tmp_path("tree/sub"), 0700) == 0);
    write_raw(tmp_path("tree/b.txt"), "hello\n");
    write_raw(tmp_path("tree/sub/a.c"), "x\n");
    write_raw(tmp_path("tree/.hidden"), "");
    Editor *e = editor_new(true);
    editor_add_buffer(e, buffer_new("scratch"));
    command_run(e, "file-tree"); /* lists the cwd; reload our dir below */
    CHECK(e->tree_visible && !e->tree_focus && tree_width(e) == 30);
    tree_load(e, root);
    CHECK(e->tree_n == 2 && e->tree[0].dir); /* dirs first, no dotfiles */
    CHECK(tree_key(e, KEY_RIGHT) && e->tree_n == 3 && e->tree[1].depth == 1);
    CHECK(tree_key(e, KEY_DOWN) && e->tree_sel == 1);
    CHECK(tree_key(e, KEY_LEFT) && e->tree_sel == 0); /* to parent */
    CHECK(tree_key(e, KEY_LEFT) && e->tree_n == 2);   /* fold */
    CHECK(!tree_key(e, 'x'));
    e->tree_focus = true;
    CHECK(tree_key(e, KEY_END) && tree_key(e, KEY_RET));
    CHECK(!e->tree_focus && strcmp(editor_buf(e)->name, "b.txt") == 0);
    render_frame(e);
    CHECK(strstr(str_cstr(&e->frame), "sub/") != NULL);
    CHECK(tree_key(e, 'q') && !e->tree_visible && tree_width(e) == 0);
    editor_free(e);
}

static void mouse_at(Editor *e, int button, int x, int y, bool drag) {
    e->mouse = (Mouse){button, x, y, false, drag};
    editor_handle_key(e, KEY_MOUSE);
}

static void test_mouse(void) {
    const char *seq = "\x1b[<0;12;3M";
    Key k;
    Mouse m;
    CHECK(input_parse((const unsigned char *)seq, strlen(seq), false, &k) ==
              strlen(seq) && k == KEY_MOUSE);
    CHECK(input_mouse((const unsigned char *)seq, strlen(seq), &m));
    CHECK(m.button == 0 && m.x == 11 && m.y == 2 && !m.release && !m.drag);
    seq = "\x1b[<32;1;1m"; /* motion bit, release */
    CHECK(input_mouse((const unsigned char *)seq, strlen(seq), &m));
    CHECK(m.drag && m.release && m.button == 0);
    seq = "\x1b[<0;0;1M"; /* columns start at 1 */
    CHECK(!input_mouse((const unsigned char *)seq, strlen(seq), &m));
    seq = "\x1b[<0;1"; /* incomplete: wait for more */
    CHECK(input_parse((const unsigned char *)seq, strlen(seq), false, &k) == 0);

    Editor *e = editor_new(true);
    Buffer *b = buffer_new("m");
    editor_add_buffer(e, b);
    Str s = {0};
    for (int i = 0; i < 100; i++) str_appendf(&s, "line %d\n", i);
    buffer_insert(b, 0, s.data, s.len);
    str_free(&s);
    b->cursor = 0;
    int left = render_text_left(e); /* gutter */
    mouse_at(e, 0, left + 2, 3, false);
    CHECK(buffer_cur_line(b) == 3 && buffer_col_of(b, b->cursor) == 2);
    mouse_at(e, 0, left + 4, 4, true); /* drag selects */
    size_t rs, re;
    CHECK(buffer_region(b, &rs, &re) && re - rs == 9 && b->shift_selection);
    mouse_at(e, 0, 0, 0, false); /* click in gutter: column 0, deselect */
    CHECK(b->cursor == 0 && !b->mark_active);
    mouse_at(e, 65, left, 0, false); /* wheel down moves the cursor along */
    CHECK(b->top == 3 && buffer_cur_line(b) == 3);
    mouse_at(e, 64, left, 0, false);
    CHECK(b->top == 0);
    mouse_at(e, 0, left, e->rows - 1, false); /* message line: ignored */
    CHECK(buffer_cur_line(b) == 3);
    mouse_at(e, 0, left + 1, 0, true); /* drag not started in text */
    CHECK(!b->mark_active);
    mouse_at(e, 0, left, 1, false);
    mouse_at(e, 0, left + 3, 1, true);
    mouse_at(e, 2, left + 1, 1, false); /* right-click inside: keeps it */
    CHECK(b->mark_active && !e->popup_active && e->popup == NULL);
    mouse_at(e, 2, left + 2, 5, false); /* outside: moves, deselects */
    CHECK(!b->mark_active && buffer_cur_line(b) == 5);
    editor_free(e);
}

int main(void) {
    setlocale(LC_CTYPE, "C.UTF-8");
    snprintf(tmpdir, sizeof tmpdir, "/tmp/cedit-test-XXXXXX");
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }
    RUN(test_text_random);
    RUN(test_utf8);
    RUN(test_input);
    RUN(test_undo);
    RUN(test_buffer_motion);
    RUN(test_file);
    RUN(test_search);
    RUN(test_syntax);
    RUN(test_json);
    RUN(test_keymap_config);
    RUN(test_editor);
    RUN(test_nano_keys);
    RUN(test_special_buffers);
    RUN(test_tree);
    RUN(test_mouse);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0) fprintf(stderr, "warning: could not remove %s\n", tmpdir);
    printf("%d checks, %d failures\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}
