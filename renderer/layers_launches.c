/*
 * layers_launches.c - Upcoming rocket launches
 *
 * Map layer: a rocket marker at each pad with a launch in the next
 * LAUNCH_DAYS days (default 1), labelled with the rocket and a
 * countdown. Pads within ~1100 km of the home QTH (Cape Canaveral
 * and Wallops from North Carolina) get a highlight ring, since those
 * launches can often be seen from home.
 *
 * Applet "Launches": the next few launches with local time, rocket,
 * mission and pad, nearby launches marked with a star.
 *
 * Data: The Space Devs Launch Library 2 (free tier, ~15 req/hour)
 *   https://ll.thespacedevs.com/2.3.0/launches/upcoming/
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "labels.h"
#include "sat_icons.h"
#include "config.h"
#include "fetch.h"
#include "spacetrack.h"
#include "json.h"
#include "nwsalert.h"   /* pic_nws_parse_time */
#include "launchvis.h"
#include "features_all.h"
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LL2_URL "https://ll.thespacedevs.com/2.3.0/launches/upcoming/?limit=20&hide_recent_previous=true"
#define LL2_PREV_URL "https://ll.thespacedevs.com/2.3.0/launches/previous/?limit=40"
#define INTDES_URL "https://celestrak.org/NORAD/elements/gp.php?INTDES=%s&FORMAT=tle"
#define LEARN_PATH "/data/cache/pi-clock-launch-inclinations.txt"
#define LEARN_DONE_PATH "/data/cache/pi-clock-launch-learned.txt"
#define LEARN_STAMP "/data/cache/pi-clock-launch-learn.stamp"
#define LL2_MAX_RESPONSE (1024 * 1024)
#define MAX_LAUNCHES 20
#define NEAR_KM 1100.0

typedef struct {
    time_t net;
    double lat, lon;
    char   status[16];    /* Go / TBC / TBD / Hold / In Flight ... */
    char   rocket[40];
    char   mission[64];
    char   pad[48];       /* Short location, e.g. "Cape Canaveral"  */
    char   provider[40];
    int    precise;       /* NET known to the minute/second         */
    char   orbit[16];     /* Target orbit, LL2 abbreviation (LEO...) */
    int    near;          /* Pad within NEAR_KM of home              */
    pic_launchvis_t vis;  /* Visibility from home (near pads)        */
} launch_t;

typedef struct {
    pthread_mutex_t mutex;
    launch_t items[MAX_LAUNCHES];
    int      count;
    int      days;
    int      labels;
    time_t   fetched;
} launches_t;

static launches_t ll;

/* "Cape Canaveral SFS, FL, USA" → "Cape Canaveral SFS, FL" */
static void short_location(char *s)
{
    char *c = strstr(s, ", USA");
    if (c) *c = '\0';
    c = strstr(s, ", People's Republic of China");
    if (c) *c = '\0';
    c = strstr(s, " Satellite Launch Center");
    if (c) memmove(c, c + 24, strlen(c + 24) + 1);
}

