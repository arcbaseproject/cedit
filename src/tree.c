/* File tree sidebar: entry list and key handling (drawn by render.c). */
#define _GNU_SOURCE
#include "cedit/tree.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cedit/editor.h"
#include "cedit/lsp.h"
#include "cedit/utf8.h"

#define TREE_WIDTH 30
#define TREE_MAX 10000 /* same cap as find-file completion */

static int cmp_entry(const void *a, const void *b) {
    const TreeEntry *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcmp(x->path, y->path);
}

/* Insert the children of entry `parent` (or of dir at the top level when
 * parent is SIZE_MAX) right after it. */
static void add_children(Editor *e, size_t parent, const char *dir) {
    const char *base = parent == SIZE_MAX ? dir : e->tree[parent].path;
    int depth = parent == SIZE_MAX ? 0 : e->tree[parent].depth + 1;
    DIR *d = opendir(base);
    if (!d) {
        editor_error(e, "Cannot open %s", base);
        return;
    }
    TreeEntry *kids = NULL;
    size_t n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && e->tree_n + n < TREE_MAX) {
        if (de->d_name[0] == '.') continue; /* also skips . and .. */
        Str p = {0};
        if (parent != SIZE_MAX || strcmp(base, ".") != 0) {
            str_appendz(&p, base);
            str_putc(&p, '/');
        }
        str_appendz(&p, de->d_name);
        struct stat st;
        bool isdir = stat(p.data, &st) == 0 && S_ISDIR(st.st_mode);
        kids = xreallocarray(kids, n + 1, sizeof *kids);
        kids[n++] = (TreeEntry){p.data, depth, isdir, false};
    }
    closedir(d);
    if (n == 0) {
        free(kids);
        return;
    }
    qsort(kids, n, sizeof *kids, cmp_entry);
    size_t at = parent == SIZE_MAX ? e->tree_n : parent + 1;
    e->tree = xreallocarray(e->tree, e->tree_n + n, sizeof *e->tree);
    memmove(e->tree + at + n, e->tree + at,
            (e->tree_n - at) * sizeof *e->tree);
    memcpy(e->tree + at, kids, n * sizeof *kids);
    e->tree_n += n;
    free(kids);
}

static void collapse(Editor *e, size_t i) {
    size_t j = i + 1;
    while (j < e->tree_n && e->tree[j].depth > e->tree[i].depth)
        free(e->tree[j++].path);
    memmove(e->tree + i + 1, e->tree + j, (e->tree_n - j) * sizeof *e->tree);
    e->tree_n -= j - i - 1;
    e->tree[i].open = false;
}

static void expand(Editor *e, size_t i) {
    if (!e->tree[i].dir || e->tree[i].open) return;
    e->tree[i].open = true;
    add_children(e, i, NULL);
}

void tree_free(Editor *e) {
    for (size_t i = 0; i < e->tree_n; i++) free(e->tree[i].path);
    free(e->tree_root);
    e->tree_root = NULL;
    free(e->tree);
    e->tree = NULL;
    e->tree_n = e->tree_sel = e->tree_top = 0;
}

void tree_load(Editor *e, const char *dir) {
    char *root = xstrdup(dir);
    tree_free(e);
    e->tree_root = root;
    add_children(e, SIZE_MAX, root);
}

