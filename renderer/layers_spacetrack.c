/*
 * layers_spacetrack.c - Space objects from Space-Track (spacedata.c)
 *
 * Four map layers, each with its own switch:
 *
 *   reentries     Objects Space-Track expects to re-enter (TIP
 *                 messages). Coming ones are drawn where they are now,
 *                 with their height and the predicted time ± window.
 *                 Ones that came down in the last 24 h are drawn at
 *                 Space-Track's best-estimate point, with the stretch
 *                 of orbit they could have come down on (± window).
 *   conjunctions  Public close-approach warnings (CDM) in the next 6 h:
 *                 where the two objects meet, both tracks across the
 *                 meeting point, miss distance, height and chance of a
 *                 collision. From 15 min before, both objects are shown
 *                 closing in and flash.
 *   geobelt       Geostationary satellites; the part of the belt above
 *                 home's horizon is shaded and its satellites bright.
 *                 GOES-East and -West are named with their elevation.
 *   debris        Pieces of the debris groups catalogued most recently,
 *                 where they are now, a panel listing the groups and a
 *                 line with the catalogue totals.
 *
 * Positions come from the elements Space-Track gives for these objects
 * (objects.tle, geo.tle, debris.tle), propagated with the clock's SGP4.
 * Labels keep clear of the clock, the side panels and the strips at
 * the bottom.
 *
 * Space-Track data is only displayed here, never passed on.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "features_all.h"
#include "spacedata.h"
#include "spacelayers.h"
#include "satellite.h"
#include "config.h"
#include <cairo/cairo.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

double pic_sat_look_angles(const pic_sat_t *s, time_t t, double qlat,
                           double qlon, double *az, int *sunlit);
extern double pic_panel_rect[2][4] __attribute__((weak));
extern double pic_ticker_bar_top __attribute__((weak));

#define D2R (M_PI / 180.0)
#define GEO_ALT 35786.0

/* ================================================================ data */

enum { P_RE, P_CJ, P_GEO, P_DEB, P_N };

static struct {
    pthread_mutex_t mutex;
    time_t parsed[P_N];              /* when each part was last read   */
    time_t mt[P_N][4];               /* source files' mtimes           */
    sl_reentry_t re[SL_MAX_RE];
    int nre;
    sl_conj_t cj[SL_MAX_CJ];
    int ncj;
    sl_geo_t *geo;
    int ngeo;
    sl_group_t grp[SL_MAX_GRP];
    int ngrp;
    sl_piece_t *pc;
    int npc;
    sl_stats_t stats;
} st = { .mutex = PTHREAD_MUTEX_INITIALIZER };

static const char *sdir(void)
{
    const char *e = getenv("PIC_SPACEDATA_DIR");
    return e && *e ? e : SPACEDATA_DIR;
}

static char *slurp(const char *name, time_t *mt)
{
    char p[256];
    struct stat sb;
    FILE *f;
    char *b = NULL;
    long n;
    snprintf(p, sizeof(p), "%s/%s", sdir(), name);
    if (mt) *mt = 0;
    if (stat(p, &sb) != 0 || !(f = fopen(p, "r"))) return NULL;
    if (mt) *mt = sb.st_mtime;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    if (n > 0 && n < (16 << 20) && (b = malloc((size_t)n + 1)) != NULL)
        b[fread(b, 1, (size_t)n, f)] = 0;
    fclose(f);
    return b;
}

static time_t mtime_of(const char *name)
{
    char p[256];
    struct stat sb;
    snprintf(p, sizeof(p), "%s/%s", sdir(), name);
    return stat(p, &sb) == 0 ? sb.st_mtime : 0;
}

/* Walk a 3LE text: calls fn(name, l1, l2, ctx) for each element set */
static void each_3le(char *txt, void (*fn)(const char *, const char *, const char *, void *), void *ctx)
{
    char *line[3] = { 0 }, *p = txt, *nl;
    int k = 0;
    while (p && *p) {
        nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (nl && nl > p && nl[-1] == '\r') nl[-1] = 0;
        line[0] = line[1]; line[1] = line[2]; line[2] = p;
        k++;
        if (k >= 3 && line[0][0] == '0' && line[0][1] == ' ' &&
            line[1][0] == '1' && line[2][0] == '2')
            fn(line[0] + 2, line[1], line[2], ctx);
        p = nl ? nl + 1 : NULL;
    }
}

static void tidy(const char *in, char *out, size_t n)
{
    char t[48];
    snprintf(t, sizeof(t), "%s", in);
    t[strcspn(t, "\r\n")] = 0;
    while (*t && t[strlen(t) - 1] == ' ') t[strlen(t) - 1] = 0;
    sl_tidy_name(t, out, n);
}

/* objects.tle -> the re-entry and close-approach objects */
static void obj_cb(const char *name, const char *l1, const char *l2, void *ctx)
{
    int id = pic_tle_catnr(l1), i, part = *(int *)ctx;
    if (part == P_RE) {
        for (i = 0; i < st.nre; i++)
            if (st.re[i].id == id && pic_sat_parse_tle(&st.re[i].sat, l1, l2) == 0) {
                st.re[i].has_sat = 1;
                tidy(name, st.re[i].name, sizeof(st.re[i].name));
            }
    } else {
        for (i = 0; i < st.ncj; i++) {
            if (st.cj[i].id1 == id && pic_sat_parse_tle(&st.cj[i].s1, l1, l2) == 0)
                st.cj[i].has_sat |= 1;
            if (st.cj[i].id2 == id && pic_sat_parse_tle(&st.cj[i].s2, l1, l2) == 0)
                st.cj[i].has_sat |= 2;
        }
    }
}

