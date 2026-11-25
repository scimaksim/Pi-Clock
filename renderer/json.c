/*
 * json.c - Small read-only JSON helper built on jsmn
 *
 * See json.h. jsmn produces a flat token array in document order;
 * an object token is followed by key,value pairs, an array token by
 * its elements, each possibly with nested children. pic_json_next()
 * skips a token and its children, which is all the navigation needs.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Pull in the jsmn implementation first (JSMN_HEADER undefined), so
 * json.h's header-only include is skipped by jsmn's include guard. */
#include "jsmn.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MAX_TOKENS (2 * 1024 * 1024)

int pic_json_parse_max(pic_json_t *j, const char *js, size_t len,
                       int max_tokens)
{
    jsmn_parser p;
    int n;

    memset(j, 0, sizeof(*j));
    if (!js || len == 0) return -1;
    if (max_tokens <= 0) max_tokens = DEFAULT_MAX_TOKENS;

    /* First pass counts tokens so the array is sized exactly. */
    jsmn_init(&p);
    n = jsmn_parse(&p, js, len, NULL, 0);
    if (n <= 0 || n > max_tokens) return -1;

    j->tok = malloc((size_t)n * sizeof(jsmntok_t));
    if (!j->tok) return -1;

    jsmn_init(&p);
    if (jsmn_parse(&p, js, len, j->tok, (unsigned int)n) != n) {
        free(j->tok);
        j->tok = NULL;
        return -1;
    }

    j->js = js;
    j->count = n;
    return 0;
}

int pic_json_parse(pic_json_t *j, const char *js, size_t len)
{
    return pic_json_parse_max(j, js, len, 0);
}

void pic_json_free(pic_json_t *j)
{
    free(j->tok);
    j->tok = NULL;
    j->count = 0;
}

int pic_json_next(const pic_json_t *j, int i)
{
    int remaining = 1;

    if (i < 0 || i >= j->count) return -1;

    /* Walk forward until this token and all descendants are consumed.
     * jsmn gives objects size = number of keys and each key string
     * size = 1 (its value), arrays size = number of elements. */
    while (remaining > 0 && i < j->count) {
        const jsmntok_t *t = &j->tok[i];
        remaining--;
        if (t->type == JSMN_OBJECT || t->type == JSMN_ARRAY)
            remaining += t->size;
        else if (t->type == JSMN_STRING && t->size > 0)
            remaining += t->size;   /* key token owning its value */
        i++;
    }
    return i;
}

static int tok_eq(const pic_json_t *j, int i, const char *s)
{
    const jsmntok_t *t = &j->tok[i];
    size_t len = (size_t)(t->end - t->start);
    return strlen(s) == len && strncmp(j->js + t->start, s, len) == 0;
}

int pic_json_get(const pic_json_t *j, int obj, const char *key)
{
    int i, k, n;

    if (obj < 0 || obj >= j->count || j->tok[obj].type != JSMN_OBJECT)
        return -1;

    n = j->tok[obj].size;
    i = obj + 1;
    for (k = 0; k < n && i >= 0 && i < j->count; k++) {
        /* i is a key; i+1 is its value */
        if (j->tok[i].type == JSMN_STRING && tok_eq(j, i, key))
            return i + 1;
        i = pic_json_next(j, i + 1);
    }
    return -1;
}

int pic_json_path(const pic_json_t *j, int obj, const char *path)
{
    char key[64];
    const char *p = path;

    while (obj >= 0 && *p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len >= sizeof(key)) return -1;
        memcpy(key, p, len);
        key[len] = '\0';
        obj = pic_json_get(j, obj, key);
        p += len;
        if (*p == '.') p++;
    }
    return obj;
}

int pic_json_at(const pic_json_t *j, int arr, int n)
{
    int i, k;

    if (arr < 0 || arr >= j->count || j->tok[arr].type != JSMN_ARRAY)
        return -1;
    if (n < 0 || n >= j->tok[arr].size) return -1;

    i = arr + 1;
    for (k = 0; k < n && i >= 0; k++)
        i = pic_json_next(j, i);
    return (i >= 0 && i < j->count) ? i : -1;
}

