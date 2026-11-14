/*
 * spacelayers.c - Parse the cached Space-Track replies (see spacedata.c)
 * for the space-object layers. No drawing here; see layers_spacetrack.c.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "spacelayers.h"
#include "json.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

time_t sl_parse_time(const char *s, double *frac)
{
    struct tm tm;
    int Y, M, D, h, m;
    double sec;
    if (frac) *frac = 0;
    if (!s || sscanf(s, "%d-%d-%d%*1[ T]%d:%d:%lf", &Y, &M, &D, &h, &m, &sec) != 6)
        return 0;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = (int)sec;
    if (frac) *frac = sec - (int)sec;
    return timegm(&tm);
}

static int jstr(const pic_json_t *j, int obj, const char *key, char *out, int n)
{
    int v = pic_json_get(j, obj, key);
    out[0] = 0;
    return v >= 0 ? pic_json_str(j, v, out, n) : -1;
}

static double jnum(const pic_json_t *j, int obj, const char *key, double def)
{
    int v = pic_json_get(j, obj, key);
    return v >= 0 ? pic_json_num(j, v, def) : def;
}

static double wrap180(double l)
{
    l = fmod(l + 180.0, 360.0);
    if (l < 0) l += 360.0;
    return l - 180.0;
}

/* ------------------------------------------------------------ TIP */

typedef struct {
    int id, window_min, descending;
    char msg[24];
    time_t decay;
    double lat, lon, incl;
} tip_t;

int sl_parse_tip(const char *js, time_t now, time_t keep, sl_reentry_t *out, int max)
{
    pic_json_t j;
    tip_t t[64];
    int n = 0, i, cnt, k, m = 0;
    if (!js || pic_json_parse(&j, js, strlen(js)) != 0) return 0;
    if (!pic_json_is_array(&j, 0)) { pic_json_free(&j); return 0; }
    cnt = pic_json_size(&j, 0);
    for (i = 0; i < cnt; i++) {
        char ep[32], msg[24], dir[16];
        int o = pic_json_at(&j, 0, i), id, slot = -1;
        if (o < 0) continue;
        id = (int)jnum(&j, o, "NORAD_CAT_ID", 0);
        jstr(&j, o, "MSG_EPOCH", msg, sizeof(msg));
        jstr(&j, o, "DECAY_EPOCH", ep, sizeof(ep));
        if (id <= 0 || !ep[0]) continue;
        for (k = 0; k < n; k++)
            if (t[k].id == id) { slot = k; break; }
        if (slot >= 0 && strcmp(msg, t[slot].msg) <= 0) continue;   /* older message */
        if (slot < 0) {
            if (n >= 64) continue;
            slot = n++;
        }
        t[slot].id = id;
        snprintf(t[slot].msg, sizeof(t[slot].msg), "%s", msg);
        t[slot].decay = sl_parse_time(ep, NULL);
        t[slot].window_min = (int)jnum(&j, o, "WINDOW", 0);
        t[slot].lat = jnum(&j, o, "LAT", 0);
        t[slot].lon = wrap180(jnum(&j, o, "LON", 0));
        t[slot].incl = jnum(&j, o, "INCL", 0);
        jstr(&j, o, "DIRECTION", dir, sizeof(dir));
        t[slot].descending = strcmp(dir, "descending") == 0;
    }
    pic_json_free(&j);
    /* sort by time, keep the recent and coming ones */
    for (i = 1; i < n; i++)
        for (k = i; k > 0 && t[k].decay < t[k - 1].decay; k--) {
            tip_t x = t[k]; t[k] = t[k - 1]; t[k - 1] = x;
        }
    for (i = 0; i < n && m < max; i++) {
        sl_reentry_t *r = &out[m];
        if (!t[i].decay || t[i].decay < now - keep) continue;
        memset(r, 0, sizeof(*r));
        r->id = t[i].id;
        snprintf(r->name, sizeof(r->name), "%d", t[i].id);
        r->decay = t[i].decay;
        r->window_min = t[i].window_min;
        r->lat = t[i].lat; r->lon = t[i].lon; r->incl = t[i].incl;
        r->descending = t[i].descending;
        m++;
    }
    return m;
}