static void geo_cb(const char *name, const char *l1, const char *l2, void *ctx)
{
    sl_geo_t *g;
    (void)ctx;
    if (st.ngeo >= SL_MAX_GEO) return;
    g = &st.geo[st.ngeo];
    memset(g, 0, sizeof(*g));
    if (pic_sat_parse_tle(&g->sat, l1, l2) != 0) return;
    snprintf(g->name, sizeof(g->name), "%s", name);
    g->name[strcspn(g->name, "\r\n")] = 0;
    st.ngeo++;
}

/* debris.tle: piece -> group by international designator */
static void deb_cb(const char *name, const char *l1, const char *l2, void *ctx)
{
    char key[12];
    int yy, i;
    sl_piece_t *p;
    (void)name; (void)ctx;
    if (st.npc >= SL_MAX_PIECE || strlen(l1) < 17) return;
    /* columns 10-17: "24140AF " -> "2024-140" */
    yy = (l1[9] - '0') * 10 + (l1[10] - '0');
    snprintf(key, sizeof(key), "%d-%.3s", yy < 57 ? 2000 + yy : 1900 + yy, l1 + 11);
    for (i = 0; i < st.ngrp; i++)
        if (strcmp(st.grp[i].key, key) == 0) break;
    if (i == st.ngrp) return;
    p = &st.pc[st.npc];
    memset(p, 0, sizeof(*p));
    if (pic_sat_parse_tle(&p->sat, l1, l2) != 0) return;
    p->sat.norad_id = pic_tle_catnr(l1);
    p->group = i;
    st.npc++;
}

static int changed(int part, const char *const *files, int n, time_t now)
{
    int i, ch = now - st.parsed[part] >= 300;     /* time filters move on */
    for (i = 0; i < n; i++) {
        time_t m = mtime_of(files[i]);
        if (m != st.mt[part][i]) { st.mt[part][i] = m; ch = 1; }
    }
    return ch;
}

static void load(int part, time_t now)
{
    static const char *const fre[] = { "tip.json", "objects.tle" };
    static const char *const fcj[] = { "cdm.json", "objects.tle" };
    static const char *const fgeo[] = { "geo.tle" };
    static const char *const fdeb[] = { "debris.json", "debris.tle", "boxscore.json", "decay.json" };
    char *a, *b;

    pthread_mutex_lock(&st.mutex);
    if (part == P_RE && changed(P_RE, fre, 2, now)) {
        a = slurp("tip.json", NULL);
        st.nre = sl_parse_tip(a, now, SL_RE_KEEP, st.re, SL_MAX_RE);
        free(a);
        if ((b = slurp("objects.tle", NULL)) != NULL) { int k = P_RE; each_3le(b, obj_cb, &k); free(b); }
        st.parsed[P_RE] = now;
    } else if (part == P_CJ && changed(P_CJ, fcj, 2, now)) {
        a = slurp("cdm.json", NULL);
        st.ncj = sl_parse_cdm(a, now, SL_CJ_HOURS, st.cj, SL_MAX_CJ);
        free(a);
        if ((b = slurp("objects.tle", NULL)) != NULL) { int k = P_CJ; each_3le(b, obj_cb, &k); free(b); }
        st.parsed[P_CJ] = now;
    } else if (part == P_GEO && changed(P_GEO, fgeo, 1, now)) {
        if (!st.geo) st.geo = calloc(SL_MAX_GEO, sizeof(*st.geo));
        st.ngeo = 0;
        if (st.geo && (b = slurp("geo.tle", NULL)) != NULL) { each_3le(b, geo_cb, NULL); free(b); }
        st.parsed[P_GEO] = now;
    } else if (part == P_DEB && changed(P_DEB, fdeb, 4, now)) {
        a = slurp("debris.json", NULL);
        st.ngrp = sl_debris_groups(a, 4, st.grp, SL_MAX_GRP);
        free(a);
        if (!st.pc) st.pc = calloc(SL_MAX_PIECE, sizeof(*st.pc));
        st.npc = 0;
        if (st.pc && (b = slurp("debris.tle", NULL)) != NULL) { each_3le(b, deb_cb, NULL); free(b); }
        a = slurp("boxscore.json", NULL);
        b = slurp("decay.json", NULL);
        sl_parse_stats(a, b, now, &st.stats);
        free(a);
        free(b);
        st.parsed[P_DEB] = now;
    }
    pthread_mutex_unlock(&st.mutex);
}

static int work_re(pic_feature_t *f)  { (void)f; load(P_RE, time(NULL));  return 120; }
static int work_cj(pic_feature_t *f)  { (void)f; load(P_CJ, time(NULL));  return 120; }
static int work_geo(pic_feature_t *f) { (void)f; load(P_GEO, time(NULL)); return 600; }
static int work_deb(pic_feature_t *f) { (void)f; load(P_DEB, time(NULL)); return 600; }

static void stopped_geo(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&st.mutex);
    free(st.geo); st.geo = NULL; st.ngeo = 0; st.parsed[P_GEO] = 0;
    memset(st.mt[P_GEO], 0, sizeof(st.mt[P_GEO]));
    pthread_mutex_unlock(&st.mutex);
}

