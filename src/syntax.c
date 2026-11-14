#include "cedit/syntax.h"

#include <stdbool.h>
#include <string.h>

enum {
    F_DQ = 1 << 0,        /* "strings" with backslash escapes */
    F_SQ = 1 << 1,        /* 'strings' with backslash escapes */
    F_CHAR = 1 << 2,      /* 'c' char literals; lone ' is not a string */
    F_BACKTICK = 1 << 3,  /* `multiline strings` */
    F_TRIPLE = 1 << 4,    /* """ and ''' multiline strings */
    F_PREPROC = 1 << 5,   /* C preprocessor lines */
    F_COMMENT_WS = 1 << 6,/* line comment only at start or after space */
    F_SECTIONS = 1 << 7,  /* [section] headers (TOML) */
    F_SHELLVAR = 1 << 8,  /* $var ${var} */
    F_QUOTE_ML = 1 << 9,  /* quoted strings may span lines (shell) */
    F_SQ_RAW = 1 << 10,   /* no escapes in single quotes (shell) */
};

enum {
    ST_NONE = 0,
    ST_BLOCK_COMMENT,
    ST_TRIPLE_DQ,
    ST_TRIPLE_SQ,
    ST_BACKTICK,
    ST_DQ,
    ST_SQ,
    ST_FENCE,
};

typedef uint8_t (*HighlightFn)(const Syntax *s, const char *line, size_t len,
                               uint8_t state, uint8_t *hl);

struct Syntax {
    const char *name;
    const char *const *match;    /* ".ext" or exact basename */
    const char *const *shebang;  /* interpreter name prefixes */
    const char *const *keywords;
    const char *const *types;
    const char *lc1, *lc2;
    const char *bc_start, *bc_end;
    unsigned flags;
    HighlightFn fn;
};

static void mark(uint8_t *hl, size_t a, size_t b, uint8_t cls) {
    if (hl && b > a) memset(hl + a, cls, b - a);
}

