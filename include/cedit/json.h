#ifndef CEDIT_JSON_H
#define CEDIT_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "cedit/util.h"

/* Minimal JSON tree for LSP messages. Input is treated as untrusted:
 * nesting depth and sizes are bounded, errors return NULL. */
typedef enum { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ } JsonType;

typedef struct Json {
    JsonType type;
    bool b;
    double num;
    char *str;          /* JSON_STR: decoded UTF-8, NUL-terminated */
    size_t len;
    struct Json *items; /* JSON_ARR / JSON_OBJ values */
    char **keys;        /* JSON_OBJ keys */
    size_t n;
} Json;

Json *json_parse(const char *s, size_t len);
void json_free(Json *j);

const Json *json_get(const Json *obj, const char *key);   /* NULL if absent */
const Json *json_at(const Json *arr, size_t i);
const char *json_str(const Json *j);                      /* NULL if not str */
double json_num(const Json *j, double def);
/* json_get chain: json_path(j, "a", "b", NULL). */
const Json *json_path(const Json *j, ...);

/* Append s as a quoted, escaped JSON string. */
void json_quote(Str *out, const char *s, size_t n);

#endif
