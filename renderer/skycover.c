/*
 * skycover.c - Cloud cover over home, for "is it visible?" questions
 *
 * A satellite pass, or anything else in the sky, only counts as
 * visible when the sky at home is clear enough to see it. Two hourly
 * forecasts are used:
 *   1. Astrospheric (the Tonight's Sky panel), when it has a key and
 *      covers the time asked about: tuned for astronomy;
 *   2. Open-Meteo (no key) for home, fetched at most once an hour by
 *      whichever worker asks first, as a fallback.
 * Each hour's value applies to the hour starting at its time stamp;
 * a window is averaged over the hours it overlaps.
 *
 * PASS_CLOUD_MAX (renderer conf, default 50): the most cloud (%) at
 * which a pass is still called visible.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include "skycover.h"
#include "astro.h"
#include "config.h"
#include "feature.h"
#include "fetch.h"
#include "json.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OM_HOURS 120

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static time_t om_t[OM_HOURS];
static float om_c[OM_HOURS];
static int om_n;
static time_t om_fetched, om_tried;

/* Average of the hourly values overlapping [t0, t1]; -1 if none */
static int avg_hours(const time_t *t, const float *c, int n, time_t t0, time_t t1)
{
    double s = 0;
    int i, k = 0;
    if (t1 < t0) t1 = t0;
    for (i = 0; i < n; i++)
        if (t[i] <= t1 && t[i] + 3600 > t0 && c[i] >= 0) {
            s += c[i];
            k++;
        }
    return k ? (int)(s / k + 0.5) : -1;
}

static int astro_cloud(time_t t0, time_t t1)
{
    pic_astro_t *a = pic_astro_shared;
    time_t t[ASTRO_MAX_HOURS];
    float c[ASTRO_MAX_HOURS];
    int i, n;
    if (!a) return -1;
    pthread_mutex_lock(&a->mutex);
    n = a->nhours < ASTRO_MAX_HOURS ? a->nhours : ASTRO_MAX_HOURS;
    for (i = 0; i < n; i++) {
        t[i] = a->hours[i].t;
        c[i] = a->hours[i].cloud;
    }
    pthread_mutex_unlock(&a->mutex);
    return avg_hours(t, c, n, t0, t1);
}

int pic_sky_cloud(time_t t0, time_t t1)
{
    int c = astro_cloud(t0, t1);
    if (c >= 0) return c;
    pthread_mutex_lock(&mu);
    c = avg_hours(om_t, om_c, om_n, t0, t1);
    pthread_mutex_unlock(&mu);
    return c;
}

/* PASS_CLOUD_MAX, re-read at most every 30 s (it is asked per frame) */
static int cloud_max(void)
{
    static int max = 50;
    static time_t read_at;
    time_t now = time(NULL);
    int v;
    pthread_mutex_lock(&mu);
    if (now - read_at < 30 && read_at) {
        v = max;
        pthread_mutex_unlock(&mu);
        return v;
    }
    read_at = now;
    pthread_mutex_unlock(&mu);
    v = pic_conf_int("PASS_CLOUD_MAX", 50);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    pthread_mutex_lock(&mu);
    max = v;
    pthread_mutex_unlock(&mu);
    return v;
}

int pic_sky_clear(time_t t0, time_t t1, int *pct)
{
    int c = pic_sky_cloud(t0, t1), max = cloud_max();
    if (pct) *pct = c;
    if (c < 0) return PIC_SKY_UNKNOWN;
    return c <= max ? PIC_SKY_CLEAR : PIC_SKY_CLOUDY;
}

void pic_skycover_set(time_t t0, const int *cloud, int n)
{
    int i;
    pthread_mutex_lock(&mu);
    if (n > OM_HOURS) n = OM_HOURS;
    for (i = 0; i < n; i++) {
        om_t[i] = t0 + (time_t)i * 3600;
        om_c[i] = (float)cloud[i];
    }
    om_n = n;
    pthread_mutex_unlock(&mu);
}

int pic_skycover_parse(const char *js, size_t len, time_t now)
{
    pic_json_t j;
    int hr, ts, cc, k, n = 0;
    time_t t[OM_HOURS];
    float c[OM_HOURS];

    if (pic_json_parse(&j, js, len) != 0) return 0;
    hr = pic_json_get(&j, 0, "hourly");
    ts = pic_json_get(&j, hr, "time");
    cc = pic_json_get(&j, hr, "cloud_cover");
    for (k = 0; ts >= 0 && cc >= 0 && k < pic_json_size(&j, ts) && n < OM_HOURS; k++) {
        char s[24];
        struct tm tm;
        int v;
        memset(&tm, 0, sizeof(tm));
        pic_json_str(&j, pic_json_at(&j, ts, k), s, sizeof(s));
        if (sscanf(s, "%d-%d-%dT%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min) != 5)
            continue;
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        v = pic_json_at(&j, cc, k);
        if (v < 0) continue;
        t[n] = timegm(&tm);
        c[n] = (float)pic_json_num(&j, v, -1);   /* null -> -1 */
        if (t[n] + 3600 > now - 3600) n++;       /* drop the past */
    }
    pic_json_free(&j);
    if (n) {
        pthread_mutex_lock(&mu);
        memcpy(om_t, t, (size_t)n * sizeof(t[0]));
        memcpy(om_c, c, (size_t)n * sizeof(c[0]));
        om_n = n;
        pthread_mutex_unlock(&mu);
    }
    return n;
}

void pic_skycover_refresh(time_t now)
{
    static pthread_mutex_t fetching = PTHREAD_MUTEX_INITIALIZER;
    char url[256], *buf;
    int len;

    if (pthread_mutex_trylock(&fetching) != 0) return;   /* another thread is on it */
    pthread_mutex_lock(&mu);
    if (now - om_fetched < 3600 || now - om_tried < 600) {
        pthread_mutex_unlock(&mu);
        pthread_mutex_unlock(&fetching);
        return;
    }
    om_tried = now;
    pthread_mutex_unlock(&mu);

    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
             "&hourly=cloud_cover&forecast_days=4&timezone=GMT",
             pic_config.qth_lat, pic_config.qth_lon);
    buf = malloc(64 * 1024);
    if (buf) {
        len = pic_fetch_to_buf(url, buf, 64 * 1024 - 1);
        if (len > 0) {
            buf[len] = 0;
            if (pic_skycover_parse(buf, (size_t)len, now) > 0) {
                pthread_mutex_lock(&mu);
                om_fetched = now;
                pthread_mutex_unlock(&mu);
            }
        }
        free(buf);
    }
    pthread_mutex_unlock(&fetching);
}
