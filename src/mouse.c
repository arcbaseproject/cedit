/* Mouse: click, drag-select, wheel and right-click menu in the text;
 * clicks in the tree. */
#include <stdlib.h>

#include "cedit/editor.h"

#define WHEEL_LINES 3

static void text_wheel(Buffer *b, bool up, int rows) {
    size_t n = text_lines(&b->text), r = (size_t)rows;
    if (up) b->top = b->top > WHEEL_LINES ? b->top - WHEEL_LINES : 0;
    else b->top = b->top + WHEEL_LINES < n ? b->top + WHEEL_LINES : n - 1;
    /* keep the cursor on screen, or render_scroll would undo the scroll */
    size_t line = buffer_cur_line(b), col = buffer_col_of(b, b->cursor);
    if (line < b->top) line = b->top;
    else if (line >= b->top + r) line = b->top + r - 1;
    else return;
    b->cursor = buffer_pos_at_col(b, line < n ? line : n - 1, col);
}

static bool in_popup(const Editor *e, const Mouse *m) {
    return e->popup_r0 >= 0 && m->y >= e->popup_r0 &&
           m->y < e->popup_r0 + e->popup_h && m->x >= e->popup_c &&
           m->x < e->popup_c + e->popup_w;
}

static size_t choose_item(Editor *e, const char *const *labels, size_t n) {
    e->popup = xcalloc(n, sizeof *e->popup);
    for (size_t i = 0; i < n; i++) e->popup[i] = xstrdup(labels[i]);
    e->popup_n = n;
    e->popup_sel = e->popup_top = 0;
    e->popup_active = true;

    size_t choice = SIZE_MAX;
    Key pending = KEY_NONE;
    while (!e->headless) {
        render_frame(e);
        Key k = editor_read_key(e);
        const Mouse *m = &e->mouse;
        if (k == KEY_RESIZE || k == KEY_NONE) continue;
        if (k == KEY_DOWN || k == 14) {
            e->popup_sel = (e->popup_sel + 1) % n;
        } else if (k == KEY_UP || k == 16) {
            e->popup_sel = (e->popup_sel + n - 1) % n;
        } else if (k == KEY_RET) {
            choice = e->popup_sel;
            break;
        } else if (k == KEY_MOUSE && (m->release || m->drag)) {
            continue; /* e.g. the release of the right click itself */
        } else if (k == KEY_MOUSE && in_popup(e, m)) {
            size_t idx = e->popup_top + (size_t)(m->y - e->popup_r0);
            if (m->button == 64) {
                e->popup_sel = (e->popup_sel + n - 1) % n;
            } else if (m->button == 65) {
                e->popup_sel = (e->popup_sel + 1) % n;
            } else if (idx < n) {
                choice = idx;
                break;
            }
        } else {
            if (k != KEY_ESC && k != 7) pending = k; /* close, then handle */
            break;
        }
    }
    for (size_t i = 0; i < n; i++) free(e->popup[i]);
    free(e->popup);
    e->popup = NULL;
    e->popup_n = e->popup_sel = 0;
    e->popup_active = false;
    e->popup_at_mouse = false;
    if (pending != KEY_NONE) editor_handle_key(e, pending);
    return choice;
}

/* Right-click menu at pos, drawn with the completion popup. */
static void context_menu(Editor *e, size_t pos) {
    static const char *const labels[] = {"Cut", "Copy", "Paste", "Select all", "Undo"};
    static const char *const commands[] = {
        "kill-region", "copy-region", "yank", "mark-whole-buffer", "undo"
    };
    Buffer *b = editor_buf(e);
    size_t rs, re;
    bool sel = buffer_region(b, &rs, &re) && pos >= rs && pos <= re;
    if (!sel) { /* clicked outside the selection: move there, like a click */
        b->mark_active = false;
        b->cursor = pos;
        b->goal_valid = false;
    }
    size_t first = sel ? 0 : 2; /* Cut and Copy need a selection */
    e->popup_anchor = pos;
    size_t choice = choose_item(e, labels + first, sizeof labels / sizeof *labels - first);
    if (choice != SIZE_MAX)
        editor_run_command(e, command_find(e, commands[first + choice]), false);
}

static void tree_mouse(Editor *e, const Mouse *m, int rows) {
    if (m->button == 64 || m->button == 65) { /* scroll; the selection follows */
        size_t r = (size_t)rows, *top = &e->tree_top;
        size_t max = e->tree_n > r ? e->tree_n - r : 0;
        if (m->button == 64) *top = *top > WHEEL_LINES ? *top - WHEEL_LINES : 0;
        else *top = *top + WHEEL_LINES < max ? *top + WHEEL_LINES : max;
        if (e->tree_sel < *top) e->tree_sel = *top;
        if (e->tree_sel >= *top + r) e->tree_sel = *top + r - 1;
        return;
    }
    size_t idx = e->tree_top + (size_t)m->y;
    if (m->button == 2 && !m->release && !m->drag) {
        static const char *const labels[] = {"Create file", "Create folder", "Rename", "Delete"};
        bool entry = idx < e->tree_n;
        if (entry) e->tree_sel = idx;
        e->popup_at_mouse = true;
        e->popup_x = m->x;
        e->popup_y = m->y;
        size_t choice = choose_item(e, labels, entry ? 4 : 2);
        if (choice != SIZE_MAX)
            tree_action(e, entry ? idx : SIZE_MAX, (TreeAction)choice);
        return;
    }
    if (m->button != 0 || m->release || m->drag || idx >= e->tree_n) return;
    e->tree_sel = idx;
    tree_key(e, KEY_RET); /* open the file or fold/unfold the folder */
}

void editor_mouse(Editor *e) {
    const Mouse *m = &e->mouse;
    Buffer *b = editor_buf(e);
    int rows = render_text_rows(e);
    bool wheel = m->button == 64 || m->button == 65;
    if (!m->drag) {
        if (m->y >= rows) { /* status and message lines */
            e->mouse_text_press = false;
            return;
        }
        if (m->x < tree_width(e)) {
            e->mouse_text_press = false;
            tree_mouse(e, m, rows);
            return;
        }
    }
    if (wheel) {
        text_wheel(b, m->button == 64, rows);
        return;
    }
    size_t n = text_lines(&b->text);
    size_t line = b->top + (size_t)(m->y < rows ? m->y : rows - 1);
    if (line >= n) line = n - 1;
    int x = m->x - render_text_left(e);
    size_t pos = buffer_pos_at_col(b, line, b->left + (size_t)(x > 0 ? x : 0));
    if (m->button == 2 && !m->release && !m->drag) {
        e->mouse_text_press = false;
        context_menu(e, pos);
        return;
    }
    if (m->button != 0 || m->release) return;
    if (!m->drag) e->mouse_text_press = true;
    else if (!e->mouse_text_press) return; /* drag that began elsewhere */

    if (!m->drag) {
        b->mark_active = false;
    } else if (!b->mark_active) {
        /* behaves like a Shift selection: typing replaces it */
        b->mark = b->cursor;
        b->mark_active = true;
        b->shift_selection = true;
    }
    b->cursor = pos;
    b->goal_valid = false;
    e->last_cmd = NULL; /* no undo coalescing across a click */
}