int pic_launches_parse(const char *js, size_t len, launch_t *out, int max)
{
    pic_json_t j;
    int res, n, i, count = 0;

    if (pic_json_parse(&j, js, len) < 0) return -1;
    res = pic_json_get(&j, 0, "results");
    if (res < 0) {            /* e.g. rate-limit reply: keep the old list */
        pic_json_free(&j);
        return -1;
    }
    n = pic_json_size(&j, res);
    for (i = 0; i < n && count < max; i++) {
        int r = pic_json_at(&j, res, i);
        int pad = pic_json_get(&j, r, "pad");
        launch_t L;
        char buf[64];

        memset(&L, 0, sizeof(L));
        pic_json_str(&j, pic_json_get(&j, r, "net"), buf, sizeof(buf));
        L.net = pic_nws_parse_time(buf);
        if (!L.net) continue;
        L.lat = pic_json_num(&j, pic_json_get(&j, pad, "latitude"), 999);
        L.lon = pic_json_num(&j, pic_json_get(&j, pad, "longitude"), 999);
        if (fabs(L.lat) > 90 || fabs(L.lon) > 180) continue;
        pic_json_str(&j, pic_json_path(&j, r, "status.abbrev"), L.status, sizeof(L.status));
        pic_json_str(&j, pic_json_path(&j, r, "rocket.configuration.name"), L.rocket, sizeof(L.rocket));
        pic_json_str(&j, pic_json_path(&j, r, "mission.name"), L.mission, sizeof(L.mission));
        if (!L.mission[0]) {
            /* "Falcon 9 Block 5 | Starlink Group 15-18" */
            char name[128], *bar;
            pic_json_str(&j, pic_json_get(&j, r, "name"), name, sizeof(name));
            bar = strstr(name, " | ");
            snprintf(L.mission, sizeof(L.mission), "%s", bar ? bar + 3 : name);
        }
        pic_json_str(&j, pic_json_path(&j, pad, "location.name"), L.pad, sizeof(L.pad));
        short_location(L.pad);
        pic_json_str(&j, pic_json_path(&j, r, "launch_service_provider.name"), L.provider, sizeof(L.provider));
        pic_json_str(&j, pic_json_path(&j, r, "mission.orbit.abbrev"), L.orbit, sizeof(L.orbit));
        pic_json_str(&j, pic_json_path(&j, r, "net_precision.abbrev"), buf, sizeof(buf));
        L.precise = (strcmp(buf, "SEC") == 0 || strcmp(buf, "MIN") == 0);
        /* Launched or scrubbed for good — not "upcoming" any more */
        if (!strcmp(L.status, "Success") || !strcmp(L.status, "Failure") ||
            !strcmp(L.status, "Deployed") || !strcmp(L.status, "Partial F"))
            continue;
        out[count++] = L;
    }
    pic_json_free(&j);
    return count;
}

/* ── Learning real inclinations from past launches ────────────────
 * Once a day: the last 40 launches; for each one from a nearby pad
 * that has a COSPAR designator (LL2 adds it a few days after launch)
 * and hasn't been learned yet, the orbits of its objects are looked up
 * (CelesTrak by designator, cached on disk; else the satellite lists
 * already on the Pi) and the median inclination recorded for the
 * launch's family. The visibility predictor then flies that family in
 * the direction it really went. */

static int designator_done(const char *des)
{
    FILE *f = fopen(LEARN_DONE_PATH, "r");
    char line[64];
    int found = 0;
    if (!f) return 0;
    while (!found && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        found = !strcmp(line, des);
    }
    fclose(f);
    return found;
}