/* ------------------------------------------------------------ CDM */

#define CDM_SCAN 64     /* pairs looked at before keeping the soonest */

int sl_parse_cdm(const char *js, time_t now, int hours, sl_conj_t *res, int max)
{
    pic_json_t j;
    char made[CDM_SCAN][32];
    sl_conj_t *out;
    int n = 0, i, k, cnt;
    if (max <= 0 || !(out = calloc(CDM_SCAN, sizeof(*out)))) return 0;
    if (!js || pic_json_parse(&j, js, strlen(js)) != 0) { free(out); return 0; }
    if (!pic_json_is_array(&j, 0)) { pic_json_free(&j); free(out); return 0; }
    cnt = pic_json_size(&j, 0);
    for (i = 0; i < cnt; i++) {
        char t[40], c[32];
        int o = pic_json_at(&j, 0, i), a, b, slot = -1;
        sl_conj_t x;
        double frac;
        time_t tca;
        if (o < 0) continue;
        jstr(&j, o, "TCA", t, sizeof(t));
        tca = sl_parse_time(t, &frac);
        if (!tca || tca <= now || tca > now + hours * 3600) continue;
        a = (int)jnum(&j, o, "SAT_1_ID", 0);
        b = (int)jnum(&j, o, "SAT_2_ID", 0);
        if (a <= 0 || b <= 0) continue;
        jstr(&j, o, "CREATED", c, sizeof(c));
        for (k = 0; k < n; k++)
            if ((out[k].id1 == a && out[k].id2 == b) || (out[k].id1 == b && out[k].id2 == a)) {
                slot = k; break;
            }
        if (slot >= 0 && strcmp(c, made[slot]) <= 0) continue;
        memset(&x, 0, sizeof(x));
        x.id1 = a; x.id2 = b;
        jstr(&j, o, "SAT_1_NAME", x.name1, sizeof(x.name1));
        jstr(&j, o, "SAT_2_NAME", x.name2, sizeof(x.name2));
        x.tca = tca; x.tca_frac = frac;
        x.rng_m = jnum(&j, o, "MIN_RNG", 0);
        x.pc = jnum(&j, o, "PC", 0);
        if (slot < 0) {
            if (n >= CDM_SCAN) continue;
            slot = n++;
        }
        out[slot] = x;
        snprintf(made[slot], sizeof(made[slot]), "%s", c);
    }
    pic_json_free(&j);
    for (i = 1; i < n; i++)
        for (k = i; k > 0 && out[k].tca < out[k - 1].tca; k--) {
            sl_conj_t t = out[k]; out[k] = out[k - 1]; out[k - 1] = t;
        }
    if (n > max) n = max;
    memcpy(res, out, sizeof(*out) * (size_t)n);
    free(out);
    return n;
}

/* ------------------------------------------------------------ debris */

#define GRP_SCAN 64