static bool is_ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident(unsigned char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

static bool is_operator(unsigned char c) {
    return c && strchr("+-*/%=<>!&|^~?:;,.()[]{}@", c) != NULL;
}

static bool starts_with(const char *line, size_t len, size_t i,
                        const char *tok) {
    if (!tok) return false;
    size_t n = strlen(tok);
    return n <= len - i && memcmp(line + i, tok, n) == 0;
}

static size_t find(const char *line, size_t len, size_t i, const char *tok) {
    size_t n = strlen(tok);
    for (; i + n <= len; i++)
        if (memcmp(line + i, tok, n) == 0) return i;
    return SIZE_MAX;
}

/* Index just past the closing quote q starting the scan at i, or
 * SIZE_MAX if the line ends first. */
static size_t scan_quote(const char *line, size_t len, size_t i, char q,
                         bool escapes) {
    while (i < len) {
        if (escapes && line[i] == '\\') {
            i += 2;
            continue;
        }
        if (line[i] == q) return i + 1;
        i++;
    }
    return SIZE_MAX;
}

static bool in_list(const char *const *list, const char *w, size_t n) {
    if (!list) return false;
    for (; *list; list++)
        if (strlen(*list) == n && memcmp(*list, w, n) == 0) return true;
    return false;
}

/* Continue a multiline construct from the previous line. Returns the
 * index where normal scanning resumes, or SIZE_MAX if the whole line is
 * consumed (state stays). */
static size_t resume(const Syntax *s, const char *line, size_t len,
                     uint8_t *state, uint8_t *hl) {
    size_t end;
    switch (*state) {
    case ST_BLOCK_COMMENT:
        end = find(line, len, 0, s->bc_end);
        if (end == SIZE_MAX) break;
        end += strlen(s->bc_end);
        mark(hl, 0, end, HL_COMMENT);
        *state = ST_NONE;
        return end;
    case ST_TRIPLE_DQ:
    case ST_TRIPLE_SQ:
        end = find(line, len, 0, *state == ST_TRIPLE_DQ ? "\"\"\"" : "'''");
        if (end == SIZE_MAX) break;
        mark(hl, 0, end + 3, HL_STRING);
        *state = ST_NONE;
        return end + 3;
    case ST_BACKTICK:
    case ST_DQ:
    case ST_SQ: {
        char q = *state == ST_BACKTICK ? '`' : *state == ST_DQ ? '"' : '\'';
        bool esc = !(*state == ST_SQ && (s->flags & F_SQ_RAW));
        end = scan_quote(line, len, 0, q, esc);
        if (end == SIZE_MAX) break;
        mark(hl, 0, end, HL_STRING);
        *state = ST_NONE;
        return end;
    }
    default:
        return 0;
    }
    mark(hl, 0, len, *state == ST_BLOCK_COMMENT ? HL_COMMENT : HL_STRING);
    return SIZE_MAX;
}

static uint8_t generic(const Syntax *s, const char *line, size_t len,
                       uint8_t state, uint8_t *hl) {
    mark(hl, 0, len, HL_NORMAL);
    size_t i = resume(s, line, len, &state, hl);
    if (i == SIZE_MAX) return state;

    size_t first = i;
    while (first < len && is_space((unsigned char)line[first])) first++;
    if ((s->flags & F_SECTIONS) && i == 0 && first < len &&
        line[first] == '[') {
        size_t end = find(line, len, first, "]");
        end = end == SIZE_MAX ? len : end + 1;
        mark(hl, first, end, HL_TYPE);
        i = end;
    }
    if ((s->flags & F_PREPROC) && first < len && line[first] == '#') {
        size_t e = first + 1;
        while (e < len && is_space((unsigned char)line[e])) e++;
        size_t w = e;
        while (e < len && is_ident((unsigned char)line[e])) e++;
        mark(hl, first, e, HL_PREPROC);
        bool include = e - w == 7 && memcmp(line + w, "include", 7) == 0;
        i = e;
        while (i < len && is_space((unsigned char)line[i])) i++;
        if (include && i < len && line[i] == '<') {
            size_t c = find(line, len, i, ">");
            c = c == SIZE_MAX ? len : c + 1;
            mark(hl, i, c, HL_STRING);
            i = c;
        }
    }

    while (i < len) {
        unsigned char c = (unsigned char)line[i];
        bool prev_sep = i == 0 || !is_ident((unsigned char)line[i - 1]);

        if ((starts_with(line, len, i, s->lc1) ||
             starts_with(line, len, i, s->lc2)) &&
            (!(s->flags & F_COMMENT_WS) || i == 0 ||
             is_space((unsigned char)line[i - 1]))) {
            mark(hl, i, len, HL_COMMENT);
            return ST_NONE;
        }
        if (starts_with(line, len, i, s->bc_start)) {
            size_t e = find(line, len, i + strlen(s->bc_start), s->bc_end);
            if (e == SIZE_MAX) {
                mark(hl, i, len, HL_COMMENT);
                return ST_BLOCK_COMMENT;
            }
            e += strlen(s->bc_end);
            mark(hl, i, e, HL_COMMENT);
            i = e;
            continue;
        }
        if ((s->flags & F_TRIPLE) && (starts_with(line, len, i, "\"\"\"") ||
                                      starts_with(line, len, i, "'''"))) {
            const char *q = c == '"' ? "\"\"\"" : "'''";
            size_t e = find(line, len, i + 3, q);
            if (e == SIZE_MAX) {
                mark(hl, i, len, HL_STRING);
                return c == '"' ? ST_TRIPLE_DQ : ST_TRIPLE_SQ;
            }
            mark(hl, i, e + 3, HL_STRING);
            i = e + 3;
            continue;
        }
        if ((c == '"' && (s->flags & F_DQ)) ||
            (c == '\'' && (s->flags & F_SQ)) ||
            (c == '`' && (s->flags & F_BACKTICK))) {
            bool esc = !(c == '\'' && (s->flags & F_SQ_RAW));
            size_t e = scan_quote(line, len, i + 1, (char)c, esc);
            if (e == SIZE_MAX) {
                mark(hl, i, len, HL_STRING);
                if (c == '`') return ST_BACKTICK;
                if (s->flags & F_QUOTE_ML) return c == '"' ? ST_DQ : ST_SQ;
                return ST_NONE;
            }
            mark(hl, i, e, HL_STRING);
            i = e;
            continue;
        }
        if (c == '\'' && (s->flags & F_CHAR)) {
            /* char literal only if it closes nearby; else Rust lifetime */
            size_t lim = i + (i + 1 < len && line[i + 1] == '\\' ? 12 : 6);
            size_t e = scan_quote(line, len < lim ? len : lim, i + 1, '\'',
                                  true);
            if (e != SIZE_MAX && e > i + 2) {
                mark(hl, i, e, HL_STRING);
                i = e;
                continue;
            }
        }
        if (c == '$' && (s->flags & F_SHELLVAR) && i + 1 < len) {
            size_t e = i + 1;
            if (line[e] == '{') {
                size_t r = find(line, len, e, "}");
                e = r == SIZE_MAX ? len : r + 1;
            } else if (is_ident((unsigned char)line[e])) {
                while (e < len && is_ident((unsigned char)line[e])) e++;
            } else if (strchr("?#@*!$-", line[e])) {
                e++;
            }
            if (e > i + 1) {
                mark(hl, i, e, HL_TYPE);
                i = e;
                continue;
            }
        }
        if (is_digit(c) && prev_sep) {
            size_t e = i + 1;
            while (e < len) {
                unsigned char d = (unsigned char)line[e];
                if (is_ident(d) || d == '.' ||
                    ((d == '+' || d == '-') &&
                     (line[e - 1] == 'e' || line[e - 1] == 'E') &&
                     !(line[i] == '0' && i + 1 < len &&
                       (line[i + 1] == 'x' || line[i + 1] == 'X'))))
                    e++;
                else
                    break;
            }
            mark(hl, i, e, HL_NUMBER);
            i = e;
            continue;
        }
        if (is_ident_start(c) && prev_sep) {
            size_t e = i + 1;
            while (e < len && is_ident((unsigned char)line[e])) e++;
            if (in_list(s->keywords, line + i, e - i))
                mark(hl, i, e, HL_KEYWORD);
            else if (in_list(s->types, line + i, e - i))
                mark(hl, i, e, HL_TYPE);
            i = e;
            continue;
        }
        if (is_operator(c)) mark(hl, i, i + 1, HL_OPERATOR);
        i++;
    }
    return ST_NONE;
}

/* ---- Markdown ---- */

static size_t md_inline(const char *line, size_t len, size_t i, uint8_t *hl) {
    while (i < len) {
        char c = line[i];
        if (c == '`') {
            size_t e = find(line, len, i + 1, "`");
            if (e != SIZE_MAX) {
                mark(hl, i, e + 1, HL_STRING);
                i = e + 1;
                continue;
            }
        } else if ((c == '*' || c == '_') && i + 1 < len &&
                   !is_space((unsigned char)line[i + 1])) {
            char d[3] = {c, 0, 0};
            if (line[i + 1] == c) d[1] = c;
            size_t dl = strlen(d);
            size_t e = find(line, len, i + dl, d);
            if (e != SIZE_MAX && e > i + dl) {
                mark(hl, i, e + dl, HL_TYPE);
                i = e + dl;
                continue;
            }
        } else if (c == '[') {
            size_t e = find(line, len, i, "](");
            size_t p = e == SIZE_MAX ? SIZE_MAX : find(line, len, e, ")");
            if (p != SIZE_MAX) {
                mark(hl, i, e + 1, HL_NUMBER);
                mark(hl, e + 1, p + 1, HL_COMMENT);
                i = p + 1;
                continue;
            }
        }
        i++;
    }
    return i;
}

static uint8_t markdown(const Syntax *s, const char *line, size_t len,
                        uint8_t state, uint8_t *hl) {
    (void)s;
    mark(hl, 0, len, HL_NORMAL);
    size_t i = 0;
    while (i < len && i < 4 && line[i] == ' ') i++;
    if (starts_with(line, len, i, "```") || starts_with(line, len, i, "~~~")) {
        mark(hl, 0, len, HL_PREPROC);
        return state == ST_FENCE ? ST_NONE : ST_FENCE;
    }
    if (state == ST_FENCE) {
        mark(hl, 0, len, HL_STRING);
        return ST_FENCE;
    }
    if (i < len && line[i] == '#') {
        mark(hl, 0, len, HL_KEYWORD);
        return ST_NONE;
    }
    if (i < len && line[i] == '>') {
        mark(hl, 0, len, HL_COMMENT);
        return ST_NONE;
    }
    if (i + 1 < len && strchr("-*+", line[i]) && line[i + 1] == ' ') {
        mark(hl, i, i + 1, HL_OPERATOR);
        i += 2;
    } else {
        size_t d = i;
        while (d < len && is_digit((unsigned char)line[d])) d++;
        if (d > i && d + 1 < len && line[d] == '.' && line[d + 1] == ' ') {
            mark(hl, i, d + 1, HL_OPERATOR);
            i = d + 2;
        }
    }
    md_inline(line, len, i, hl);
    return ST_NONE;
}

/* ---- Makefile ---- */

static const char *const make_directives[] = {
    "include", "-include", "sinclude", "ifeq",   "ifneq",    "ifdef",
    "ifndef",  "else",     "endif",    "define", "endef",    "export",
    "unexport", "override", "vpath",   "private", "undefine", NULL};

static void make_vars(const char *line, size_t i, size_t end, uint8_t *hl) {
    while (i < end) {
        if (line[i] == '$' && i + 1 < end) {
            size_t e = i + 1;
            if (line[e] == '(' || line[e] == '{') {
                char open = line[e], close = open == '(' ? ')' : '}';
                int depth = 0;
                for (; e < end; e++) {
                    if (line[e] == open) depth++;
                    else if (line[e] == close && --depth == 0) break;
                }
                e = e < end ? e + 1 : end;
            } else {
                e++;
            }
            mark(hl, i, e, HL_TYPE);
            i = e;
            continue;
        }
        i++;
    }
}

static uint8_t makefile(const Syntax *s, const char *line, size_t len,
                        uint8_t state, uint8_t *hl) {
    (void)s;
    (void)state;
    mark(hl, 0, len, HL_NORMAL);
    if (len && line[0] == '\t') { /* recipe: shell code */
        make_vars(line, 0, len, hl);
        return ST_NONE;
    }
    size_t end = len;
    for (size_t i = 0; i < len; i++) {
        if (line[i] == '#' && (i == 0 || line[i - 1] != '\\')) {
            mark(hl, i, len, HL_COMMENT);
            end = i;
            break;
        }
    }
    size_t i = 0;
    while (i < end && is_space((unsigned char)line[i])) i++;
    size_t w = i;
    while (w < end && !is_space((unsigned char)line[w]) && line[w] != ':' &&
           line[w] != '=' && line[w] != '(')
        w++;
    if (in_list(make_directives, line + i, w - i)) {
        mark(hl, i, w, HL_KEYWORD);
        make_vars(line, w, end, hl);
        return ST_NONE;
    }
    int depth = 0;
    for (size_t j = i; j < end; j++) {
        char c = line[j];
        if (c == '(' || c == '{') depth++;
        else if ((c == ')' || c == '}') && depth) depth--;
        if (depth) continue;
        if (c == '=') {
            size_t op = j;
            if (op > i && strchr(":?+!", line[op - 1])) op--;
            if (op > i && line[op - 1] == ':' ) op--; /* ::= */
            mark(hl, i, op, HL_PREPROC);
            mark(hl, op, j + 1, HL_OPERATOR);
            break;
        }
        if (c == ':') {
            if (j + 1 < end && line[j + 1] == '=') continue;
            mark(hl, i, j, HL_KEYWORD);
            mark(hl, j, j + 1, HL_OPERATOR);
            break;
        }
    }
    make_vars(line, i, end, hl);
    return ST_NONE;
}

/* ---- Language table ---- */

#define LIST(...) ((const char *const[]){__VA_ARGS__, NULL})

#define C_KEYWORDS                                                          \
    "auto", "break", "case", "const", "continue", "default", "do", "else",  \
        "enum", "extern", "for", "goto", "if", "inline", "register",        \
        "restrict", "return", "sizeof", "static", "struct", "switch",       \
        "typedef", "union", "volatile", "while", "_Alignas", "_Alignof",    \
        "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary",           \
        "_Noreturn", "_Static_assert", "_Thread_local", "NULL", "true",     \
        "false"
#define C_TYPES                                                             \
    "int", "long", "short", "char", "float", "double", "signed",            \
        "unsigned", "void", "bool", "size_t", "ssize_t", "int8_t",          \
        "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", \
        "uint64_t", "intptr_t", "uintptr_t", "ptrdiff_t", "FILE",           \
        "wchar_t", "off_t", "pid_t"

static const Syntax languages[] = {
    {.name = "C",
     .match = LIST(".c", ".h"),
     .keywords = LIST(C_KEYWORDS),
     .types = LIST(C_TYPES),
     .lc1 = "//", .bc_start = "/*", .bc_end = "*/",
     .flags = F_DQ | F_CHAR | F_PREPROC},
    {.name = "C++",
     .match = LIST(".cc", ".cpp", ".cxx", ".c++", ".hh", ".hpp", ".hxx",
                   ".h++", ".ipp"),
     .keywords = LIST(C_KEYWORDS, "alignas", "alignof", "and", "asm",
                      "catch", "class", "concept", "consteval", "constexpr",
                      "constinit", "const_cast", "co_await", "co_return",
                      "co_yield", "decltype", "delete", "dynamic_cast",
                      "explicit", "export", "final", "friend", "mutable",
                      "namespace", "new", "noexcept", "not", "nullptr",
                      "operator", "or", "override", "private", "protected",
                      "public", "reinterpret_cast", "requires",
                      "static_assert", "static_cast", "template", "this",
                      "throw", "try", "typeid", "typename", "using",
                      "virtual"),
     .types = LIST(C_TYPES, "std", "string", "vector", "auto", "char8_t",
                   "char16_t", "char32_t"),
     .lc1 = "//", .bc_start = "/*", .bc_end = "*/",
     .flags = F_DQ | F_CHAR | F_PREPROC},
    {.name = "Rust",
     .match = LIST(".rs"),
     .keywords = LIST("as", "async", "await", "break", "const", "continue",
                      "crate", "dyn", "else", "enum", "extern", "false",
                      "fn", "for", "if", "impl", "in", "let", "loop",
                      "match", "mod", "move", "mut", "pub", "ref", "return",
                      "self", "Self", "static", "struct", "super", "trait",
                      "true", "type", "unsafe", "use", "where", "while",
                      "yield", "macro_rules"),
     .types = LIST("i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16",
                   "u32", "u64", "u128", "usize", "f32", "f64", "bool",
                   "char", "str", "String", "Vec", "Option", "Result", "Box",
                   "Some", "None", "Ok", "Err"),
     .lc1 = "//", .bc_start = "/*", .bc_end = "*/",
     .flags = F_DQ | F_CHAR | F_QUOTE_ML},
    {.name = "Go",
     .match = LIST(".go"),
     .keywords = LIST("break", "case", "chan", "const", "continue",
                      "default", "defer", "else", "fallthrough", "for",
                      "func", "go", "goto", "if", "import", "interface",
                      "map", "package", "range", "return", "select",
                      "struct", "switch", "type", "var", "true", "false",
                      "nil", "iota"),
     .types = LIST("bool", "byte", "complex64", "complex128", "error",
                   "float32", "float64", "int", "int8", "int16", "int32",
                   "int64", "rune", "string", "uint", "uint8", "uint16",
                   "uint32", "uint64", "uintptr", "any"),
     .lc1 = "//", .bc_start = "/*", .bc_end = "*/",
     .flags = F_DQ | F_CHAR | F_BACKTICK},
    {.name = "Python",
     .match = LIST(".py", ".pyw", ".pyi", "SConstruct", "SConscript"),
     .shebang = LIST("python"),
     .keywords = LIST("False", "None", "True", "and", "as", "assert",
                      "async", "await", "break", "class", "continue", "def",
                      "del", "elif", "else", "except", "finally", "for",
                      "from", "global", "if", "import", "in", "is", "lambda",
                      "nonlocal", "not", "or", "pass", "raise", "return",
                      "try", "while", "with", "yield", "match", "case",
                      "self"),
     .types = LIST("int", "float", "str", "bytes", "bool", "list", "dict",
                   "set", "tuple", "object", "type", "print", "len",
                   "range"),
     .lc1 = "#",
     .flags = F_DQ | F_SQ | F_TRIPLE},
    {.name = "Shell",
     .match = LIST(".sh", ".bash", ".zsh", ".ksh", ".bashrc",
                   ".bash_profile", ".profile", ".zshrc", "PKGBUILD",
                   ".install"),
     .shebang = LIST("sh", "bash", "zsh", "dash", "ksh", "ash"),
     .keywords = LIST("if", "then", "else", "elif", "fi", "case", "esac",
                      "for", "while", "until", "do", "done", "in",
                      "function", "select", "return", "exit", "local",
                      "export", "readonly", "declare", "unset", "shift",
                      "break", "continue", "time"),
     .types = LIST("echo", "printf", "cd", "test", "read", "source", "eval",
                   "exec", "set", "trap", "true", "false"),
     .lc1 = "#",
     .flags = F_DQ | F_SQ | F_SQ_RAW | F_QUOTE_ML | F_COMMENT_WS |
              F_SHELLVAR},
    {.name = "JavaScript",
     .match = LIST(".js", ".mjs", ".cjs", ".jsx"),
     .shebang = LIST("node", "deno", "bun"),
     .keywords = LIST("break", "case", "catch", "class", "const",
                      "continue", "debugger", "default", "delete", "do",
                      "else", "export", "extends", "finally", "for",
                      "function", "if", "import", "in", "instanceof", "let",
                      "new", "return", "super", "switch", "this", "throw",
                      "try", "typeof", "var", "void", "while", "with",
                      "yield", "async", "await", "of", "static", "get",
                      "set", "true", "false", "null", "undefined"),
     .types = LIST("Array", "Object", "String", "Number", "Boolean",
                   "Promise", "Map", "Set", "Symbol", "Math", "JSON",
                   "console", "Error"),
     .lc1 = "//", .bc_start = "/*", .bc_end = "*/",
     .flags = F_DQ | F_SQ | F_BACKTICK},
    {.name = "JSON",
     .match = LIST(".json", ".jsonc", ".geojson", ".webmanifest"),
     .keywords = LIST("true", "false", "null"),
     .flags = F_DQ},
    {.name = "TOML",
     .match = LIST(".toml", "Cargo.lock", "Pipfile"),
     .keywords = LIST("true", "false", "inf", "nan"),
     .lc1 = "#",
     .flags = F_DQ | F_SQ | F_SQ_RAW | F_TRIPLE | F_SECTIONS},
    {.name = "Markdown",
     .match = LIST(".md", ".markdown", ".mkd", ".mdown"),
     .fn = markdown},
    {.name = "Makefile",
     .match = LIST("Makefile", "makefile", "GNUmakefile", ".mk", ".mak"),
     .fn = makefile},
};

#define NLANG (sizeof languages / sizeof languages[0])

const char *syntax_name(const Syntax *s) { return s ? s->name : "Text"; }

const char *syntax_names(void) {
    return "C, C++, Rust, Go, Python, Shell, JavaScript, JSON, TOML, "
           "Markdown, Makefile";
}

const Syntax *syntax_by_name(const char *name) {
    for (size_t i = 0; i < NLANG; i++) {
        const char *a = languages[i].name, *b = name;
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) a++, b++;
        if (!*a && !*b) return &languages[i];
    }
    return NULL;
}

