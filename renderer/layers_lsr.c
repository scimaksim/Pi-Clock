/*
 * layers_lsr.c - NWS Local Storm Reports (last 24 hours)
 *
 * Tornado, hail, wind damage, flooding and other reports filed by
 * spotters, emergency managers and the public, via the Iowa
 * Environmental Mesonet's national LSR feed. Each report is a small
 * symbol that fades with age; in the local view it is labelled with
 * the type and magnitude (e.g. "Hail 1.75 in").
 *
 * Settings: LSR_HOURS=6|12|24|48 (default 24), LSR_FOG=0|1 (fog and
 * routine non-thunderstorm wind gusts are hidden by default).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include "nwsalert.h"   /* pic_nws_parse_time */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LSR_URL "https://mesonet.agron.iastate.edu/geojson/lsr.php?inc_ap=1&hours=%d"
#define MAX_LSR 1500

typedef struct {
    float  lat, lon, mag;
    char   type;          /* IEM single-letter code */
    time_t t;
    char   city[32], unit[8];
    char   text[20];      /* the report's own type wording, e.g. "Waterspout" */
} lsr_t;

static struct {
    pthread_mutex_t mutex;
    lsr_t  r[MAX_LSR];
    int    n, hours, show_minor;
} ls;

int pic_lsr_parse(const char *js, size_t len, lsr_t *out, int max)
{
    pic_json_t j;
    int feats, n, i, c = 0;
    if (pic_json_parse_max(&j, js, len, 400000) < 0) return -1;
    feats = pic_json_get(&j, 0, "features");
    n = pic_json_size(&j, feats);
    for (i = 0; i < n && c < max; i++) {
        int p = pic_json_get(&j, pic_json_at(&j, feats, i), "properties");
        char ty[4] = "", t[32] = "";
        lsr_t r;
        memset(&r, 0, sizeof(r));
        pic_json_str(&j, pic_json_get(&j, p, "type"), ty, sizeof(ty));
        pic_json_str(&j, pic_json_get(&j, p, "valid"), t, sizeof(t));
        r.type = ty[0];
        {   /* NWS's own name for the event, as "Tornado" not "TORNADO" */
            char tt[40] = "";
            size_t q;
            pic_json_str(&j, pic_json_get(&j, p, "typetext"), tt, sizeof(tt));
            for (q = 0; tt[q] && q + 1 < sizeof(r.text); q++)
                r.text[q] = (char)(q ? tolower((unsigned char)tt[q]) : toupper((unsigned char)tt[q]));
        }
        r.lat = (float)pic_json_num(&j, pic_json_get(&j, p, "lat"), 999);
        r.lon = (float)pic_json_num(&j, pic_json_get(&j, p, "lon"), 999);
        r.mag = (float)pic_json_num(&j, pic_json_get(&j, p, "magf"), 0);
        pic_json_str(&j, pic_json_get(&j, p, "city"), r.city, sizeof(r.city));
        pic_json_str(&j, pic_json_get(&j, p, "unit"), r.unit, sizeof(r.unit));
        /* "2026-10-30T21:05:00Z" (IEM may omit the Z) */
        if (t[0] && !strchr(t, 'Z') && strlen(t) < sizeof(t) - 1) strcat(t, "Z");
        r.t = pic_nws_parse_time(t);
        if (!r.type || fabsf(r.lat) > 90 || fabsf(r.lon) > 180) continue;
        out[c++] = r;
    }
    pic_json_free(&j);
    return c;
}

static int lsr_work(pic_feature_t *f)
{
    static lsr_t tmp[MAX_LSR];
    char url[160], *buf = malloc(4 * 1024 * 1024);
    int n, hours;
    (void)f;
    if (!buf) return 300;
    pthread_mutex_lock(&ls.mutex);
    hours = ls.hours;
    pthread_mutex_unlock(&ls.mutex);
    snprintf(url, sizeof(url), LSR_URL, hours);
    n = pic_fetch_to_buf(url, buf, 4 * 1024 * 1024 - 1);
    if (n > 0) {
        buf[n] = '\0';
        n = pic_lsr_parse(buf, (size_t)n, tmp, MAX_LSR);
    }
    free(buf);
    if (n < 0) return 300;
    pthread_mutex_lock(&ls.mutex);
    memcpy(ls.r, tmp, (size_t)n * sizeof(lsr_t));
    ls.n = n;
    pthread_mutex_unlock(&ls.mutex);
    printf("lsr: %d storm reports\n", n);
    return 10 * 60;
}