static void stopped_deb(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&st.mutex);
    free(st.pc); st.pc = NULL; st.npc = 0; st.parsed[P_DEB] = 0;
    memset(st.mt[P_DEB], 0, sizeof(st.mt[P_DEB]));
    pthread_mutex_unlock(&st.mutex);
}

/* ================================================================ labels */

#define MAXBOX 48
static double boxes[P_N][MAXBOX][4];
static int nbox[P_N];
static time_t box_at[P_N];         /* when each part last drew */

static int hits(const double *a, const double *b)
{
    return !(a[2] < b[0] || a[0] > b[2] || a[3] < b[1] || a[1] > b[3]);
}

static int clear_at(const double *bx, int w, int h, int part)
{
    double keep[4][4] = {
        { 0, 0, w * 0.21, h * 0.115 },          /* UTC clock */
        { w * 0.79, 0, w, h * 0.13 },           /* local clock + address */
        { 0, fmin(h * 0.905, pic_legend_top(h)), w, h },   /* notes and key at the bottom */
        { 0, 0, 0, 0 },
    };
    int i, p;
    if (bx[0] < 3 || bx[1] < 3 || bx[2] > w - 3 || bx[3] > h - 3) return 0;
    if (&pic_ticker_bar_top && pic_ticker_bar_top > 0) keep[3][1] = pic_ticker_bar_top - h * 0.06, keep[3][2] = w, keep[3][3] = h;
    for (i = 0; i < 4; i++)
        if (keep[i][2] > keep[i][0] && hits(bx, keep[i])) return 0;
    if (&pic_panel_rect)
        for (i = 0; i < 2; i++)
            if (pic_panel_rect[i][2] > pic_panel_rect[i][0] && hits(bx, pic_panel_rect[i])) return 0;
    for (p = 0; p < P_N; p++) {
        if (p != part && time(NULL) - box_at[p] > 300) continue;   /* layer no longer drawn */
        for (i = 0; i < nbox[p]; i++)
            if (hits(bx, boxes[p][i])) return 0;
    }
    return 1;
}

static void halo_text(cairo_t *cr, double x, double y, const char *s,
                      double r, double g, double b, double fs)
{
    cairo_move_to(cr, x, y);
    cairo_text_path(cr, s);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
    cairo_set_line_width(cr, fs * 0.22);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
    cairo_move_to(cr, x, y);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_show_text(cr, s);
}

/* Two-line label beside (x, y), placed where it hides nothing else */
static void label2(cairo_t *cr, int w, int h, int part, double x, double y,
                   const char *l1, const double c1[3], const char *l2, const double c2[3])
{
    double fs = h / 72.0, gap = h / 70.0, bw, bh = fs * 2.35, bx[4] = { 0 };
    double w1, w2, dx = gap, dy = -bh;
    cairo_text_extents_t e;
    int k, al = 0, found = 0;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, l1, &e); w1 = e.x_advance;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.85);
    cairo_text_extents(cr, l2, &e); w2 = e.x_advance;
    bw = (w1 > w2 ? w1 : w2) + 4;

    /* beside it (right/left, above/below/level), then further out */
    for (k = 0; k < 6 + 4 * 40 && !found; k++) {
        static const double oy[3] = { -1.05, 0.15, -0.5 };
        double off;
        if (k < 6) {
            al = k & 1;
            off = gap;
            dy = oy[k / 2] * bh;
        } else {
            int m = k - 6;
            al = m & 1;
            dy = (m >> 1) & 1 ? 0.15 * bh : -1.05 * bh;
            off = gap + (double)(m / 4 + 1) * h / 50.0;
        }
        dx = al ? -off - bw : off;
        bx[0] = x + dx; bx[1] = y + dy; bx[2] = x + dx + bw; bx[3] = y + dy + bh;
        found = clear_at(bx, w, h, part);
    }
    if (!found) return;                     /* nowhere to put it: icon only */
    if (nbox[part] < MAXBOX) memcpy(boxes[part][nbox[part]++], bx, sizeof(bx));

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    halo_text(cr, bx[0] + 2, bx[1] + fs, l1, c1[0], c1[1], c1[2], fs);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.85);
    halo_text(cr, bx[0] + 2, bx[1] + fs * 2.1, l2, c2[0], c2[1], c2[2], fs);
}

/* ================================================================ icons */

/* An angular fragment */
static void shard(cairo_t *cr, double x, double y, double r, double rot,
                  double cr_, double cg, double cb, double a)
{
    static const double k[6][3] = {
        { 1, .2, 1 }, { .55, .95, .7 }, { -.4, .7, .9 },
        { -1, -.1, .75 }, { -.3, -.85, 1 }, { .6, -.6, .65 } };
    int i;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_rotate(cr, rot);
    for (i = 0; i < 6; i++) {
        double px = k[i][0] * r * k[i][2], py = k[i][1] * r * k[i][2];
        if (i == 0) cairo_move_to(cr, px, py); else cairo_line_to(cr, px, py);
    }
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, cr_, cg, cb, a);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.7 * a);
    cairo_set_line_width(cr, r * 0.3);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* Object coming apart on the way down: hot head, plasma trail, pieces
 * shed behind it. heading in radians (screen), s = size scale. */
