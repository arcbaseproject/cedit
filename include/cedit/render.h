#ifndef CEDIT_RENDER_H
#define CEDIT_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "cedit/syntax.h"

struct Editor;

/* Style slots: syntax classes first, then UI elements. */
enum {
    S_LINENR = HL_CLASS_COUNT,
    S_CURLINENR,
    S_STATUS,
    S_SELECTION,
    S_MATCH,
    S_CURMATCH,
    S_SPECIAL,
    S_ERROR,
    S_FILLER,
    S_DIAG_ERROR,
    S_DIAG_WARN,
    S_POPUP,
    S_POPUP_SEL,
    S_COUNT
};

enum { A_BOLD = 1, A_ITALIC = 2, A_UNDERLINE = 4, A_REVERSE = 8 };

typedef struct {
    int32_t fg, bg;   /* 0xRRGGBB, or -1 for the terminal default */
    uint8_t attr;
} Style;

bool theme_builtin(Style styles[S_COUNT], const char *name);
const char *theme_names(void);
/* Slot index for "keyword", "status", ... or -1. */
int style_slot(const char *name);

/* Draw the editor; only changed rows are sent to the terminal. */
void render_frame(struct Editor *e);
/* Force a full repaint on the next frame (resize, suspend). */
void render_invalidate(struct Editor *e);
/* Adjust the current buffer's scroll so the cursor is visible. */
void render_scroll(struct Editor *e);
int render_text_rows(const struct Editor *e);
/* Screen column where buffer text starts (after tree and gutter). */
int render_text_left(struct Editor *e);
void render_free(struct Editor *e);

#endif