static const Syntax *by_shebang(const char *line, size_t len) {
    if (len < 3 || line[0] != '#' || line[1] != '!') return NULL;
    char buf[128];
    size_t n = len - 2 < sizeof buf - 1 ? len - 2 : sizeof buf - 1;
    memcpy(buf, line + 2, n);
    buf[n] = '\0';
    char *p = buf;
    for (int pass = 0; pass < 2; pass++) {
        while (*p == ' ' || *p == '\t') p++;
        char *w = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        char save = *p;
        *p = '\0';
        char *base = strrchr(w, '/');
        base = base ? base + 1 : w;
        if (strcmp(base, "env") == 0 && pass == 0) {
            *p = save;
            continue;
        }
        for (size_t i = 0; i < NLANG; i++) {
            const char *const *sb = languages[i].shebang;
            for (; sb && *sb; sb++) {
                size_t sl = strlen(*sb);
                if (strncmp(base, *sb, sl) == 0 &&
                    (base[sl] == '\0' || is_digit((unsigned char)base[sl]) ||
                     base[sl] == '.'))
                    return &languages[i];
            }
        }
        break;
    }
    return NULL;
}

const Syntax *syntax_detect(const char *path, const char *first_line,
                            size_t len) {
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path;
    if (base) {
        const char *dot = strrchr(base, '.');
        for (size_t i = 0; i < NLANG; i++) {
            for (const char *const *m = languages[i].match; *m; m++) {
                if ((*m)[0] == '.' && dot && strcmp(dot, *m) == 0)
                    return &languages[i];
                if (strcmp(base, *m) == 0) return &languages[i];
            }
        }
    }
    return first_line ? by_shebang(first_line, len) : NULL;
}

bool syntax_quote_pairs(const Syntax *s, char q) {
    if (!s) return false;
    return (q == '"' && (s->flags & F_DQ)) || (q == '\'' && (s->flags & F_SQ)) ||
           (q == '`' && (s->flags & F_BACKTICK));
}

uint8_t syntax_highlight(const Syntax *s, const char *line, size_t len,
                         uint8_t state, uint8_t *hl) {
    if (!s) {
        mark(hl, 0, len, HL_NORMAL);
        return 0;
    }
    return s->fn ? s->fn(s, line, len, state, hl)
                 : generic(s, line, len, state, hl);
}
