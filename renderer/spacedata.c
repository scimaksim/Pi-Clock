/*
 * spacedata.c - Space-Track data for the reentry, close-approach,
 * geostationary and breakup-debris layers, and catalogue counts
 *
 * Runs only while a Space-Track account is saved. Each refresh is one
 * session (sign in, several queries, sign out), well inside the
 * account limits:
 *   every 4 h  reentry predictions (TIP), decay predictions, public
 *              close approaches, then TLEs for the objects they name
 *   every 24 h geostationary payloads, newest debris in the catalogue,
 *              catalogue counts
 * Replies are cached in SPACEDATA_DIR for the layers to read.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "feature.h"
#include "spacetrack.h"
#include "spacedata.h"

#define FAST (4 * 3600)
#define SLOW (24 * 3600)
#define BASE "/basicspacedata/query/class/"

static const char *dir(void)
{
    const char *e = getenv("PIC_SPACEDATA_DIR");
    return e && *e ? e : SPACEDATA_DIR;
}

static void path_of(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", dir(), name);
}

static time_t age_of(const char *name)
{
    char p[256];
    struct stat st;
    path_of(p, sizeof(p), name);
    return stat(p, &st) == 0 ? time(NULL) - st.st_mtime : (time_t)1 << 30;
}

static char *slurp(const char *name)
{
    char p[256];
    FILE *f;
    long n;
    char *b;
    path_of(p, sizeof(p), name);
    f = fopen(p, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    b = n > 0 && n < (8 << 20) ? malloc((size_t)n + 1) : NULL;
    if (b) { b[fread(b, 1, (size_t)n, f)] = 0; }
    fclose(f);
    return b;
}

/* Add every value of "key":"123" in txt to a comma list (no repeats) */
void pic_spacedata_ids(const char *txt, const char *key, char *list, size_t n)
{
    char pat[48];
    const char *p = txt;
    if (!txt) return;
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    while ((p = strstr(p, pat)) != NULL) {
        char id[16];
        int k = 0;
        size_t used = strlen(list);
        p += strlen(pat);
        while (*p >= '0' && *p <= '9' && k < 9) id[k++] = *p++;
        id[k] = 0;
        if (!k) continue;
        {   /* already listed? */
            const char *h = list;
            int dup = 0;
            while ((h = strstr(h, id)) != NULL) {
                if ((h == list || h[-1] == ',') && (h[k] == ',' || h[k] == 0)) { dup = 1; break; }
                h += k;
            }
            if (dup) continue;
        }
        if (used + k + 2 >= n) break;
        snprintf(list + used, n - used, "%s%s", used ? "," : "", id);
    }
}

static void finish(const char *tmpname, const char *name)
{
    char a[256], b[256];
    path_of(a, sizeof(a), tmpname);
    path_of(b, sizeof(b), name);
    rename(a, b);
}

