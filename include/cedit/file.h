#ifndef CEDIT_FILE_H
#define CEDIT_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* Identity of a file on disk, used to detect external modification. */
typedef struct {
    bool valid;
    dev_t dev;
    ino_t ino;
    off_t size;
    struct timespec mtime;
} FileStamp;

/* Reads a regular file completely. On success returns 0 and a malloc'd
 * buffer (*data, may be non-NUL-terminated, *len bytes). On failure
 * returns the errno value and writes a message into err. */
int file_read_all(const char *path, char **data, size_t *len,
                  FileStamp *stamp, char *err, size_t errlen);

/*
 * Writes the concatenation a+b to path, converting LF to CRLF if crlf.
 *
 * Default strategy: write a temporary file in the target directory,
 * fsync it, copy the original mode (and owner when running as root),
 * rename it over the target and fsync the directory. Symlinks are
 * followed so the link itself is kept. A failure at any step leaves the
 * original file untouched.
 *
 * Files with several hard links, files owned by another user (when not
 * root) and files in unwritable directories are rewritten in place so
 * links and ownership survive; *in_place is set and a failure there may
 * leave the file truncated (the buffer in memory is still intact).
 */
int file_write(const char *path, const char *a, size_t alen, const char *b,
               size_t blen, bool crlf, FileStamp *stamp, bool *in_place,
               char *err, size_t errlen);

int file_stamp(const char *path, FileStamp *stamp);
bool file_stamp_equal(const FileStamp *x, const FileStamp *y);

#endif
