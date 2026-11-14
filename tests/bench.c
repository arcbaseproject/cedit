/* Micro-benchmarks for the core paths. Prints measured numbers only. */
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "cedit/editor.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static long max_rss_kb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

static unsigned seed = 1;
static unsigned rnd(void) {
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

#define REPORT(name, secs, ops)                                              \
    printf("%-34s %10.2f ms  (%.3f us/op)\n", name, (secs) * 1e3,            \
           (secs) * 1e6 / (double)(ops))

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "C.UTF-8");
    size_t nlines = argc > 1 ? strtoul(argv[1], NULL, 10) : 1000000;
    char dir[] = "/tmp/cedit-bench-XXXXXX", path[64];
    if (!mkdtemp(dir)) return 1;
    snprintf(path, sizeof path, "%s/bench.c", dir); /* .c: highlighting on */
    FILE *f = fopen(path, "w");
    if (!f) return 1;
    for (size_t i = 0; i < nlines; i++)
        fprintf(f, "    int value_%zu = compute(%zu, \"str\"); /* c */\n", i, i);
    fclose(f);

    long rss0 = max_rss_kb();
    Editor *e = editor_new(true);
    e->rows = 50;
    e->cols = 160;
    Buffer *b = buffer_new("bench");
    editor_init_buffer(e, b);
    editor_add_buffer(e, b);
    char err[256];
    bool is_new;
    double t = now();
    if (buffer_load(b, path, &is_new, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    double load = now() - t;
    size_t bytes = buffer_len(b);
    printf("file: %zu lines, %.1f MB\n", text_lines(&b->text), (double)bytes / 1e6);
    REPORT("load", load, 1);
    printf("%-34s %10ld KB (baseline %ld KB)\n", "max RSS after load", max_rss_kb(), rss0);

    t = now();
    render_frame(e);
    REPORT("first render (top)", now() - t, 1);

    b->cursor = bytes / 2;
    t = now();
    render_frame(e);
    REPORT("render after jump to middle", now() - t, 1);

    t = now();
    for (int i = 0; i < 1000; i++) {
        b->cursor = buffer_next_char(b, b->cursor);
        render_frame(e);
    }
    REPORT("render per keystroke (x1000)", now() - t, 1000);

    t = now();
    size_t at = bytes / 2;
    for (int i = 0; i < 100000; i++) {
        buffer_insert(b, at, "x", 1);
        at++;
    }
    REPORT("typing 100k chars at middle", now() - t, 100000);

    t = now();
    for (int i = 0; i < 10000; i++) {
        size_t pos = rnd() % buffer_len(b);
        buffer_insert(b, pos, "y\n", 2);
    }
    REPORT("10k inserts at random offsets", now() - t, 10000);

    t = now();
    for (int i = 0; i < 100000; i++) buffer_delete(b, at - 1 - (size_t)i, 1);
    REPORT("100k backspaces at middle", now() - t, 100000);

    t = now();
    for (int i = 0; i < 10000; i++) buffer_delete(b, rnd() % (buffer_len(b) - 2), 2);
    REPORT("10k deletes at random offsets", now() - t, 10000);

    t = now();
    while (buffer_undo(b)) {}
    REPORT("undo everything", now() - t, 1);

    Search s;
    size_t ms, me;
    char needle[64];
    snprintf(needle, sizeof needle, "value_%zu ", nlines - 1);
    search_compile(&s, needle, false, err, sizeof err);
    t = now();
    bool found = search_find(&s, b, 0, true, false, &ms, &me);
    REPORT("literal search, match at end", now() - t, 1);
    search_free(&s);
    search_compile(&s, "value_[0-9]+9999 =", true, err, sizeof err);
    t = now();
    size_t count = 0, pos = 0;
    while (search_find(&s, b, pos, true, false, &ms, &me)) {
        count++;
        pos = me;
    }
    REPORT("regex search, all matches", now() - t, 1);
    search_free(&s);
    printf("(found=%d, regex matches=%zu)\n", found, count);

    b->cursor = buffer_len(b);
    t = now();
    render_frame(e);
    REPORT("render after jump to end", now() - t, 1);

    t = now();
    bool in_place;
    buffer_save(b, path, &in_place, err, sizeof err);
    REPORT("atomic save (fsync)", now() - t, 1);
    printf("%-34s %10ld KB\n", "max RSS at end", max_rss_kb());

    editor_free(e);
    unlink(path);
    rmdir(dir);
    return 0;
}
