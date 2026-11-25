/*
 * layers_dsn.c - NASA Deep Space Network, live
 *
 * Map layer: the three DSN complexes (Goldstone, Madrid, Canberra)
 * with each active antenna drawn as a beam pointing along its
 * azimuth (longer = lower elevation), and the spacecraft each one is
 * talking to listed beside the site.
 *
 * Applet "DSN Now": per complex, antenna → spacecraft, downlink
 * rate, uplink indicator and distance in light time.
 *
 * Data: DSN Now (eyes.nasa.gov/dsn), refreshed every 5 s by NASA;
 * we poll every 60 s.
 *   dsn.xml    - <station name friendlyName/> then its <dish> siblings,
 *                each with <upSignal>/<downSignal>/<target> children
 *   config.xml - spacecraft friendly names, site coordinates
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE  /* strcasecmp, memmem */
#include "feature.h"
#include "config.h"
#include "fetch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DSN_URL     "https://eyes.nasa.gov/dsn/data/dsn.xml"
#define DSN_CFG_URL "https://eyes.nasa.gov/dsn/config.xml"
#define DSN_MAX_RESPONSE (256 * 1024)
#define MAX_DISHES  8
#define MAX_SC      256

typedef struct {
    char   dish[8];        /* "DSS43"                        */
    char   target[40];     /* Friendly spacecraft name       */
    double az, el;
    double down_bps;       /* Active downlink data rate      */
    int    down, up;       /* Active signals                 */
    double rtlt;           /* Round-trip light time, s (-1)  */
    double range_km;
} dsn_dish_t;

typedef struct {
    char       name[16];   /* "Goldstone"                    */
    double     lat, lon;
    dsn_dish_t dishes[MAX_DISHES];
    int        ndishes;
} dsn_station_t;

typedef struct {
    pthread_mutex_t mutex;
    dsn_station_t   st[3];
    int             nst;
    time_t          fetched;
} dsn_data_t;

static dsn_data_t dsn;

/* Spacecraft short name → friendly name (filled from config.xml) */
static struct { char code[12]; char name[40]; } sc_names[MAX_SC];
static int n_sc_names;

static int get_attr(const char *tag, const char *end, const char *name,
                    char *out, int n)
{
    char pat[32];
    const char *p, *q;
    snprintf(pat, sizeof(pat), " %s=\"", name);
    p = memmem(tag, (size_t)(end - tag), pat, strlen(pat));
    if (!p) { out[0] = '\0'; return 0; }
    p += strlen(pat);
    q = memchr(p, '"', (size_t)(end - p));
    if (!q) { out[0] = '\0'; return 0; }
    snprintf(out, (size_t)n, "%.*s", (int)(q - p), p);
    return 1;
}

static double attr_num(const char *tag, const char *end, const char *name,
                       double def)
{
    char b[32];
    return get_attr(tag, end, name, b, sizeof(b)) && b[0] ? atof(b) : def;
}

/* Parse config.xml spacecraft names; returns count. */
int pic_dsn_parse_config(const char *xml)
{
    const char *p = xml;
    int n = 0;
    while (n < MAX_SC && (p = strstr(p, "<spacecraft ")) != NULL) {
        const char *e = strchr(p, '>');
        if (!e) break;
        get_attr(p, e, "name", sc_names[n].code, sizeof(sc_names[n].code));
        get_attr(p, e, "friendlyName", sc_names[n].name, sizeof(sc_names[n].name));
        if (sc_names[n].code[0] && sc_names[n].name[0]) n++;
        p = e;
    }
    n_sc_names = n;
    return n;
}

static void friendly(const char *code, char *out, int n)
{
    int i;
    for (i = 0; i < n_sc_names; i++)
        if (strcasecmp(sc_names[i].code, code) == 0) {
            snprintf(out, (size_t)n, "%s", sc_names[i].name);
            return;
        }
    snprintf(out, (size_t)n, "%s", code);
}

static const struct { const char *code; double lat, lon; } sites[] = {
    { "gdscc",  35.2443523, -116.8895382 },
    { "mdscc",  40.2413554,   -4.2480085 },
    { "cdscc", -35.2209189,  148.9812673 },
};

