/* extract_audio.c - audio/sequence.json.
 *
 * Emits the ODSEQ1 sparse write stream: it follows the sound data reachable
 * from the request table and writes the ordered list of byte runs the native
 * sequencer consumes. No PCM, no executable code.
 */
#include "fsutil.h"
#include "jsonw.h"
#include "rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NBANKS 64
#define BANK_BYTES 8192

typedef struct {
    int16_t *used;   /* [bank * BANK_BYTES + offset] = byte or -1 */
    uint8_t *seen;   /* traversal marks */
} scan;

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, "%s", msg);
}

/* Ported `read`: follow the MMC3 windows and record every byte touched. */
static int audio_read(scan *s, const uint8_t *prg, int bank, uint32_t address,
                      uint32_t n, uint8_t *out, char *err, size_t err_cap)
{
    while (n > 0) {
        uint32_t base;
        if (address >= 0x8000 && address < 0xA000) {
            bank = 10;
            base = 0x8000;
        } else if (address >= 0xA000 && address < 0xC000) {
            base = 0xA000;
        } else if (address >= 0xC000 && address < 0xE000) {
            bank = 14;
            base = 0xC000;
        } else if (address >= 0xE000 && address < 0x10000) {
            bank = 15;
            base = 0xE000;
        } else {
            set_err(err, err_cap, "non-audio address in the sequencer stream");
            return -1;
        }
        if (bank < 0 || bank >= NBANKS) {
            set_err(err, err_cap, "audio bank out of range");
            return -1;
        }
        uint32_t avail = base + BANK_BYTES - address;
        uint32_t take = n < avail ? n : avail;
        for (uint32_t i = 0; i < take; i++) {
            uint8_t b = prg[(size_t)bank * BANK_BYTES + (address - base) + i];
            s->used[(size_t)bank * BANK_BYTES + (address - base) + i] = (int16_t)b;
            if (out != NULL)
                out[i] = b;
        }
        address += take;
        n -= take;
    }
    return 0;
}

static int audio_read1(scan *s, const uint8_t *prg, int bank, uint32_t address,
                       uint8_t *out, char *err, size_t err_cap)
{
    return audio_read(s, prg, bank, address, 1, out, err, err_cap);
}

/* Marking little-endian word read. */
static int audio_word(scan *s, const uint8_t *prg, int bank, uint32_t address,
                      uint16_t *out, char *err, size_t err_cap)
{
    uint8_t b[2];
    if (audio_read(s, prg, bank, address, 2, b, err, err_cap) != 0)
        return -1;
    *out = (uint16_t)(b[0] | (b[1] << 8));
    return 0;
}

