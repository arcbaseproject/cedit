#include "cedit/file.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cedit/utf8.h"

static void set_err(char *err, size_t errlen, const char *what,
                    const char *path, int e) {
    char safe[256];
    sanitize_for_display(path, safe, sizeof safe);
    snprintf(err, errlen, "%s %s: %s", what, safe, strerror(e));
}

static void fill_stamp(const struct stat *st, FileStamp *stamp) {
    stamp->valid = true;
    stamp->dev = st->st_dev;
    stamp->ino = st->st_ino;
    stamp->size = st->st_size;
    stamp->mtime = st->st_mtim;
}

int file_stamp(const char *path, FileStamp *stamp) {
    struct stat st;
    if (stat(path, &st) != 0) {
        stamp->valid = false;
        return errno;
    }
    fill_stamp(&st, stamp);
    return 0;
}

bool file_stamp_equal(const FileStamp *x, const FileStamp *y) {
    return x->valid == y->valid &&
           (!x->valid ||
            (x->dev == y->dev && x->ino == y->ino && x->size == y->size &&
             x->mtime.tv_sec == y->mtime.tv_sec &&
             x->mtime.tv_nsec == y->mtime.tv_nsec));
}

int file_read_all(const char *path, char **data, size_t *len,
                  FileStamp *stamp, char *err, size_t errlen) {
    /* O_NONBLOCK: opening a FIFO must not hang the editor. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        int e = errno;
        set_err(err, errlen, "Cannot open", path, e);
        return e;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        set_err(err, errlen, "Cannot stat", path, e);
        return e;
    }
    if (!S_ISREG(st.st_mode)) {
        int e = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        close(fd);
        if (e == EINVAL) {
            char safe[256];
            snprintf(err, errlen, "%s: not a regular file",
                     sanitize_for_display(path, safe, sizeof safe));
        } else {
            set_err(err, errlen, "Cannot open", path, e);
        }
        return e;
    }
    if ((uintmax_t)st.st_size >= SIZE_MAX / 2) {
        close(fd);
        set_err(err, errlen, "Cannot open", path, EFBIG);
        return EFBIG;
    }
    size_t cap = (size_t)st.st_size + 1, n = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        set_err(err, errlen, "Cannot load", path, ENOMEM);
        return ENOMEM;
    }
    for (;;) {
        if (n == cap) { /* file grew while reading */
            if (cap > SIZE_MAX / 4) {
                free(buf);
                close(fd);
                set_err(err, errlen, "Cannot load", path, EFBIG);
                return EFBIG;
            }
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                close(fd);
                set_err(err, errlen, "Cannot load", path, ENOMEM);
                return ENOMEM;
            }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            free(buf);
            close(fd);
            set_err(err, errlen, "Cannot read", path, e);
            return e;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    if (stamp) fill_stamp(&st, stamp);
    close(fd);
    *data = buf;
    *len = n;
    return 0;
}

/* Buffered writer with optional LF -> CRLF conversion. */
typedef struct {
    int fd;
    int error;
    size_t used;
    off_t total;
    char buf[65536];
} Out;

static void out_flush(Out *o) {
    size_t off = 0;
    while (!o->error && off < o->used) {
        ssize_t w = write(o->fd, o->buf + off, o->used - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            o->error = errno;
        } else if (w == 0) {
            o->error = EIO;
        } else {
            off += (size_t)w;
        }
    }
    o->used = 0;
}

static void out_put(Out *o, const char *p, size_t n) {
    while (n && !o->error) {
        if (o->used == sizeof o->buf) out_flush(o);
        size_t c = sizeof o->buf - o->used;
        if (c > n) c = n;
        memcpy(o->buf + o->used, p, c);
        o->used += c;
        o->total += (off_t)c;
        p += c;
        n -= c;
    }
}

static void out_segment(Out *o, const char *p, size_t n, bool crlf) {
    if (!crlf) {
        out_put(o, p, n);
        return;
    }
    const char *end = p + n;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) {
            out_put(o, p, (size_t)(end - p));
            break;
        }
        out_put(o, p, (size_t)(nl - p));
        out_put(o, "\r\n", 2);
        p = nl + 1;
    }
}

static int write_contents(int fd, const char *a, size_t alen, const char *b,
                          size_t blen, bool crlf, off_t *total) {
    Out *o = malloc(sizeof *o);
    if (!o) return ENOMEM;
    o->fd = fd;
    o->error = 0;
    o->used = 0;
    o->total = 0;
    out_segment(o, a, alen, crlf);
    out_segment(o, b, blen, crlf);
    out_flush(o);
    int e = o->error;
    *total = o->total;
    free(o);
    return e;
}

static void fsync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd >= 0) {
        fsync(fd); /* best effort: some filesystems refuse */
        close(fd);
    }
}