static void designator_mark(const char *des)
{
    FILE *f = fopen(LEARN_DONE_PATH, "a");
    if (f) { fprintf(f, "%s\n", des); fclose(f); }
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Median inclination of the objects of launch des ("2026-237") from a
 * TLE file; objects counted in *n. Designator in line 1 is "26237A". */
static double file_inclination(const char *path, const char *des, int *n)
{
    FILE *f = fopen(path, "r");
    char key[8], l1[200], l2[200];
    double inc[64];
    int k = 0;
    *n = 0;
    if (!f || strlen(des) < 8) { if (f) fclose(f); return -1; }
    snprintf(key, sizeof(key), "%.2s%.3s", des + 2, des + 5);
    while (fgets(l1, sizeof(l1), f)) {
        if (l1[0] != '1' || l1[1] != ' ' || strncmp(l1 + 9, key, 5)) continue;
        if (!fgets(l2, sizeof(l2), f) || l2[0] != '2') break;
        if (k < 64) inc[k++] = atof(l2 + 8);
    }
    fclose(f);
    if (!k) return -1;
    qsort(inc, (size_t)k, sizeof(inc[0]), cmp_double);
    *n = k;
    return inc[k / 2];
}

/* CelesTrak requests one learning run may make (the rest wait a day) */
static int intdes_budget;

static double launch_inclination(const char *des, int *n)
{
    static const char *lists[] = { "/tmp/pi-clock-last30.tle", "/tmp/pi-clock-cache/visual.tle",
                                   "/tmp/pi-clock-cache/satnogs.tle", "/tmp/pi-clock-cache/amsat.tle", NULL };
    char path[128], url[200];
    double inc;
    int i;
    /* CelesTrak by designator: a one-off request per launch, kept */
    snprintf(path, sizeof(path), "/tmp/pi-clock-cache/intdes-%s.tle", des);
    if (access(path, R_OK) != 0 && intdes_budget > 0) {
        intdes_budget--;
        snprintf(url, sizeof(url), INTDES_URL, des);
        if (pic_fetch_to_file(url, path, 30) != 0) {
            char q[200];
            unlink(path);
            snprintf(q, sizeof(q), ST_Q_INTDES, des);
            if (pic_spacetrack_fetch(q, path) != 0) unlink(path);
        }
    }
    inc = file_inclination(path, des, n);
    if (*n) return inc;
    for (i = 0; lists[i]; i++) {
        inc = file_inclination(lists[i], des, n);
        if (*n) return inc;
    }
    return -1;
}

static void learn_from_past(void)
{
    char *buf = malloc(LL2_MAX_RESPONSE);
    pic_json_t j;
    int res, n, i, learned = 0;
    time_t now = time(NULL);

    if (!buf) return;
    n = pic_fetch_to_buf(LL2_PREV_URL, buf, LL2_MAX_RESPONSE - 1);
    if (n <= 0 || pic_json_parse(&j, buf, (size_t)n) < 0) { free(buf); return; }
    res = pic_json_get(&j, 0, "results");
    n = res >= 0 ? pic_json_size(&j, res) : 0;
    for (i = 0; i < n; i++) {
        int r = pic_json_at(&j, res, i), pad = pic_json_get(&j, r, "pad"), objects = 0;
        char des[16], net[32], rocket[40], mission[64], family[64];
        double lat, lon, inc;
        time_t t;
        pic_json_str(&j, pic_json_get(&j, r, "launch_designator"), des, sizeof(des));
        if (strlen(des) < 8) continue;
        pic_json_str(&j, pic_json_get(&j, r, "net"), net, sizeof(net));
        t = pic_nws_parse_time(net);
        if (!t || now - t > 120 * 86400 || now - t < 3 * 86400) continue;
        lat = pic_json_num(&j, pic_json_get(&j, pad, "latitude"), 999);
        lon = pic_json_num(&j, pic_json_get(&j, pad, "longitude"), 999);
        if (pic_dist_km(pic_config.qth_lat, pic_config.qth_lon, lat, lon) > NEAR_KM) continue;
        if (designator_done(des)) continue;
        pic_json_str(&j, pic_json_path(&j, r, "rocket.configuration.name"), rocket, sizeof(rocket));
        pic_json_str(&j, pic_json_path(&j, r, "mission.name"), mission, sizeof(mission));
        inc = launch_inclination(des, &objects);
        if (objects > 0) {
            pic_launchvis_family(rocket, mission, family, sizeof(family));
            pic_launchvis_learn(family, inc, objects);
            printf("launches: learned %s flies %.1f deg (%s, %d objects)\n", family, inc, des, objects);
            learned++;
        }
        /* Done either way: objects not yet catalogued won't appear later
         * in these lists for a launch this old */
        if (objects > 0 || now - t > 30 * 86400) designator_mark(des);
    }
    pic_json_free(&j);
    free(buf);
    if (learned) pic_launchvis_save(LEARN_PATH);
}

static int launches_work(pic_feature_t *f)
{
    static time_t last_learn;
    static int loaded;
    if (!loaded) {
        int k = pic_launchvis_load(LEARN_PATH);
        if (k) printf("launches: %d launch families with learned inclinations\n", k);
        loaded = 1;
    }
    /* Once a day, remembered across restarts (a restart used to start
     * a fresh burst of CelesTrak requests) */
    {
        struct stat st;
        if (!last_learn && stat(LEARN_STAMP, &st) == 0) last_learn = st.st_mtime;
    }
    if (time(NULL) - last_learn > 86400) {
        FILE *sf;
        last_learn = time(NULL);
        if ((sf = fopen(LEARN_STAMP, "w")) != NULL) fclose(sf);
        intdes_budget = 4;
        learn_from_past();
    }
    static launch_t tmp[MAX_LAUNCHES];
    char *buf = malloc(LL2_MAX_RESPONSE);
    int n, count;

    (void)f;
    if (!buf) return 600;
    n = pic_fetch_to_buf(LL2_URL, buf, LL2_MAX_RESPONSE - 1);
    if (n <= 0) {
        free(buf);
        printf("launches: fetch failed\n");
        return 15 * 60;   /* Stay well inside the 15/hour limit */
    }
    buf[n] = '\0';
    count = pic_launches_parse(buf, (size_t)n, tmp, MAX_LAUNCHES);
    free(buf);
    if (count < 0) {
        printf("launches: parse failed (rate limited?)\n");
        return 30 * 60;
    }
    {
        /* Visibility from home for launches from nearby pads, and a
         * faster refresh (status, slips, liftoff) around their launch */
        time_t now = time(NULL);
        int i, soon = 0;
        for (i = 0; i < count; i++) {
            launch_t *L = &tmp[i];
            L->near = pic_dist_km(pic_config.qth_lat, pic_config.qth_lon,
                                  L->lat, L->lon) < NEAR_KM;
            if (L->near) {
                pic_launchvis_predict(L->lat, L->lon, L->orbit, L->mission, L->rocket,
                                      L->net, pic_config.qth_lat, pic_config.qth_lon,
                                      &L->vis);
                if (L->net > now - 1800 && L->net < now + 3600) soon = 1;
            }
        }
        pthread_mutex_lock(&ll.mutex);
        memcpy(ll.items, tmp, sizeof(tmp));
        ll.count = count;
        ll.fetched = now;
        pthread_mutex_unlock(&ll.mutex);
        printf("launches: %d upcoming%s\n", count, soon ? " (nearby launch soon: 5 min refresh)" : "");
        /* LL2 free tier allows ~15 requests an hour */
        return soon ? 5 * 60 : 30 * 60;
    }
}

static void launches_reload(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&ll.mutex);
    ll.days = pic_conf_int("LAUNCH_DAYS", 1);
    if (ll.days < 1) ll.days = 1;
    ll.labels = pic_conf_int("LAUNCH_LABELS", 1);
    pthread_mutex_unlock(&ll.mutex);
}