static const char *leaf(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int remove_entry(const char *path, const struct stat *st, int type,
                        struct FTW *walk) {
    (void)st;
    (void)walk;
    return type == FTW_DP || type == FTW_DNR ? rmdir(path) : unlink(path);
}

static int rename_unused(const char *from, const char *to) {
#if defined(__linux__) && defined(RENAME_NOREPLACE)
    return renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE);
#elif defined(__APPLE__) && defined(RENAME_EXCL)
    return renamex_np(from, to, RENAME_EXCL);
#else
    struct stat st;
    if (lstat(to, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    return errno == ENOENT ? rename(from, to) : -1;
#endif
}

/* Resolve the parent, keeping the final symlink as a directory entry. */
static char *entry_path(const char *path) {
    const char *name = leaf(path);
    char *parent = name == path ? xstrdup(".") : xstrndup(path, (size_t)(name - path));
    char *dir = realpath(parent, NULL);
    free(parent);
    if (!dir) return NULL;
    Str full = {0};
    str_appendf(&full, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
    free(dir);
    return full.data;
}

static int change_entry(Editor *e, const char *from, const char *to, bool recursive) {
    char *base = entry_path(from);
    if (!base) return -1;
    size_t len = strlen(base);
    char **paths = xcalloc(e->nbufs, sizeof *paths);
    for (size_t i = 0; i < e->nbufs; i++) {
        Buffer *b = e->bufs[i];
        if (!b->path) continue;
        char *p = entry_path(b->path);
        if (p && !strncmp(p, base, len) && (p[len] == '/' || !p[len]))
            paths[i] = p;
        else free(p);
    }

    int rc;
    if (to) rc = rename_unused(from, to);
    else if (recursive) rc = nftw(from, remove_entry, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
    else rc = unlink(from);
    int error = errno;
    for (size_t i = 0; i < e->nbufs; i++) {
        if (!paths[i]) continue;
        struct stat st;
        if (rc == 0 || (!to && lstat(paths[i], &st) < 0 && errno == ENOENT)) {
            Buffer *b = e->bufs[i];
            lsp_buffer_closed(e, b);
            free(b->path);
            b->path = NULL;
            buffer_clear_diags(b);
            if (to) {
                Str dest = {0};
                str_appendf(&dest, "%s%s", to, paths[i] + len);
                b->path = dest.data;
                free(b->name);
                b->name = xstrdup(leaf(b->path));
                editor_unique_name(e, b);
                buffer_set_syntax(b, syntax_detect(b->path, NULL, 0));
                lsp_buffer_opened(e, b);
            }
        }
        free(paths[i]);
    }
    free(paths);
    free(base);
    errno = error;
    return rc;
}

void tree_action(Editor *e, size_t idx, TreeAction action) {
    TreeEntry *item = idx < e->tree_n ? &e->tree[idx] : NULL;
    bool create = action == TREE_CREATE_FILE || action == TREE_CREATE_FOLDER;
    if (!create && !item) return;
    size_t parent = SIZE_MAX;
    if (item && item->dir && create) parent = idx;
    else if (item && item->depth) {
        parent = idx - 1;
        while (e->tree[parent].depth >= item->depth) parent--;
    }
    const char *dir = parent == SIZE_MAX ? e->tree_root : e->tree[parent].path;
    Str dest = {0};
    int rc;
    if (action == TREE_DELETE) {
        char question[128], shown[80];
        struct stat st;
        bool recursive = lstat(item->path, &st) == 0 && S_ISDIR(st.st_mode);
        snprintf(question, sizeof question, "Delete %s%s? (y/n)",
                 sanitize_for_display(leaf(item->path), shown, sizeof shown),
                 recursive ? " and its contents" : "");
        if (editor_ask(e, question, "yn") != 'y') return;
        rc = change_entry(e, item->path, NULL, recursive);
    } else {
        const char *prompt = action == TREE_RENAME ? "Rename to: " :
            action == TREE_CREATE_FILE ? "Create file: " : "Create folder: ";
        char *name = editor_prompt(e, prompt, action == TREE_RENAME ? leaf(item->path) : "",
                                   NULL, NULL, NULL);
        if (!name) return;
        if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
            editor_error(e, "Enter a single file or folder name");
            free(name);
            return;
        }
        if (strcmp(dir, ".")) str_appendf(&dest, "%s/", dir);
        str_appendz(&dest, name);
        free(name);
        if (action == TREE_CREATE_FILE) {
            int fd = open(dest.data, O_CREAT | O_EXCL | O_WRONLY, 0666);
            rc = fd < 0 ? -1 : close(fd);
        } else if (action == TREE_CREATE_FOLDER) rc = mkdir(dest.data, 0777);
        else rc = strcmp(item->path, dest.data) ? change_entry(e, item->path, dest.data, false) : 0;
    }

    int error = errno;
    if (rc == 0 || action == TREE_DELETE) {
        if (parent == SIZE_MAX) tree_load(e, dir);
        else {
            collapse(e, parent);
            expand(e, parent);
            e->tree_sel = parent;
        }
        for (size_t i = 0; dest.len && i < e->tree_n; i++)
            if (!strcmp(e->tree[i].path, dest.data)) { e->tree_sel = i; break; }
    }
    if (rc < 0) editor_error(e, "%s", strerror(error));
    str_free(&dest);
}

int tree_width(const Editor *e) {
    if (!e->tree_visible) return 0;
    return TREE_WIDTH < e->cols / 2 ? TREE_WIDTH : e->cols / 2;
}

void tree_scroll(Editor *e, int rows) {
    size_t r = rows > 0 ? (size_t)rows : 1;
    if (e->tree_sel < e->tree_top) e->tree_top = e->tree_sel;
    if (e->tree_sel >= e->tree_top + r) e->tree_top = e->tree_sel - r + 1;
}

bool tree_key(Editor *e, Key k) {
    size_t n = e->tree_n, *sel = &e->tree_sel;
    TreeEntry *t = n ? &e->tree[*sel] : NULL;
    if (k == KEY_DOWN || k == 14) {
        if (*sel + 1 < n) ++*sel;
    } else if (k == KEY_UP || k == 16) {
        if (*sel > 0) --*sel;
    } else if (k == KEY_HOME) {
        *sel = 0;
    } else if (k == KEY_END) {
        *sel = n ? n - 1 : 0;
    } else if (k == KEY_RIGHT) {
        if (t) expand(e, *sel);
    } else if (k == KEY_LEFT) {
        if (t && t->open) {
            collapse(e, *sel);
        } else if (t) {
            while (*sel > 0 && e->tree[*sel].depth >= t->depth) --*sel;
        }
    } else if (k == KEY_RET) {
        if (!t) return true;
        if (t->dir) {
            if (t->open) collapse(e, *sel);
            else expand(e, *sel);
        } else {
            editor_open(e, t->path);
            e->tree_focus = false;
        }
    } else if (k == 'q') {
        e->tree_visible = e->tree_focus = false;
    } else if (k == KEY_ESC || k == 7 || k == KEY_TAB) {
        e->tree_focus = false;
    } else {
        return false;
    }
    return true;
}

static void cmd_file_tree(Editor *e) {
    if (!e->tree_visible) {
        tree_load(e, ".");
        e->tree_visible = true;
    }
    e->tree_focus = true;
    Key pending = KEY_NONE;
    while (e->tree_focus && !e->headless) {
        render_frame(e);
        Key k = editor_read_key(e);
        if (k == KEY_RESIZE || k == KEY_NONE) continue;
        if (k == KEY_MOUSE && e->mouse.x < tree_width(e) && !e->mouse.drag) {
            editor_mouse(e); /* clicks in the tree keep focus there */
            continue;
        }
        const char *cmd = NULL;
        const Keymap *sub;
        if (keymap_lookup(e->keymap, k, &cmd, &sub) == KM_COMMAND &&
            strcmp(cmd, "file-tree") == 0) { /* same key again: hide */
            e->tree_visible = false;
            break;
        }
        if (!tree_key(e, k)) {
            pending = k;
            break;
        }
    }
    e->tree_focus = false;
    if (pending != KEY_NONE) editor_handle_key(e, pending);
}

void tree_register_commands(Editor *e) {
    command_register(e, "file-tree", cmd_file_tree,
                     "Show the file tree and focus it (again: hide)", 0, -1);
}