static void lsr_reload(pic_feature_t *f)
{
    int h = pic_conf_int("LSR_HOURS", 24);
    if (h != 6 && h != 12 && h != 24 && h != 48) h = 24;
    pthread_mutex_lock(&ls.mutex);
    if (h != ls.hours) pic_feature_poke(f);
    ls.hours = h;
    ls.show_minor = pic_conf_int("LSR_FOG", 0);
    pthread_mutex_unlock(&ls.mutex);
}

/* Category: colour + short label; NULL = skip */
static const char *lsr_kind(char t, int minor, double *r, double *g, double *b)
{
    switch (t) {
    case 'T': case 'C': case 'W':
        *r = 1.0; *g = 0.15; *b = 0.25;
        return t == 'C' ? "Funnel" : t == 'W' ? "Waterspout" : "Tornado";
    case 'Q': *r = 1.0; *g = 0.65; *b = 0.15; return "Tropical storm";
    case 'H': *r = 0.3; *g = 0.85; *b = 1.0; return "Hail";
    case 'D': case 'G': case 'M': case 'O':
        *r = 1.0; *g = 0.65; *b = 0.15; return "Wind";
    case 'F': case 'E': case 'v': case 'x':
        *r = 0.2; *g = 0.95; *b = 0.4; return "Flood";
    case 'S': case 'Z': case 'B': case '5': case 's':
        *r = 0.85; *g = 0.85; *b = 1.0; return "Snow/ice";
    case 'R': *r = 0.3; *g = 0.6; *b = 1.0; return "Rain";
    case 'L': *r = 1.0; *g = 1.0; *b = 0.4; return "Lightning";
    case 'J': case 'N':
        if (!minor) return NULL;
        *r = 0.7; *g = 0.7; *b = 0.75; return t == 'J' ? "Fog" : "Gust";
    default:
        if (!minor) return NULL;
        *r = 0.8; *g = 0.8; *b = 0.8; return "Report";
    }
}

void pic_layer_render_lsr(cairo_t *cr, int width, int height, time_t now,
                          void *ud)
{
    double s = height / 160.0 * (1 + 0.3 * (pic_symbol_zoom() - 1));
    double fs = height / 95.0;
    int labels = pic_zoom_factor() >= 4, i, hours;

    (void)ud;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    pthread_mutex_lock(&ls.mutex);
    hours = ls.hours;
    for (i = 0; i < ls.n; i++) {
        const lsr_t *r = &ls.r[i];
        double cr_, cg, cb, x, y, age, a;
        const char *k = lsr_kind(r->type, ls.show_minor, &cr_, &cg, &cb);
        /* tornado-family and tropical reports: use the report's own
         * wording rather than a name guessed from the one-letter code */
        if (k && r->text[0] && strchr("TCWQ", r->type)) k = r->text;
        char lab[64];
        if (!k) continue;
        x = pic_lon_to_x(r->lon, width);
        y = pic_lat_to_y(r->lat, height);
        if (x < -20 || x > width + 20 || y < -20 || y > height + 20) continue;
        age = r->t ? difftime(now, r->t) / 3600.0 : 0;
        a = 1.0 - 0.7 * (age / hours);
        if (a < 0.3) a = 0.3;
        /* Inverted triangle (storm-report style) */
        cairo_move_to(cr, x - s, y - s * 0.8);
        cairo_line_to(cr, x + s, y - s * 0.8);
        cairo_line_to(cr, x, y + s * 0.9);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, cr_, cg, cb, a);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.8 * a);
        cairo_set_line_width(cr, s * 0.18);
        cairo_stroke(cr);
        if (!labels) continue;
        if (r->mag > 0 && !strcasecmp(r->unit, "Inch"))
            snprintf(lab, sizeof(lab), "%s %.4g in", k, r->mag);
        else if (r->mag > 0 && (!strcasecmp(r->unit, "MPH") || !strcasecmp(r->unit, "Knots")))
            snprintf(lab, sizeof(lab), "%s %.0f %s", k, r->mag,
                     !strcasecmp(r->unit, "MPH") ? "mph" : "kt");
        else
            snprintf(lab, sizeof(lab), "%s", k);
        pic_draw_text_pill(cr, x + s * 1.3, y + fs * 0.35, lab,
                           (cr_ + 1) / 2, (cg + 1) / 2, (cb + 1) / 2, 0.55 * a);
    }
    pthread_mutex_unlock(&ls.mutex);
}

static pic_feature_t def = {
    .name = "lsr", .layer = pic_layer_render_lsr, .interval = 60,
    .extra_mb = 4, .reload = lsr_reload, .work = lsr_work,
    .needs_network = 1,
};

pic_feature_t *pic_feature_lsr(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&ls.mutex, NULL);
        ls.hours = 24;
        inited = 1;
    }
    return &def;
}