static int is_near(const launch_t *L)
{
    return L->near;
}

/* Short visibility note for a nearby launch: "Visible +3–9 min · look
 * S → E · up to 15°"; NULL when there is nothing useful to say */
static const char *vis_note(const launch_t *L, char *out, size_t n, int *good)
{
    const pic_launchvis_t *v = &L->vis;
    *good = 0;
    if (!L->near || !v->known) return NULL;
    if (v->visible) {
        *good = 1;
        snprintf(out, n, "%s +%d\xe2\x80\x93%d min \xc2\xb7 look %s\xe2\x86\x92%s \xc2\xb7 %d\xc2\xb0%s",
                 v->sunlit ? "Sunlit plume" : "Visible",
                 (v->t_start + 30) / 60, (v->t_end + 30) / 60,
                 pic_launchvis_compass(v->az_start), pic_launchvis_compass(v->az_end),
                 (int)(v->max_el + 0.5), "");
    } else if (v->daytime) {
        snprintf(out, n, "Daylight here \xe2\x80\x94 not visible from home");
    } else {
        snprintf(out, n, "Too low to see from home");
    }
    return out;
}

/* Text of one launch's map label */
static void launch_label(const launch_t *L, time_t now, char *label, size_t n)
{
    char when[24];
    if (L->net <= now) snprintf(when, sizeof(when), "launching");
    else pic_fmt_until(when, sizeof(when), now, L->net);
    if (!L->precise && L->net > now + 86400) {
        char d[16];
        pic_fmt_when(d, sizeof(d), now, L->net);
        snprintf(when, sizeof(when), "NET %s", d);
    }
    snprintf(label, n, "%s \xc2\xb7 %s", L->rocket, when);
}