static int fetch_fast(void)
{
    char p[3][256];
    const char *q[3] = {
        BASE "tip/MSG_EPOCH/%3Enow-3/orderby/MSG_EPOCH%20desc/format/json",
        BASE "decay/DECAY_EPOCH/%3Enow-1/orderby/DECAY_EPOCH%20asc/limit/400/format/json",
        BASE "cdm_public/TCA/%3Enow/orderby/TCA%20asc/limit/300/format/json",
    };
    const char *pp[3];
    int got, i;
    path_of(p[0], sizeof(p[0]), "tip.json.new");
    path_of(p[1], sizeof(p[1]), "decay.json.new");
    path_of(p[2], sizeof(p[2]), "cdm.json.new");
    for (i = 0; i < 3; i++) pp[i] = p[i];
    got = pic_spacetrack_fetch_raw(q, pp, 3);
    if (got <= 0) return -1;
    if (access(p[0], F_OK) == 0) finish("tip.json.new", "tip.json");
    if (access(p[1], F_OK) == 0) finish("decay.json.new", "decay.json");
    if (access(p[2], F_OK) == 0) finish("cdm.json.new", "cdm.json");

    /* TLEs for the objects named, to draw ground tracks and orbits */
    {
        static char ids[16 * 1024];
        char *t = slurp("tip.json"), *c = slurp("cdm.json"), *qq;
        ids[0] = 0;
        pic_spacedata_ids(t, "NORAD_CAT_ID", ids, sizeof(ids));
        pic_spacedata_ids(c, "SAT_1_ID", ids, sizeof(ids));
        pic_spacedata_ids(c, "SAT_2_ID", ids, sizeof(ids));
        free(t);
        free(c);
        {
            int n = ids[0] ? 1 : 0;
            const char *k;
            for (k = ids; *k; k++) n += *k == ',';
            printf("spacedata: %d reentry / close-approach objects\n", n);
        }
        if (ids[0] && asprintf(&qq, BASE "gp/NORAD_CAT_ID/%s/format/3le", ids) > 0) {
            char tp[256];
            const char *qa[1], *pa[1];
            path_of(tp, sizeof(tp), "objects.tle.new");
            qa[0] = qq;
            pa[0] = tp;
            if (pic_spacetrack_fetch_raw(qa, pa, 1) == 1) finish("objects.tle.new", "objects.tle");
            free(qq);
        }
    }
    return 0;
}

static int fetch_slow(void)
{
    char p[3][256];
    const char *q[3] = {
        BASE "gp/MEAN_MOTION/0.99--1.01/ECCENTRICITY/%3C0.01/OBJECT_TYPE/PAYLOAD/EPOCH/%3Enow-10"
             "/DECAY_DATE/null-val/format/3le",
        BASE "satcat/OBJECT_TYPE/DEBRIS/orderby/NORAD_CAT_ID%20desc/limit/1000"
             "/predicates/NORAD_CAT_ID,OBJECT_ID,OBJECT_NAME,COUNTRY,LAUNCH,DECAY/format/json",
        BASE "boxscore/format/json",
    };
    const char *pp[3];
    int got, i;
    path_of(p[0], sizeof(p[0]), "geo.tle.new");
    path_of(p[1], sizeof(p[1]), "debris.json.new");
    path_of(p[2], sizeof(p[2]), "boxscore.json.new");
    for (i = 0; i < 3; i++) pp[i] = p[i];
    got = pic_spacetrack_fetch_raw(q, pp, 3);
    if (got <= 0) return -1;
    if (access(p[0], F_OK) == 0) finish("geo.tle.new", "geo.tle");
    if (access(p[1], F_OK) == 0) finish("debris.json.new", "debris.json");
    if (access(p[2], F_OK) == 0) finish("boxscore.json.new", "boxscore.json");
    return 0;
}

static time_t seen_stamp;

static int spacedata_work(pic_feature_t *f)
{
    time_t stamp = pic_spacetrack_stamp();
    (void)f;
    static int first = 1;
    if (!pic_spacetrack_configured()) return 600;
    mkdir(dir(), 0755);
    if (first) { first = 0; seen_stamp = stamp; }
    if (stamp != seen_stamp) {
        /* account saved or changed: refresh now */
        seen_stamp = stamp;
        printf("spacedata: Space-Track account found, refreshing\n");
        fetch_fast();
        fetch_slow();
        return 900;
    }
    if (age_of("tip.json") >= FAST - 60) fetch_fast();
    else if (age_of("geo.tle") >= SLOW - 60) fetch_slow();
    return 900;
}

static int spacedata_wanted(pic_feature_t *f)
{
    (void)f;
    return pic_spacetrack_configured();
}

static pic_feature_t spacedata_feature = {
    .name = "spacedata",
    .work = spacedata_work,
    .wanted = spacedata_wanted,
    .needs_network = 1,
};

pic_feature_t *pic_feature_spacedata(void)
{
    return &spacedata_feature;
}
