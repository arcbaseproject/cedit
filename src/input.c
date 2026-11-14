#include "cedit/input.h"

#include <stdio.h>
#include <string.h>

#include "cedit/utf8.h"

#define CSI_MAX 64

static size_t parse_plain(const unsigned char *buf, size_t len, bool final,
                          Key *key) {
    unsigned char b = buf[0];
    if (b < 0x80) {
        *key = b;
        return 1;
    }
    size_t need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
    if (need > 1 && len < need && !final) {
        size_t i = 1;
        while (i < len && utf8_is_cont(buf[i])) i++;
        if (i == len) return 0; /* plausible prefix, wait for the rest */
    }
    uint32_t cp;
    size_t n = utf8_decode((const char *)buf, len, &cp, NULL);
    *key = (Key)cp;
    return n;
}

static Key apply_mods(Key k, long mod) {
    if (mod < 2) return k;
    long m = mod - 1;
    if (m & 1) k |= KM_SHIFT;
    if (m & 2) k |= KM_ALT;
    if (m & 4) k |= KM_CTRL;
    return k;
}

static Key tilde_key(long p) {
    switch (p) {
    case 1: case 7: return KEY_HOME;
    case 4: case 8: return KEY_END;
    case 2: return KEY_INSERT;
    case 3: return KEY_DELETE;
    case 5: return KEY_PGUP;
    case 6: return KEY_PGDN;
    case 200: return KEY_PASTE_START;
    case 201: return KEY_PASTE_END;
    default: return KEY_NONE;
    }
}

static Key letter_key(unsigned char c) {
    switch (c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    default: return KEY_NONE;
    }
}

static Key kitty_key(long code, long mod) {
    long m = mod >= 2 ? mod - 1 : 0;
    Key k;
    if (code < 0 || code > 0x10FFFF) return KEY_NONE;
    k = (Key)code;
    if ((m & 4) && ((k >= 'a' && k <= 'z') || k == ' ' || k == '@')) {
        k = k == ' ' || k == '@' ? 0 : k - 'a' + 1;
        m &= ~4;
    }
    if (m & 2) k |= KM_ALT;
    return k;
}

static size_t parse_csi(const unsigned char *buf, size_t len, bool final,
                        Key *key) {
    long params[4] = {0, 0, 0, 0};
    size_t np = 0;
    bool have_digit = false, private_marker = false;
    size_t i = 2;
    for (; i < len && i < CSI_MAX; i++) {
        unsigned char c = buf[i];
        if (c >= '0' && c <= '9') {
            if (np < 4 && params[np] < 100000)
                params[np] = params[np] * 10 + (c - '0');
            have_digit = true;
        } else if (c == ';' || c == ':') {
            if (np < 4) np++;
            have_digit = false;
        } else if (c >= 0x3C && c <= 0x3F) {
            private_marker = true;
        } else if (c >= 0x20 && c <= 0x2F) {
            /* intermediate byte: tolerated, ignored */
        } else if (c >= 0x40 && c <= 0x7E) {
            break;
        } else {
            /* invalid byte inside CSI: drop the sequence before it */
            *key = KEY_NONE;
            return i;
        }
    }
    if (i >= CSI_MAX) {
        *key = KEY_NONE;
        return i;
    }
    if (i >= len) {
        if (!final) return 0;
        *key = KEY_NONE;
        return len;
    }
    if (have_digit || np) np = np < 4 ? np + 1 : 4;
    unsigned char f = buf[i];
    long mod = np >= 2 ? params[1] : 0;
    Key k = KEY_NONE;
    if (private_marker) {
        k = KEY_NONE;
    } else if (letter_key(f) != KEY_NONE) {
        k = apply_mods(letter_key(f), mod);
    } else if (f == 'Z') {
        k = KEY_BACKTAB;
    } else if (f == '~' && np >= 1) {
        k = tilde_key(params[0]);
        if (k != KEY_NONE && k != KEY_PASTE_START && k != KEY_PASTE_END)
            k = apply_mods(k, mod);
    } else if (f == 'u' && np >= 1) {
        k = kitty_key(params[0], mod);
    }
    *key = k;
    return i + 1;
}

