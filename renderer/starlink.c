/*
 * starlink.c - Starlink train finder and visible-pass predictor
 *
 * See starlink.h. TLE line 1 columns 10-17 hold the international
 * designator "YYNNNPPP" (launch year, launch number, piece); objects
 * sharing YYNNN came up on the same rocket.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "starlink.h"
#include "astro.h"
#include "config.h"
#include "fetch.h"
#include "math_utils.h"
#include "solar.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* CelesTrak asks clients not to re-download the same data more often
 * than every couple of hours; 4 h keeps trains current. */
#define SL_URL "https://celestrak.org/NORAD/elements/gp.php?GROUP=last-30-days&FORMAT=tle"
#define SL_CACHE "/tmp/pi-clock-last30.tle"
#define FETCH_INTERVAL (4 * 3600)
/* CelesTrak blocks addresses that ask too often (their guidance: the
 * same data at most once every 2 hours), so failures back off to 2 h */
#define RETRY_INTERVAL (2 * 3600)
/* Pass predictions are refreshed more often than the TLEs */
#define PREDICT_INTERVAL (15 * 60)

#define EARTH_R 6371.0
#define MIN_EL  10.0      /* degrees above horizon to count as visible */
#define OBS_SUN_MAX (-6.0)/* observer needs at least civil-twilight dark */

static pthread_t sl_thread;
static volatile int sl_running = 0;
static pic_starlink_t *cfg_data = NULL;

void pic_sl_init(pic_starlink_t *data)
{
    memset(data, 0, sizeof(*data));
    data->max_age_days = 10;
    pthread_mutex_init(&data->mutex, NULL);
}

/* ── Geometry ─────────────────────────────────────────────────── */

static void ecef(double lat, double lon, double r, double v[3])
{
    double la = lat * DEG_TO_RAD, lo = lon * DEG_TO_RAD;
    v[0] = r * cos(la) * cos(lo);
    v[1] = r * cos(la) * sin(lo);
    v[2] = r * sin(la);
}

