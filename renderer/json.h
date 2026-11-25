/*
 * json.h - Small read-only JSON helper built on jsmn
 *
 * Wraps the jsmn tokenizer (jsmn.h, MIT licence) with the handful of
 * lookups the data fetchers need: object member by key, array element
 * by index, and typed reads of strings, numbers and booleans.
 *
 * Usage:
 *   pic_json_t j;
 *   if (pic_json_parse(&j, buf, len) == 0) {
 *       int feats = pic_json_get(&j, 0, "features");
 *       for (i = 0; i < pic_json_size(&j, feats); i++) {
 *           int f = pic_json_at(&j, feats, i);
 *           ...
 *       }
 *       pic_json_free(&j);
 *   }
 *
 * All lookups return a token index, or -1 if not found; every reader
 * accepts -1 and returns a default, so lookups can be chained without
 * checking each step.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_JSON_H
#define PIC_JSON_H

#include <stddef.h>

#define JSMN_HEADER
#include "jsmn.h"
#undef JSMN_HEADER

typedef struct {
    const char *js;     /* Source text (not owned)        */
    jsmntok_t  *tok;    /* Token array (owned)            */
    int         count;  /* Number of tokens               */
} pic_json_t;

/*
 * pic_json_parse - Tokenise `len` bytes of JSON.
 * Returns 0 on success, -1 on malformed input or allocation failure.
 * Inputs needing more than `max_tokens` tokens are rejected
 * (0 = default cap of 2 million, ~32 MB).
 */
int  pic_json_parse_max(pic_json_t *j, const char *js, size_t len,
                        int max_tokens);
int  pic_json_parse(pic_json_t *j, const char *js, size_t len);
void pic_json_free(pic_json_t *j);

/* Index of the token after `i` and all its children. */
int pic_json_next(const pic_json_t *j, int i);

/* Member value of object `obj` with key `key`, or -1. */
int pic_json_get(const pic_json_t *j, int obj, const char *key);

/* Follow a dotted path of object keys, e.g. "properties.url.geometry". */
int pic_json_path(const pic_json_t *j, int obj, const char *path);

/* Element `n` of array `arr`, or -1. */
int pic_json_at(const pic_json_t *j, int arr, int n);

/* Number of elements (array) or members (object); 0 for others. */
int pic_json_size(const pic_json_t *j, int i);

int pic_json_is_array(const pic_json_t *j, int i);
int pic_json_is_object(const pic_json_t *j, int i);

/* True if token `i` is a string or primitive equal to `s`. */
int pic_json_eq(const pic_json_t *j, int i, const char *s);

/*
 * pic_json_str - Copy a string/primitive value, decoding escapes
 * (\uXXXX is emitted as UTF-8). Returns length, or -1 (out = "").
 */
int pic_json_str(const pic_json_t *j, int i, char *out, int out_size);

/* Numeric value of a number or numeric string; `def` if absent/null. */
double pic_json_num(const pic_json_t *j, int i, double def);

/* true/"true"/1 → 1, false/"false"/0 → 0, otherwise `def`. */
int pic_json_bool(const pic_json_t *j, int i, int def);

#endif /* PIC_JSON_H */
