/* a save state travels between OpenDana versions. Core-only, no SDL.
 *
 * The 32-byte build field of the identity footer (formerly the SHA-256 of the
 * executable) is written but never compared. What still decides is the format
 * (od_state_build_id and the size checks) and the imported content identity,
 * and each refusal says which of the two it was (od_state_last_reject).
 *
 * Positive control: tests/state-compat-run.sh builds this file against the
 * earlier state.c (OD_STATE_SRC=...) with -DOD_COMPAT_NO_REASON; the first
 * check (a state from another executable loads) must fail there. */
#include "opendana/ext/state.h"
#include "opendana/flow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define EXPECT(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)

static unsigned char *slurp(const char *p, size_t *n)
{
    FILE *in = fopen(p, "rb");
    if (!in) return NULL;
    fseek(in, 0, SEEK_END); *n = (size_t)ftell(in); rewind(in);
    unsigned char *b = malloc(*n);
    if (b && fread(b, 1, *n, in) != *n) { free(b); b = NULL; }
    fclose(in);
    return b;
}
static void spit(const char *p, const unsigned char *b, size_t n)
{
    FILE *o = fopen(p, "wb");
    if (o) { fwrite(b, 1, n, o); fclose(o); }
}
static void fix_checksum(unsigned char *b, size_t n)
{
    uint32_t s = od_state_checksum(b, n - 4);
    for (unsigned i = 0; i < 4; i++) b[n - 4 + i] = (unsigned char)(s >> (8 * i));
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/state-compat.odstate";
    od_flow *f = calloc(1, sizeof *f);
    if (!f) return 2;
    od_state_context a = {0}, b = {0}, c = {0};
    memset(a.build, 0x11, 32);      /* "executable A" (an old state's digest) */
    memset(b.build, 0x22, 32);      /* "executable B"                         */
    memset(a.content, 0x33, 32);
    memset(b.content, 0x33, 32);    /* same imported content                  */
    memset(c.content, 0x44, 32);    /* other imported content                 */

    EXPECT(od_state_save_ex(f, path, &a) == OD_OK, "save with executable A");
    EXPECT(od_state_load_ex(f, path, &b) == OD_OK,
           "a state saved by executable A loads in executable B");
    memset(b.build, 0, 32);
    EXPECT(od_state_load_ex(f, path, &b) == OD_OK,
           "a state with an old executable digest loads with a zero build field");
#ifndef OD_COMPAT_NO_REASON
    EXPECT(od_state_last_reject() == OD_STATE_REJECT_NONE, "no reason after success");
#endif

    /* Different content identity: refused, with the content reason. */
    od_state_context d = a; memcpy(d.content, c.content, 32);
    EXPECT(od_state_load_ex(f, path, &d) == OD_ERR_FORMAT, "other content is refused");
#ifndef OD_COMPAT_NO_REASON
    EXPECT(od_state_last_reject() == OD_STATE_REJECT_CONTENT, "content reason");
#endif

    /* Forged format id: refused, with the format reason. */
    size_t n = 0;
    unsigned char *buf = slurp(path, &n);
    EXPECT(buf != NULL, "read back");
    if (buf) {
        buf[6] ^= 0x5a;
        fix_checksum(buf, n);
        spit(path, buf, n);
        EXPECT(od_state_load_ex(f, path, &a) == OD_ERR_FORMAT, "forged format is refused");
#ifndef OD_COMPAT_NO_REASON
        EXPECT(od_state_last_reject() == OD_STATE_REJECT_FORMAT, "format reason");
#endif
        free(buf);
    }
    remove(path);
    free(f);
    printf("state-compat: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
