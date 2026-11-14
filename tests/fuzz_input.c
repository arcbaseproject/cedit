/* libFuzzer target: the terminal input parser must consume every byte
 * stream without reading out of bounds, looping forever or crashing. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "cedit/input.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t off = 0;
    while (off < size) {
        Key k;
        size_t n = input_parse(data + off, size - off, false, &k);
        if (n == 0) n = input_parse(data + off, size - off, true, &k);
        if (n == 0 || n > size - off) abort(); /* final parse must progress */
        off += n;
    }
    /* key names must never overflow */
    char name[48];
    for (size_t i = 0; i + 4 <= size; i += 4) {
        Key k = (Key)((uint32_t)data[i] | (uint32_t)data[i + 1] << 8 |
                      (uint32_t)data[i + 2] << 16 | (uint32_t)data[i + 3] << 24);
        key_name(k, name, sizeof name);
        Key back;
        (void)key_parse(name, sizeof name > 0 ? __builtin_strlen(name) : 0, &back);
    }
    return 0;
}
