#include <stdio.h>
#include <string.h>

/* Count words and lines in a string. */
static int count_words(const char *s) {
    int words = 0, in_word = 0;
    for (; *s; s++) {
        if (*s == ' ' || *s == '\n') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }
    return words;
}

static int count_lines(const char *s) {
    int lines = 0;
    while ((s = strchr(s, '\n')) != NULL) {
        lines++;
        s++;
    }
    return lines;
}

int main(void) {
    const char *text = "hello cedit\nsmall and fast\n";
    printf("words: %d\n", count_words(text));
    return 0;
}
