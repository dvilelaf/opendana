/* replay.c - parser for the reference-compatible scripted input format. */
#include "opendana/replay.h"
#include "opendana/game.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned od_replay_button_bit(const char *name)
{
    if (!strcmp(name, "A"))      return OD_BTN_A;
    if (!strcmp(name, "B"))      return OD_BTN_B;
    if (!strcmp(name, "SELECT")) return OD_BTN_SELECT;
    if (!strcmp(name, "START"))  return OD_BTN_START;
    if (!strcmp(name, "UP"))     return OD_BTN_UP;
    if (!strcmp(name, "DOWN"))   return OD_BTN_DOWN;
    if (!strcmp(name, "LEFT"))   return OD_BTN_LEFT;
    if (!strcmp(name, "RIGHT"))  return OD_BTN_RIGHT;
    return 0;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

/* Parse a BUTTONS field (`-` or names joined by '+') into a mask. */
static unsigned parse_buttons(const char *s)
{
    if (s[0] == '-' && s[1] == '\0')
        return 0;
    unsigned mask = 0;
    char buf[128];
    size_t n = strlen(s);
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        char *t = trim(tok);
        /* Tolerate an optional "port:" prefix from the reference format. */
        char *colon = strchr(t, ':');
        if (colon)
            t = colon + 1;
        unsigned bit = od_replay_button_bit(t);
        if (!bit)
            return (unsigned)-1; /* signal a parse error */
        mask |= bit;
    }
    return mask;
}

od_status od_replay_parse(od_replay *out, const char *text, size_t size)
{
    if (out == NULL || text == NULL)
        return OD_ERR_FORMAT;
    memset(out, 0, sizeof(*out));

    int capacity = 32;
    out->events = (od_replay_event *)malloc((size_t)capacity * sizeof(*out->events));
    if (out->events == NULL)
        return OD_ERR_NOMEM;

    /* Work on a mutable copy so we can tokenise lines in place. */
    char *copy = (char *)malloc(size + 1);
    if (copy == NULL) {
        od_replay_free(out);
        return OD_ERR_NOMEM;
    }
    memcpy(copy, text, size);
    copy[size] = '\0';

    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *l = trim(line);
        if (*l == '\0' || *l == '#')
            continue;
        char *sp = l;
        while (*sp && !isspace((unsigned char)*sp))
            sp++;
        if (*sp == '\0')
            continue; /* a bare frame with no buttons: ignore */
        *sp++ = '\0';
        char *buttons = trim(sp);
        if (*buttons == '\0')
            continue;
        long frame = strtol(l, NULL, 10);
        unsigned mask = parse_buttons(buttons);
        if (mask == (unsigned)-1) {
            od_replay_free(out);
            free(copy);
            return OD_ERR_FORMAT;
        }
        if (out->count == capacity) {
            capacity *= 2;
            od_replay_event *grown =
                (od_replay_event *)realloc(out->events, (size_t)capacity * sizeof(*grown));
            if (grown == NULL) {
                od_replay_free(out);
                free(copy);
                return OD_ERR_NOMEM;
            }
            out->events = grown;
        }
        out->events[out->count].frame = frame;
        out->events[out->count].buttons = mask;
        out->count++;
    }

    free(copy);
    /* Keep events ordered; the format is already sorted, but be defensive. */
    for (int i = 1; i < out->count; i++) {
        od_replay_event e = out->events[i];
        int j = i - 1;
        while (j >= 0 && out->events[j].frame > e.frame) {
            out->events[j + 1] = out->events[j];
            j--;
        }
        out->events[j + 1] = e;
    }
    out->last_frame = out->count ? out->events[out->count - 1].frame : 0;
    return OD_OK;
}

od_status od_replay_load_file(od_replay *out, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return OD_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return OD_ERR_IO;
    }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return OD_ERR_IO;
    }
    char *buffer = (char *)malloc((size_t)len + 1);
    if (buffer == NULL) {
        fclose(f);
        return OD_ERR_NOMEM;
    }
    size_t got = fread(buffer, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buffer);
        return OD_ERR_IO;
    }
    od_status st = od_replay_parse(out, buffer, (size_t)len);
    free(buffer);
    return st;
}

void od_replay_free(od_replay *r)
{
    if (r == NULL)
        return;
    free(r->events);
    r->events = NULL;
    r->count = 0;
    r->last_frame = 0;
}

unsigned od_replay_buttons_at(const od_replay *r, long frame)
{
    unsigned buttons = 0;
    for (int i = 0; i < r->count; i++) {
        if (r->events[i].frame > frame)
            break;
        buttons = r->events[i].buttons;
    }
    return buttons;
}