/*
 * Predicted ascent of a nearby launch, from 30 min before liftoff until
 * second-stage cut-off: aqua where it can be seen from home, grey
 * dashes elsewhere, a tick each minute and a "+N min" label where it
 * comes into view. Once it is in flight (or past its time while still
 * "Go"), a marker shows where it should be now.
 */
static void draw_ascent(cairo_t *cr, int width, int height, const launch_t *L,
                        time_t now, double s, double fs)
{
    const pic_launchvis_t *v = &L->vis;
    double wrap = width / 3.0, px = 0, py = 0, lw = s * 0.32;
    int t, prev_vis = 0, labelled = 0;

    for (t = 0; t <= PIC_LV_T_END; t += 10) {
        double la, lo, h, x, y, el;
        int vis;
        pic_launchvis_position(L->lat, L->lon, v->az_launch, t, &la, &lo, &h);
        x = pic_lon_to_x(lo, width);
        y = pic_lat_to_y(la, height);
        el = pic_launchvis_look(pic_config.qth_lat, pic_config.qth_lon, la, lo, h, NULL);
        vis = v->visible && t >= v->t_start && t <= v->t_end && el >= PIC_LV_MIN_EL;
        if (t > 0 && fabs(x - px) < wrap) {
            cairo_new_path(cr);
            cairo_move_to(cr, px, py);
            cairo_line_to(cr, x, y);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
            cairo_set_line_width(cr, lw * (vis ? 3.2 : 2.4));
            cairo_stroke_preserve(cr);
            if (vis) {
                cairo_set_source_rgba(cr, 0.3, 1.0, 0.92, 0.95);
                cairo_set_line_width(cr, lw * 1.8);
            } else {
                double dash[2] = { s * 0.6, s * 0.45 };
                cairo_set_dash(cr, dash, 2, 0);
                cairo_set_source_rgba(cr, 0.8, 0.82, 0.86, 0.75);
                cairo_set_line_width(cr, lw);
            }
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);
        }
        if (t > 0 && t % 60 == 0) {                    /* minute ticks */
            cairo_new_path(cr);
            cairo_arc(cr, x, y, lw * (vis ? 1.6 : 1.1), 0, 2 * M_PI);
            cairo_set_source_rgba(cr, vis ? 0.3 : 0.85, vis ? 1.0 : 0.87, vis ? 0.92 : 0.9, 0.95);
            cairo_fill(cr);
        }
        if (vis && !prev_vis && !labelled && x > 0 && x < width && y > 0 && y < height) {
            char lab[48];
            snprintf(lab, sizeof(lab), "%s from home +%d min",
                     v->sunlit ? "Sunlit plume" : "Visible", (t + 30) / 60);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, fs * 0.85);
            pic_draw_text_pill(cr, x + s * 0.6, y + fs * 0.3, lab, 0.3, 1.0, 0.92, 0.6);
            labelled = 1;
        }
        prev_vis = vis;
        px = x; py = y;
    }

    /* Where it should be now */
    if (now >= L->net && now <= L->net + PIC_LV_T_END &&
        (!strcmp(L->status, "In Flight") || !strcmp(L->status, "Go"))) {
        double la, lo, h, x, y;
        char lab[64];
        pic_launchvis_position(L->lat, L->lon, v->az_launch, (double)(now - L->net), &la, &lo, &h);
        x = pic_lon_to_x(lo, width);
        y = pic_lat_to_y(la, height);
        cairo_new_path(cr);
        cairo_arc(cr, x, y, s * 0.7, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 0.75, 0.3, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
        cairo_set_line_width(cr, lw);
        cairo_stroke(cr);
        snprintf(lab, sizeof(lab), "%s T+%ld:%02ld%s", L->rocket, (long)(now - L->net) / 60,
                 (long)(now - L->net) % 60,
                 strcmp(L->status, "In Flight") ? " (if on time)" : "");
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs * 0.85);
        pic_draw_text_pill(cr, x + s * 1.0, y - s * 0.6, lab, 1.0, 0.85, 0.5, 0.6);
    }
}

/* Label boxes placed in the last frame (for tests) */
static double last_label[MAX_LAUNCHES][4];
static int last_nlabel;

