/*
 * spacetrack.c - Space-Track.org as a back-up source of orbital data
 *
 * When CelesTrak can't be reached, the satellite features ask
 * Space-Track (US Space Force, free account) for the same data. The
 * account is entered in the dashboard and stored root-only in
 * SPACETRACK_CRED ("identity\npassword\n"); it is never logged, never
 * put on a command line (the form goes to wget in a 0600 temp file),
 * and only sent to www.space-track.org.
 *
 * Each request is one POST to /ajaxauth/login with the query included,
 * so there is no session to keep. Space-Track's guidelines: under 30
 * requests a minute and 300 an hour, and don't fetch the same GP data
 * more than about once an hour. This stays far inside that: at most
 * 12 requests an hour, and a failed sign-in stops all requests for a
 * day (repeated bad logins can lock an account). State is kept in
 * /data/cache, so restarts can't add requests.
 *
 * Replies come as 3LE ("0 NAME" lines); they are rewritten to the
 * CelesTrak TLE layout (name line without the "0 ") so the callers'
 * parsers read them unchanged.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "spacetrack.h"
#include "fetch.h"
#include "datastatus.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ST_LOGIN    "https://www.space-track.org/ajaxauth/login"
#define ST_BASE     "https://www.space-track.org"
#define ST_STATE    "/data/cache/pi-clock-spacetrack.state"
#define ST_PER_HOUR 12

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int loaded;
static time_t pause_until, recent[ST_PER_HOUR];

static const char *cred_path(void)
{
    const char *e = getenv("PIC_SPACETRACK_CRED");      /* tests */
    return e && *e ? e : SPACETRACK_CRED;
}

static const char *state_path(void)
{
    const char *e = getenv("PIC_SPACETRACK_STATE");     /* tests */
    return e && *e ? e : ST_STATE;
}

static void load(void)
{
    FILE *f;
    int i;
    if (loaded) return;
    loaded = 1;
    f = fopen(state_path(), "r");
    if (!f) return;
    if (fscanf(f, "%ld", (long *)&pause_until) != 1) pause_until = 0;
    for (i = 0; i < ST_PER_HOUR; i++)
        if (fscanf(f, "%ld", (long *)&recent[i]) != 1) recent[i] = 0;
    fclose(f);
}

static void save(void)
{
    char tmp[256];
    FILE *f;
    int i;
    mkdir("/data/cache", 0755);
    snprintf(tmp, sizeof(tmp), "%s.tmp", state_path());
    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%ld\n", (long)pause_until);
    for (i = 0; i < ST_PER_HOUR; i++) fprintf(f, "%ld\n", (long)recent[i]);
    fclose(f);
    rename(tmp, state_path());
}

/* identity and password from the root-only file; 0 if none */
static int read_cred(char *id, size_t idn, char *pw, size_t pwn)
{
    FILE *f = fopen(cred_path(), "r");
    int ok = 0;
    if (!f) return 0;
    if (fgets(id, (int)idn, f) && fgets(pw, (int)pwn, f)) {
        id[strcspn(id, "\r\n")] = 0;
        pw[strcspn(pw, "\r\n")] = 0;
        ok = id[0] && pw[0];
    }
    fclose(f);
    return ok;
}

int pic_spacetrack_configured(void)
{
    char id[160], pw[160];
    int ok = read_cred(id, sizeof(id), pw, sizeof(pw));
    memset(pw, 0, sizeof(pw));
    return ok;
}

/* application/x-www-form-urlencoded */
static void urlenc(char *out, size_t n, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

/* 3LE -> CelesTrak-style TLE (drop the "0 " before names); checks it
 * really is TLE data. Returns the number of satellites. */
static int tidy_tle(const char *path)
{
    FILE *in = fopen(path, "r"), *out;
    char line[256], tmp[300];
    int n = 0;
    if (!in) return 0;
    snprintf(tmp, sizeof(tmp), "%s.tidy", path);
    out = fopen(tmp, "w");
    if (!out) { fclose(in); return 0; }
    while (fgets(line, sizeof(line), in)) {
        if (line[0] == '0' && line[1] == ' ') fputs(line + 2, out);
        else {
            if (line[0] == '2' && line[1] == ' ') n++;
            fputs(line, out);
        }
    }
    fclose(in);
    fclose(out);
    if (n > 0) rename(tmp, path);
    else unlink(tmp);
    return n;
}

int pic_spacetrack_fetch(const char *query, const char *path)
{
    char id[160], pw[160], body[6400], eid[480], epw[480], q[2400], eq[4800];
    time_t now = time(NULL);
    int i, oldest = 0, rc, n;
    FILE *f;
    char head[64] = "";

    if (!read_cred(id, sizeof(id), pw, sizeof(pw))) return -1;

    pthread_mutex_lock(&mu);
    load();
    if (now < pause_until) {
        pthread_mutex_unlock(&mu);
        memset(pw, 0, sizeof(pw));
        pic_status_report(ST_BASE, 0, "held back after a failed sign-in");
        return -1;
    }
    for (i = 1; i < ST_PER_HOUR; i++) if (recent[i] < recent[oldest]) oldest = i;
    if (now - recent[oldest] < 3600 && recent[oldest] <= now) {
        pthread_mutex_unlock(&mu);
        memset(pw, 0, sizeof(pw));
        pic_status_report(ST_BASE, 0, "held back (12 requests an hour max)");
        return -1;
    }
    recent[oldest] = now;
    save();
    pthread_mutex_unlock(&mu);

    snprintf(q, sizeof(q), "%s%s", ST_BASE, query);
    urlenc(eid, sizeof(eid), id);
    urlenc(epw, sizeof(epw), pw);
    urlenc(eq, sizeof(eq), q);
    snprintf(body, sizeof(body), "identity=%s&password=%s&query=%s", eid, epw, eq);
    memset(pw, 0, sizeof(pw));
    memset(epw, 0, sizeof(epw));

    rc = pic_fetch_post_form_to_file(ST_LOGIN, body, path, 90);
    memset(body, 0, sizeof(body));
    if (rc != 0) { unlink(path); return -1; }

    /* A refused sign-in still answers 200: {"Login":"Failed"} */
    if ((f = fopen(path, "r")) != NULL) {
        if (!fgets(head, sizeof(head), f)) head[0] = 0;
        fclose(f);
    }
    if (strstr(head, "Login") && strstr(head, "Failed")) {
        pthread_mutex_lock(&mu);
        pause_until = now + 86400;
        save();
        pthread_mutex_unlock(&mu);
        unlink(path);
        printf("spacetrack: sign-in refused, check the account in the dashboard (no requests for 24 h)\n");
        pic_status_report(ST_BASE, 0, "sign-in refused");
        return -1;
    }
    n = tidy_tle(path);
    if (n <= 0) {
        /* "[]" or an error page: nothing usable */
        unlink(path);
        return -1;
    }
    printf("spacetrack: %d satellites (%s)\n", n, query);
    return 0;
}