static void icon_reentry(cairo_t *cr, double x, double y, double s, double heading)
{
    static const double fr[4][4] = {
        { -12, -8, 2.6, .4 }, { -19, 7, 2.2, 1.3 }, { -26, -5, 1.8, 2.2 }, { -9, 9, 1.7, .9 } };
    cairo_pattern_t *g;
    int i;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_rotate(cr, heading);
    cairo_scale(cr, s, s);
    g = cairo_pattern_create_linear(0, 0, -34, 0);
    cairo_pattern_add_color_stop_rgba(g, 0, 1, .85, .4, .95);
    cairo_pattern_add_color_stop_rgba(g, .5, 1, .45, .1, .6);
    cairo_pattern_add_color_stop_rgba(g, 1, .9, .2, .05, 0);
    cairo_move_to(cr, 3, -4.5); cairo_line_to(cr, -34, -1.2);
    cairo_line_to(cr, -34, 1.2); cairo_line_to(cr, 3, 4.5); cairo_close_path(cr);
    cairo_set_source(cr, g); cairo_fill(cr);
    cairo_pattern_destroy(g);
    g = cairo_pattern_create_radial(0, 0, 0, 0, 0, 9);
    cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, .85, 1);
    cairo_pattern_add_color_stop_rgba(g, .45, 1, .65, .2, .9);
    cairo_pattern_add_color_stop_rgba(g, 1, 1, .3, 0, 0);
    cairo_arc(cr, 0, 0, 9, 0, 2 * M_PI);
    cairo_set_source(cr, g); cairo_fill(cr);
    cairo_pattern_destroy(g);
    for (i = 0; i < 4; i++) shard(cr, fr[i][0], fr[i][1], fr[i][2], fr[i][3], 1, .62, .25, 1);
    cairo_restore(cr);
}

/* Two objects on crossing paths inside a warning ring; flash 0..1 */
static void icon_conj(cairo_t *cr, double x, double y, double s, double flash)
{
    int i;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);
    if (flash > 0) {
        cairo_pattern_t *g = cairo_pattern_create_radial(0, 0, 4, 0, 0, 22);
        cairo_pattern_add_color_stop_rgba(g, 0, 1, .25, .35, .75 * flash);
        cairo_pattern_add_color_stop_rgba(g, 1, 1, .2, .3, 0);
        cairo_arc(cr, 0, 0, 22, 0, 2 * M_PI);
        cairo_set_source(cr, g); cairo_fill(cr);
        cairo_pattern_destroy(g);
    }
    cairo_set_line_width(cr, 1.6);
    for (i = 0; i < 2; i++) {
        double a = (i ? 30 : -30) * D2R, dx = cos(a), dy = sin(a);
        double c[3] = { i ? 1 : .55, i ? .8 : .9, i ? .35 : 1 };
        double dash[2] = { 3, 2 };
        cairo_set_source_rgba(cr, c[0], c[1], c[2], .9);
        cairo_set_dash(cr, dash, 2, 0);
        cairo_move_to(cr, -14 * dx, -14 * dy); cairo_line_to(cr, -4 * dx, -4 * dy);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
        cairo_arc(cr, -4.5 * dx, -4.5 * dy, 2.6, 0, 2 * M_PI);
        cairo_set_source_rgb(cr, c[0], c[1], c[2]); cairo_fill(cr);
    }
    cairo_arc(cr, 0, 0, 9, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 1, .3, .4, .95);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* A small cloud of pieces */
static void icon_debris(cairo_t *cr, double x, double y, double s)
{
    unsigned seed = 3;
    int i;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);
    for (i = 0; i < 9; i++) {
        double a, d, r, rot;
        seed = seed * 1103515245u + 12345u; a = (seed >> 8) % 6283 / 1000.0;
        seed = seed * 1103515245u + 12345u; d = 1 + (seed >> 8) % 1000 / 100.0;
        seed = seed * 1103515245u + 12345u; r = 1.6 + (seed >> 8) % 180 / 100.0;
        seed = seed * 1103515245u + 12345u; rot = (seed >> 8) % 6283 / 1000.0;
        shard(cr, d * cos(a), d * sin(a), r, rot, .85, .87, .9, 1);
    }
    cairo_restore(cr);
}

/* ================================================================ helpers */

static double px_x(double lon, int w) { return pic_lon_to_x(lon, w); }
static double px_y(double lat, int h) { return pic_lat_to_y(lat, h); }

/* Screen heading (radians) of a polyline's last segment */
static double heading_of(const double *xs, const double *ys, int n, double wrap)
{
    int i;
    for (i = n - 1; i > 0; i--)
        if (fabs(xs[i] - xs[i - 1]) < wrap && (xs[i] != xs[i - 1] || ys[i] != ys[i - 1]))
            return atan2(ys[i] - ys[i - 1], xs[i] - xs[i - 1]);
    return 0;
}

static void fmt_window(char *o, size_t n, int min)
{
    if (min < 90) snprintf(o, n, "%d min", min);
    else snprintf(o, n, "%d h", (int)lround(min / 60.0));
}

static void fmt_thousands(char *o, size_t n, long v)
{
    char t[32];
    int len, i, j = 0;
    snprintf(t, sizeof(t), "%ld", v);
    len = (int)strlen(t);
    for (i = 0; i < len && j + 2 < (int)n; i++) {
        o[j++] = t[i];
        if ((len - i - 1) % 3 == 0 && i < len - 1) o[j++] = ',';
    }
    o[j] = 0;
}

