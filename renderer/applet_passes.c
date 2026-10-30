/*
 * applet_passes.c - Visible passes of bright satellites
 *
 * Lists the next naked-eye passes over the home QTH for the space
 * stations (ISS, Tiangong) and Hubble: when, how long, peak
 * elevation and direction of travel. A pass counts as visible when
 * the satellite is at least 10° up, sunlit, and the observer's sun is
 * below -6° (civil twilight or darker) — the same rule the Starlink
 * train layer uses.
 *
 * TLEs: CelesTrak GP API (one request per satellite, every 12 h).
 * Satellites: PASS_SATS=25544,48274,20580 (NORAD IDs, max 6).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "astro.h"
#include "config.h"
#include "fetch.h"
#include "math_utils.h"
#include "satellite.h"
#include "solar.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PSATS   6
#define MAX_PASSES  24
#define EARTH_R     6371.0
#define MIN_EL      10.0
#define OBS_SUN_MAX (-6.0)
#define HORIZON_H   72
#define STEP_S      10

typedef struct {
    char   sat[16];
    time_t start, max, end;
    double max_el;
    double az_start, az_end;
} pass_t;

typedef struct {
    pthread_mutex_t mutex;
    pass_t  passes[MAX_PASSES];
    int     npasses;
    int     ids[MAX_PSATS];
    int     nids;
    time_t  computed;
    int     have_tle;
} passes_t;

static passes_t ps;

static const struct { int id; const char *name; } known[] = {
    { 25544, "ISS" }, { 48274, "Tiangong" }, { 20580, "Hubble" },
    { 33591, "NOAA 19" }, { 25338, "NOAA 15" }, { 43013, "NOAA 20" },
};

static void sat_name(int id, char *out, int n)
{
    size_t i;
    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (known[i].id == id) {
            snprintf(out, (size_t)n, "%s", known[i].name);
            return;
        }
    snprintf(out, (size_t)n, "#%d", id);
}

static void ecef(double lat, double lon, double r, double v[3])
{
    double la = lat * DEG_TO_RAD, lo = lon * DEG_TO_RAD;
    v[0] = r * cos(la) * cos(lo);
    v[1] = r * cos(la) * sin(lo);
    v[2] = r * sin(la);
}

/* Elevation and azimuth (degrees) of a satellite from the QTH. */
static double look_angles(const pic_sat_t *s, time_t t, double qlat,
                          double qlon, double *az, int *sunlit)
{
    double lat, lon, alt, p[3], o[3], rel[3], d, e, n, u;
    double la = qlat * DEG_TO_RAD, lo = qlon * DEG_TO_RAD;
    int i;

    pic_sat_position_at(s, t, &lat, &lon, &alt);
    ecef(lat, lon, EARTH_R + alt, p);
    ecef(qlat, qlon, EARTH_R, o);
    for (i = 0; i < 3; i++) rel[i] = p[i] - o[i];
    d = sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);

    /* Local east / north / up */
    e = -sin(lo) * rel[0] + cos(lo) * rel[1];
    n = -sin(la) * cos(lo) * rel[0] - sin(la) * sin(lo) * rel[1] + cos(la) * rel[2];
    u = cos(la) * cos(lo) * rel[0] + cos(la) * sin(lo) * rel[1] + sin(la) * rel[2];
    if (az) {
        *az = atan2(e, n) * RAD_TO_DEG;
        if (*az < 0) *az += 360.0;
    }
    if (sunlit) {
        pic_solar_position_t sp = pic_solar_position(t);
        double sv[3], pdot, perp2;
        ecef(sp.declination * RAD_TO_DEG, -sp.gha * RAD_TO_DEG, 1.0, sv);
        pdot = p[0] * sv[0] + p[1] * sv[1] + p[2] * sv[2];
        perp2 = (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - pdot * pdot;
        *sunlit = !(pdot < 0 && perp2 < EARTH_R * EARTH_R);
    }
    return asin(u / d) * RAD_TO_DEG;
}