/* Parse dsn.xml into stations. Only dishes tracking a spacecraft. */
int pic_dsn_parse(const char *xml, dsn_station_t *st, int max_st)
{
    const char *p = xml;
    int nst = -1, k;

    while ((p = strchr(p, '<')) != NULL) {
        const char *e = strchr(p, '>');
        if (!e) break;

        if (strncmp(p, "<station ", 9) == 0) {
            char code[16];
            if (nst + 1 >= max_st) break;
            nst++;
            memset(&st[nst], 0, sizeof(st[nst]));
            get_attr(p, e, "friendlyName", st[nst].name, sizeof(st[nst].name));
            get_attr(p, e, "name", code, sizeof(code));
            for (k = 0; k < 3; k++)
                if (!strcmp(code, sites[k].code)) {
                    st[nst].lat = sites[k].lat;
                    st[nst].lon = sites[k].lon;
                }
        } else if (strncmp(p, "<dish ", 6) == 0 && nst >= 0) {
            const char *dend = strstr(e, "</dish>");
            const char *c;
            dsn_dish_t d;
            char tname[24] = "";

            if (!dend) dend = e;
            memset(&d, 0, sizeof(d));
            get_attr(p, e, "name", d.dish, sizeof(d.dish));
            d.az = attr_num(p, e, "azimuthAngle", 0);
            d.el = attr_num(p, e, "elevationAngle", 90);
            d.rtlt = -1;

            for (c = e; (c = strchr(c + 1, '<')) != NULL && c < dend; ) {
                const char *ce = strchr(c, '>');
                char b[24];
                if (!ce) break;
                if (!strncmp(c, "<downSignal ", 12)) {
                    get_attr(c, ce, "active", b, sizeof(b));
                    if (!strcmp(b, "true")) {
                        d.down = 1;
                        d.down_bps = attr_num(c, ce, "dataRate", 0);
                    }
                } else if (!strncmp(c, "<upSignal ", 10)) {
                    get_attr(c, ce, "active", b, sizeof(b));
                    if (!strcmp(b, "true")) d.up = 1;
                } else if (!strncmp(c, "<target ", 8) && !tname[0]) {
                    get_attr(c, ce, "name", b, sizeof(b));
                    /* DSN/STA/TEST are maintenance pseudo-targets */
                    if (b[0] && strcmp(b, "DSN") && strcmp(b, "STA") &&
                        strcasecmp(b, "TEST") && strcmp(b, "RFC")) {
                        snprintf(tname, sizeof(tname), "%s", b);
                        d.rtlt = attr_num(c, ce, "rtlt", -1);
                        d.range_km = attr_num(c, ce, "downlegRange", -1);
                    }
                }
                c = ce;
            }
            if (tname[0] && (d.down || d.up) &&
                st[nst].ndishes < MAX_DISHES) {
                friendly(tname, d.target, sizeof(d.target));
                st[nst].dishes[st[nst].ndishes++] = d;
            }
            p = dend;
            continue;
        }
        p = e;
    }
    return nst + 1;
}

static int dsn_work(pic_feature_t *f)
{
    static dsn_station_t tmp[3];
    char *buf;
    int n, nst;

    (void)f;
    buf = malloc(DSN_MAX_RESPONSE);
    if (!buf) return 300;

    if (n_sc_names == 0) {
        n = pic_fetch_to_buf(DSN_CFG_URL, buf, DSN_MAX_RESPONSE - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("dsn: %d spacecraft names\n", pic_dsn_parse_config(buf));
        }
    }

    n = pic_fetch_to_buf(DSN_URL, buf, DSN_MAX_RESPONSE - 1);
    if (n <= 0) {
        free(buf);
        return 120;
    }
    buf[n] = '\0';
    nst = pic_dsn_parse(buf, tmp, 3);
    free(buf);

    pthread_mutex_lock(&dsn.mutex);
    memcpy(dsn.st, tmp, sizeof(tmp));
    dsn.nst = nst;
    dsn.fetched = time(NULL);
    pthread_mutex_unlock(&dsn.mutex);
    return 60;
}

static void fmt_rate(char *out, int n, double bps)
{
    if (bps >= 1e6)      snprintf(out, (size_t)n, "%.1f Mb/s", bps / 1e6);
    else if (bps >= 1e3) snprintf(out, (size_t)n, "%.0f kb/s", bps / 1e3);
    else if (bps > 0)    snprintf(out, (size_t)n, "%.0f b/s", bps);
    else                 out[0] = '\0';
}

/* One-way light time: "4.6 s", "12 m", "23.1 h" */
static void fmt_light(char *out, int n, double rtlt)
{
    double s = rtlt / 2.0;
    if (rtlt <= 0)       out[0] = '\0';
    else if (s < 120)    snprintf(out, (size_t)n, "%.1f ls", s);
    else if (s < 7200)   snprintf(out, (size_t)n, "%.0f lm", s / 60);
    else                 snprintf(out, (size_t)n, "%.1f lh", s / 3600);
}

