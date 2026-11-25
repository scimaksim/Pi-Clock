/*
 * applet_coast.c - Coastal buoy conditions (NOAA NDBC)
 *
 *   Coast
 *   Wrightsville (41110)   3.0 ft  9 s   71°F
 *     wind S 12 kt
 *   Diamond Shoals (41025) ...
 *
 * Latest observation from each buoy in COAST_BUOYS (default the
 * Masonboro Inlet / Wrightsville Beach, Diamond Shoals and Frying Pan
 * Shoals buoys off North Carolina), refreshed every 30 minutes.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE  /* timegm */
#include "feature.h"
#include "config.h"
#include "fetch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NDBC_FMT "https://www.ndbc.noaa.gov/data/realtime2/%s.txt"
#define MAX_BUOYS 4

typedef struct {
    char   id[8];
    int    ok;
    time_t t;
    double wvht_m, dpd_s, wtmp_c, wspd_ms, wdir;
} buoy_t;

static struct {
    pthread_mutex_t mutex;
    buoy_t b[MAX_BUOYS];
    int    n;
    int    metric;
} cs;

static const struct { const char *id, *name; } names[] = {
    { "41110", "Wrightsville" }, { "41025", "Diamond Shoals" },
    { "41013", "Frying Pan" }, { "41037", "Wrightsville offshore" },
    { "44014", "Virginia Beach" }, { "41004", "Edisto" },
    { "41008", "Grays Reef" }, { "41159", "Onslow Bay" },
};

static const char *buoy_name(const char *id)
{
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(names[i].id, id)) return names[i].name;
    return id;
}

/* Parse the newest line of an NDBC realtime2 standard met file */
int pic_coast_parse(const char *txt, buoy_t *b)
{
    const char *p = txt;
    char tok[20][12];
    int n = 0;
    struct tm tm;

    while (*p == '#') {             /* skip the two header lines */
        p = strchr(p, '\n');
        if (!p) return -1;
        p++;
    }
    while (n < 20) {
        int k = 0;
        while (*p == ' ') p++;
        if (!*p || *p == '\n') break;
        while (*p && *p != ' ' && *p != '\n' && k < 11) tok[n][k++] = *p++;
        tok[n][k] = '\0';
        while (*p && *p != ' ' && *p != '\n') p++;
        n++;
    }
    if (n < 15) return -1;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = atoi(tok[0]) - 1900;
    tm.tm_mon = atoi(tok[1]) - 1;
    tm.tm_mday = atoi(tok[2]);
    tm.tm_hour = atoi(tok[3]);
    tm.tm_min = atoi(tok[4]);
    b->t = timegm(&tm);
#define VAL(i) (strcmp(tok[i], "MM") ? atof(tok[i]) : NAN)
    b->wdir = VAL(5);
    b->wspd_ms = VAL(6);
    b->wvht_m = VAL(8);
    b->dpd_s = VAL(9);
    b->wtmp_c = VAL(14);
#undef VAL
    b->ok = 1;
    return 0;
}

static int coast_work(pic_feature_t *f)
{
    static buoy_t tmp[MAX_BUOYS];
    char url[128], *buf = malloc(64 * 1024);
    int i, n;

    (void)f;
    if (!buf) return 600;
    pthread_mutex_lock(&cs.mutex);
    n = cs.n;
    memcpy(tmp, cs.b, sizeof(tmp));
    pthread_mutex_unlock(&cs.mutex);
    for (i = 0; i < n; i++) {
        int len;
        snprintf(url, sizeof(url), NDBC_FMT, tmp[i].id);
        /* The file is ~200 KB; the newest line is first */
        len = pic_fetch_to_buf(url, buf, 64 * 1024 - 1);
        if (len <= 0) continue;
        buf[len] = '\0';
        pic_coast_parse(buf, &tmp[i]);
    }
    free(buf);
    pthread_mutex_lock(&cs.mutex);
    for (i = 0; i < n && i < cs.n; i++)
        if (!strcmp(cs.b[i].id, tmp[i].id)) cs.b[i] = tmp[i];
    pthread_mutex_unlock(&cs.mutex);
    return 30 * 60;
}