/* Find visible passes of one satellite in [t0, t0 + hours). */
int pic_passes_predict(const pic_sat_t *s, const char *name, time_t t0,
                       int hours, double qlat, double qlon,
                       pass_t *out, int max)
{
    time_t t, end = t0 + (time_t)hours * 3600;
    int n = 0, in = 0;
    pass_t cur;

    memset(&cur, 0, sizeof(cur));
    for (t = t0; t < end && n < max; t += STEP_S) {
        int lit = 0, vis;
        double az, el = look_angles(s, t, qlat, qlon, &az, NULL);

        if (el < MIN_EL) {
            vis = 0;
        } else {
            look_angles(s, t, qlat, qlon, NULL, &lit);
            vis = lit && pic_astro_sun_alt(t, qlat, qlon) < OBS_SUN_MAX;
        }
        if (vis) {
            if (!in) {
                memset(&cur, 0, sizeof(cur));
                snprintf(cur.sat, sizeof(cur.sat), "%s", name);
                cur.start = t;
                cur.az_start = az;
                in = 1;
            }
            if (el > cur.max_el) { cur.max_el = el; cur.max = t; }
            cur.end = t;
            cur.az_end = az;
        } else if (in) {
            in = 0;
            if (cur.end - cur.start >= 30) out[n++] = cur;
        }
        /* Below the horizon: skip ahead faster */
        if (el < -15) t += 50;
    }
    if (in && n < max && cur.end - cur.start >= 30) out[n++] = cur;
    return n;
}

/*
 * One satellite's TLE: CelesTrak first, then the tle.ivanstanojevic.me
 * API (CelesTrak throttles bursts of requests).
 */
static int fetch_tle(int id, char *l1, char *l2)
{
    char url[128], buf[1536], *a, *b;
    int len;

    snprintf(url, sizeof(url),
             "https://celestrak.org/NORAD/elements/gp.php?CATNR=%d&FORMAT=TLE", id);
    len = pic_fetch_to_buf(url, buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        a = strstr(buf, "\n1 ");
        b = strstr(buf, "\n2 ");
        if (a && b) {
            snprintf(l1, 96, "%.*s", (int)strcspn(a + 1, "\r\n"), a + 1);
            snprintf(l2, 96, "%.*s", (int)strcspn(b + 1, "\r\n"), b + 1);
            return 0;
        }
    }
    snprintf(url, sizeof(url), "https://tle.ivanstanojevic.me/api/tle/%d", id);
    len = pic_fetch_to_buf(url, buf, sizeof(buf) - 1);
    if (len <= 0) return -1;
    buf[len] = '\0';
    a = strstr(buf, "\"line1\":\"");
    b = strstr(buf, "\"line2\":\"");
    if (!a || !b) return -1;
    a += 9;
    b += 9;
    snprintf(l1, 96, "%.*s", (int)strcspn(a, "\""), a);
    snprintf(l2, 96, "%.*s", (int)strcspn(b, "\""), b);
    return 0;
}

static int cmp_pass(const void *a, const void *b)
{
    const pass_t *x = a, *y = b;
    return (x->start > y->start) - (x->start < y->start);
}

static int passes_work(pic_feature_t *f)
{
    static pic_sat_t sats[MAX_PSATS];
    static int loaded[MAX_PSATS];
    static time_t tle_time;
    static pass_t tmp[MAX_PASSES * 2];
    int ids[MAX_PSATS], nids, i, n = 0;
    time_t now = time(NULL);
    double qlat = pic_config.qth_lat, qlon = pic_config.qth_lon;

    (void)f;
    pthread_mutex_lock(&ps.mutex);
    nids = ps.nids;
    memcpy(ids, ps.ids, sizeof(ids));
    pthread_mutex_unlock(&ps.mutex);

    /* Refresh TLEs every 12 h (or when the list changed) */
    for (i = 0; i < nids; i++)
        if (!loaded[i] || sats[i].norad_id != ids[i]) tle_time = 0;
    if (now - tle_time > 12 * 3600) {
        int ok = 0;
        for (i = 0; i < nids; i++) {
            char l1[96], l2[96];
            if (sats[i].norad_id != ids[i]) loaded[i] = 0;
            if (fetch_tle(ids[i], l1, l2) != 0) continue;
            memset(&sats[i], 0, sizeof(sats[i]));
            if (pic_sat_parse_tle(&sats[i], l1, l2) == 0) {
                sats[i].norad_id = ids[i];
                loaded[i] = 1;
                ok++;
            }
        }
        if (ok) tle_time = now;
        printf("passes: loaded %d/%d TLEs\n", ok, nids);
        if (!ok) return 15 * 60;
    }

    for (i = 0; i < nids; i++) {
        char name[16];
        if (!loaded[i]) continue;
        sat_name(ids[i], name, sizeof(name));
        n += pic_passes_predict(&sats[i], name, now - 600, HORIZON_H,
                                qlat, qlon, tmp + n, MAX_PASSES * 2 - n);
    }
    qsort(tmp, (size_t)n, sizeof(pass_t), cmp_pass);
    if (n > MAX_PASSES) n = MAX_PASSES;

    pthread_mutex_lock(&ps.mutex);
    memcpy(ps.passes, tmp, (size_t)n * sizeof(pass_t));
    ps.npasses = n;
    ps.computed = now;
    ps.have_tle = 1;
    pthread_mutex_unlock(&ps.mutex);
    printf("passes: %d visible passes in %dh\n", n, HORIZON_H);
    return 30 * 60;
}