/* Does r overlap any of rs[0..n) other than rs[skip]? */
static int rect_hits(const double r[4], double (*rs)[4], int n, int skip)
{
    int i;
    for (i = 0; i < n; i++)
        if (i != skip && !(r[2] < rs[i][0] || r[0] > rs[i][2] || r[3] < rs[i][1] || r[1] > rs[i][3]))
            return 1;
    return 0;
}

/*
 * Launches within LAUNCH_DAYS: one rocket per launch site (the soonest
 * launch there), labelled with up to two launches and "+N more". Label
 * blocks go right of the rocket, or left / below when that would hit
 * another label or rocket or run off screen; with no room, only the
 * rocket is drawn.
 */
void pic_layer_render_launches(cairo_t *cr, int width, int height,
                               time_t now, void *ud)
{
    double s = height / 70.0, fs = height / 80.0, H = s * 4.0;
    struct { double x, y; int first, n, idx[MAX_LAUNCHES]; } site[MAX_LAUNCHES];
    double rects[MAX_LAUNCHES * 2][4];
    int nsite = 0, nrect = 0, i, k, lbl = pic_labels_owner("launches");

    (void)ud;
    pic_labels_begin(lbl);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);

    pthread_mutex_lock(&ll.mutex);
    /* Group the launches by site */
    for (i = 0; i < ll.count; i++) {
        const launch_t *L = &ll.items[i];
        double x, y;
        if (L->net < now - 3 * 3600 || L->net > now + (time_t)ll.days * 86400)
            continue;
        x = pic_lon_to_x(L->lon, width);
        y = pic_lat_to_y(L->lat, height);
        if (x < -s * 4 || x > width + s * 4 || y < -H || y > height + H) continue;
        for (k = 0; k < nsite; k++)
            if (fabs(site[k].x - x) < s * 1.5 && fabs(site[k].y - y) < s * 1.5) break;
        if (k == nsite) {
            site[k].x = x; site[k].y = y; site[k].first = i; site[k].n = 0;
            nsite++;
        }
        site[k].idx[site[k].n++] = i;
    }

    /* Predicted ascents of nearby launches around launch time */
    for (i = 0; i < ll.count; i++) {
        const launch_t *L = &ll.items[i];
        if (L->near && L->vis.known && now >= L->net - 1800 &&
            now <= L->net + PIC_LV_T_END + 60)
            draw_ascent(cr, width, height, L, now, s, fs);
    }
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);

    /* Rockets first; their outlines are obstacles for the labels */
    for (k = 0; k < nsite; k++) {
        const launch_t *L = &ll.items[site[k].first];
        double x = site[k].x, y = site[k].y;
        if (is_near(L)) {
            cairo_pattern_t *p;
            cairo_new_path(cr);
            p = cairo_pattern_create_radial(x, y, 0, x, y, s * 3);
            cairo_pattern_add_color_stop_rgba(p, 0, 1.0, 0.85, 0.3, 0.6);
            cairo_pattern_add_color_stop_rgba(p, 1, 1.0, 0.85, 0.3, 0.0);
            cairo_set_source(cr, p);
            cairo_arc(cr, x, y, s * 3, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_pattern_destroy(p);
        }
        /* Real rockets are very slender; widen a little so the shape
         * still reads at map size. Lit when launching within the hour */
        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, 1.6, 1.0);
        pic_draw_launch_icon(cr, L->rocket, L->mission, 0, 0, H, L->net <= now + 3600);
        cairo_restore(cr);
        rects[nrect][0] = x - s * 0.8; rects[nrect][1] = y - H * 1.12;
        rects[nrect][2] = x + s * 0.8; rects[nrect][3] = y + s * 0.3;
        pic_labels_claim(lbl, rects[nrect]);         /* other layers' labels keep off the rocket */
        nrect++;
    }

    last_nlabel = 0;
    if (ll.labels) {
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        for (k = 0; k < nsite; k++) {
            char lines[3][96];
            int nl = 0, j, pos, near = is_near(&ll.items[site[k].first]);
            double w = 0, lh = fs * 1.5, x = site[k].x, y = site[k].y, bx = 0, by = 0, r[4];

            for (j = 0; j < site[k].n && nl < 2; j++)
                launch_label(&ll.items[site[k].idx[j]], now, lines[nl++], sizeof(lines[0]));
            if (site[k].n > 2)
                snprintf(lines[nl++], sizeof(lines[0]), "+%d more", site[k].n - 2);
            for (j = 0; j < nl; j++) {
                cairo_text_extents_t e;
                cairo_text_extents(cr, lines[j], &e);
                if (e.x_advance > w) w = e.x_advance;
            }
            /* Candidate spots: right, left, below-right, then above and
             * below-left; clear of this layer's rockets and labels and
             * of every other layer's labels */
            for (pos = 0; pos < 5; pos++) {
                if (pos == 0)      { bx = x + s * 0.9;          by = y - s * 0.4; }
                else if (pos == 1) { bx = x - s * 0.9 - w;      by = y - s * 0.4; }
                else if (pos == 2) { bx = x + s * 0.9;          by = y + fs * 1.6; }
                else if (pos == 3) { bx = x + s * 0.9;          by = y - H * 1.12 - lh * (nl - 1); }
                else               { bx = x - s * 0.9 - w;      by = y + fs * 1.6; }
                r[0] = bx - fs * 0.4; r[1] = by - fe.ascent - fs * 0.3;
                r[2] = bx + w + fs * 0.4; r[3] = by + lh * (nl - 1) + fe.descent + fs * 0.3;
                if (r[0] < 0 || r[2] > width || r[1] < 0 || r[3] > height) continue;
                if (!rect_hits(r, rects, nrect, k) && pic_labels_free(lbl, r)) break;   /* rect k: own rocket */
            }
            if (pos == 5) continue;                 /* no room: rocket only */
            memcpy(rects[nrect++], r, sizeof(r));
            pic_labels_claim(lbl, r);
            if (last_nlabel < MAX_LAUNCHES) memcpy(last_label[last_nlabel++], r, sizeof(r));
            for (j = 0; j < nl; j++) {
                int more = j == nl - 1 && site[k].n > 2;
                pic_draw_text_pill(cr, bx, by + lh * j, lines[j],
                                   more ? 0.75 : near ? 1.0 : 0.95,
                                   more ? 0.78 : near ? 0.88 : 0.95,
                                   more ? 0.82 : near ? 0.5 : 0.95, 0.6);
            }
        }
    }
    pthread_mutex_unlock(&ll.mutex);
}