static double scale_of(int h) { return h / 1080.0; }

/* ================================================================ reentries */

static void render_reentries(cairo_t *cr, int w, int h, time_t now, void *ud)
{
    static const double ca[3] = { 1, .78, .45 }, cb[3] = { 1, .85, .7 };
    double wrap = pic_wrap_threshold_px(w), s = scale_of(h);
    int i;
    (void)ud;
    nbox[P_RE] = 0;
    box_at[P_RE] = time(NULL);
    pthread_mutex_lock(&st.mutex);
    for (i = 0; i < st.nre; i++) {
        sl_reentry_t *r = &st.re[i];
        char l1[96], l2[96], when[32], win[16];
        double xs[121], ys[121];
        int k, n = 0, imminent = r->window_min <= 60;

        fmt_window(win, sizeof(win), r->window_min);
        pic_fmt_when(when, sizeof(when), now, r->decay);

        if (r->decay <= now || (imminent && r->decay - now < 3 * 3600)) {
            /* Best-estimate point and the stretch of orbit (± window)
             * it could have come down on, brightest at the estimate */
            int half = r->window_min < 50 ? r->window_min : 50;
            if (half < 1) half = 1;
            for (k = 0; k <= 120; k++) {
                double la, lo;
                sl_incl_track(r->lat, r->lon, r->incl, r->descending, 150.0,
                              -half + 2.0 * half * k / 120.0, &la, &lo);
                xs[n] = px_x(lo, w); ys[n] = px_y(la, h); n++;
            }
            for (k = 1; k < n; k++) {
                double f = 1 - fabs(k - n / 2.0) / (n / 2.0);
                if (fabs(xs[k] - xs[k - 1]) >= wrap) continue;
                cairo_set_source_rgba(cr, 1, .55, .2, .15 + .75 * f);
                cairo_set_line_width(cr, (1 + 2.5 * f) * s);
                cairo_move_to(cr, xs[k - 1], ys[k - 1]);
                cairo_line_to(cr, xs[k], ys[k]);
                cairo_stroke(cr);
            }
            icon_reentry(cr, xs[60], ys[60], s, heading_of(xs, ys, 61, wrap));
            if (r->decay <= now) {
                long ago = (long)(now - r->decay);
                char a[24];
                if (ago < 3600) snprintf(a, sizeof(a), "%ld min ago", ago / 60);
                else snprintf(a, sizeof(a), "%ld h ago", (ago + 1800) / 3600);
                pic_fmt_hm(when, sizeof(when), r->decay);
                {   /* yesterday's: name the day */
                    struct tm a1, b1;
                    char d[8];
                    localtime_r(&now, &a1); localtime_r(&r->decay, &b1);
                    if (a1.tm_yday != b1.tm_yday) {
                        char t2[32];
                        strftime(d, sizeof(d), "%a", &b1);
                        snprintf(t2, sizeof(t2), "%s %s", d, when);
                        snprintf(when, sizeof(when), "%s", t2);
                    }
                }
                snprintf(l1, sizeof(l1), "%s re-entered", r->name);
                snprintf(l2, sizeof(l2), "%s \xc2\xb1%s \xc2\xb7 %s", when, win, a);
            } else {
                snprintf(l1, sizeof(l1), "%s re-entering", r->name);
                snprintf(l2, sizeof(l2), "about %s \xc2\xb1%s, along this path", when, win);
            }
            label2(cr, w, h, P_RE, xs[60], ys[60], l1, ca, l2, cb);
            continue;
        }
        if (!r->has_sat) continue;
        {   /* where it is now, with the last 12 min of its path */
            double la, lo, alt;
            double dash[2] = { 5 * s, 4 * s };
            for (k = 0; k <= 12; k++) {
                pic_sat_position_at(&r->sat, now - (12 - k) * 60, &la, &lo, &alt);
                xs[n] = px_x(lo, w); ys[n] = px_y(la, h); n++;
            }
            cairo_set_source_rgba(cr, 1, .55, .2, .5);
            cairo_set_line_width(cr, 2 * s);
            cairo_set_dash(cr, dash, 2, 0);
            for (k = 1; k < n; k++)
                if (fabs(xs[k] - xs[k - 1]) < wrap) {
                    cairo_move_to(cr, xs[k - 1], ys[k - 1]);
                    cairo_line_to(cr, xs[k], ys[k]);
                }
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);
            if (alt < 60) continue;           /* elements gone stale */
            icon_reentry(cr, xs[n - 1], ys[n - 1], .85 * s, heading_of(xs, ys, n, wrap));
            snprintf(l1, sizeof(l1), "%s \xc2\xb7 %.0f km up", r->name, alt);
            snprintf(l2, sizeof(l2), "falls %s \xc2\xb1%s", when, win);
            label2(cr, w, h, P_RE, xs[n - 1], ys[n - 1], l1, ca, l2, cb);
        }
    }
    pthread_mutex_unlock(&st.mutex);
}

/* ================================================================ close approaches */