double pic_sl_elevation(const pic_sat_t *s, time_t t,
                        double qth_lat, double qth_lon, int *sunlit)
{
    double lat, lon, alt, p[3], o[3], rel[3], up[3], d, el;
    int i;

    pic_sat_position_at(s, t, &lat, &lon, &alt);
    ecef(lat, lon, EARTH_R + alt, p);
    ecef(qth_lat, qth_lon, EARTH_R, o);

    for (i = 0; i < 3; i++) { rel[i] = p[i] - o[i]; up[i] = o[i] / EARTH_R; }
    d = sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
    el = asin((rel[0] * up[0] + rel[1] * up[1] + rel[2] * up[2]) / d) * RAD_TO_DEG;

    if (sunlit) {
        /* Sun direction: sub-solar point is (dec, -GHA). Satellite is in
         * shadow if behind Earth (p.s < 0) and inside the shadow
         * cylinder (distance from the Sun-Earth axis < R). */
        pic_solar_position_t sp = pic_solar_position(t);
        double sv[3], ps, perp2;
        ecef(sp.declination * RAD_TO_DEG, -sp.gha * RAD_TO_DEG, 1.0, sv);
        ps = p[0] * sv[0] + p[1] * sv[1] + p[2] * sv[2];
        perp2 = (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - ps * ps;
        *sunlit = !(ps < 0 && perp2 < EARTH_R * EARTH_R);
    }
    return el;
}

void pic_sl_predict_pass(pic_sl_train_t *t, time_t t0, int hours,
                         double qth_lat, double qth_lon)
{
    const int step = 30;
    time_t tt, end = t0 + (time_t)hours * 3600;
    const pic_sat_t *lead;
    int in_pass = 0;

    t->pass_start = t->pass_max = t->pass_end = 0;
    t->pass_max_el = 0;
    if (t->nsats == 0) return;
    lead = &t->sats[0];

    for (tt = t0; tt < end; tt += step) {
        int sunlit, vis;
        double el = pic_sl_elevation(lead, tt, qth_lat, qth_lon, &sunlit);

        vis = el >= MIN_EL && sunlit &&
              pic_astro_sun_alt(tt, qth_lat, qth_lon) < OBS_SUN_MAX;

        if (vis) {
            if (!in_pass) { in_pass = 1; t->pass_start = tt; }
            if (el > t->pass_max_el) { t->pass_max_el = el; t->pass_max = tt; }
            t->pass_end = tt;
        } else if (in_pass) {
            return;                  /* first visible pass found */
        }
    }
}

/* ── Catalogue → trains ───────────────────────────────────────── */

static time_t epoch_unix(const pic_sat_t *s)
{
    return (time_t)((s->epoch_jd - 2440587.5) * 86400.0);
}

/* Along-track angle now, used to order satellites within a train */
static double along_track(const pic_sat_t *s, time_t t)
{
    double dt_min = difftime(t, epoch_unix(s)) / 60.0;
    double n = s->mean_motion * 2 * PI / 1440.0;
    return fmod(s->mean_anomaly + s->arg_perigee + n * dt_min, 2 * PI);
}

/* Sorting context: time and the train's circular-mean angle, so a
 * train straddling the 0/2pi seam still sorts head to tail. */
static time_t g_sort_t;
static double g_sort_ref;

static double rel_angle(const pic_sat_t *s)
{
    double d = along_track(s, g_sort_t) - g_sort_ref;
    while (d > PI)  d -= 2 * PI;
    while (d < -PI) d += 2 * PI;
    return d;
}

static int cmp_along(const void *a, const void *b)
{
    double x = rel_angle((const pic_sat_t *)a);
    double y = rel_angle((const pic_sat_t *)b);
    return (x < y) ? 1 : (x > y) ? -1 : 0;   /* leader first */
}

static void sort_train(pic_sl_train_t *t, time_t now)
{
    double sx = 0, sy = 0;
    int k;
    g_sort_t = now;
    for (k = 0; k < t->nsats; k++) {
        double a = along_track(&t->sats[k], now);
        sx += cos(a);
        sy += sin(a);
    }
    g_sort_ref = atan2(sy, sx);
    qsort(t->sats, (size_t)t->nsats, sizeof(pic_sat_t), cmp_along);
}

/*
 * next_tle - Read the next 3-line entry from *pp. Returns 0 at end.
 */
static int next_tle(const char **pp, char *l0, char *l1, char *l2)
{
    char *lines[3];
    int k;

    lines[0] = l0; lines[1] = l1; lines[2] = l2;
    for (k = 0; k < 3; k++) {
        const char *p = *pp, *e;
        size_t len;
        if (!*p) return 0;
        e = strchr(p, '\n');
        len = e ? (size_t)(e - p) : strlen(p);
        if (len >= 80) len = 79;
        memcpy(lines[k], p, len);
        lines[k][len] = '\0';
        if (len && lines[k][len - 1] == '\r') lines[k][len - 1] = '\0';
        *pp = e ? e + 1 : p + len;
    }
    return 1;
}

int pic_sl_build_trains(const char *text, pic_sl_train_t *out, int max,
                        time_t now, int max_age_days)
{
    /* Pass 1: list launches (designator, earliest epoch, count) */
    enum { MAX_LAUNCHES = 64 };
    struct { char key[6]; time_t launched; int n; } L[MAX_LAUNCHES];
    int nl = 0, i, j, nout = 0;
    const char *p = text;
    char l0[80], l1[80], l2[80];

    while (next_tle(&p, l0, l1, l2)) {
        pic_sat_t s;
        char key[6];
        time_t ep;

        if (strncmp(l0, "STARLINK", 8) != 0 || l1[0] != '1' || l2[0] != '2')
            continue;
        memset(&s, 0, sizeof(s));
        if (pic_sat_parse_tle(&s, l1, l2) != 0) continue;
        memcpy(key, l1 + 9, 5);
        key[5] = '\0';
        ep = epoch_unix(&s);

        for (j = 0; j < nl && strcmp(L[j].key, key) != 0; j++) ;
        if (j == nl) {
            if (nl == MAX_LAUNCHES) continue;
            memcpy(L[nl].key, key, 6);
            L[nl].launched = ep;
            L[nl].n = 0;
            nl++;
        }
        if (ep < L[j].launched) L[j].launched = ep;
        L[j].n++;
    }

    /* Newest launches first */
    for (i = 0; i < nl; i++)
        for (j = i + 1; j < nl; j++)
            if (L[j].launched > L[i].launched) {
                char k[6]; time_t t; int n;
                memcpy(k, L[i].key, 6); t = L[i].launched; n = L[i].n;
                memcpy(L[i].key, L[j].key, 6); L[i].launched = L[j].launched; L[i].n = L[j].n;
                memcpy(L[j].key, k, 6); L[j].launched = t; L[j].n = n;
            }

    /* Pass 2: load satellites for the chosen launches */
    for (i = 0; i < nl && nout < max; i++) {
        pic_sl_train_t *t = &out[nout];

        if (difftime(now, L[i].launched) > max_age_days * 86400.0) continue;
        if (L[i].n < 3) continue;          /* a few pieces, not a train */

        memset(t, 0, sizeof(*t));
        snprintf(t->launch, sizeof(t->launch), "20%.2s-%.3s",
                 L[i].key, L[i].key + 2);
        t->launched = L[i].launched;

        p = text;
        while (next_tle(&p, l0, l1, l2) && t->nsats < SL_MAX_PER_TRAIN) {
            pic_sat_t *s = &t->sats[t->nsats];
            if (strncmp(l0, "STARLINK", 8) != 0 || l1[0] != '1') continue;
            if (strncmp(l1 + 9, L[i].key, 5) != 0) continue;
            memset(s, 0, sizeof(*s));
            if (pic_sat_parse_tle(s, l1, l2) != 0) continue;
            snprintf(s->name, sizeof(s->name), "%.31s", l0);
            s->norad_id = atoi(l1 + 2);
            s->enabled = 1;
            t->nsats++;
        }

        sort_train(t, now);
        nout++;
    }
    return nout;
}

/* ── Thread ───────────────────────────────────────────────────── */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    long n;
    char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    if (n <= 0 || n > 8 * 1024 * 1024) { fclose(f); return NULL; }
    b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = '\0';
    fclose(f);
    return b;
}