double pic_applet_render_launches(cairo_t *cr, double width, time_t now,
                                  void *ud)
{
    double fs = width / 17.0, y, line_h = fs * 1.3;
    int i, shown = 0, max_rows = 5, rows = 0;

    (void)ud;
    pthread_mutex_lock(&ll.mutex);
    for (i = 0; i < ll.count; i++)
        if (ll.items[i].net > now - 3600) rows++;
    pthread_mutex_unlock(&ll.mutex);
    if (rows > max_rows) rows = max_rows;
    if (rows == 0) rows = 1;
    if (!cr) return pic_applet_title(NULL, width, "") + fs * 0.4 + rows * line_h * 2.0;

    y = pic_applet_title(cr, width, "Launches") + fs * 0.4;
    cairo_set_font_size(cr, fs);

    pthread_mutex_lock(&ll.mutex);
    if (ll.count == 0) {
        cairo_set_source_rgba(cr, 0.6, 0.6, 0.65, 0.9);
        cairo_move_to(cr, 0, y + fs * 0.6);
        cairo_show_text(cr, ll.fetched ? "No launches scheduled" : "Loading\xe2\x80\xa6");
    }
    for (i = 0; i < ll.count && shown < rows; i++) {
        const launch_t *L = &ll.items[i];
        char when[48], line[160];
        int near = is_near(L);
        int go = !strcmp(L->status, "Go");

        if (L->net < now - 3600) continue;
        shown++;
        pic_fmt_when(when, sizeof(when), now, L->net);
        if (!L->precise) {
            char t[48];
            snprintf(t, sizeof(t), "NET %s", when);
            snprintf(when, sizeof(when), "%s", t);
        }

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        snprintf(line, sizeof(line), "%s%s  %s", near ? "\xe2\x98\x85 " : "",
                 when, L->rocket);
        pic_fit_text(cr, line, width - fs * 3.4);
        cairo_save(cr);                  /* widened like the map icon */
        cairo_translate(cr, fs * 0.45, y + line_h * 1.75);
        cairo_scale(cr, 1.7, 1.0);
        pic_draw_launch_icon(cr, L->rocket, L->mission, 0, 0, line_h * 1.75, 0);
        cairo_restore(cr);
        if (near) cairo_set_source_rgba(cr, 1.0, 0.85, 0.4, 0.95);
        else      cairo_set_source_rgba(cr, 0.9, 0.9, 0.92, 0.95);
        cairo_move_to(cr, fs * 1.2, y + fs);
        cairo_show_text(cr, line);

        /* Status chip at the right */
        cairo_set_font_size(cr, fs * 0.75);
        {
            cairo_text_extents_t e;
            cairo_text_extents(cr, L->status, &e);
            if (go) cairo_set_source_rgba(cr, 0.4, 0.9, 0.5, 0.95);
            else    cairo_set_source_rgba(cr, 0.7, 0.7, 0.75, 0.9);
            cairo_move_to(cr, width - e.x_advance, y + fs);
            cairo_show_text(cr, L->status);
        }

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs * 0.85);
        {
            /* Visible from home: say when and where to look instead of
             * the pad (the star and the gold already say it's nearby) */
            char note[96];
            int good;
            if (vis_note(L, note, sizeof(note), &good) && good) {
                snprintf(line, sizeof(line), "%s", note);
                cairo_set_source_rgba(cr, 0.3, 1.0, 0.92, 0.95);
            } else {
                snprintf(line, sizeof(line), "%s \xc2\xb7 %s", L->mission, L->pad);
                cairo_set_source_rgba(cr, 0.65, 0.68, 0.75, 0.9);
            }
        }
        pic_fit_text(cr, line, width - fs * 1.4);
        cairo_move_to(cr, fs * 1.4, y + fs + line_h * 0.85);
        cairo_show_text(cr, line);
        y += line_h * 2.0;
    }
    pthread_mutex_unlock(&ll.mutex);
    return pic_applet_title(NULL, width, "") + fs * 0.4 + rows * line_h * 2.0;
}