int sl_debris_groups(const char *js, int min_pieces, sl_group_t *out, int max)
{
    pic_json_t j;
    sl_group_t g[GRP_SCAN];
    int n = 0, i, k, cnt, a = 0;
    if (!js || pic_json_parse(&j, js, strlen(js)) != 0) return 0;
    if (!pic_json_is_array(&j, 0)) { pic_json_free(&j); return 0; }
    cnt = pic_json_size(&j, 0);
    for (i = 0; i < cnt; i++) {
        char oid[24], key[12], name[40], ctry[8], lau[16];
        int o = pic_json_at(&j, 0, i), id, slot = -1;
        if (o < 0) continue;
        jstr(&j, o, "OBJECT_ID", oid, sizeof(oid));
        if (strlen(oid) < 8) continue;
        snprintf(key, sizeof(key), "%.8s", oid);
        id = (int)jnum(&j, o, "NORAD_CAT_ID", 0);
        {   /* only pieces still in orbit */
            int dv = pic_json_get(&j, o, "DECAY");
            char dd[16];
            if (dv >= 0 && !pic_json_eq(&j, dv, "null") && pic_json_str(&j, dv, dd, sizeof(dd)) > 0)
                continue;
        }
        for (k = 0; k < n; k++)
            if (strcmp(g[k].key, key) == 0) { slot = k; break; }
        if (slot < 0) {
            char *d;
            if (n >= GRP_SCAN) continue;
            slot = n++;
            memset(&g[slot], 0, sizeof(g[slot]));
            snprintf(g[slot].key, sizeof(g[slot].key), "%s", key);
            jstr(&j, o, "OBJECT_NAME", name, sizeof(name));
            if ((d = strstr(name, " DEB")) != NULL) *d = 0;
            snprintf(g[slot].name, sizeof(g[slot].name), "%s", name);
            jstr(&j, o, "COUNTRY", ctry, sizeof(ctry));
            snprintf(g[slot].country, sizeof(g[slot].country), "%s", ctry);
            jstr(&j, o, "LAUNCH", lau, sizeof(lau));
            g[slot].year = atoi(lau);
        }
        g[slot].count++;
        if (id > g[slot].newest) g[slot].newest = id;
    }
    pic_json_free(&j);
    for (i = 0; i < n; i++)
        if (g[i].count >= min_pieces) g[a++] = g[i];
    n = a;
    for (i = 1; i < n; i++)
        for (k = i; k > 0 && g[k].newest > g[k - 1].newest; k--) {
            sl_group_t t = g[k]; g[k] = g[k - 1]; g[k - 1] = t;
        }
    if (n > max) n = max;
    memcpy(out, g, sizeof(g[0]) * (size_t)n);
    return n;
}

void sl_debris_ids(const char *js, const sl_group_t *g, int ngroups, char *list, size_t n)
{
    pic_json_t j;
    int i, k, cnt;
    size_t used = 0;
    list[0] = 0;
    if (!js || ngroups <= 0 || pic_json_parse(&j, js, strlen(js)) != 0) return;
    cnt = pic_json_is_array(&j, 0) ? pic_json_size(&j, 0) : 0;
    for (i = 0; i < cnt; i++) {
        char oid[24];
        int o = pic_json_at(&j, 0, i), id;
        if (o < 0) continue;
        jstr(&j, o, "OBJECT_ID", oid, sizeof(oid));
        {
            int dv = pic_json_get(&j, o, "DECAY");
            char dd[16];
            if (dv >= 0 && !pic_json_eq(&j, dv, "null") && pic_json_str(&j, dv, dd, sizeof(dd)) > 0)
                continue;
        }
        for (k = 0; k < ngroups; k++)
            if (strncmp(oid, g[k].key, 8) == 0) break;
        if (k == ngroups) continue;
        id = (int)jnum(&j, o, "NORAD_CAT_ID", 0);
        if (id <= 0) continue;
        if (used + 12 >= n) break;
        used += (size_t)snprintf(list + used, n - used, "%s%d", used ? "," : "", id);
    }
    pic_json_free(&j);
}

/* ------------------------------------------------------------ stats */