static void render_conj(cairo_t *cr, int w, int h, time_t now, void *ud)
{
    static const double ca[3] = { 1, .6, .65 }, cb[3] = { 1, .82, .84 };
    static const double col[2][3] = { { .55, .9, 1 }, { 1, .8, .35 } };
    double wrap = pic_wrap_threshold_px(w), s = scale_of(h);
    int i, shown = 0, flashing = 0;
    (void)ud;
    nbox[P_CJ] = 0;
    box_at[P_CJ] = time(NULL);
    pthread_mutex_lock(&st.mutex);
    for (i = 0; i < st.ncj && shown < 5; i++) {
        sl_conj_t *c = &st.cj[i];
        double la, lo, alt, X, Y, until = difftime(c->tca, now);
        char n1[40], n2[40], l1[120], l2[120], t[24], extra[32] = "";
        int o;
        if (c->has_sat != 3 || until < -120) continue;
        shown++;
        /* both paths across the meeting point (± 6 min) */
        for (o = 0; o < 2; o++) {
            const pic_sat_t *sat = o ? &c->s2 : &c->s1;
            double px = 0, py = 0;
            int k;
            cairo_set_source_rgba(cr, col[o][0], col[o][1], col[o][2], .8);
            cairo_set_line_width(cr, 2 * s);
            for (k = -18; k <= 18; k++) {
                double x, y;
                pic_sat_position_at(sat, c->tca + k * 20, &la, &lo, &alt);
                x = px_x(lo, w); y = px_y(la, h);
                if (k > -18 && fabs(x - px) < wrap) {
                    cairo_move_to(cr, px, py); cairo_line_to(cr, x, y);
                }
                px = x; py = y;
            }
            cairo_stroke(cr);
        }
        pic_sat_position_at(&c->s1, c->tca, &la, &lo, &alt);
        X = px_x(lo, w); Y = px_y(la, h);

        if (until <= 15 * 60) {
            /* closing in: both objects where they are now, pulsing */
            double f = (now & 1) ? 1.0 : 0.4;
            flashing = 1;
            for (o = 0; o < 2; o++) {
                double pla, plo, palt, x, y;
                cairo_pattern_t *g;
                pic_sat_position_at(o ? &c->s2 : &c->s1, now, &pla, &plo, &palt);
                x = px_x(plo, w); y = px_y(pla, h);
                g = cairo_pattern_create_radial(x, y, 2 * s, x, y, 14 * s);
                cairo_pattern_add_color_stop_rgba(g, 0, 1, .3, .4, .9 * f);
                cairo_pattern_add_color_stop_rgba(g, 1, 1, .3, .4, 0);
                cairo_arc(cr, x, y, 14 * s, 0, 2 * M_PI);
                cairo_set_source(cr, g); cairo_fill(cr);
                cairo_pattern_destroy(g);
                cairo_arc(cr, x, y, 4 * s, 0, 2 * M_PI);
                cairo_set_source_rgb(cr, col[o][0], col[o][1], col[o][2]);
                cairo_fill_preserve(cr);
                cairo_set_source_rgb(cr, 0, 0, 0);
                cairo_set_line_width(cr, 1);
                cairo_stroke(cr);
            }
            icon_conj(cr, X, Y, s, f);
        } else {
            icon_conj(cr, X, Y, s, 0);
        }

        sl_tidy_name(c->name1, n1, sizeof(n1));
        sl_tidy_name(c->name2, n2, sizeof(n2));
        snprintf(l1, sizeof(l1), "%s \xe2\x86\x94 %s", n1, n2);
        pic_fmt_hm(t, sizeof(t), c->tca);
        if (c->pc > 0) {
            char num[24];
            double inv = 1.0 / c->pc;
            inv = inv >= 1000 ? round(inv / 100) * 100 : round(inv / 10) * 10;
            if (inv < 1) inv = 1;
            fmt_thousands(num, sizeof(num), (long)inv);
            snprintf(extra, sizeof(extra), " \xc2\xb7 1 in %s", num);
        }
        if (c->rng_m < 1000)
            snprintf(l2, sizeof(l2), "%.0f m apart at %s \xc2\xb7 %.0f km up%s", c->rng_m, t, alt, extra);
        else
            snprintf(l2, sizeof(l2), "%.1f km apart at %s \xc2\xb7 %.0f km up%s", c->rng_m / 1000, t, alt, extra);
        label2(cr, w, h, P_CJ, X, Y, l1, ca, l2, cb);
    }
    pthread_mutex_unlock(&st.mutex);
    pic_layer_set_interval("conjunctions", flashing ? 1 : 15);
}

/* ================================================================ GEO belt */

/* Elevation of a geostationary point at longitude lon seen from home */
static double geo_el(double qlat, double qlon, double lon)
{
    const double re = 6378.137, r = re + GEO_ALT;
    double la = qlat * D2R, dl = (lon - qlon) * D2R;
    double c = cos(la) * cos(dl);                  /* cos of central angle */
    return atan2(c - re / r, sqrt(1 - c * c)) / D2R;
}