static int write_in_place(const char *target, const char *a, size_t alen,
                          const char *b, size_t blen, bool crlf,
                          FileStamp *stamp, char *err, size_t errlen) {
    int fd = open(target, O_WRONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        int e = errno;
        set_err(err, errlen, "Cannot write", target, e);
        return e;
    }
    off_t total = 0;
    int e = write_contents(fd, a, alen, b, blen, crlf, &total);
    if (!e && ftruncate(fd, total) != 0) e = errno;
    if (!e && fsync(fd) != 0 && errno != EINVAL) e = errno;
    struct stat st;
    if (!e && fstat(fd, &st) == 0 && stamp) fill_stamp(&st, stamp);
    if (close(fd) != 0 && !e) e = errno;
    if (e) {
        char safe[256];
        snprintf(err, errlen,
                 "Write to %s failed (%s); file may be damaged, buffer is "
                 "intact - save elsewhere",
                 sanitize_for_display(target, safe, sizeof safe),
                 strerror(e));
    }
    return e;
}

int file_write(const char *path, const char *a, size_t alen, const char *b,
               size_t blen, bool crlf, FileStamp *stamp, bool *in_place,
               char *err, size_t errlen) {
    struct stat st;
    bool exists;
    char *target = realpath(path, NULL);
    *in_place = false;

    if (target) {
        exists = true;
        if (stat(target, &st) != 0) {
            int e = errno;
            set_err(err, errlen, "Cannot stat", path, e);
            free(target);
            return e;
        }
        if (!S_ISREG(st.st_mode)) {
            char safe[256];
            snprintf(err, errlen, "%s: not a regular file",
                     sanitize_for_display(path, safe, sizeof safe));
            free(target);
            return EINVAL;
        }
    } else if (errno == ENOENT) {
        /* Never replace a dangling symlink with a regular file. */
        if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
            set_err(err, errlen, "Cannot save dangling symlink", path, ENOENT);
            return ENOENT;
        }
        exists = false;
        target = strdup(path);
        if (!target) {
            set_err(err, errlen, "Cannot save", path, ENOMEM);
            return ENOMEM;
        }
    } else {
        int e = errno;
        set_err(err, errlen, "Cannot save", path, e);
        return e;
    }

    uid_t euid = geteuid();
    if (exists && (st.st_nlink > 1 || (st.st_uid != euid && euid != 0))) {
        *in_place = true;
        int e = write_in_place(target, a, alen, b, blen, crlf, stamp, err,
                               errlen);
        free(target);
        return e;
    }

    char *dcopy = strdup(target);
    char *tmp = NULL;
    int e = 0;
    if (!dcopy) {
        e = ENOMEM;
        goto fail;
    }
    const char *dir = dirname(dcopy);
    size_t tlen = strlen(dir) + 32;
    tmp = malloc(tlen);
    if (!tmp) {
        e = ENOMEM;
        goto fail;
    }
    snprintf(tmp, tlen, "%s/.cedit-XXXXXX", dir);

    int fd = mkstemp(tmp);
    if (fd < 0) {
        e = errno;
        if (exists && (e == EACCES || e == EPERM || e == EROFS)) {
            /* Directory not writable but maybe the file is. */
            free(tmp);
            free(dcopy);
            *in_place = true;
            e = write_in_place(target, a, alen, b, blen, crlf, stamp, err,
                               errlen);
            free(target);
            return e;
        }
        free(tmp);
        tmp = NULL;
        goto fail;
    }

    mode_t mode;
    if (exists) {
        mode = st.st_mode & 07777;
        if (euid == 0 && fchown(fd, st.st_uid, st.st_gid) != 0) {
            /* keep going: permissions still preserved */
        } else if (euid != 0 && st.st_gid != getegid() &&
                   fchown(fd, (uid_t)-1, st.st_gid) != 0) {
            /* group could not be preserved; not fatal */
        }
    } else {
        mode_t mask = umask(0);
        umask(mask);
        mode = 0666 & ~mask;
    }
    if (fchmod(fd, mode) != 0) e = errno;

    off_t total = 0;
    if (!e) e = write_contents(fd, a, alen, b, blen, crlf, &total);
    if (!e && fsync(fd) != 0 && errno != EINVAL) e = errno;
    struct stat nst;
    bool have_nst = !e && fstat(fd, &nst) == 0;
    if (close(fd) != 0 && !e) e = errno;
    if (!e && rename(tmp, target) != 0) e = errno;
    if (e) {
        unlink(tmp);
        goto fail;
    }
    fsync_dir(dir);
    if (stamp) {
        if (have_nst)
            fill_stamp(&nst, stamp);
        else
            file_stamp(target, stamp);
    }
    free(tmp);
    free(dcopy);
    free(target);
    return 0;

fail:
    set_err(err, errlen, "Cannot save", path, e);
    free(tmp);
    free(dcopy);
    free(target);
    return e;
}
