/* Minimal test helpers: CHECK prints and counts failures */
#ifndef PIC_TEST_CHECK_H
#define PIC_TEST_CHECK_H
#include <stdio.h>
static int check_failures, check_count;
#define CHECK(cond, ...) do { check_count++; if (!(cond)) { check_failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_DONE(name) (printf("%s: %d checks, %d failed\n", name, check_count, check_failures), \
    check_failures ? 1 : 0)
#endif
