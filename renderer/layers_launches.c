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
#include "sat_icons.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include "nwsalert.h"   /* pic_nws_parse_time */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LL2_URL "https://ll.thespacedevs.com/2.3.0/launches/upcoming/?limit=20&hide_recent_previous=true"
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

static int launches_work(pic_feature_t *f)
{
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
    pthread_mutex_lock(&ll.mutex);
    memcpy(ll.items, tmp, sizeof(tmp));
    ll.count = count;
    ll.fetched = time(NULL);
    pthread_mutex_unlock(&ll.mutex);
    printf("launches: %d upcoming\n", count);
    return 30 * 60;
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
    return pic_dist_km(pic_config.qth_lat, pic_config.qth_lon,
                       L->lat, L->lon) < NEAR_KM;
}

void pic_layer_render_launches(cairo_t *cr, int width, int height,
                               time_t now, void *ud)
{
    double s = height / 70.0, fs = height / 80.0;
    double placed_x[MAX_LAUNCHES], placed_y[MAX_LAUNCHES];
    int i, k, nplaced = 0;

    (void)ud;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);

    pthread_mutex_lock(&ll.mutex);
    for (i = 0; i < ll.count; i++) {
        const launch_t *L = &ll.items[i];
        double x, y, lift = 0;
        int near, stacked = 0;
        char when[24], label[96];

        if (L->net < now - 3 * 3600 || L->net > now + (time_t)ll.days * 86400)
            continue;
        x = pic_lon_to_x(L->lon, width);
        y = pic_lat_to_y(L->lat, height);
        if (x < -s * 4 || x > width + s * 4) continue;

        /* Several launches from one site: only the soonest gets a
         * marker; later ones are listed under it. */
        for (k = 0; k < nplaced; k++)
            if (fabs(placed_x[k] - x) < s * 1.5 && fabs(placed_y[k] - y) < s * 1.5)
                stacked++;
        if (stacked >= 2) continue;
        if (nplaced < MAX_LAUNCHES) {
            placed_x[nplaced] = x;
            placed_y[nplaced++] = y;
        }
        lift = stacked * fs * 1.5;
        near = is_near(L);

        if (!stacked) {
            if (near) {
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
            /* Silhouette of the actual vehicle (Falcon 9, Starship,
             * Electron...), lit when it's launching within the hour */
            /* Real rockets are very slender; widen a little so the
             * shape still reads at map size */
            cairo_save(cr);
            cairo_translate(cr, x, y);
            cairo_scale(cr, 1.6, 1.0);
            pic_draw_launch_icon(cr, L->rocket, L->mission, 0, 0, s * 4.0,
                                 L->net <= now + 3600);
            cairo_restore(cr);
        }

        if (!ll.labels) continue;
        if (L->net <= now) snprintf(when, sizeof(when), "launching");
        else pic_fmt_until(when, sizeof(when), now, L->net);
        if (!L->precise && L->net > now + 86400) {
            char d[16];
            pic_fmt_when(d, sizeof(d), now, L->net);
            snprintf(when, sizeof(when), "NET %s", d);
        }
        snprintf(label, sizeof(label), "%s \xc2\xb7 %s", L->rocket, when);
        pic_draw_text_pill(cr, x + s * 0.9, y - s * 0.4 + lift, label,
                           near ? 1.0 : 0.95, near ? 0.88 : 0.95,
                           near ? 0.5 : 0.95, 0.6);
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
        snprintf(line, sizeof(line), "%s \xc2\xb7 %s", L->mission, L->pad);
        pic_fit_text(cr, line, width - fs * 1.4);
        cairo_set_source_rgba(cr, 0.65, 0.68, 0.75, 0.9);
        cairo_move_to(cr, fs * 1.4, y + fs + line_h * 0.85);
        cairo_show_text(cr, line);
        y += line_h * 2.0;
    }
    pthread_mutex_unlock(&ll.mutex);
    return pic_applet_title(NULL, width, "") + fs * 0.4 + rows * line_h * 2.0;
}

static pic_feature_t feature_def = {
    .name = "launches", .layer = pic_layer_render_launches,
    .interval = 30, .extra_mb = 1,
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
