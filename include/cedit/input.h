#ifndef CEDIT_INPUT_H
#define CEDIT_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Keys are Unicode code points (control characters stay raw: C-a is 1,
 * TAB 9, RET 13, ESC 27, DEL 127) or special values above the Unicode
 * range, optionally OR-ed with modifier bits.
 */
typedef int32_t Key;

#define KEY_NONE (-1)
#define KM_SHIFT 0x01000000
#define KM_ALT 0x02000000
#define KM_CTRL 0x04000000
#define KM_MASK (KM_SHIFT | KM_ALT | KM_CTRL)

#define KEY_TAB 9
#define KEY_RET 13
#define KEY_ESC 27
#define KEY_DEL 127

enum {
    KEY_SPECIAL = 0x200000,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_INSERT,
    KEY_DELETE,
    KEY_BACKTAB,
    KEY_PASTE_START,
    KEY_PASTE_END,
    KEY_RESIZE,     /* synthesized: screen needs a redraw (resize, LSP) */
};

static inline Key key_base(Key k) { return k & ~KM_MASK; }

/*
 * Parses one key from buf[0..len). Returns the number of bytes consumed
 * (>= 1) and stores the key (KEY_NONE for ignored sequences). Returns 0
 * if the bytes form an incomplete sequence and more may arrive; when
 * final is true (input timed out) incomplete data is resolved instead
 * (a lone ESC becomes KEY_ESC). Never reads past len.
 */
size_t input_parse(const unsigned char *buf, size_t len, bool final,
                   Key *key);

/* "C-x", "M-<left>", "RET" ... Returns false on unknown syntax. */
bool key_parse(const char *s, size_t n, Key *key);
/* Human-readable name into buf. */
void key_name(Key k, char *buf, size_t size);

#endif