static void *sl_thread_func(void *arg)
{
    static pic_sl_train_t temp[SL_MAX_TRAINS];
    time_t next_fetch = 0;
    (void)arg;

    if (!pic_wait_for_network("starlink", &sl_running))
        return NULL;

    while (sl_running) {
        time_t now = time(NULL);
        int i, n, age;

        if (now >= next_fetch) {
            struct stat st;
            /* A renderer restart reuses a recent download rather than
             * asking CelesTrak again (it survives until reboot) */
            int cached = next_fetch == 0 && stat(SL_CACHE, &st) == 0 &&
                         st.st_size > 1000 && now - st.st_mtime < FETCH_INTERVAL;
            /* Slow TLS on single-core Pis: allow a generous timeout.
             * Download beside the cache so a failure keeps the old copy. */
            if (cached || (pic_fetch_to_file(SL_URL, SL_CACHE ".new", 60) == 0 &&
                           rename(SL_CACHE ".new", SL_CACHE) == 0)) {
                char *text = read_file(SL_CACHE);
                /* Don't come straight back if the file can't be read */
                next_fetch = now + RETRY_INTERVAL;
                pthread_mutex_lock(&cfg_data->mutex);
                age = cfg_data->max_age_days;
                pthread_mutex_unlock(&cfg_data->mutex);
                if (text) {
                    n = pic_sl_build_trains(text, temp, SL_MAX_TRAINS, now, age);
                    free(text);
                    pthread_mutex_lock(&cfg_data->mutex);
                    memcpy(cfg_data->trains, temp, (size_t)n * sizeof(temp[0]));
                    cfg_data->ntrains = n;
                    cfg_data->fetched = now;
                    pthread_mutex_unlock(&cfg_data->mutex);
                    printf("starlink: %d recent train(s)%s\n", n,
                           cached ? " (cached TLEs)" : "");
                    next_fetch = (cached ? st.st_mtime : now) + FETCH_INTERVAL;
                }
            } else {
                unlink(SL_CACHE ".new");
                printf("starlink: CelesTrak fetch failed, retry in 2 h\n");
                next_fetch = now + RETRY_INTERVAL;
            }
        }

        /* Refresh pass predictions on copies, outside the lock */
        pthread_mutex_lock(&cfg_data->mutex);
        n = cfg_data->ntrains;
        memcpy(temp, cfg_data->trains, (size_t)n * sizeof(temp[0]));
        pthread_mutex_unlock(&cfg_data->mutex);
        for (i = 0; i < n && sl_running; i++)
            pic_sl_predict_pass(&temp[i], now, 48,
                                pic_config.qth_lat, pic_config.qth_lon);
        pthread_mutex_lock(&cfg_data->mutex);
        for (i = 0; i < n && i < cfg_data->ntrains; i++) {
            cfg_data->trains[i].pass_start  = temp[i].pass_start;
            cfg_data->trains[i].pass_max    = temp[i].pass_max;
            cfg_data->trains[i].pass_end    = temp[i].pass_end;
            cfg_data->trains[i].pass_max_el = temp[i].pass_max_el;
        }
        pthread_mutex_unlock(&cfg_data->mutex);

        for (i = 0; i < PREDICT_INTERVAL && sl_running; i++)
            sleep(1);
    }
    return NULL;
}

int pic_sl_start(pic_starlink_t *data)
{
    if (sl_running) return 0;
    cfg_data = data;
    pic_sl_reload(data);
    sl_running = 1;
    if (pthread_create(&sl_thread, NULL, sl_thread_func, NULL) != 0) {
        sl_running = 0;
        return -1;
    }
    printf("starlink: thread started\n");
    return 0;
}

void pic_sl_stop(void)
{
    if (!sl_running) return;
    sl_running = 0;
    pthread_join(sl_thread, NULL);
    cfg_data = NULL;
}

void pic_sl_reload(pic_starlink_t *data)
{
    FILE *f;
    char line[128];
    int age = 10;

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f))
            if (strncmp(line, "STARLINK_MAX_AGE_D=", 19) == 0)
                age = atoi(line + 19);
        fclose(f);
    }
    if (age < 1) age = 1;
    if (age > 30) age = 30;
    pthread_mutex_lock(&data->mutex);
    data->max_age_days = age;
    pthread_mutex_unlock(&data->mutex);
}