size_t input_parse(const unsigned char *buf, size_t len, bool final,
                   Key *key) {
    *key = KEY_NONE;
    if (len == 0) return 0;
    if (buf[0] != KEY_ESC) return parse_plain(buf, len, final, key);
    if (len == 1) {
        if (!final) return 0;
        *key = KEY_ESC;
        return 1;
    }
    if (buf[1] == '[') return parse_csi(buf, len, final, key);
    if (buf[1] == 'O') {
        if (len < 3) {
            if (!final) return 0;
            *key = 'O' | KM_ALT;
            return 2;
        }
        *key = letter_key(buf[2]); /* F1-F4 etc. are ignored */
        return 3;
    }
    if (buf[1] == KEY_ESC) {
        *key = KEY_ESC;
        return 1;
    }
    Key k;
    size_t n = parse_plain(buf + 1, len - 1, final, &k);
    if (n == 0) return 0;
    *key = k == KEY_NONE ? KEY_NONE : k | KM_ALT;
    return n + 1;
}

static const struct {
    const char *name;
    Key key;
} named[] = {
    {"<up>", KEY_UP},         {"<down>", KEY_DOWN},
    {"<left>", KEY_LEFT},     {"<right>", KEY_RIGHT},
    {"<home>", KEY_HOME},     {"<end>", KEY_END},
    {"<prior>", KEY_PGUP},    {"<next>", KEY_PGDN},
    {"<pgup>", KEY_PGUP},     {"<pgdn>", KEY_PGDN},
    {"<insert>", KEY_INSERT}, {"<delete>", KEY_DELETE},
    {"<backtab>", KEY_BACKTAB},
    {"SPC", ' '},             {"TAB", KEY_TAB},
    {"RET", KEY_RET},         {"ESC", KEY_ESC},
    {"DEL", KEY_DEL},         {"<backspace>", KEY_DEL},
};

bool key_parse(const char *s, size_t n, Key *key) {
    Key mods = 0;
    while (n > 2 && s[1] == '-' && strchr("CMS", s[0])) {
        mods |= s[0] == 'C' ? KM_CTRL : s[0] == 'M' ? KM_ALT : KM_SHIFT;
        s += 2;
        n -= 2;
    }
    if (n == 0) return false;
    Key base = KEY_NONE;
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
        if (strlen(named[i].name) == n && memcmp(named[i].name, s, n) == 0) {
            base = named[i].key;
            break;
        }
    }
    if (base == KEY_NONE) {
        uint32_t cp;
        bool valid;
        size_t used = utf8_decode(s, n, &cp, &valid);
        if (!valid || used != n) return false;
        base = (Key)cp;
    }
    if (mods & KM_CTRL) {
        if (base >= 'a' && base <= 'z') {
            base = base - 'a' + 1;
            mods &= ~KM_CTRL;
        } else if (base == ' ' || base == '@') {
            base = 0;
            mods &= ~KM_CTRL;
        } else if (base == '/' || base == '_') {
            base = 31;
            mods &= ~KM_CTRL;
        } else if (base == '\\' || base == ']' || base == '^') {
            base = base == '\\' ? 28 : base == ']' ? 29 : 30;
            mods &= ~KM_CTRL;
        }
    }
    if ((mods & KM_SHIFT) && base < KEY_SPECIAL) {
        if (base >= 'a' && base <= 'z') base -= 32;
        mods &= ~KM_SHIFT;
    }
    *key = base | mods;
    return true;
}

void key_name(Key k, char *buf, size_t size) {
    char base[16];
    Key b = key_base(k);
    const char *pre_c = (k & KM_CTRL) ? "C-" : "";
    const char *pre_m = (k & KM_ALT) ? "M-" : "";
    const char *pre_s = (k & KM_SHIFT) ? "S-" : "";
    base[0] = '\0';
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
        if (named[i].key == b) {
            snprintf(base, sizeof base, "%s", named[i].name);
            break;
        }
    }
    if (!base[0]) {
        if (b == 0) {
            snprintf(base, sizeof base, "C-SPC");
        } else if (b >= 1 && b <= 26) {
            snprintf(base, sizeof base, "C-%c", 'a' + b - 1);
        } else if (b >= 28 && b <= 31) {
            snprintf(base, sizeof base, "C-%c", "\\]^_"[b - 28]);
        } else if (b > 32 && b <= 0x10FFFF) {
            char enc[4];
            size_t n = utf8_encode((uint32_t)b, enc);
            memcpy(base, enc, n);
            base[n] = '\0';
        } else {
            snprintf(base, sizeof base, "<%d>", (int)b);
        }
    }
    snprintf(buf, size, "%s%s%s%s", pre_c, pre_m, pre_s, base);
}
