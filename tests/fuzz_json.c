/* libFuzzer target: the JSON parser sees language server output. */
#include <stddef.h>
#include <stdint.h>

#include "cedit/json.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Json *j = json_parse((const char *)data, size);
    if (j) {
        Str s = {0};
        if (j->type == JSON_STR) json_quote(&s, j->str, j->len);
        str_free(&s);
        (void)json_path(j, "result", "items", NULL);
        json_free(j);
    }
    return 0;
}