static void render_geo(cairo_t *cr, int w, int h, time_t now, void *ud)
{
    double qlat = pic_config.qth_lat, qlon = pic_config.qth_lon, s = scale_of(h);
    double a0 = 999, a1 = -999, l;
    int i, nvis = 0;
    (void)ud;
    nbox[P_GEO] = 0;
    box_at[P_GEO] = time(NULL);

    /* the part of the belt above home's horizon */
    for (l = -180; l < 180; l += 0.5)
        if (geo_el(qlat, qlon, l) > 0) {
            double d = l - qlon;
            while (d > 180) d -= 360;
            while (d < -180) d += 360;
            if (d < a0) a0 = d;
            if (d > a1) a1 = d;
        }
    if (a1 > a0) {
        double y0 = px_y(4, h), y1 = px_y(-4, h), x0 = px_x(qlon + a0, w), x1 = px_x(qlon + a1, w);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, y0, 0, y1);
        cairo_pattern_add_color_stop_rgba(g, 0, .3, .9, 1, 0);
        cairo_pattern_add_color_stop_rgba(g, .5, .3, .9, 1, .16);
        cairo_pattern_add_color_stop_rgba(g, 1, .3, .9, 1, 0);
        cairo_set_source(cr, g);
        if (x1 > x0) cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
        else {                                   /* crosses the map edge */
            cairo_rectangle(cr, x0, y0, w - x0, y1 - y0);
            cairo_rectangle(cr, 0, y0, x1, y1 - y0);
        }
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    pthread_mutex_lock(&st.mutex);
    for (i = 0; i < st.ngeo; i++) {
        sl_geo_t *g = &st.geo[i];
        double la, lo, alt, el, az, x, y, r = 1.6 * s;
        pic_sat_position_at(&g->sat, now, &la, &lo, &alt);
        if (fabs(alt - GEO_ALT) > 600) continue;   /* drifting away / stale */
        el = pic_sat_look_angles(&g->sat, now, qlat, qlon, &az, NULL);
        x = px_x(lo, w); y = px_y(la, h);
        cairo_rectangle(cr, x - r, y - r, 2 * r, 2 * r);
        if (el > 0) { cairo_set_source_rgba(cr, .45, .95, 1, .95); nvis++; }
        else cairo_set_source_rgba(cr, .55, .6, .7, .55);
        cairo_fill(cr);
        /* GOES-East (75.2 W) and GOES-West (137.0 W) by position */
        if (el > 0 && strncmp(g->name, "GOES", 4) == 0 &&
            (fabs(lo + 75.2) < 1.0 || fabs(lo + 137.0) < 1.0)) {
            char l1[48], l2[48];
            static const char *cmp[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
            static const double c1[3] = { .6, .95, 1 }, c2[3] = { .8, .95, 1 };
            cairo_arc(cr, x, y, 4 * s, 0, 2 * M_PI);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_fill(cr);
            snprintf(l1, sizeof(l1), "%s (%s)", fabs(lo + 75.2) < 1.0 ? "GOES-East" : "GOES-West", g->name);
            snprintf(l2, sizeof(l2), "%.0f\xc2\xb0 up in the %s", el, cmp[(int)((az + 22.5) / 45) % 8]);
            label2(cr, w, h, P_GEO, x, y, l1, c1, l2, c2);
        }
    }
    pthread_mutex_unlock(&st.mutex);

    if (nvis && a1 > a0) {
        char t[96];
        static const double c1[3] = { .55, .9, 1 }, c2[3] = { .75, .9, 1 };
        snprintf(t, sizeof(t), "%d geostationary satellites above your horizon", nvis);
        label2(cr, w, h, P_GEO, px_x(qlon + (a0 + a1) / 2, w), px_y(-3, h), "Geostationary belt", c1, t, c2);
    }
}

/* ================================================================ debris */

static void render_debris(cairo_t *cr, int w, int h, time_t now, void *ud)
{
    static const double c1[3] = { .92, .93, .96 }, c2[3] = { .8, .84, .9 };
    double s = scale_of(h), fs = h / 72.0;
    int i, labelled[SL_MAX_GRP] = { 0 };
    (void)ud;
    nbox[P_DEB] = 0;
    box_at[P_DEB] = time(NULL);
    pthread_mutex_lock(&st.mutex);

    /* the pieces, where they are now */
    for (i = 0; i < st.npc; i++) {
        double la, lo, alt;
        pic_sat_position_at(&st.pc[i].sat, now, &la, &lo, &alt);
        if (alt < 80) continue;
        shard(cr, px_x(lo, w), px_y(la, h), 3.2 * s, (st.pc[i].sat.norad_id % 63) / 10.0,
              .85, .87, .9, .95);
    }
    /* panel beside the left panels, above the strips at the bottom */
    if (st.ngrp > 0) {
        double pw = 420 * s, row = fs * 1.5, ph = fs * 2.4 + row * st.ngrp, x, y, r = 6 * s;
        x = (&pic_panel_rect && pic_panel_rect[0][2] > 0) ? pic_panel_rect[0][2] + h / 80.0 : h / 80.0;
        y = h * 0.9 - ph;
        /* the strips at the bottom sit higher in some views (more
         * key lines): stay above the notes drawn last frame */
        if (pic_legend_top(h) - h / 120.0 < h * 0.9) y = pic_legend_top(h) - h / 120.0 - ph;
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + pw - r, y + r, r, -M_PI / 2, 0);
        cairo_arc(cr, x + pw - r, y + ph - r, r, 0, M_PI / 2);
        cairo_arc(cr, x + r, y + ph - r, r, M_PI / 2, M_PI);
        cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.05, 0.7);
        cairo_fill(cr);
        {
            double bx[4] = { x, y, x + pw, y + ph };
            if (nbox[P_DEB] < MAXBOX) memcpy(boxes[P_DEB][nbox[P_DEB]++], bx, sizeof(bx));
        }
        icon_debris(cr, x + fs * 1.3, y + fs * 1.2, .9 * s);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgb(cr, c1[0], c1[1], c1[2]);
        cairo_move_to(cr, x + fs * 2.6, y + fs * 1.55);
        cairo_show_text(cr, "Debris catalogued most recently");
        for (i = 0; i < st.ngrp; i++) {
            sl_group_t *g = &st.grp[i];
            double yy = y + fs * 1.55 + row * (i + 1);
            char t[64], nm[40];
            cairo_text_extents_t e;
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, fs * 0.9);
            cairo_set_source_rgb(cr, 1, .85, .5);
            snprintf(t, sizeof(t), "%d", g->count);
            cairo_move_to(cr, x + fs * 0.8, yy);
            cairo_show_text(cr, t);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            sl_tidy_name(g->name, nm, sizeof(nm));
            cairo_set_source_rgb(cr, .9, .92, .95);
            cairo_move_to(cr, x + fs * 3.4, yy);
            cairo_show_text(cr, nm);
            cairo_set_font_size(cr, fs * 0.8);
            snprintf(t, sizeof(t), "pieces of a %d launch (%s)", g->year, g->country);
            cairo_text_extents(cr, t, &e);
            cairo_set_source_rgb(cr, c2[0] * .85, c2[1] * .85, c2[2] * .85);
            cairo_move_to(cr, x + pw - fs * 0.7 - e.x_advance, yy);
            cairo_show_text(cr, t);
        }
    }
    /* one label per group, at its newest piece */
    for (i = st.npc - 1; i >= 0; i--) {
        sl_piece_t *p = &st.pc[i];
        double la, lo, alt;
        char l1[64], l2[64], nm[40];
        if (labelled[p->group] || p->sat.norad_id != st.grp[p->group].newest) continue;
        pic_sat_position_at(&p->sat, now, &la, &lo, &alt);
        if (alt < 80) continue;
        labelled[p->group] = 1;
        sl_tidy_name(st.grp[p->group].name, nm, sizeof(nm));
        snprintf(l1, sizeof(l1), "%s debris", nm);
        snprintf(l2, sizeof(l2), "%d pieces \xc2\xb7 %.0f km up", st.grp[p->group].count, alt);
        label2(cr, w, h, P_DEB, px_x(lo, w), px_y(la, h), l1, c1, l2, c2);
    }

    pthread_mutex_unlock(&st.mutex);
}

