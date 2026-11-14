#ifndef CEDIT_TEST_H
#define CEDIT_TEST_H

#include <stdio.h>
#include <string.h>

static int test_failures, test_checks;

#define CHECK(cond)                                                         \
    do {                                                                    \
        test_checks++;                                                      \
        if (!(cond)) {                                                      \
            test_failures++;                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                 \
        }                                                                   \
    } while (0)

#define CHECK_STR(a, b)                                                     \
    do {                                                                    \
        const char *a_ = (a), *b_ = (b);                                    \
        test_checks++;                                                      \
        if (strcmp(a_, b_) != 0) {                                          \
            test_failures++;                                                \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
                    a_, b_);                                                \
        }                                                                   \
    } while (0)

#define RUN(fn)                                                             \
    do {                                                                    \
        int before_ = test_failures;                                        \
        fn();                                                               \
        printf("%-28s %s\n", #fn, test_failures == before_ ? "ok" : "FAIL"); \
    } while (0)

#endif
