#include "test.h"
#include <stdlib.h>

int od_test_failures = 0;
int od_test_checks = 0;

/* Auto-registered cases (see OD_TEST in test.h). A case beyond the table is
 * never dropped silently: it is counted and fails the run (MAPA-PASO found
 * 260 cases against the old 256 slots, the last four never ran). */
#define OD_MAX_AUTO_TESTS 1024
static const char *auto_names[OD_MAX_AUTO_TESTS];
static od_test_fn auto_fns[OD_MAX_AUTO_TESTS];
static const char *auto_sources[OD_MAX_AUTO_TESTS];
static int auto_count = 0;
static int auto_dropped = 0;

void od_test_register(const char *name, od_test_fn fn)
{
    /* Compatibility callers have no auditable source: fail classification. */
    od_test_register_source(name, fn, "unknown");
}

void od_test_register_source(const char *name, od_test_fn fn, const char *source)
{
    if (auto_count < OD_MAX_AUTO_TESTS) {
        auto_names[auto_count] = name;
        auto_fns[auto_count] = fn;
        auto_sources[auto_count] = source;
        auto_count++;
    } else {
        ++auto_dropped;
    }
}

typedef struct { const char *source, *name, *category, *reason; } dependency;
static const dependency dependencies[] = {
#include "public-tests-cases.inc"
/* The lab-only categories (content, corpus, legacy, rom) live in a file the
 * public tree does not have. */
#if defined(__has_include)
#if __has_include("lab-tests-cases.inc")
#include "lab-tests-cases.inc"
#endif
#endif
};

static const dependency *classify(const char *source, const char *name)
{
    const char *base = strrchr(source, '/');
    base = base ? base + 1 : source;
    const dependency *file = NULL;
    for (size_t i = 0; i < sizeof dependencies / sizeof dependencies[0]; i++) {
        const dependency *d = &dependencies[i];
        if (strcmp(base, d->source)) continue;
        if (!strcmp(name, d->name)) return d;
        if (!strcmp(d->name, "*")) file = d;
    }
    return file;
}

int main(int argc, char **argv)
{
    /* A redirected diagnostic must identify the current case before a signal. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *category = "all";
    const char *only_case = NULL;
    int list = 0, executed = 0, deferred = 0, manual_count = 0;
    if (argc == 2 && !strcmp(argv[1], "--list")) list = 1;
    else if (argc == 3 && !strcmp(argv[1], "--category")) category = argv[2];
    else if (argc == 5 && !strcmp(argv[1], "--category") && !strcmp(argv[3], "--case")) {
        category = argv[2]; only_case = argv[4];
    }
    else if (argc != 1) { fprintf(stderr, "usage: opendana_tests [--list | --category unit|content|corpus|legacy|rom|all [--case exact-name]]\n"); return 2; }
    if (strcmp(category,"unit") && strcmp(category,"content") && strcmp(category,"corpus") &&
        strcmp(category,"legacy") && strcmp(category,"rom") && strcmp(category,"all")) return 2;
#ifndef OD_PUBLIC_TESTS_ONLY
    od_test_register_source("test_assets", test_assets, "test_assets.c");
    od_test_register_source("test_levels", test_levels, "test_levels.c");
    od_test_register_source("test_tiles", test_tiles, "test_tiles.c");
    od_test_register_source("test_compose", test_compose, "test_compose.c");
    od_test_register_source("test_game", test_game, "test_game.c");
    manual_count = 5;
#endif
    printf("OpenDana tests category=%s\n", category);
    if (only_case) printf("DIAGNOSTIC single-case selection=%s (not category coverage)\n", only_case);

    for (int i = 0; i < auto_count; i++) {
        /* Keep the five manual comparators before constructor cases as before. */
        int index = (i + auto_count - manual_count) % auto_count;
        const dependency *d = classify(auto_sources[index], auto_names[index]);
        if (!d) { fprintf(stderr,"ERROR unclassified: %s:%s\n", auto_sources[index], auto_names[index]); od_test_failures++; continue; }
        if (list) { printf("CASE\t%s\t%s\t%s\t%s\n", d->source, auto_names[index], d->category, d->reason); continue; }
        if (only_case && strcmp(only_case, auto_names[index])) {
            printf("FILTER diagnostic %s:%s category=%s\n", d->source, auto_names[index], d->category);
            deferred++; continue;
        }
        if (strcmp(category,"all") && strcmp(category,d->category)) {
            printf("DEFER %s:%s -> %s (%s)\n", d->source, auto_names[index], d->category, d->reason);
            deferred++; continue;
        }
        printf("[%s] source=%s category=%s\n", auto_names[index], d->source, d->category);
        int before = od_test_checks;
        auto_fns[index]();
        if (od_test_checks == before) {
            fprintf(stderr,"ERROR case performed no checks (skip is not success): %s\n", auto_names[index]);
            od_test_failures++;
        }
        executed++;
    }
    if (auto_dropped) {
        printf("FAIL %d registered cases beyond OD_MAX_AUTO_TESTS (%d) did not run\n",
               auto_dropped, OD_MAX_AUTO_TESTS);
        od_test_failures += auto_dropped;
    }

    if (!list && executed == 0) od_test_failures++;
    printf("\n%d checks, %d failures (%d executed, %d explicitly deferred, %d registered cases)\n", od_test_checks,
           od_test_failures, executed, deferred, auto_count);
    return od_test_failures == 0 ? 0 : 1;
}