int pic_launches_visible(pic_launch_alert_t *out, int max)
{
    int i, n = 0;
    pthread_mutex_lock(&ll.mutex);
    for (i = 0; i < ll.count && n < max; i++) {
        const launch_t *L = &ll.items[i];
        if (!L->near || !L->vis.visible) continue;
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].rocket, sizeof(out[n].rocket), "%s", L->rocket);
        snprintf(out[n].mission, sizeof(out[n].mission), "%s", L->mission);
        snprintf(out[n].pad, sizeof(out[n].pad), "%s", L->pad);
        snprintf(out[n].status, sizeof(out[n].status), "%s", L->status);
        out[n].net = L->net;
        out[n].precise = L->precise;
        out[n].vis = L->vis;
        n++;
    }
    pthread_mutex_unlock(&ll.mutex);
    return n;
}

/* Keep fetching for Home Assistant launch alerts even with the layer
 * and applet off */
static int launches_wanted(pic_feature_t *f)
{
    (void)f;
    return pic_hanotify_wants("HA_NOTIFY_LAUNCHES");
}

static pic_feature_t feature_def = {
    .name = "launches", .wanted = launches_wanted, .layer = pic_layer_render_launches,
    /* 5 s: countdowns and the live ascent marker during a launch */
    .interval = 5, .extra_mb = 1,
    .applet = pic_applet_render_launches, .label = "Rocket Launches",
    .applet_w = 220, .side = APPLET_SIDE_RIGHT,
    .reload = launches_reload, .work = launches_work, .needs_network = 1,
};

pic_feature_t *pic_feature_launches(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&ll.mutex, NULL);
        ll.days = 7;
        ll.labels = 1;
        inited = 1;
    }
    return &feature_def;
}