static void coast_reload(pic_feature_t *f)
{
    char list[64] = "", *p;
    int n = 0;
    buoy_t nb[MAX_BUOYS];

    if (!pic_conf_str("COAST_BUOYS", list, sizeof(list)) || !list[0])
        snprintf(list, sizeof(list), "41110,41025,41013");
    memset(nb, 0, sizeof(nb));
    for (p = strtok(list, ","); p && n < MAX_BUOYS; p = strtok(NULL, ",")) {
        size_t k;
        for (k = 0; p[k]; k++)
            if (!((p[k] >= '0' && p[k] <= '9') || (p[k] >= 'A' && p[k] <= 'Z') ||
                  (p[k] >= 'a' && p[k] <= 'z')))
                break;
        if (p[k] || k == 0 || k > 6) continue;
        snprintf(nb[n++].id, sizeof(nb[0].id), "%s", p);
    }
    pthread_mutex_lock(&cs.mutex);
    {
        int i, changed = n != cs.n;
        for (i = 0; i < n && !changed; i++) changed = strcmp(nb[i].id, cs.b[i].id) != 0;
        if (changed) {
            memcpy(cs.b, nb, sizeof(nb));
            cs.n = n;
            pic_feature_poke(f);
        }
    }
    cs.metric = pic_conf_int("COAST_METRIC", 0);
    pthread_mutex_unlock(&cs.mutex);
}

static const char *compass(double d)
{
    static const char *p[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return isnan(d) ? "" : p[(int)((d + 22.5) / 45.0) % 8];
}

double pic_applet_render_coast(cairo_t *cr, double width, time_t now, void *ud)
{
    double fs = width / 17.0, line_h = fs * 1.25, y;
    double total;
    int i, rows;

    (void)ud;
    pthread_mutex_lock(&cs.mutex);
    rows = cs.n ? cs.n : 1;
    total = pic_applet_title(NULL, width, "") + fs * 0.3 + rows * line_h * 2;
    if (!cr) {
        pthread_mutex_unlock(&cs.mutex);
        return total;
    }
    y = pic_applet_title(cr, width, "Coast") + fs * 0.3;
    for (i = 0; i < cs.n; i++) {
        const buoy_t *b = &cs.b[i];
        char l1[64], l2[96];
        cairo_text_extents_t e;
        int stale = !b->ok || now - b->t > 6 * 3600;

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgba(cr, 0.6, 0.85, 1.0, 0.95);
        cairo_move_to(cr, 0, y + fs);
        cairo_show_text(cr, buoy_name(b->id));
        {
            char wv[16] = "\xe2\x80\x94", wt[16] = "";
            if (b->ok && !isnan(b->wvht_m))
                snprintf(wv, sizeof(wv), cs.metric ? "%.1f m" : "%.1f ft",
                         cs.metric ? b->wvht_m : b->wvht_m * 3.28084);
            if (b->ok && !isnan(b->wtmp_c))
                snprintf(wt, sizeof(wt), cs.metric ? "  %.0f\xc2\xb0" "C" : "  %.0f\xc2\xb0" "F",
                         cs.metric ? b->wtmp_c : b->wtmp_c * 9 / 5 + 32);
            snprintf(l1, sizeof(l1), "%s%s", wv, wt);
        }
        cairo_text_extents(cr, l1, &e);
        cairo_set_source_rgba(cr, 0.92, 0.95, 1.0, stale ? 0.5 : 0.95);
        cairo_move_to(cr, width - e.x_advance, y + fs);
        cairo_show_text(cr, l1);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs * 0.85);
        if (b->ok) {
            char wind[32] = "", per[24] = "";
            if (!isnan(b->wspd_ms))
                snprintf(wind, sizeof(wind), "wind %s %.0f kt", compass(b->wdir),
                         b->wspd_ms * 1.94384);
            if (!isnan(b->dpd_s)) snprintf(per, sizeof(per), "waves every %.0f s", b->dpd_s);
            snprintf(l2, sizeof(l2), "%s%s%s%s", per, per[0] && wind[0] ? " \xc2\xb7 " : "",
                     wind, stale ? " (old)" : "");
        } else {
            snprintf(l2, sizeof(l2), "buoy %s: no data yet", b->id);
        }
        cairo_set_source_rgba(cr, 0.65, 0.7, 0.78, 0.9);
        cairo_move_to(cr, fs * 0.2, y + fs + line_h * 0.85);
        cairo_show_text(cr, l2);
        y += line_h * 2;
    }
    pthread_mutex_unlock(&cs.mutex);
    return total;
}

static pic_feature_t def = {
    .name = "coast", .is_applet = 1, .applet = pic_applet_render_coast,
    .label = "Coast", .interval = 60, .applet_w = 220,
    .side = APPLET_SIDE_RIGHT, .reload = coast_reload, .work = coast_work,
    .needs_network = 1,
};

pic_feature_t *pic_feature_coast(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&cs.mutex, NULL);
        inited = 1;
    }
    return &def;
}