/* Catalogue totals, in the notes above the map key */
static void legend_debris(cairo_t *cr, int w, int h, time_t now, void *ud)
{
    char t[200], a[16], b[16], c[16], d[16];
    int ok;
    (void)now; (void)ud;
    pthread_mutex_lock(&st.mutex);
    if (st.stats.ok) {
        fmt_thousands(a, sizeof(a), st.stats.total);
        fmt_thousands(b, sizeof(b), st.stats.payload);
        fmt_thousands(c, sizeof(c), st.stats.rocket);
        fmt_thousands(d, sizeof(d), st.stats.debris);
        snprintf(t, sizeof(t), "In orbit: %s objects \xc2\xb7 %s satellites \xc2\xb7 %s rocket bodies"
                 " \xc2\xb7 %s debris \xc2\xb7 %d expected to re-enter this week", a, b, c, d, st.stats.week);
    }
    ok = st.stats.ok;
    pthread_mutex_unlock(&st.mutex);
    if (ok) pic_draw_legend_line(cr, w, h, 0, t, .8, .9, 1);
}

/* ================================================================ features */

static int idle_part(int part)
{
    int n;
    pthread_mutex_lock(&st.mutex);
    n = part == P_RE ? st.nre : part == P_CJ ? st.ncj : st.ngeo;
    pthread_mutex_unlock(&st.mutex);
    if (n == 0) nbox[part] = 0;        /* nothing drawn: its labels are gone */
    return n == 0;
}
static int idle_re(time_t now, void *d)  { (void)now; (void)d; return idle_part(P_RE); }
static int idle_cj(time_t now, void *d)  { (void)now; (void)d; return idle_part(P_CJ); }
static int idle_geo(time_t now, void *d) { (void)now; (void)d; return idle_part(P_GEO); }

static pic_feature_t f_re = {
    .name = "reentries", .layer = render_reentries, .idle = idle_re,
    .interval = 10, .extra_mb = 1, .work = work_re,
};
static pic_feature_t f_cj = {
    .name = "conjunctions", .layer = render_conj, .idle = idle_cj,
    .interval = 15, .extra_mb = 1, .work = work_cj,
};
static pic_feature_t f_geo = {
    .name = "geobelt", .layer = render_geo, .idle = idle_geo,
    .interval = 120, .extra_mb = 4, .work = work_geo, .stopped = stopped_geo,
};
static pic_feature_t f_deb = {
    .name = "debris", .layer = render_debris, .legend = legend_debris,
    .interval = 20, .extra_mb = 2, .work = work_deb, .stopped = stopped_deb,
};

pic_feature_t *pic_feature_reentries(void)    { return &f_re; }
pic_feature_t *pic_feature_conjunctions(void) { return &f_cj; }
pic_feature_t *pic_feature_geobelt(void)      { return &f_geo; }
pic_feature_t *pic_feature_debris(void)       { return &f_deb; }

/* For the tests: read the cache now */
void pic_spacelayers_load_all(time_t now)
{
    int p;
    for (p = 0; p < P_N; p++) {
        st.parsed[p] = 0;
        load(p, now);
    }
}
