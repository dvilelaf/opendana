/* jsonw.h - tiny ordered JSON writer used by the ROM importer.
 *
 * The importer must reproduce the content package byte for byte, so the
 * writer follows one fixed formatting: two-space (or one-space) indentation, `": "` after a key
 * and `",\n"` between entries, `[]`/`{}` when empty. Strings are escaped the
 * same way (`"`, `\\`, control characters as \uXXXX).
 */
#ifndef OD_IMPORT_JSONW_H
#define OD_IMPORT_JSONW_H

#include <stdio.h>

typedef enum {
    OD_JW_OBJECT,
    OD_JW_ARRAY,
    OD_JW_STRING,
    OD_JW_INTEGER,
    OD_JW_BOOL
} od_jw_type;

typedef struct od_jw od_jw;

od_jw *od_jw_object(void);
od_jw *od_jw_array(void);
od_jw *od_jw_integer(long long value);
od_jw *od_jw_string(const char *value);
od_jw *od_jw_bool(int value);

/* Append `value` to an object under `key`. Takes ownership of `value`. */
void od_jw_add(od_jw *object, const char *key, od_jw *value);
/* Append `value` to an array. Takes ownership of `value`. */
void od_jw_push(od_jw *array, od_jw *value);

/* Write the document with Python-compatible indentation. Returns 0 on success. */
int od_jw_write(FILE *out, const od_jw *node, int indent);

void od_jw_free(od_jw *node);

#endif /* OD_IMPORT_JSONW_H */
