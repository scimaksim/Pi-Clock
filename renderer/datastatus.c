/*
 * datastatus.c - Health of every data source, for the dashboard
 *
 * Every download through fetch.c, and every connection by the stream
 * clients (lightning, ships, DX cluster), is recorded per source host:
 * when it last worked, when it last failed, how many failures in a row
 * and the last error. The table is written as JSON to PIC_STATUS_PATH
 * (tmpfs, world-readable, no secrets: hosts only, never URLs, which can
 * carry API keys) for the dashboard's Data Sources card.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "datastatus.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_SOURCES 64
#define MAX_USERS   4

__thread const char *pic_status_feature;

typedef struct {
    char   host[64];
    char   users[MAX_USERS][24];
    int    nusers;
    time_t last_ok, last_fail, fail_since;
    int    streak, ok_count, fail_count;
    char   error[80];
} source_t;

static source_t src[MAX_SOURCES];
static int nsrc;
static time_t written;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

/* "https://user:pw@host:443/path?key=..." -> "host" (no credentials,
 * paths or queries ever kept) */
static void host_of(const char *s, char *out, size_t n)
{
    const char *p = strstr(s, "://"), *at, *end;
    size_t len;
    p = p ? p + 3 : s;
    end = p + strcspn(p, "/?#");
    at = memchr(p, '@', (size_t)(end - p));
    if (at) p = at + 1;
    len = strcspn(p, ":/?#");
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        if ((unsigned char)*s >= 0x20) fputc(*s, f);
    }
    fputc('"', f);
}

/* Caller holds mu */
static void write_file(time_t now)
{
    char tmp[] = PIC_STATUS_PATH ".tmp";
    FILE *f = fopen(tmp, "w");
    int i, k;
    if (!f) return;
    fprintf(f, "{\"updated\":%ld,\"sources\":[", (long)now);
    for (i = 0; i < nsrc; i++) {
        const source_t *s = &src[i];
        fprintf(f, "%s{\"host\":", i ? "," : "");
        json_str(f, s->host);
        fprintf(f, ",\"users\":[");
        for (k = 0; k < s->nusers; k++) {
            if (k) fputc(',', f);
            json_str(f, s->users[k]);
        }
        fprintf(f, "],\"last_ok\":%ld,\"last_fail\":%ld,\"fail_since\":%ld,"
                   "\"streak\":%d,\"ok\":%d,\"fail\":%d,\"error\":",
                (long)s->last_ok, (long)s->last_fail, (long)s->fail_since,
                s->streak, s->ok_count, s->fail_count);
        json_str(f, s->error);
        fputc('}', f);
    }
    fprintf(f, "]}\n");
    fclose(f);
    rename(tmp, PIC_STATUS_PATH);
    written = now;
}

void pic_status_report_as(const char *user, const char *source, int ok,
                          const char *detail)
{
    char host[64];
    time_t now = time(NULL);
    source_t *s = NULL;
    int i;

    if (!source || !*source) return;
    host_of(source, host, sizeof(host));
    if (!host[0]) return;
    pthread_mutex_lock(&mu);
    for (i = 0; i < nsrc; i++)
        if (!strcmp(src[i].host, host)) { s = &src[i]; break; }
    if (!s && nsrc < MAX_SOURCES) {
        s = &src[nsrc++];
        memset(s, 0, sizeof(*s));
        snprintf(s->host, sizeof(s->host), "%s", host);
    }
    if (s) {
        int changed = 0;
        if (user && *user) {
            for (i = 0; i < s->nusers; i++)
                if (!strcmp(s->users[i], user)) break;
            if (i == s->nusers && s->nusers < MAX_USERS) {
                snprintf(s->users[s->nusers++], sizeof(s->users[0]), "%s", user);
                changed = 1;
            }
        }
        if (ok) {
            changed |= s->streak != 0;
            s->last_ok = now;
            s->streak = 0;
            s->fail_since = 0;
            s->ok_count++;
        } else {
            if (s->streak == 0) s->fail_since = now;
            s->streak++;
            s->last_fail = now;
            s->fail_count++;
            snprintf(s->error, sizeof(s->error), "%s", detail ? detail : "failed");
            changed = 1;
        }
        /* Rewrite on any change of state, else at most once a minute */
        if (changed ? now - written >= 2 : now - written >= 60)
            write_file(now);
    }
    pthread_mutex_unlock(&mu);
}

void pic_status_report(const char *source, int ok, const char *detail)
{
    pic_status_report_as(pic_status_feature, source, ok, detail);
}
