#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cedit/editor.h"
#include "cedit/script.h"

#ifndef CEDIT_VERSION
#define CEDIT_VERSION "1.2.0"
#endif

static void usage(FILE *out) {
    fputs("Usage: cedit [OPTION]... [+LINE[:COL]] [FILE]...\n"
          "A lightweight, Emacs-inspired terminal text editor.\n\n"
          "  -e, --emacs        use the Emacs keymap (C-x C-s save, C-x C-c quit)\n"
          "  -s, --simple       use the simple keymap (Ctrl+S save, Ctrl+Q quit)\n"
          "  -n, --no-config    ignore the configuration file\n"
          "      --no-plugins   do not load Lua plugins\n"
          "  -h, --help         show this help\n"
          "  -V, --version      show version information\n"
          "      --update       check for and install a new version (also: cedit update)\n\n"
          "Default (nano-style) keys: ^O save, ^X exit, ^G help.\n",
          out);
}

static void ensure_utf8_ctype(void) {
    setlocale(LC_CTYPE, "");
    const char *cs = nl_langinfo(CODESET);
    if (!cs || (strcmp(cs, "UTF-8") != 0 && strcmp(cs, "utf8") != 0)) {
        /* display widths come from the locale; prefer a UTF-8 one */
        if (!setlocale(LC_CTYPE, "C.UTF-8")) setlocale(LC_CTYPE, "en_US.UTF-8");
    }
}

/* `cedit update`: run the cedit-update script installed next to us. */
static int run_update(void) {
    char exe[4096], prefix[4200], script[4200];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const char *lua = script_available() ? "--lua" : NULL;
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        *slash = '\0';
        snprintf(script, sizeof script, "%s/cedit-update", exe);
        snprintf(prefix, sizeof prefix, "%s/..", exe);
        execl(script, "cedit-update", "--check-update", "--prefix", prefix,
              lua, (char *)NULL);
    }
    /* no /proc (macOS) or no script beside the binary: try PATH */
    execlp("cedit-update", "cedit-update", "--check-update", lua, (char *)NULL);
    fprintf(stderr, "cedit: cannot run cedit-update: %s\n"
                    "Installed from a package? Update with your package manager.\n",
            strerror(errno));
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "update") || !strcmp(argv[1], "--update")))
        return run_update();
    int keymap = -1;
    bool no_config = false, no_plugins = false;
    int first_file = argc;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            first_file = i + 1;
            break;
        }
        if (a[0] != '-' || !a[1]) {
            first_file = i;
            break;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("cedit %s (Lua scripting: %s)\n", CEDIT_VERSION,
                   script_available() ? "yes" : "no");
            return 0;
        } else if (!strcmp(a, "-s") || !strcmp(a, "--simple")) {
            keymap = KEYMAP_SIMPLE;
        } else if (!strcmp(a, "-e") || !strcmp(a, "--emacs")) {
            keymap = KEYMAP_EMACS;
        } else if (!strcmp(a, "-n") || !strcmp(a, "--no-config")) {
            no_config = true;
        } else if (!strcmp(a, "--no-plugins")) {
            no_plugins = true;
        } else {
            fprintf(stderr, "cedit: unknown option '%s'\n", a);
            usage(stderr);
            return 2;
        }
    }

    ensure_utf8_ctype();
    Editor *e = editor_new(false);
    Str cfg_errors = {0};
    if (!no_config) {
        char *path = config_path("config");
        if (path) config_load_file(e, path, &cfg_errors);
        free(path);
    }
    if (keymap >= 0) {
        e->cfg.keymap = keymap;
        editor_reset_keymap(e);
    }
    if (no_plugins || no_config) e->cfg.load_plugins = false;

    if (term_init() != 0) {
        fprintf(stderr, "cedit: cannot initialize terminal: %s\n",
                strerror(errno));
        editor_free(e);
        return 1;
    }
    term_size(&e->rows, &e->cols);
    recent_load(e);

    Buffer *scratch = buffer_new("*scratch*");
    if (!scratch) {
        term_restore();
        fputs("cedit: out of memory\n", stderr);
        return 1;
    }
    editor_init_buffer(e, scratch);
    editor_add_buffer(e, scratch);

    if (!no_config) script_init(e);

    unsigned long line = 0, col = 0;
    for (int i = first_file; i < argc; i++) {
        if (argv[i][0] == '+' && i + 1 < argc) {
            char *end;
            line = strtoul(argv[i] + 1, &end, 10);
            col = *end == ':' ? strtoul(end + 1, NULL, 10) : 0;
            continue;
        }
        Buffer *b = editor_open(e, argv[i]);
        if (!b) continue;
        if (line) {
            size_t n = text_lines(&b->text);
            size_t l = line > n ? n - 1 : line - 1;
            b->cursor = buffer_pos_at_col(b, l, col ? col - 1 : 0);
            line = col = 0;
        }
    }
    if (e->nbufs > 1 && e->cur != 0) {
        /* show the first file given on the command line */
        for (size_t i = 0; i < e->nbufs; i++)
            if (e->bufs[i]->path) {
                editor_switch(e, i);
                break;
            }
    }
    if (cfg_errors.len) {
        char *nl = strchr(cfg_errors.data, '\n');
        if (nl) *nl = '\0';
        editor_error(e, "config: %s", cfg_errors.data);
    } else if (!e->msg[0]) {
        static const char *const hints[] = {
            "^O save  ^X exit  ^W search  ^K cut  ^U paste  ^G help",
            "C-x C-s save  C-x C-c quit  C-c ? help",
            "Ctrl+S save  Ctrl+Q quit  Ctrl+P commands"};
        editor_message(e, "%s", hints[e->cfg.keymap]);
    }
    str_free(&cfg_errors);

    editor_run(e);
    term_restore();
    editor_free(e);
    return 0;
}
