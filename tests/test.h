/* Test harness: no external framework. */
#ifndef OD_TEST_H
#define OD_TEST_H

#include <stdio.h>
#include <string.h>

extern int od_test_failures;
extern int od_test_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        od_test_checks++;                                                     \
        if (!(cond)) {                                                         \
            od_test_failures++;                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

#define CHECK_EQ_U32(got, want)                                                \
    do {                                                                       \
        od_test_checks++;                                                     \
        unsigned long g_ = (unsigned long)(got), w_ = (unsigned long)(want);   \
        if (g_ != w_) {                                                        \
            od_test_failures++;                                               \
            printf("  FAIL %s:%d: %s = %lu, want %lu\n", __FILE__, __LINE__,   \
                   #got, g_, w_);                                              \
        }                                                                      \
    } while (0)

#define CHECK_EQ_MEM(a, b, n)                                                  \
    do {                                                                       \
        od_test_checks++;                                                     \
        if (memcmp((a), (b), (n)) != 0) {                                      \
            od_test_failures++;                                               \
            printf("  FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);    \
        }                                                                      \
    } while (0)

void test_assets(void);
void test_levels(void);
void test_tiles(void);
void test_compose(void);
void test_game(void);

/* Auto-registering tests.
 *
 * A test file can register its own cases without editing test_main.c or this
 * header, which keeps parallel work free of shared-file conflicts:
 *
 *     static void my_case(void);
 *     OD_TEST(my_case);
 *
 * Registration runs at startup via a constructor. */
typedef void (*od_test_fn)(void);
void od_test_register(const char *name, od_test_fn fn);
void od_test_register_source(const char *name, od_test_fn fn, const char *source);

#define OD_TEST(fn)                                                           \
    static void od_reg_ctor_##fn(void) __attribute__((constructor));          \
    static void od_reg_ctor_##fn(void) { od_test_register_source(#fn, fn, __FILE__); }

#endif /* OD_TEST_H */
