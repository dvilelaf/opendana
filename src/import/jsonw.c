/* jsonw.c - tiny ordered JSON writer. See jsonw.h. */
#include "jsonw.h"

#include <stdlib.h>
#include <string.h>

struct od_jw {
    od_jw_type type;
    char *key;   /* object member name, or NULL */
    char *str;   /* string payload */
    long long num;
    od_jw *head;
    od_jw *tail;
    od_jw *next;
};

static char *dup_str(const char *s)
{
    size_t n = strlen(s != NULL ? s : "") + 1;
    char *out = (char *)malloc(n);
    if (out != NULL)
        memcpy(out, s != NULL ? s : "", n);
    return out;
}

static od_jw *node_new(od_jw_type type)
{
    od_jw *n = (od_jw *)calloc(1, sizeof(*n));
    if (n != NULL)
        n->type = type;
    return n;
}

od_jw *od_jw_object(void) { return node_new(OD_JW_OBJECT); }
od_jw *od_jw_array(void) { return node_new(OD_JW_ARRAY); }
od_jw *od_jw_integer(long long value)
{
    od_jw *n = node_new(OD_JW_INTEGER);
    if (n != NULL)
        n->num = value;
    return n;
}

od_jw *od_jw_bool(int value)
{
    od_jw *n = node_new(OD_JW_BOOL);
    if (n != NULL)
        n->num = value ? 1 : 0;
    return n;
}

od_jw *od_jw_string(const char *value)
{
    od_jw *n = node_new(OD_JW_STRING);
    if (n == NULL)
        return NULL;
    n->str = dup_str(value);
    if (n->str == NULL) {
        free(n);
        return NULL;
    }
    return n;
}

static void append(od_jw *parent, od_jw *value)
{
    if (parent == NULL || value == NULL)
        return;
    if (parent->tail == NULL)
        parent->head = value;
    else
        parent->tail->next = value;
    parent->tail = value;
}

void od_jw_add(od_jw *object, const char *key, od_jw *value)
{
    if (object == NULL || value == NULL)
        return;
    value->key = dup_str(key);
    if (value->key == NULL)
        return;
    append(object, value);
}

void od_jw_push(od_jw *array, od_jw *value) { append(array, value); }

void od_jw_free(od_jw *node)
{
    while (node != NULL) {
        od_jw *next = node->next;
        free(node->key);
        free(node->str);
        od_jw_free(node->head);
        free(node);
        node = next;
    }
}

static void indent_to(FILE *out, int indent, int level)
{
    for (int i = 0; i < indent * level; i++)
        fputc(' ', out);
}

static void write_escaped(FILE *out, const char *s)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        switch (*p) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\b': fputs("\\b", out); break;
        case '\f': fputs("\\f", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 0x20)
                fprintf(out, "\\u%04x", *p);
            else
                fputc(*p, out);
        }
    }
    fputc('"', out);
}

static int write_node(FILE *out, const od_jw *node, int indent, int level)
{
    if (node == NULL)
        return -1;
    switch (node->type) {
    case OD_JW_STRING:
        write_escaped(out, node->str != NULL ? node->str : "");
        return 0;
    case OD_JW_INTEGER:
        fprintf(out, "%lld", node->num);
        return 0;
    case OD_JW_BOOL:
        fputs(node->num ? "true" : "false", out);
        return 0;
    case OD_JW_OBJECT:
    case OD_JW_ARRAY: {
        int is_obj = node->type == OD_JW_OBJECT;
        if (node->head == NULL) {
            fputs(is_obj ? "{}" : "[]", out);
            return 0;
        }
        fputs(is_obj ? "{\n" : "[\n", out);
        for (const od_jw *c = node->head; c != NULL; c = c->next) {
            indent_to(out, indent, level + 1);
            if (is_obj) {
                write_escaped(out, c->key != NULL ? c->key : "");
                fputs(": ", out);
            }
            if (write_node(out, c, indent, level + 1) != 0)
                return -1;
            if (c->next != NULL)
                fputc(',', out);
            fputc('\n', out);
        }
        indent_to(out, indent, level);
        fputs(is_obj ? "}" : "]", out);
        return 0;
    }
    }
    return -1;
}

int od_jw_write(FILE *out, const od_jw *node, int indent)
{
    if (indent < 1)
        indent = 1;
    return write_node(out, node, indent, 0);
}