int pic_json_size(const pic_json_t *j, int i)
{
    if (i < 0 || i >= j->count) return 0;
    if (j->tok[i].type == JSMN_ARRAY || j->tok[i].type == JSMN_OBJECT)
        return j->tok[i].size;
    return 0;
}

int pic_json_is_array(const pic_json_t *j, int i)
{
    return i >= 0 && i < j->count && j->tok[i].type == JSMN_ARRAY;
}

int pic_json_is_object(const pic_json_t *j, int i)
{
    return i >= 0 && i < j->count && j->tok[i].type == JSMN_OBJECT;
}

int pic_json_eq(const pic_json_t *j, int i, const char *s)
{
    if (i < 0 || i >= j->count) return 0;
    if (j->tok[i].type != JSMN_STRING && j->tok[i].type != JSMN_PRIMITIVE)
        return 0;
    return tok_eq(j, i, s);
}

static int put_utf8(char *out, int w, int out_size, unsigned cp)
{
    if (cp < 0x80) {
        if (w + 1 >= out_size) return -1;
        out[w++] = (char)cp;
    } else if (cp < 0x800) {
        if (w + 2 >= out_size) return -1;
        out[w++] = (char)(0xC0 | (cp >> 6));
        out[w++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (w + 3 >= out_size) return -1;
        out[w++] = (char)(0xE0 | (cp >> 12));
        out[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[w++] = (char)(0x80 | (cp & 0x3F));
    }
    return w;
}

int pic_json_str(const pic_json_t *j, int i, char *out, int out_size)
{
    const char *s, *end;
    int w = 0;

    if (out_size <= 0) return -1;
    out[0] = '\0';
    if (i < 0 || i >= j->count) return -1;
    if (j->tok[i].type != JSMN_STRING && j->tok[i].type != JSMN_PRIMITIVE)
        return -1;

    s = j->js + j->tok[i].start;
    end = j->js + j->tok[i].end;

    while (s < end) {
        int nw;
        if (*s == '\\' && s + 1 < end) {
            char c = s[1];
            s += 2;
            switch (c) {
            case 'n': case 'r': case 't': c = ' '; break;
            case 'b': case 'f': continue;
            case 'u':
                if (s + 4 <= end) {
                    char hex[5] = { s[0], s[1], s[2], s[3], 0 };
                    unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                    s += 4;
                    /* Surrogate pairs are rare in our feeds; drop them */
                    if (cp >= 0xD800 && cp <= 0xDFFF) continue;
                    nw = put_utf8(out, w, out_size, cp);
                    if (nw < 0) goto done;
                    w = nw;
                }
                continue;
            default: break;   /* \" \\ \/ */
            }
            nw = put_utf8(out, w, out_size, (unsigned char)c);
        } else {
            if (w + 1 >= out_size) goto done;
            out[w++] = *s++;
            continue;
        }
        if (nw < 0) goto done;
        w = nw;
    }
done:
    out[w] = '\0';
    return w;
}

double pic_json_num(const pic_json_t *j, int i, double def)
{
    char buf[48];
    char *endp;
    double v;

    if (pic_json_str(j, i, buf, sizeof(buf)) <= 0) return def;
    if (strcmp(buf, "null") == 0) return def;
    v = strtod(buf, &endp);
    return (endp == buf) ? def : v;
}

int pic_json_bool(const pic_json_t *j, int i, int def)
{
    if (pic_json_eq(j, i, "true") || pic_json_eq(j, i, "1") ||
        pic_json_eq(j, i, "True"))
        return 1;
    if (pic_json_eq(j, i, "false") || pic_json_eq(j, i, "0") ||
        pic_json_eq(j, i, "False"))
        return 0;
    return def;
}
