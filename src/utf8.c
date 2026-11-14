#define _XOPEN_SOURCE 700
#include "cedit/utf8.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

size_t utf8_decode(const char *str, size_t len, uint32_t *cp, bool *valid) {
    const unsigned char *s = (const unsigned char *)str;
    bool ok = false;
    size_t n = 1;
    uint32_t c = UTF8_REPLACEMENT;

    if (len == 0) {
        *cp = UTF8_REPLACEMENT;
        if (valid) *valid = false;
        return 0;
    }
    if (s[0] < 0x80) {
        c = s[0];
        ok = true;
    } else if (s[0] >= 0xC2 && s[0] <= 0xDF) {
        if (len >= 2 && utf8_is_cont(s[1])) {
            c = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            n = 2;
            ok = true;
        }
    } else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        if (len >= 3 && utf8_is_cont(s[1]) && utf8_is_cont(s[2])) {
            c = ((uint32_t)(s[0] & 0x0F) << 12) |
                ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            if (c >= 0x800 && (c < 0xD800 || c > 0xDFFF)) {
                n = 3;
                ok = true;
            }
        }
    } else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        if (len >= 4 && utf8_is_cont(s[1]) && utf8_is_cont(s[2]) &&
            utf8_is_cont(s[3])) {
            c = ((uint32_t)(s[0] & 0x07) << 18) |
                ((uint32_t)(s[1] & 0x3F) << 12) |
                ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
            if (c >= 0x10000 && c <= 0x10FFFF) {
                n = 4;
                ok = true;
            }
        }
    }
    if (!ok) {
        c = UTF8_REPLACEMENT;
        n = 1;
    }
    *cp = c;
    if (valid) *valid = ok;
    return n;
}

size_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Bidi controls can visually reorder text (Trojan Source); never pass
 * them to the terminal. */
static bool is_bidi_control(uint32_t cp) {
    return (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) ||
           cp == 0x200E || cp == 0x200F || cp == 0x061C;
}

bool cp_is_combining(uint32_t cp) {
    if (cp < 0x300 || is_bidi_control(cp)) return false;
    return wcwidth((wchar_t)cp) == 0;
}

static void glyph_special(Glyph *g, size_t bytes, const char *text) {
    size_t n = strlen(text);
    if (n > sizeof g->out) n = sizeof g->out;
    memcpy(g->out, text, n);
    g->outlen = n;
    g->width = (int)n; /* special text is pure ASCII */
    g->bytes = bytes;
    g->special = true;
}

void glyph_next(const char *s, size_t len, size_t col, int tab_width,
                Glyph *g) {
    unsigned char b = (unsigned char)s[0];
    char tmp[16];

    g->special = false;
    if (b == '\t') {
        int tw = tab_width < 1 ? 1 : (tab_width > 16 ? 16 : tab_width);
        int w = tw - (int)(col % (size_t)tw);
        memset(g->out, ' ', (size_t)w);
        g->outlen = (size_t)w;
        g->width = w;
        g->bytes = 1;
        return;
    }
    if (b < 0x20 || b == 0x7F) {
        tmp[0] = '^';
        tmp[1] = (char)(b == 0x7F ? '?' : b + 0x40);
        tmp[2] = '\0';
        glyph_special(g, 1, tmp);
        return;
    }
    if (b < 0x80) {
        g->out[0] = (char)b;
        g->outlen = 1;
        g->width = 1;
        g->bytes = 1;
        return;
    }

    uint32_t cp;
    bool valid;
    size_t n = utf8_decode(s, len, &cp, &valid);
    if (!valid) {
        snprintf(tmp, sizeof tmp, "<%02x>", b);
        glyph_special(g, 1, tmp);
        return;
    }
    if (cp < 0xA0) { /* C1 controls: 0x9B is a CSI on some terminals */
        snprintf(tmp, sizeof tmp, "<%02x>", (unsigned)cp);
        glyph_special(g, n, tmp);
        return;
    }
    if (is_bidi_control(cp)) {
        snprintf(tmp, sizeof tmp, "<U+%04X>", (unsigned)cp);
        glyph_special(g, n, tmp);
        return;
    }
    int w = wcwidth((wchar_t)cp);
    if (w < 0) {
        glyph_special(g, n, "?");
        return;
    }
    memcpy(g->out, s, n);
    g->outlen = n;
    g->width = w;
    g->bytes = n;
}

size_t display_width(const char *s, size_t len, int tab_width) {
    size_t col = 0, i = 0;
    Glyph g;
    while (i < len) {
        glyph_next(s + i, len - i, col, tab_width, &g);
        col += (size_t)g.width;
        i += g.bytes;
    }
    return col;
}

char *sanitize_for_display(const char *src, char *dst, size_t size) {
    size_t len = strlen(src), i = 0, o = 0;
    Glyph g;
    if (size == 0) return dst;
    while (i < len) {
        glyph_next(src + i, len - i, o, 1, &g);
        if (g.width == 0 && !g.special && o == 0) { /* lone combining */
            i += g.bytes;
            continue;
        }
        if (o + g.outlen + 1 > size) break;
        memcpy(dst + o, g.out, g.outlen);
        o += g.outlen;
        i += g.bytes;
    }
    dst[o] = '\0';
    return dst;
}