int sl_parse_stats(const char *box, const char *decay, time_t now, sl_stats_t *st)
{
    pic_json_t j;
    int i, cnt;
    memset(st, 0, sizeof(*st));
    if (box && pic_json_parse(&j, box, strlen(box)) == 0) {
        cnt = pic_json_is_array(&j, 0) ? pic_json_size(&j, 0) : 0;
        for (i = 0; i < cnt; i++) {
            char c[16];
            int o = pic_json_at(&j, 0, i);
            if (o < 0) continue;
            jstr(&j, o, "COUNTRY", c, sizeof(c));
            if (strcmp(c, "ALL") != 0) continue;
            st->total = (long)jnum(&j, o, "ORBITAL_TOTAL_COUNT", 0);
            st->payload = (long)jnum(&j, o, "ORBITAL_PAYLOAD_COUNT", 0);
            st->rocket = (long)jnum(&j, o, "ORBITAL_ROCKET_BODY_COUNT", 0);
            st->debris = (long)jnum(&j, o, "ORBITAL_DEBRIS_COUNT", 0);
            st->ok = st->total > 0;
        }
        pic_json_free(&j);
    }
    if (decay && pic_json_parse(&j, decay, strlen(decay)) == 0) {
        /* one prediction per object counts (the list can repeat one) */
        int seen[512], ns = 0, k;
        cnt = pic_json_is_array(&j, 0) ? pic_json_size(&j, 0) : 0;
        for (i = 0; i < cnt; i++) {
            char ep[32];
            int o = pic_json_at(&j, 0, i), id;
            time_t t;
            if (o < 0) continue;
            jstr(&j, o, "DECAY_EPOCH", ep, sizeof(ep));
            t = sl_parse_time(ep, NULL);
            if (!t || t < now || t > now + 7 * 86400) continue;
            id = (int)jnum(&j, o, "NORAD_CAT_ID", 0);
            for (k = 0; k < ns; k++) if (seen[k] == id) break;
            if (k < ns || ns >= 512) continue;
            seen[ns++] = id;
        }
        st->week = ns;
        pic_json_free(&j);
    }
    return st->ok;
}

/* ------------------------------------------------------------ geometry */

void sl_incl_track(double lat, double lon, double incl, int descending,
                   double alt_km, double tau_min, double *olat, double *olon)
{
    const double d2r = M_PI / 180.0, mu = 398600.4418, we = 7.2921159e-5;
    double i = incl * d2r, si = sin(i), u0, a, nm, node, u, s;
    if (fabs(si) < 1e-6) si = 1e-6;
    s = sin(lat * d2r) / si;
    if (s > 1) s = 1;
    if (s < -1) s = -1;
    u0 = asin(s);
    if (descending) u0 = M_PI - u0;
    a = 6378.137 + alt_km;
    nm = sqrt(mu / (a * a * a));
    node = lon * d2r - atan2(cos(i) * sin(u0), cos(u0));
    u = u0 + nm * tau_min * 60.0;
    *olat = asin(si * sin(u)) / d2r;
    *olon = wrap180((node + atan2(cos(i) * sin(u), cos(u)) - we * tau_min * 60.0) / d2r);
}

/* ------------------------------------------------------------ names */

void sl_tidy_name(const char *in, char *out, size_t n)
{
    size_t o = 0;
    const char *p = in;
    out[0] = 0;
    while (*p && o + 1 < n) {
        const char *w = p;
        size_t len, alpha = 0, k;
        char word[48];
        while (*p && *p != ' ') p++;
        len = (size_t)(p - w);
        if (len >= sizeof(word)) len = sizeof(word) - 1;
        memcpy(word, w, len);
        word[len] = 0;
        while (alpha < len && isalpha((unsigned char)word[alpha])) alpha++;
        if (strcmp(word, "DEB") == 0) snprintf(word, sizeof(word), "debris");
        else if (strcmp(word, "R/B") == 0) snprintf(word, sizeof(word), "rocket body");
        else if (alpha >= 5)      /* FENGYUN -> Fengyun, STARLINK-1 -> Starlink-1 */
            for (k = 1; k < alpha; k++) word[k] = (char)tolower((unsigned char)word[k]);
        o += (size_t)snprintf(out + o, n - o, "%s%s", o ? " " : "", word);
        if (o >= n) { out[n - 1] = 0; return; }
        while (*p == ' ') p++;
    }
}
