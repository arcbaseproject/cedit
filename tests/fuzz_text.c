/* libFuzzer target: random edit scripts against the buffer (gap buffer,
 * line index, undo/redo, UTF-8 motion, syntax highlighting), checked
 * against a plain reference string. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cedit/buffer.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void verify(Buffer *b, const char *ref, size_t rlen) {
    if (buffer_len(b) != rlen) abort();
    char *s = text_dup(&b->text, 0, rlen);
    if (!s || memcmp(s, ref, rlen) != 0) abort();
    size_t line = 0;
    for (size_t i = 0; i < rlen; i++)
        if (s[i] == '\n' && text_line_start(&b->text, ++line) != i + 1) abort();
    if (text_lines(&b->text) != line + 1) abort();
    free(s);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Buffer *b = buffer_new("fuzz");
    if (!b) return 0;
    b->syntax = syntax_by_name((size && data[0] & 1) ? "C" : "Python");
    char *ref = calloc(1, 1);
    size_t rlen = 0, i = 0;
    while (i + 3 <= size) {
        uint8_t op = data[i], a = data[i + 1], n = data[i + 2] % 16;
        i += 3;
        size_t pos = rlen ? a % (rlen + 1) : 0;
        switch (op % 6) {
        case 0: case 1: { /* insert n raw bytes from the input */
            if (n > size - i) n = (uint8_t)(size - i);
            if (buffer_insert(b, pos, (const char *)data + i, n) == 0) {
                ref = realloc(ref, rlen + n + 1);
                memmove(ref + pos + n, ref + pos, rlen - pos);
                memcpy(ref + pos, data + i, n);
                rlen += n;
            }
            i += n;
            break;
        }
        case 2: {
            if (n > rlen - pos) n = (uint8_t)(rlen - pos);
            if (buffer_delete(b, pos, n) == 0) {
                memmove(ref + pos, ref + pos + n, rlen - pos - n);
                rlen -= n;
            }
            break;
        }
        case 3: /* undo/redo then resync the reference */
            undo_boundary(&b->undo);
            if (a & 1) buffer_undo(b); else buffer_redo(b);
            free(ref);
            rlen = buffer_len(b);
            ref = text_dup(&b->text, 0, rlen);
            break;
        case 4: { /* motion must stay in bounds */
            size_t p = buffer_next_char(b, pos);
            p = buffer_prev_char(b, p);
            if (p > buffer_len(b)) abort();
            size_t line = text_line_of(&b->text, pos);
            if (buffer_pos_at_col(b, line, a) > buffer_len(b)) abort();
            break;
        }
        case 5: /* highlight state over all lines */
            buffer_hl_state(b, text_lines(&b->text) - 1);
            break;
        }
        undo_boundary(&b->undo);
        verify(b, ref, rlen);
    }
    free(ref);
    buffer_free(b);
    return 0;
}