od_status od_import_audio(const od_rom *rom, const char *dir, char *err, size_t err_cap)
{
    const uint8_t *prg = rom->prg;
    scan s;
    s.used = (int16_t *)malloc((size_t)NBANKS * BANK_BYTES * sizeof(int16_t));
    s.seen = (uint8_t *)calloc((size_t)NBANKS * 65536, 1);
    if (s.used == NULL || s.seen == NULL) {
        free(s.used);
        free(s.seen);
        set_err(err, err_cap, "out of memory for the audio scan");
        return OD_ERR_NOMEM;
    }
    for (size_t i = 0; i < (size_t)NBANKS * BANK_BYTES; i++)
        s.used[i] = -1;

    int ok = audio_read(&s, prg, 10, 0x87e7, 24, NULL, err, err_cap) == 0 &&
             audio_read(&s, prg, 10, 0x8873, 238, NULL, err, err_cap) == 0 &&
             audio_read(&s, prg, 10, 0x8962, 119, NULL, err, err_cap) == 0 &&
             audio_read(&s, prg, 10, 0x84af, 15, NULL, err, err_cap) == 0 &&
             audio_read(&s, prg, 10, 0x9688, 98, NULL, err, err_cap) == 0 &&
             audio_read(&s, prg, 10, 0x96ea, 176, NULL, err, err_cap) == 0;

    typedef struct { int bank; uint16_t address; } frame;
    frame *pending = NULL;
    size_t pending_count = 0, pending_cap = 0;

#define PUSH(b, a) do {                                                            \
        if (pending_count == pending_cap) {                                        \
            size_t ncap = pending_cap ? pending_cap * 2 : 256;                     \
            frame *np = (frame *)realloc(pending, ncap * sizeof(*np));             \
            if (np == NULL) { ok = 0; goto done; }                                 \
            pending = np; pending_cap = ncap;                                      \
        }                                                                          \
        pending[pending_count].bank = (b);                                         \
        pending[pending_count].address = (a);                                      \
        pending_count++;                                                           \
    } while (0)

    uint8_t byte;
    for (int song = 1; ok && song < 120; song++) {
        if (audio_read1(&s, prg, 10, 0x8961 + song, &byte, err, err_cap) != 0) {
            ok = 0;
            break;
        }
        int bank = byte;
        uint16_t address;
        if (audio_word(&s, prg, 10, 0x8871 + song * 2, &address, err, err_cap) != 0) {
            ok = 0;
            break;
        }
        uint16_t at = address;
        int channels = 0;
        for (;;) {
            uint8_t ch;
            if (audio_read1(&s, prg, bank, at, &ch, err, err_cap) != 0) {
                ok = 0;
                break;
            }
            if (ch == 255)
                break;
            if (ch > 7 || channels >= 8) {
                set_err(err, err_cap, "invalid audio request");
                ok = 0;
                break;
            }
            uint16_t ptr;
            if (audio_word(&s, prg, bank, (uint32_t)at + 1, &ptr, err, err_cap) != 0) {
                ok = 0;
                break;
            }
            PUSH(bank, ptr);
            channels++;
            at += 3;
        }
    }

    static const int one_ops[] = {0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xeb, 0xed, 0xf0};
    while (ok && pending_count > 0) {
        frame fr = pending[--pending_count];
        int bank = fr.bank;
        uint16_t at = fr.address;
        for (;;) {
            size_t key = (size_t)bank * 65536 + at;
            if (s.seen[key])
                break;
            s.seen[key] = 1;
            uint8_t op;
            if (audio_read1(&s, prg, bank, at, &op, err, err_cap) != 0) {
                ok = 0;
                break;
            }
            at++;
            int is_one = 0;
            for (size_t k = 0; k < sizeof(one_ops) / sizeof(one_ops[0]); k++)
                if (op == one_ops[k])
                    is_one = 1;
            if (is_one) {
                uint8_t arg;
                if (audio_read1(&s, prg, bank, at, &arg, err, err_cap) != 0) {
                    ok = 0;
                    break;
                }
                at++;
                if (op == 0xe0) {
                    uint16_t ptr;
                    if (audio_word(&s, prg, 10, (uint32_t)(0x89d9 + 2 * arg), &ptr,
                                   err, err_cap) != 0) {
                        ok = 0;
                        break;
                    }
                    for (int offset = 0; offset < 256; offset += 2) {
                        uint8_t pair[2];
                        if (audio_read(&s, prg, bank, (uint32_t)(ptr + offset), 2, pair,
                                       err, err_cap) != 0) {
                            ok = 0;
                            break;
                        }
                        if (pair[1] == 0)
                            break;
                    }
                }
            } else if (op == 0xe8 || op == 0xe9) {
                uint16_t target;
                if (audio_word(&s, prg, bank, at, &target, err, err_cap) != 0) {
                    ok = 0;
                    break;
                }
                PUSH(bank, target);
                at += 2;
                if (op == 0xe8)
                    break;
            } else if (op == 0xea || op == 0xff) {
                break;
            } else if (op >= 0xc0 && op < 0xe0) {
                uint8_t index = op & 63;
                uint8_t descriptor[4];
                if (audio_read(&s, prg, 10, (uint32_t)(0x87ff + index * 4), 4, descriptor,
                               err, err_cap) != 0) {
                    ok = 0;
                    break;
                }
                if (audio_read(&s, prg, 14,
                               (uint32_t)(0xc000 + descriptor[2] * 64),
                               (uint32_t)descriptor[3] * 16 + 1, NULL, err, err_cap) != 0) {
                    ok = 0;
                    break;
                }
            }
        }
    }

done:
    free(pending);
    free(s.seen);
    if (!ok) {
        free(s.used);
        return OD_ERR_FORMAT;
    }

    /* Emit the sorted, contiguous write list (export_gfx.audio_json). */
    od_jw *doc = od_jw_object();
    od_jw_add(doc, "schema_version", od_jw_integer(1));
    od_jw_add(doc, "id", od_jw_string("sequence"));
    od_jw_add(doc, "format", od_jw_string("ODSEQ1"));
    od_jw *writes = od_jw_array();
    for (int bank = 0; bank < NBANKS; bank++) {
        for (int off = 0; off < BANK_BYTES;) {
            if (s.used[(size_t)bank * BANK_BYTES + off] < 0) {
                off++;
                continue;
            }
            int start = off;
            while (off < BANK_BYTES && s.used[(size_t)bank * BANK_BYTES + off] >= 0 &&
                   off - start < 32)
                off++;
            od_jw *w = od_jw_object();
            od_jw_add(w, "bank", od_jw_integer(bank));
            uint32_t base = (bank == 10) ? 0x8000u : (bank == 14) ? 0xC000u :
                            (bank == 15) ? 0xE000u : 0xA000u;
            od_jw_add(w, "address", od_jw_integer((long long)(base + (uint32_t)start)));
            od_jw *data = od_jw_array();
            for (int i = start; i < off; i++)
                od_jw_push(data, od_jw_integer(s.used[(size_t)bank * BANK_BYTES + i]));
            od_jw_add(w, "data", data);
            od_jw_push(writes, w);
        }
    }
    od_jw_add(doc, "writes", writes);
    free(s.used);

    char path[4096];
    od_fs_join(path, sizeof(path), dir, "audio/sequence.json");
    od_fs_mkdir_parent(path);
    FILE *f = od_fs_fopen(path, "wb");
    if (f == NULL) {
        od_jw_free(doc);
        set_err(err, err_cap, "cannot write audio/sequence.json");
        return OD_ERR_IO;
    }
    od_jw_write(f, doc, 1);
    fputc('\n', f);
    fclose(f);
    od_jw_free(doc);
    return OD_OK;
}
