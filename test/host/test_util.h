/* test_util.h — the whole test harness. No framework; core/ has no
 * allocator and the tests should not need one either. */
#ifndef PICO_ATOM_TEST_UTIL_H
#define PICO_ATOM_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>

/* CTest treats this as "skipped" (SKIP_RETURN_CODE in CMakeLists). */
#define TEST_SKIP_CODE 77

static int test_failures = 0;

/* The guest clock a test runs at, in MHz: 1, or 2 or 4 when CTest runs
 * its _2mhz or _4mhz registration with PICO_ATOM_TEST_MHZ set
 * (design.md §12.1). */
static inline unsigned test_mhz(void) {
    const char *e = getenv("PICO_ATOM_TEST_MHZ");
    return e && e[0] == '4' ? 4u : e && e[0] == '2' ? 2u : 1u;
}

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "%s:%d: FAIL %s\n    ", __FILE__, __LINE__,    \
                    #cond);                                                \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
            if (++test_failures > 20) {                                    \
                fprintf(stderr, "too many failures, stopping\n");          \
                return 1;                                                  \
            }                                                              \
        }                                                                  \
    } while (0)

#define TEST_DONE()                                                        \
    do {                                                                   \
        if (test_failures) {                                               \
            fprintf(stderr, "%d failure(s)\n", test_failures);             \
            return 1;                                                      \
        }                                                                  \
        printf("ok\n");                                                    \
        return 0;                                                          \
    } while (0)

#endif /* PICO_ATOM_TEST_UTIL_H */