static void passes_reload(pic_feature_t *f)
{
    char buf[128], *p;
    int ids[MAX_PSATS], n = 0;

    if (!pic_conf_str("PASS_SATS", buf, sizeof(buf)) || !buf[0])
        snprintf(buf, sizeof(buf), "25544,48274,20580");
    for (p = buf; *p && n < MAX_PSATS; ) {
        int id = atoi(p);
        if (id > 0) ids[n++] = id;
        p += strcspn(p, ",");
        if (*p) p++;
    }
    pthread_mutex_lock(&ps.mutex);
    if (n != ps.nids || memcmp(ids, ps.ids, (size_t)n * sizeof(int)) != 0) {
        memcpy(ps.ids, ids, sizeof(ids));
        ps.nids = n;
        pic_feature_poke(f);
    }
    pthread_mutex_unlock(&ps.mutex);
}

static const char *compass(double az)
{
    static const char *pts[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return pts[(int)((az + 22.5) / 45.0) % 8];
}

double pic_applet_render_passes(cairo_t *cr, double width, time_t now,
                                void *ud)
{
    double fs = width / 17.0, line_h = fs * 1.25, y;
    int i, shown = 0, rows = 4;
    double total = pic_applet_title(NULL, width, "") + fs * 0.3 +
                   rows * line_h * 2.0;

    (void)ud;
    if (!cr) return total;

    y = pic_applet_title(cr, width, "Visible Passes") + fs * 0.3;

    pthread_mutex_lock(&ps.mutex);
    for (i = 0; i < ps.npasses && shown < rows; i++) {
        const pass_t *p = &ps.passes[i];
        char when[24], line[96];
        int live = now >= p->start && now <= p->end;
        cairo_text_extents_t e;

        if (p->end < now) continue;
        shown++;

        if (live) snprintf(when, sizeof(when), "visible now");
        else pic_fmt_when(when, sizeof(when), now, p->start);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        /* Live passes green; high (bright) passes gold */
        if (live)                 cairo_set_source_rgba(cr, 0.5, 1.0, 0.6, 1.0);
        else if (p->max_el >= 40) cairo_set_source_rgba(cr, 1.0, 0.9, 0.5, 0.95);
        else                      cairo_set_source_rgba(cr, 0.9, 0.9, 0.92, 0.9);
        cairo_move_to(cr, 0, y + fs);
        cairo_show_text(cr, p->sat);
        cairo_text_extents(cr, when, &e);
        cairo_move_to(cr, width - e.x_advance, y + fs);
        cairo_show_text(cr, when);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs * 0.85);
        snprintf(line, sizeof(line),
                 "%d min \xc2\xb7 max %d\xc2\xb0 \xc2\xb7 %s \xe2\x86\x92 %s",
                 (int)((p->end - p->start + 30) / 60), (int)(p->max_el + 0.5),
                 compass(p->az_start), compass(p->az_end));
        cairo_set_source_rgba(cr, 0.65, 0.7, 0.78, 0.9);
        cairo_move_to(cr, fs * 0.2, y + fs + line_h * 0.85);
        cairo_show_text(cr, line);
        y += line_h * 2.0;
    }
    if (shown == 0) {
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgba(cr, 0.6, 0.6, 0.65, 0.9);
        cairo_move_to(cr, 0, y + fs);
        cairo_show_text(cr, !ps.have_tle ? "Loading\xe2\x80\xa6" :
                        "No visible passes in 3 days");
    }
    pthread_mutex_unlock(&ps.mutex);
    return total;
}

static pic_feature_t feature_def = {
    .name = "passes", .is_applet = 1,
    .applet = pic_applet_render_passes, .label = "Visible Passes",
    .interval = 30, .applet_w = 220, .side = APPLET_SIDE_LEFT,
    .reload = passes_reload, .work = passes_work, .needs_network = 1,
};

pic_feature_t *pic_feature_passes(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&ps.mutex, NULL);
        inited = 1;
    }
    return &feature_def;
}