void pic_layer_render_dsn(cairo_t *cr, int width, int height,
                          time_t now, void *ud)
{
    double fs = height / 85.0, s = height / 60.0;
    int i, k;

    (void)ud;
    pthread_mutex_lock(&dsn.mutex);
    /* These are live links: when updates have failed for 10 minutes
     * the last ones are no longer what the antennas are doing */
    if (dsn.fetched && now - dsn.fetched > 600) {
        pthread_mutex_unlock(&dsn.mutex);
        return;
    }
    for (i = 0; i < dsn.nst; i++) {
        const dsn_station_t *st = &dsn.st[i];
        double x = pic_lon_to_x(st->lon, width);
        double y = pic_lat_to_y(st->lat, height);
        char line[96];

        if (!st->name[0] || x < -200 || x > width + 50) continue;

        /* Beams: azimuth on screen (north up), length by elevation */
        for (k = 0; k < st->ndishes; k++) {
            const dsn_dish_t *d = &st->dishes[k];
            double a = d->az * M_PI / 180.0;
            double len = s * (1.2 + 2.0 * (1.0 - d->el / 90.0));
            cairo_set_source_rgba(cr, 0.45, 0.85, 1.0, 0.75);
            cairo_set_line_width(cr, s * 0.12);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_move_to(cr, x, y);
            cairo_line_to(cr, x + sin(a) * len, y - cos(a) * len);
            cairo_stroke(cr);
        }

        /* Dish icon: bowl + stand */
        cairo_set_source_rgba(cr, 0.92, 0.95, 1.0, 0.95);
        cairo_set_line_width(cr, s * 0.14);
        cairo_new_path(cr);
        cairo_arc_negative(cr, x, y - s * 0.55, s * 0.5, M_PI * 0.85, M_PI * 0.15);
        cairo_stroke(cr);
        cairo_move_to(cr, x, y - s * 0.1);
        cairo_line_to(cr, x - s * 0.3, y + s * 0.3);
        cairo_move_to(cr, x, y - s * 0.1);
        cairo_line_to(cr, x + s * 0.3, y + s * 0.3);
        cairo_stroke(cr);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        snprintf(line, sizeof(line), "DSN %s", st->name);
        pic_draw_text_pill(cr, x + s * 0.8, y + fs * 0.4, line,
                           0.6, 0.9, 1.0, 0.6);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs * 0.9);
        for (k = 0; k < st->ndishes; k++) {
            snprintf(line, sizeof(line), "%s", st->dishes[k].target);
            pic_fit_text(cr, line, fs * 14);
            pic_draw_text_pill(cr, x + s * 0.8, y + fs * 0.4 + fs * 1.35 * (k + 1),
                               line, 0.85, 0.9, 0.95, 0.5);
        }
    }
    pthread_mutex_unlock(&dsn.mutex);
}

double pic_applet_render_dsn(cairo_t *cr, double width, time_t now, void *ud)
{
    double fs = width / 17.0, line_h = fs * 1.25, y;
    int i, k, rows = 0;

    (void)now; (void)ud;
    pthread_mutex_lock(&dsn.mutex);
    for (i = 0; i < dsn.nst; i++) rows += 1 + dsn.st[i].ndishes;
    if (rows == 0) rows = 1;
    y = pic_applet_title(NULL, width, "") + rows * line_h + fs * 0.3;
    if (!cr) {
        pthread_mutex_unlock(&dsn.mutex);
        return y;
    }

    pic_applet_title(cr, width, "Deep Space Network");
    y = pic_applet_title(NULL, width, "") + fs * 0.3;
    if (dsn.nst == 0) {
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgba(cr, 0.6, 0.6, 0.65, 0.9);
        cairo_move_to(cr, 0, y + fs);
        cairo_show_text(cr, "Loading\xe2\x80\xa6");
    }
    for (i = 0; i < dsn.nst; i++) {
        const dsn_station_t *st = &dsn.st[i];
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgba(cr, 0.55, 0.85, 1.0, 0.95);
        cairo_move_to(cr, 0, y + fs);
        cairo_show_text(cr, st->name);
        y += line_h;

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs * 0.9);
        for (k = 0; k < st->ndishes; k++) {
            const dsn_dish_t *d = &st->dishes[k];
            char rate[24], light[24], right[64], name[48];
            cairo_text_extents_t e;

            fmt_rate(rate, sizeof(rate), d->down_bps);
            fmt_light(light, sizeof(light), d->rtlt);
            snprintf(right, sizeof(right), "%s%s%s%s",
                     d->up ? "\xe2\x86\x91 " : "", rate,
                     rate[0] && light[0] ? "  " : "", light);
            cairo_text_extents(cr, right, &e);
            cairo_set_source_rgba(cr, 0.65, 0.7, 0.78, 0.9);
            cairo_move_to(cr, width - e.x_advance, y + fs);
            cairo_show_text(cr, right);

            snprintf(name, sizeof(name), "%s", d->target);
            pic_fit_text(cr, name, width - e.x_advance - fs * 4.2);
            cairo_set_source_rgba(cr, 0.55, 0.58, 0.65, 0.9);
            cairo_move_to(cr, fs * 0.4, y + fs);
            cairo_show_text(cr, d->dish);
            cairo_set_source_rgba(cr, 0.92, 0.92, 0.95, 0.95);
            cairo_move_to(cr, fs * 3.6, y + fs);
            cairo_show_text(cr, name);
            y += line_h;
        }
    }
    pthread_mutex_unlock(&dsn.mutex);
    return pic_applet_title(NULL, width, "") + rows * line_h + fs * 0.3;
}

static pic_feature_t feature_def = {
    .name = "dsn", .layer = pic_layer_render_dsn, .interval = 30,
    .extra_mb = 1,
    .applet = pic_applet_render_dsn, .label = "Deep Space Network",
    .applet_w = 220, .side = APPLET_SIDE_RIGHT,
    .work = dsn_work, .needs_network = 1,
};

pic_feature_t *pic_feature_dsn(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&dsn.mutex, NULL);
        inited = 1;
    }
    return &feature_def;
}
