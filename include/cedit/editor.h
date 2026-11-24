#ifndef CEDIT_EDITOR_H
#define CEDIT_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "cedit/buffer.h"
#include "cedit/commands.h"
#include "cedit/config.h"
#include "cedit/input.h"
#include "cedit/keymap.h"
#include "cedit/render.h"
#include "cedit/search.h"
#include "cedit/terminal.h"
#include "cedit/tree.h"
#include "cedit/util.h"

typedef struct Editor Editor;

/* Called after every change of the minibuffer text (key == KEY_NONE)
 * and for each key before default handling; return true to consume the
 * key. */
typedef bool (*PromptHook)(Editor *e, const char *text, Key key, void *ud);
/* Tab completion: may rewrite text, may set a message listing
 * candidates. */
typedef void (*Completer)(Editor *e, Str *text);

struct Editor {
    Buffer **bufs;
    size_t nbufs, cur;
    int rows, cols;
    bool headless;          /* no terminal (tests, benchmarks) */

    Config cfg;
    Style styles[S_COUNT];
    ColorMode color;
    Keymap *keymap;
    Command *cmds;
    size_t ncmds, cmdcap;

    Str kill;               /* kill buffer (clipboard) */
    bool this_kill, last_kill;

    char msg[512];
    bool msg_error;

    const Keymap *prefix;   /* pending prefix map (C-x ...) */
    char prefix_desc[64];
    const Command *this_cmd, *last_cmd;
    Key last_key;
    int insert_streak;
    bool pasting;
    bool quit;

    /* minibuffer */
    bool mb_active;
    char mb_prompt[128];
    Str mb_text;
    size_t mb_cursor;

    /* search highlighting */
    Search *hl_search;
    bool hl_have_cur;
    size_t hl_cur_start, hl_cur_end;
    char *last_search;
    bool last_search_regex;

    /* pending terminal input */
    unsigned char inbuf[4096];
    size_t inlen;

    /* renderer cache */
    bool bar_cursor;
    Str *rows_prev;
    int rows_prev_n;
    Str frame;
    uint8_t *hl_scratch;
    size_t hl_scratch_cap;

    /* completion popup */
    bool popup_active, popup_at_mouse;
    int popup_x, popup_y;
    char **popup;           /* sanitized display lines */
    size_t popup_n, popup_sel, popup_top;
    size_t popup_anchor;    /* buffer offset the popup is attached to */
    int popup_r0, popup_h, popup_c, popup_w; /* last drawn box; r0 -1: none */

    /* mouse (mouse.c) */
    Mouse mouse;            /* last report, set with KEY_MOUSE */
    bool mouse_on;          /* reporting enabled on the terminal */
    bool mouse_text_press;  /* left button went down in the text */

    /* file tree sidebar (tree.c) */
    bool tree_visible, tree_focus;
    char *tree_root;
    TreeEntry *tree;
    size_t tree_n, tree_sel, tree_top;

    void *lsp;              /* language server state (lsp.c) */
    double last_input;      /* monotonic time of the last key */

    char **recent;
    size_t nrecent;
    time_t last_disk_check;

    void *script;
};

Editor *editor_new(bool headless);
void editor_free(Editor *e);
Buffer *editor_buf(Editor *e);

void editor_message(Editor *e, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void editor_error(Editor *e, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Next key; handles resize (returns KEY_RESIZE) and termination
 * signals (emergency save + exit). */
Key editor_read_key(Editor *e);
void editor_handle_key(Editor *e, Key k);
void editor_run_command(Editor *e, const Command *c, bool shifted);
void editor_run(Editor *e);
/* Act on e->mouse: place the cursor, select by dragging, scroll, click
 * the file tree. */
void editor_mouse(Editor *e);

/* Modal minibuffer prompt. Returns malloc'd text or NULL if cancelled. */
char *editor_prompt(Editor *e, const char *prompt, const char *initial,
                    PromptHook hook, void *ud, Completer comp);
/* Single-key question; returns one of choices, or 0 if cancelled. */
int editor_ask(Editor *e, const char *question, const char *choices);

/* Buffer list. editor_open switches to an existing buffer visiting the
 * same file or loads it. */
Buffer *editor_open(Editor *e, const char *path);
void editor_add_buffer(Editor *e, Buffer *b);
void editor_switch(Editor *e, size_t idx);
/* Close buffer idx (no prompting). Keeps at least one buffer. */
void editor_close(Editor *e, size_t idx);
/* Make b->name unique among buffers. */
void editor_unique_name(Editor *e, Buffer *b);
bool editor_any_modified(const Editor *e);

/* Apply config defaults (tab width etc.) to a buffer. */
void editor_init_buffer(Editor *e, Buffer *b);
/* Rebuild keymap from defaults (after "keymap" option changes). */
void editor_reset_keymap(Editor *e);

/* Write modified buffers to <file>.save and report each to stderr.
 * Allocation-free (usable from the OOM handler), but not
 * async-signal-safe: called from the main loop only. */
void editor_emergency_save(Editor *e);

/* Show text in a read-only buffer called name and switch to it. */
void editor_show_text(Editor *e, const char *name, const char *text);
double editor_now(void);

void recent_add(Editor *e, const char *path);
void recent_load(Editor *e);

#endif
